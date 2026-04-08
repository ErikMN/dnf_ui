#include "transaction_service_client.hpp"

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
  g_variant_unref(reply);

  result_out.stage = stage ? stage : "";
  result_out.finished = finished;
  result_out.success = success;
  result_out.details = details ? details : "";
  return true;
}

static bool
wait_for_transaction_stage(GDBusConnection *connection,
                           const std::string &transaction_path,
                           const char *running_stage,
                           TransactionServiceResult &result_out,
                           std::string &error_out)
{
  error_out.clear();

  while (true) {
    if (!get_transaction_result(connection, transaction_path, result_out, error_out)) {
      return false;
    }

    if (result_out.stage != running_stage) {
      return true;
    }

    g_usleep(200 * 1000);
  }
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
  GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
  if (!connection) {
    error_out = error ? error->message : "Could not connect to the transaction service bus.";
    g_clear_error(&error);
    return false;
  }

  bool ok = false;
  TransactionServiceResult result;
  std::string transaction_path;

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

    append_progress("Waiting for privileged transaction preview...");
    if (!wait_for_transaction_stage(connection, transaction_path, "preview-running", result, error_out)) {
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
    if (!wait_for_transaction_stage(connection, transaction_path, "apply-running", result, error_out)) {
      break;
    }

    if (result.stage != "apply-succeeded" || !result.finished || !result.success) {
      error_out = result.details.empty() ? "Privileged apply failed." : result.details;
      break;
    }

    append_progress(result.details.empty() ? "Transaction applied successfully." : result.details);
    ok = true;
  } while (false);

  g_object_unref(connection);
  return ok;
}
