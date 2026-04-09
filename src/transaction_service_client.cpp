// -----------------------------------------------------------------------------
// src/transaction_service_client.cpp
// GUI-side D-Bus client for the transaction service
// Starts transaction requests, waits for preview and apply state changes, reads
// structured preview data, forwards service progress lines, and releases
// finished requests when the GUI no longer needs them.
// -----------------------------------------------------------------------------
#include "transaction_service_client.hpp"

#include "debug_trace.hpp"
#include "dnf_backend.hpp"
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

// -----------------------------------------------------------------------------
// Select the D-Bus bus used by the transaction service client
// -----------------------------------------------------------------------------
static GBusType
get_transaction_service_bus_type()
{
  // Docker GUI testing uses the session bus path while native Polkit uses the system bus.
  const char *bus_mode = g_getenv("DNF_UI_TRANSACTION_BUS");
  if (bus_mode && g_strcmp0(bus_mode, "session") == 0) {
    return G_BUS_TYPE_SESSION;
  }

  return G_BUS_TYPE_SYSTEM;
}

// -----------------------------------------------------------------------------
// Build the StartTransaction argument tuple for the D-Bus service call
// -----------------------------------------------------------------------------
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

  // Pack the request arrays in install, remove, and reinstall order.
  return g_variant_new("(asasas)", &install_builder, &remove_builder, &reinstall_builder);
}

// -----------------------------------------------------------------------------
// Connect to the D-Bus transaction service used by the GUI client
// -----------------------------------------------------------------------------
static GDBusConnection *
connect_transaction_service(std::string &error_out)
{
  error_out.clear();

  GError *error = nullptr;
  GBusType bus_type = get_transaction_service_bus_type();
  DNF_UI_TRACE("Transaction service client connect bus=%s", bus_type == G_BUS_TYPE_SESSION ? "session" : "system");
  GDBusConnection *connection = g_bus_get_sync(bus_type, nullptr, &error);
  if (!connection) {
    error_out = error ? error->message : "Could not connect to the transaction service bus.";
    g_clear_error(&error);
    return nullptr;
  }

  return connection;
}

// -----------------------------------------------------------------------------
// Start a new transaction request and return its service object path
// -----------------------------------------------------------------------------
static bool
start_transaction_request(GDBusConnection *connection,
                          const TransactionRequest &request,
                          std::string &transaction_path_out,
                          std::string &error_out)
{
  transaction_path_out.clear();
  error_out.clear();

  if (!connection) {
    error_out = "Transaction service connection is not available.";
    return false;
  }

  GError *error = nullptr;
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
    return false;
  }

  const gchar *path = nullptr;
  g_variant_get(start_reply, "(&o)", &path);
  transaction_path_out = path ? path : "";
  g_variant_unref(start_reply);

  if (transaction_path_out.empty()) {
    error_out = "Transaction service returned an empty request path.";
    return false;
  }

  DNF_UI_TRACE("Transaction service client start path=%s", transaction_path_out.c_str());

  return true;
}

// -----------------------------------------------------------------------------
// Read the current state of a service transaction request
// -----------------------------------------------------------------------------
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

  // Read stage, finished, success, and details from the GetResult reply.
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

// -----------------------------------------------------------------------------
// Read the structured preview data from a prepared service request
// -----------------------------------------------------------------------------
static bool
get_transaction_preview(GDBusConnection *connection,
                        const std::string &transaction_path,
                        TransactionPreview &preview_out,
                        std::string &error_out)
{
  preview_out = {};
  error_out.clear();

  GError *error = nullptr;
  GVariant *reply = g_dbus_connection_call_sync(connection,
                                                kTransactionServiceName,
                                                transaction_path.c_str(),
                                                kTransactionServiceRequestInterface,
                                                "GetPreview",
                                                nullptr,
                                                G_VARIANT_TYPE("(asasasasasx)"),
                                                G_DBUS_CALL_FLAGS_NONE,
                                                -1,
                                                nullptr,
                                                &error);
  if (!reply) {
    error_out = error ? error->message : "Failed to read transaction service preview.";
    g_clear_error(&error);
    return false;
  }

  // Unpack the preview reply into owned string arrays and the disk space delta.
  gchar **install = nullptr;
  gchar **upgrade = nullptr;
  gchar **downgrade = nullptr;
  gchar **reinstall = nullptr;
  gchar **remove = nullptr;
  gint64 disk_space_delta = 0;
  g_variant_get(reply, "(^as^as^as^as^asx)", &install, &upgrade, &downgrade, &reinstall, &remove, &disk_space_delta);

  auto append_specs = [](std::vector<std::string> &target, gchar **specs) {
    if (!specs) {
      return;
    }

    for (gchar **it = specs; *it; ++it) {
      target.emplace_back(*it);
    }
  };

  append_specs(preview_out.install, install);
  append_specs(preview_out.upgrade, upgrade);
  append_specs(preview_out.downgrade, downgrade);
  append_specs(preview_out.reinstall, reinstall);
  append_specs(preview_out.remove, remove);
  preview_out.disk_space_delta = static_cast<long long>(disk_space_delta);

  g_strfreev(install);
  g_strfreev(upgrade);
  g_strfreev(downgrade);
  g_strfreev(reinstall);
  g_strfreev(remove);
  g_variant_unref(reply);

  return true;
}

