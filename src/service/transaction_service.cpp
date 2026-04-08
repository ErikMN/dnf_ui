#include "transaction_service.hpp"

#include "base_manager.hpp"
#include "debug_trace.hpp"
#include "dnf_backend.hpp"
#include "transaction_request.hpp"

#include <gio/gio.h>
#include <glib-unix.h>

#include <atomic>
#include <cstdio>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

// -----------------------------------------------------------------------------
// Transaction service D-Bus names and introspection data
// -----------------------------------------------------------------------------
constexpr const char *kServiceName = "com.fedora.Dnfui.Transaction1";
constexpr const char *kManagerObjectPath = "/com/fedora/Dnfui/Transaction1";
constexpr const char *kManagerInterface = "com.fedora.Dnfui.Transaction1";
constexpr const char *kTransactionInterface = "com.fedora.Dnfui.TransactionRequest1";

constexpr const char *kManagerIntrospectionXml = R"XML(
<node>
  <interface name="com.fedora.Dnfui.Transaction1">
    <method name="StartTransaction">
      <arg name="install" type="as" direction="in"/>
      <arg name="remove" type="as" direction="in"/>
      <arg name="reinstall" type="as" direction="in"/>
      <arg name="transaction_path" type="o" direction="out"/>
    </method>
  </interface>
</node>
)XML";

constexpr const char *kTransactionIntrospectionXml = R"XML(
<node>
  <interface name="com.fedora.Dnfui.TransactionRequest1">
    <method name="Cancel"/>
    <method name="Apply"/>
    <method name="GetResult">
      <arg name="stage" type="s" direction="out"/>
      <arg name="finished" type="b" direction="out"/>
      <arg name="success" type="b" direction="out"/>
      <arg name="details" type="s" direction="out"/>
    </method>
    <signal name="Progress">
      <arg name="line" type="s"/>
    </signal>
    <signal name="Finished">
      <arg name="stage" type="s"/>
      <arg name="success" type="b"/>
      <arg name="details" type="s"/>
    </signal>
  </interface>
</node>
)XML";

// -----------------------------------------------------------------------------
// Transaction service runtime state
// -----------------------------------------------------------------------------
struct TransactionService;

enum class TransactionStage {
  PREVIEW_RUNNING,
  PREVIEW_READY,
  PREVIEW_FAILED,
  APPLY_RUNNING,
  APPLY_SUCCEEDED,
  APPLY_FAILED,
  CANCELLED,
};

struct TransactionSession {
  TransactionService *service = nullptr;
  guint registration_id = 0;
  std::string object_path;
  TransactionRequest request;
  std::atomic<bool> finished { false };
  std::atomic<bool> cancelled { false };
  TransactionStage stage = TransactionStage::PREVIEW_RUNNING;
  bool success = false;
  std::string details;
};

struct TransactionService {
  GMainLoop *loop = nullptr;
  GDBusConnection *connection = nullptr;
  GDBusNodeInfo *manager_node_info = nullptr;
  GDBusNodeInfo *transaction_node_info = nullptr;
  guint owner_id = 0;
  guint manager_registration_id = 0;
  guint next_transaction_id = 1;
  std::atomic<bool> apply_running { false };
  std::map<std::string, std::unique_ptr<TransactionSession>> transactions;
};

// -----------------------------------------------------------------------------
// Transaction session signal helpers
// -----------------------------------------------------------------------------
static void emit_transaction_progress(TransactionSession *session, const std::string &line);
static void emit_transaction_finished(TransactionSession *session,
                                      TransactionStage stage,
                                      bool success,
                                      const std::string &details);

// -----------------------------------------------------------------------------
// Transaction preview formatting
// -----------------------------------------------------------------------------
static std::string format_transaction_preview_details(const TransactionPreview &preview);
static const char *transaction_stage_name(TransactionStage stage);

