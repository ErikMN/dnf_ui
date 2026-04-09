#include "transaction_service_client.hpp"

#include "debug_trace.hpp"
#include "service/transaction_service_dbus.hpp"
#include "transaction_request.hpp"

#include <gio/gio.h>
#include <glib.h>

#include <string>

namespace {

// -----------------------------------------------------------------------------
// Transaction service result state returned by GetResult
// -----------------------------------------------------------------------------
struct TransactionServiceResult {
  std::string stage;
  bool finished = false;
  bool success = false;
  std::string details;
};

struct TransactionServiceProgressForwarder {
  const std::function<void(const std::string &)> *progress_callback = nullptr;
};

static GBusType
get_transaction_service_bus_type()
{
  const char *bus_mode = g_getenv("DNF_UI_TRANSACTION_BUS");
  if (bus_mode && g_strcmp0(bus_mode, "session") == 0) {
    return G_BUS_TYPE_SESSION;
  }

  return G_BUS_TYPE_SYSTEM;
}

static GVariant *
build_start_transaction_parameters(const TransactionRequest &request)
{
  GVariantBuilder install_builder;
  GVariantBuilder remove_builder;
  GVariantBuilder reinstall_builder;
  g_variant_builder_init(&install_builder, G_VARIANT_TYPE("as"));
  g_variant_builder_init(&remove_builder, G_VARIANT_TYPE("as"));
  g_variant_builder_init(&reinstall_builder, G_VARIANT_TYPE("as"));

  for (const auto &spec : request.install) {
    g_variant_builder_add(&install_builder, "s", spec.c_str());
  }

  for (const auto &spec : request.remove) {
    g_variant_builder_add(&remove_builder, "s", spec.c_str());
  }

  for (const auto &spec : request.reinstall) {
    g_variant_builder_add(&reinstall_builder, "s", spec.c_str());
  }

  return g_variant_new("(asasas)", &install_builder, &remove_builder, &reinstall_builder);
}

static bool
get_transaction_result(GDBusConnection *connection,
                       const std::string &transaction_path,
                       TransactionServiceResult &result_out,
                       std::string &error_out)
{
  result_out = {};
  error_out.clear();

  GError *error = nullptr;
  GVariant *reply = g_dbus_connection_call_sync(connection,
                                                kTransactionServiceName,
                                                transaction_path.c_str(),
                                                kTransactionServiceRequestInterface,
                                                "GetResult",
                                                nullptr,
                                                G_VARIANT_TYPE("(sbbs)"),
                                                G_DBUS_CALL_FLAGS_NONE,
                                                -1,
                                                nullptr,
                                                &error);
  if (!reply) {
    error_out = error ? error->message : "Failed to read transaction service result.";
    g_clear_error(&error);
    return false;
  }

  const gchar *stage = nullptr;
  gboolean finished = FALSE;
  gboolean success = FALSE;
  const gchar *details = nullptr;
  g_variant_get(reply, "(&sbbs)", &stage, &finished, &success, &details);
  result_out.stage = stage ? stage : "";
  result_out.finished = finished;
  result_out.success = success;
  result_out.details = details ? details : "";
  g_variant_unref(reply);
  return true;
}

static bool
wait_for_transaction_stage(GDBusConnection *connection,
                           const std::string &transaction_path,
                           const char *running_stage,
                           GMainContext *signal_context,
                           TransactionServiceResult &result_out,
                           std::string &error_out)
{
  error_out.clear();
  std::string last_stage;

  while (true) {
    if (signal_context) {
      while (g_main_context_pending(signal_context)) {
        g_main_context_iteration(signal_context, FALSE);
      }
    }

    if (!get_transaction_result(connection, transaction_path, result_out, error_out)) {
      return false;
    }

    if (result_out.stage != last_stage) {
      DNF_UI_TRACE("Transaction service client stage path=%s stage=%s finished=%d success=%d",
                   transaction_path.c_str(),
                   result_out.stage.c_str(),
                   result_out.finished ? 1 : 0,
                   result_out.success ? 1 : 0);
      last_stage = result_out.stage;
    }

    if (result_out.stage != running_stage) {
      return true;
    }

    g_usleep(200 * 1000);
  }
}

static void
on_transaction_progress_signal(GDBusConnection *,
                               const gchar *,
                               const gchar *,
                               const gchar *,
                               const gchar *,
                               GVariant *parameters,
                               gpointer user_data)
{
  auto *forwarder = static_cast<TransactionServiceProgressForwarder *>(user_data);
  if (!forwarder || !forwarder->progress_callback || !*forwarder->progress_callback) {
    return;
  }

  const gchar *line = nullptr;
  g_variant_get(parameters, "(&s)", &line);
  if (!line || !*line) {
    return;
  }

  DNF_UI_TRACE("Transaction service client progress line=%s", line);
  (*forwarder->progress_callback)(line);
}

} // namespace