// -----------------------------------------------------------------------------
// Release a finished service request that is no longer needed by the GUI
// -----------------------------------------------------------------------------
static bool
release_transaction_request(GDBusConnection *connection, const std::string &transaction_path, std::string &error_out)
{
  error_out.clear();

  if (!connection || transaction_path.empty()) {
    return true;
  }

  GError *error = nullptr;
  GVariant *reply = g_dbus_connection_call_sync(connection,
                                                kTransactionServiceName,
                                                transaction_path.c_str(),
                                                kTransactionServiceRequestInterface,
                                                "Release",
                                                nullptr,
                                                nullptr,
                                                G_DBUS_CALL_FLAGS_NONE,
                                                -1,
                                                nullptr,
                                                &error);
  if (!reply) {
    error_out = error ? error->message : "Failed to release transaction service request.";
    g_clear_error(&error);
    return false;
  }

  g_variant_unref(reply);
  return true;
}

// -----------------------------------------------------------------------------
// Wait until the service request leaves the current running stage
// -----------------------------------------------------------------------------
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

// -----------------------------------------------------------------------------
// Forward transaction progress signals from the service to the GUI callback
// -----------------------------------------------------------------------------
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
// Resolve a service-backed transaction preview and return its request path
// -----------------------------------------------------------------------------
bool
transaction_service_client_preview_request(const TransactionRequest &request,
                                           TransactionPreview &preview_out,
                                           std::string &transaction_path_out,
                                           std::string &error_out)
{
  preview_out = {};
  transaction_path_out.clear();
  error_out.clear();

  if (!request.validate(error_out)) {
    return false;
  }

  std::string connect_error;
  GDBusConnection *connection = connect_transaction_service(connect_error);
  if (!connection) {
    error_out = connect_error;
    return false;
  }

  TransactionServiceResult result;
  if (!start_transaction_request(connection, request, transaction_path_out, error_out)) {
    g_object_unref(connection);
    return false;
  }

  if (!wait_for_transaction_stage(connection, transaction_path_out, "preview-running", nullptr, result, error_out)) {
    std::string release_error;
    release_transaction_request(connection, transaction_path_out, release_error);
    g_object_unref(connection);
    return false;
  }

  if (result.stage != "preview-ready" || !result.finished || !result.success) {
    error_out = result.details.empty() ? "Privileged transaction preview failed." : result.details;
    DNF_UI_TRACE(
        "Transaction service client preview failed path=%s error=%s", transaction_path_out.c_str(), error_out.c_str());
    std::string release_error;
    release_transaction_request(connection, transaction_path_out, release_error);
    g_object_unref(connection);
    return false;
  }

  if (!get_transaction_preview(connection, transaction_path_out, preview_out, error_out)) {
    DNF_UI_TRACE("Transaction service client get preview failed path=%s error=%s",
                 transaction_path_out.c_str(),
                 error_out.c_str());
    std::string release_error;
    release_transaction_request(connection, transaction_path_out, release_error);
    g_object_unref(connection);
    return false;
  }

  g_object_unref(connection);

  return true;
}

// -----------------------------------------------------------------------------
// Apply a previously previewed transaction request through the service
// -----------------------------------------------------------------------------
bool
transaction_service_client_apply_started_request(const std::string &transaction_path,
                                                 const std::function<void(const std::string &)> &progress_callback,
                                                 std::string &error_out)
{
  error_out.clear();

  if (transaction_path.empty()) {
    error_out = "Transaction service request path is empty.";
    return false;
  }

  auto append_progress = [&](const std::string &message) {
    if (progress_callback && !message.empty()) {
      progress_callback(message);
    }
  };

  append_progress("Connecting to transaction service...");

  std::string connect_error;
  GDBusConnection *connection = connect_transaction_service(connect_error);
  if (!connection) {
    error_out = connect_error;
    return false;
  }

  bool ok = false;
  TransactionServiceResult result;
  GMainContext *signal_context = g_main_context_new();
  g_main_context_push_thread_default(signal_context);
  TransactionServiceProgressForwarder progress_forwarder { &progress_callback };
  guint progress_subscription_id = 0;

  do {
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

    append_progress("Privileged transaction preview ready.");
    append_progress("Requesting authorization and starting apply...");

    GError *error = nullptr;
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

// -----------------------------------------------------------------------------
// Release a finished service request after it has been applied or discarded
// -----------------------------------------------------------------------------
void
transaction_service_client_release_request(const std::string &transaction_path)
{
  if (transaction_path.empty()) {
    return;
  }

  std::string connect_error;
  GDBusConnection *connection = connect_transaction_service(connect_error);
  if (!connection) {
    DNF_UI_TRACE("Transaction service client release connect failed path=%s error=%s",
                 transaction_path.c_str(),
                 connect_error.c_str());
    return;
  }

  std::string error_out;
  if (!release_transaction_request(connection, transaction_path, error_out)) {
    DNF_UI_TRACE(
        "Transaction service client release failed path=%s error=%s", transaction_path.c_str(), error_out.c_str());
  } else {
    DNF_UI_TRACE("Transaction service client release done path=%s", transaction_path.c_str());
  }

  g_object_unref(connection);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