// -----------------------------------------------------------------------------
// Transaction request conversion
// -----------------------------------------------------------------------------
static TransactionRequest
request_from_variant(GVariant *parameters)
{
  gchar **install = nullptr;
  gchar **remove = nullptr;
  gchar **reinstall = nullptr;
  g_variant_get(parameters, "(^as^as^as)", &install, &remove, &reinstall);

  TransactionRequest request;

  auto append_specs = [](std::vector<std::string> &target, gchar **specs) {
    if (!specs) {
      return;
    }
    for (gchar **it = specs; *it; ++it) {
      target.emplace_back(*it);
    }
  };

  append_specs(request.install, install);
  append_specs(request.remove, remove);
  append_specs(request.reinstall, reinstall);

  g_strfreev(install);
  g_strfreev(remove);
  g_strfreev(reinstall);

  return request;
}

// -----------------------------------------------------------------------------
// Main loop dispatch helpers
// -----------------------------------------------------------------------------
struct QueuedProgressMessage {
  TransactionSession *session = nullptr;
  std::string line;
};

struct QueuedFinishedResult {
  TransactionSession *session = nullptr;
  TransactionStage stage = TransactionStage::PREVIEW_FAILED;
  bool success = false;
  std::string details;
};

static gboolean
dispatch_transaction_progress(gpointer user_data)
{
  std::unique_ptr<QueuedProgressMessage> message(static_cast<QueuedProgressMessage *>(user_data));
  if (!message || !message->session) {
    return G_SOURCE_REMOVE;
  }

  emit_transaction_progress(message->session, message->line);
  return G_SOURCE_REMOVE;
}

static gboolean
dispatch_transaction_finished(gpointer user_data)
{
  std::unique_ptr<QueuedFinishedResult> result(static_cast<QueuedFinishedResult *>(user_data));
  if (!result || !result->session) {
    return G_SOURCE_REMOVE;
  }

  emit_transaction_finished(result->session, result->stage, result->success, result->details);
  return G_SOURCE_REMOVE;
}

static void
emit_transaction_progress(TransactionSession *session, const std::string &line)
{
  if (!session || !session->service || !session->service->connection || session->finished.load()) {
    return;
  }

  g_dbus_connection_emit_signal(session->service->connection,
                                nullptr,
                                session->object_path.c_str(),
                                kTransactionInterface,
                                "Progress",
                                g_variant_new("(s)", line.c_str()),
                                nullptr);
}

static void
emit_transaction_finished(TransactionSession *session, TransactionStage stage, bool success, const std::string &details)
{
  if (!session || !session->service || !session->service->connection) {
    return;
  }

  bool expected = false;
  if (!session->finished.compare_exchange_strong(expected, true)) {
    return;
  }

  session->stage = stage;
  session->success = success;
  session->details = details;
  g_dbus_connection_emit_signal(session->service->connection,
                                nullptr,
                                session->object_path.c_str(),
                                kTransactionInterface,
                                "Finished",
                                g_variant_new("(sbs)", transaction_stage_name(stage), success, details.c_str()),
                                nullptr);
}

static void
queue_transaction_progress(TransactionSession *session, const std::string &line)
{
  if (!session || line.empty() || session->finished.load()) {
    return;
  }

  auto *message = new QueuedProgressMessage();
  message->session = session;
  message->line = line;
  g_main_context_invoke(nullptr, dispatch_transaction_progress, message);
}

static void
queue_transaction_finished(TransactionSession *session,
                           TransactionStage stage,
                           bool success,
                           const std::string &details)
{
  if (!session) {
    return;
  }

  auto *result = new QueuedFinishedResult();
  result->session = session;
  result->stage = stage;
  result->success = success;
  result->details = details;
  g_main_context_invoke(nullptr, dispatch_transaction_finished, result);
}

static std::string
format_transaction_preview_space_change(long long delta_bytes)
{
  if (delta_bytes == 0) {
    return "Disk space usage will be unchanged.";
  }

  unsigned long long abs_bytes =
      delta_bytes > 0 ? static_cast<unsigned long long>(delta_bytes) : static_cast<unsigned long long>(-delta_bytes);
  char *formatted = g_format_size(abs_bytes);
  std::string line;

  if (delta_bytes > 0) {
    line = std::string(formatted) + " extra disk space will be used.";
  } else {
    line = std::string(formatted) + " of disk space will be freed.";
  }

  g_free(formatted);
  return line;
}