// -----------------------------------------------------------------------------
// Run a complete privileged apply request through the system bus service
// -----------------------------------------------------------------------------
bool
transaction_service_client_apply_request(const TransactionRequest &request,
                                         const std::function<void(const std::string &)> &progress_callback,
                                         std::string &error_out)
{
  error_out.clear();

  if (!request.validate(error_out)) {
    return false;
  }

  auto append_progress = [&](const std::string &message) {
    if (progress_callback && !message.empty()) {
      progress_callback(message);
    }
  };

  append_progress("Connecting to transaction service...");

  GError *error = nullptr;
  GBusType bus_type = get_transaction_service_bus_type();
  DNF_UI_TRACE("Transaction service client connect bus=%s", bus_type == G_BUS_TYPE_SESSION ? "session" : "system");
  GDBusConnection *connection = g_bus_get_sync(bus_type, nullptr, &error);
  if (!connection) {
    error_out = error ? error->message : "Could not connect to the transaction service bus.";
    g_clear_error(&error);
    return false;
  }

  bool ok = false;
  TransactionServiceResult result;
  std::string transaction_path;
  GMainContext *signal_context = g_main_context_new();
  g_main_context_push_thread_default(signal_context);
  TransactionServiceProgressForwarder progress_forwarder { &progress_callback };
  guint progress_subscription_id = 0;

  do {
    GVariant *start_reply = g_dbus_connection_call_sync(connection,
                                                        kTransactionServiceName,
                                                        kTransactionServiceManagerPath,
                                                        kTransactionServiceManagerInterface,
                                                        "StartTransaction",
                                                        build_start_transaction_parameters(request),
                                                        G_VARIANT_TYPE("(o)"),
                                                        G_DBUS_CALL_FLAGS_NONE,
                                                        -1,
                                                        nullptr,
                                                        &error);
    if (!start_reply) {
      error_out = error ? error->message : "Could not start the transaction service request.";
      g_clear_error(&error);
      break;
    }

    const gchar *path = nullptr;
    g_variant_get(start_reply, "(&o)", &path);
    transaction_path = path ? path : "";
    g_variant_unref(start_reply);

    if (transaction_path.empty()) {
      error_out = "Transaction service returned an empty request path.";
      break;
    }

    DNF_UI_TRACE("Transaction service client start path=%s", transaction_path.c_str());
    progress_subscription_id = g_dbus_connection_signal_subscribe(connection,
                                                                  kTransactionServiceName,
                                                                  kTransactionServiceRequestInterface,
                                                                  "Progress",
                                                                  transaction_path.c_str(),
                                                                  nullptr,
                                                                  G_DBUS_SIGNAL_FLAGS_NONE,
                                                                  on_transaction_progress_signal,
                                                                  &progress_forwarder,
                                                                  nullptr);

    append_progress("Waiting for privileged transaction preview...");
    if (!wait_for_transaction_stage(
            connection, transaction_path, "preview-running", signal_context, result, error_out)) {
      break;
    }

    if (result.stage != "preview-ready" || !result.finished || !result.success) {
      error_out = result.details.empty() ? "Privileged transaction preview failed." : result.details;
      break;
    }

    append_progress("Privileged transaction preview ready.");
    append_progress("Requesting authorization and starting apply...");

    GVariant *apply_reply = g_dbus_connection_call_sync(connection,
                                                        kTransactionServiceName,
                                                        transaction_path.c_str(),
                                                        kTransactionServiceRequestInterface,
                                                        "Apply",
                                                        nullptr,
                                                        nullptr,
                                                        G_DBUS_CALL_FLAGS_NONE,
                                                        -1,
                                                        nullptr,
                                                        &error);
    if (!apply_reply) {
      error_out = error ? error->message : "Could not start the privileged apply request.";
      g_clear_error(&error);
      break;
    }
    g_variant_unref(apply_reply);

    append_progress("Waiting for privileged apply to finish...");
    if (!wait_for_transaction_stage(connection, transaction_path, "apply-running", signal_context, result, error_out)) {
      break;
    }

    if (result.stage != "apply-succeeded" || !result.finished || !result.success) {
      error_out = result.details.empty() ? "Privileged apply failed." : result.details;
      break;
    }

    append_progress(result.details.empty() ? "Transaction applied successfully." : result.details);
    DNF_UI_TRACE("Transaction service client apply done path=%s", transaction_path.c_str());
    ok = true;
  } while (false);

  if (!ok) {
    DNF_UI_TRACE(
        "Transaction service client apply failed path=%s error=%s", transaction_path.c_str(), error_out.c_str());
  }

  if (progress_subscription_id != 0) {
    g_dbus_connection_signal_unsubscribe(connection, progress_subscription_id);
  }
  g_main_context_pop_thread_default(signal_context);
  g_main_context_unref(signal_context);
  g_object_unref(connection);
  return ok;
}