static void
append_transaction_preview_section(std::ostringstream &summary,
                                   const char *title,
                                   const std::vector<std::string> &items)
{
  if (!title || items.empty()) {
    return;
  }

  summary << title << ":\n";
  for (const auto &item : items) {
    summary << "  " << item << "\n";
  }
  summary << "\n";
}

static std::string
format_transaction_preview_details(const TransactionPreview &preview)
{
  std::ostringstream summary;

  auto append_count_line = [&](size_t count, const char *verb) {
    if (count == 0) {
      return;
    }
    summary << count << " package" << (count == 1 ? "" : "s") << " will be " << verb << ".\n";
  };

  append_count_line(preview.install.size(), "installed");
  append_count_line(preview.upgrade.size(), "upgraded");
  append_count_line(preview.downgrade.size(), "downgraded");
  append_count_line(preview.reinstall.size(), "reinstalled");
  append_count_line(preview.remove.size(), "removed");
  summary << format_transaction_preview_space_change(preview.disk_space_delta) << "\n\n";

  append_transaction_preview_section(summary, "To be installed", preview.install);
  append_transaction_preview_section(summary, "To be upgraded", preview.upgrade);
  append_transaction_preview_section(summary, "To be downgraded", preview.downgrade);
  append_transaction_preview_section(summary, "To be reinstalled", preview.reinstall);
  append_transaction_preview_section(summary, "To be removed", preview.remove);

  return summary.str();
}

static const char *
transaction_stage_name(TransactionStage stage)
{
  switch (stage) {
  case TransactionStage::PREVIEW_RUNNING:
    return "preview-running";
  case TransactionStage::PREVIEW_READY:
    return "preview-ready";
  case TransactionStage::PREVIEW_FAILED:
    return "preview-failed";
  case TransactionStage::APPLY_RUNNING:
    return "apply-running";
  case TransactionStage::APPLY_SUCCEEDED:
    return "apply-succeeded";
  case TransactionStage::APPLY_FAILED:
    return "apply-failed";
  case TransactionStage::CANCELLED:
    return "cancelled";
  }

  return "unknown";
}

static void
set_transaction_running(TransactionSession *session, TransactionStage stage)
{
  if (!session) {
    return;
  }

  session->stage = stage;
  session->finished = false;
  session->success = false;
  session->details.clear();
}

// -----------------------------------------------------------------------------
// Transaction execution
// -----------------------------------------------------------------------------
static void
run_transaction_preview(TransactionSession *session)
{
  if (!session) {
    return;
  }

  TransactionPreview preview;
  std::string error_out;
  auto progress_cb = [session](const std::string &line) { queue_transaction_progress(session, line); };

  DNF_UI_TRACE("Transaction service preview start path=%s", session->object_path.c_str());
  bool ok = dnf_backend_preview_transaction(
      session->request.install, session->request.remove, session->request.reinstall, preview, error_out, progress_cb);

  if (session->cancelled.load()) {
    DNF_UI_TRACE("Transaction service preview cancelled path=%s", session->object_path.c_str());
    queue_transaction_finished(session, TransactionStage::CANCELLED, false, "Transaction preview was cancelled.");
    return;
  }

  if (!ok) {
    DNF_UI_TRACE("Transaction service preview failed path=%s", session->object_path.c_str());
    queue_transaction_finished(session, TransactionStage::PREVIEW_FAILED, false, error_out);
    return;
  }

  DNF_UI_TRACE("Transaction service preview done path=%s items=%zu",
               session->object_path.c_str(),
               preview.install.size() + preview.upgrade.size() + preview.downgrade.size() + preview.reinstall.size() +
                   preview.remove.size());
  queue_transaction_finished(
      session, TransactionStage::PREVIEW_READY, true, format_transaction_preview_details(preview));
}

static gboolean
start_transaction_preview(gpointer user_data)
{
  TransactionSession *session = static_cast<TransactionSession *>(user_data);
  if (!session || session->finished.load()) {
    return G_SOURCE_REMOVE;
  }

  GThread *thread = g_thread_new(
      "dnf-ui-preview",
      +[](gpointer data) -> gpointer {
        run_transaction_preview(static_cast<TransactionSession *>(data));
        return nullptr;
      },
      session);
  g_thread_unref(thread);
  return G_SOURCE_REMOVE;
}

static void
run_transaction_apply(TransactionSession *session)
{
  if (!session || !session->service) {
    return;
  }

  std::string error_out;
  auto progress_cb = [session](const std::string &line) { queue_transaction_progress(session, line); };

  DNF_UI_TRACE("Transaction service apply start path=%s", session->object_path.c_str());
  bool ok = dnf_backend_apply_transaction(
      session->request.install, session->request.remove, session->request.reinstall, error_out, progress_cb);

  std::string details;
  TransactionStage stage = TransactionStage::APPLY_FAILED;
  bool success = false;

  if (ok) {
    details = "Transaction applied successfully.";
    stage = TransactionStage::APPLY_SUCCEEDED;
    success = true;

    try {
      queue_transaction_progress(session, "Refreshing backend state...");
      BaseManager::instance().rebuild();
    } catch (const std::exception &e) {
      details += "\nBackend refresh failed: " + std::string(e.what());
    }
  } else {
    details = error_out;
  }

  session->service->apply_running = false;

  DNF_UI_TRACE("Transaction service apply done path=%s success=%d", session->object_path.c_str(), success ? 1 : 0);
  queue_transaction_finished(session, stage, success, details);
}

static gboolean
start_transaction_apply(gpointer user_data)
{
  TransactionSession *session = static_cast<TransactionSession *>(user_data);
  if (!session) {
    return G_SOURCE_REMOVE;
  }

  GThread *thread = g_thread_new(
      "dnf-ui-apply",
      +[](gpointer data) -> gpointer {
        run_transaction_apply(static_cast<TransactionSession *>(data));
        return nullptr;
      },
      session);
  g_thread_unref(thread);
  return G_SOURCE_REMOVE;
}

// -----------------------------------------------------------------------------
// Per transaction object handling
// -----------------------------------------------------------------------------
static void
on_transaction_method_call(GDBusConnection *,
                           const gchar *,
                           const gchar *object_path,
                           const gchar *interface_name,
                           const gchar *method_name,
                           GVariant *,
                           GDBusMethodInvocation *invocation,
                           gpointer user_data)
{
  TransactionSession *session = static_cast<TransactionSession *>(user_data);
  if (!session) {
    g_dbus_method_invocation_return_error(
        invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "Transaction session is not available.");
    return;
  }

  if (g_strcmp0(interface_name, kTransactionInterface) != 0) {
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown method.");
    return;
  }

  if (g_strcmp0(method_name, "Cancel") == 0) {
    DNF_UI_TRACE("Transaction service cancel path=%s", object_path);
    if (session->stage == TransactionStage::APPLY_RUNNING) {
      g_dbus_method_invocation_return_error(invocation,
                                            G_DBUS_ERROR,
                                            G_DBUS_ERROR_NOT_SUPPORTED,
                                            "Cancellation is not supported while apply is running.");
      return;
    }

    session->cancelled = true;
    if (session->stage == TransactionStage::PREVIEW_READY && session->finished.load()) {
      session->stage = TransactionStage::CANCELLED;
      session->success = false;
      session->details = "Transaction preview was cancelled.";
      emit_transaction_progress(session, "Transaction preview was cancelled.");
      g_dbus_connection_emit_signal(
          session->service->connection,
          nullptr,
          session->object_path.c_str(),
          kTransactionInterface,
          "Finished",
          g_variant_new("(sbs)", transaction_stage_name(TransactionStage::CANCELLED), FALSE, session->details.c_str()),
          nullptr);
      g_dbus_method_invocation_return_value(invocation, nullptr);
      return;
    }

    if (!session->finished.load()) {
      emit_transaction_progress(session, "Transaction preview was cancelled.");
      emit_transaction_finished(session, TransactionStage::CANCELLED, false, "Transaction preview was cancelled.");
    }

    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }

  if (g_strcmp0(method_name, "Apply") == 0) {
    if (session->stage != TransactionStage::PREVIEW_READY || !session->finished.load() || !session->success) {
      g_dbus_method_invocation_return_error(
          invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "Transaction preview must succeed before apply can start.");
      return;
    }

    bool expected = false;
    if (!session->service->apply_running.compare_exchange_strong(expected, true)) {
      g_dbus_method_invocation_return_error(
          invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "Another transaction apply is already running.");
      return;
    }

    session->cancelled = false;
    set_transaction_running(session, TransactionStage::APPLY_RUNNING);
    g_dbus_method_invocation_return_value(invocation, nullptr);
    g_idle_add_full(G_PRIORITY_DEFAULT, start_transaction_apply, session, nullptr);
    return;
  }

  if (g_strcmp0(method_name, "GetResult") == 0) {
    g_dbus_method_invocation_return_value(invocation,
                                          g_variant_new("(sbbs)",
                                                        transaction_stage_name(session->stage),
                                                        session->finished.load(),
                                                        session->success,
                                                        session->details.c_str()));
    return;
  }

  g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown method.");
}

static const GDBusInterfaceVTable kTransactionVTable = {
  on_transaction_method_call,
  nullptr,
  nullptr,
};

static TransactionSession *
create_transaction_session(TransactionService *service, const TransactionRequest &request, std::string &error_out)
{
  error_out.clear();
  if (!service || !service->connection || !service->transaction_node_info) {
    error_out = "Transaction service is not ready.";
    return nullptr;
  }

  auto session = std::make_unique<TransactionSession>();
  session->service = service;
  session->request = request;
  session->object_path =
      std::string(kManagerObjectPath) + "/requests/" + std::to_string(service->next_transaction_id++);

  GError *error = nullptr;
  session->registration_id = g_dbus_connection_register_object(service->connection,
                                                               session->object_path.c_str(),
                                                               service->transaction_node_info->interfaces[0],
                                                               &kTransactionVTable,
                                                               session.get(),
                                                               nullptr,
                                                               &error);
  if (session->registration_id == 0) {
    error_out = error ? error->message : "Failed to register transaction object.";
    g_clear_error(&error);
    return nullptr;
  }

  TransactionSession *raw = session.get();
  service->transactions.emplace(session->object_path, std::move(session));
  return raw;
}

// -----------------------------------------------------------------------------
// Manager object handling
// -----------------------------------------------------------------------------
static void
on_manager_method_call(GDBusConnection *,
                       const gchar *,
                       const gchar *,
                       const gchar *interface_name,
                       const gchar *method_name,
                       GVariant *parameters,
                       GDBusMethodInvocation *invocation,
                       gpointer user_data)
{
  TransactionService *service = static_cast<TransactionService *>(user_data);
  if (!service) {
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "Service is not available.");
    return;
  }

  if (g_strcmp0(interface_name, kManagerInterface) != 0 || g_strcmp0(method_name, "StartTransaction") != 0) {
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown method.");
    return;
  }

  TransactionRequest request = request_from_variant(parameters);
  std::string error_out;
  if (!request.validate(error_out)) {
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "%s", error_out.c_str());
    return;
  }

  DNF_UI_TRACE("Transaction service start install=%zu remove=%zu reinstall=%zu",
               request.install.size(),
               request.remove.size(),
               request.reinstall.size());

  TransactionSession *session = create_transaction_session(service, request, error_out);
  if (!session) {
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "%s", error_out.c_str());
    return;
  }

  g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", session->object_path.c_str()));
  // Queue the preview start after the request object is returned to the caller.
  g_idle_add_full(G_PRIORITY_DEFAULT, start_transaction_preview, session, nullptr);
}

static const GDBusInterfaceVTable kManagerVTable = {
  on_manager_method_call,
  nullptr,
  nullptr,
};

// -----------------------------------------------------------------------------
// Main loop and bus ownership callbacks
// -----------------------------------------------------------------------------
static gboolean
on_quit_signal(gpointer user_data)
{
  GMainLoop *loop = static_cast<GMainLoop *>(user_data);
  if (loop) {
    g_main_loop_quit(loop);
  }
  return G_SOURCE_REMOVE;
}

static void
on_bus_acquired(GDBusConnection *connection, const gchar *, gpointer user_data)
{
  TransactionService *service = static_cast<TransactionService *>(user_data);
  if (!service) {
    return;
  }

  service->connection = G_DBUS_CONNECTION(g_object_ref(connection));
  GError *error = nullptr;
  service->manager_registration_id = g_dbus_connection_register_object(service->connection,
                                                                       kManagerObjectPath,
                                                                       service->manager_node_info->interfaces[0],
                                                                       &kManagerVTable,
                                                                       service,
                                                                       nullptr,
                                                                       &error);

  if (service->manager_registration_id == 0) {
    std::fprintf(stderr, "Failed to register transaction service object: %s\n", error ? error->message : "unknown");
    g_clear_error(&error);
    if (service->loop) {
      g_main_loop_quit(service->loop);
    }
    return;
  }

  DNF_UI_TRACE("Transaction service bus ready");
}

static void
on_name_lost(GDBusConnection *, const gchar *, gpointer user_data)
{
  TransactionService *service = static_cast<TransactionService *>(user_data);
  DNF_UI_TRACE("Transaction service name lost");
  if (service && service->loop) {
    g_main_loop_quit(service->loop);
  }
}

// -----------------------------------------------------------------------------
// Service cleanup
// -----------------------------------------------------------------------------
static void
cleanup_service(TransactionService &service)
{
  for (auto &[path, session] : service.transactions) {
    if (service.connection && session->registration_id != 0) {
      g_dbus_connection_unregister_object(service.connection, session->registration_id);
    }
  }
  service.transactions.clear();

  if (service.connection && service.manager_registration_id != 0) {
    g_dbus_connection_unregister_object(service.connection, service.manager_registration_id);
  }

  if (service.owner_id != 0) {
    g_bus_unown_name(service.owner_id);
  }

  if (service.connection) {
    g_object_unref(service.connection);
    service.connection = nullptr;
  }

  if (service.manager_node_info) {
    g_dbus_node_info_unref(service.manager_node_info);
    service.manager_node_info = nullptr;
  }

  if (service.transaction_node_info) {
    g_dbus_node_info_unref(service.transaction_node_info);
    service.transaction_node_info = nullptr;
  }

  if (service.loop) {
    g_main_loop_unref(service.loop);
    service.loop = nullptr;
  }
}

} // namespace

// -----------------------------------------------------------------------------
// Transaction service entrypoint
// -----------------------------------------------------------------------------
int
transaction_service_run(const TransactionServiceOptions &options)
{
  TransactionService service;
  service.loop = g_main_loop_new(nullptr, FALSE);

  GError *error = nullptr;
  service.manager_node_info = g_dbus_node_info_new_for_xml(kManagerIntrospectionXml, &error);
  if (!service.manager_node_info) {
    std::fprintf(stderr, "Failed to parse manager introspection XML: %s\n", error ? error->message : "unknown");
    g_clear_error(&error);
    cleanup_service(service);
    return 1;
  }

  service.transaction_node_info = g_dbus_node_info_new_for_xml(kTransactionIntrospectionXml, &error);
  if (!service.transaction_node_info) {
    std::fprintf(stderr, "Failed to parse transaction introspection XML: %s\n", error ? error->message : "unknown");
    g_clear_error(&error);
    cleanup_service(service);
    return 1;
  }

  service.owner_id = g_bus_own_name(options.bus_type,
                                    kServiceName,
                                    G_BUS_NAME_OWNER_FLAGS_NONE,
                                    on_bus_acquired,
                                    nullptr,
                                    on_name_lost,
                                    &service,
                                    nullptr);

  g_unix_signal_add(SIGINT, on_quit_signal, service.loop);
  g_unix_signal_add(SIGTERM, on_quit_signal, service.loop);

  DNF_UI_TRACE("Transaction service run loop start");
  g_main_loop_run(service.loop);
  DNF_UI_TRACE("Transaction service run loop stop");

  cleanup_service(service);
  return 0;
}
