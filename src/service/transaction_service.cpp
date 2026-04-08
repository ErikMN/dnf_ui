#include "transaction_service.hpp"

#include "debug_trace.hpp"
#include "transaction_request.hpp"

#include <gio/gio.h>
#include <glib-unix.h>

#include <cstdio>
#include <map>
#include <memory>
#include <string>

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
    <signal name="Progress">
      <arg name="line" type="s"/>
    </signal>
    <signal name="Finished">
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

struct TransactionSession {
  TransactionService *service = nullptr;
  guint registration_id = 0;
  std::string object_path;
  TransactionRequest request;
  bool finished = false;
};

struct TransactionService {
  GMainLoop *loop = nullptr;
  GDBusConnection *connection = nullptr;
  GDBusNodeInfo *manager_node_info = nullptr;
  GDBusNodeInfo *transaction_node_info = nullptr;
  guint owner_id = 0;
  guint manager_registration_id = 0;
  guint next_transaction_id = 1;
  std::map<std::string, std::unique_ptr<TransactionSession>> transactions;
};

// -----------------------------------------------------------------------------
// Transaction session signal helpers
// -----------------------------------------------------------------------------
static void emit_transaction_progress(TransactionSession *session, const std::string &line);
static void emit_transaction_finished(TransactionSession *session, bool success, const std::string &details);

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
// Proof of concept transaction completion
// -----------------------------------------------------------------------------
static gboolean
finish_transaction_poc(gpointer user_data)
{
  TransactionSession *session = static_cast<TransactionSession *>(user_data);
  if (!session || session->finished) {
    return G_SOURCE_REMOVE;
  }

  DNF_UI_TRACE(
      "Transaction service PoC finish path=%s items=%zu", session->object_path.c_str(), session->request.item_count());
  emit_transaction_progress(session, "Transaction service proof of concept accepted the request.");
  emit_transaction_finished(session, false, "Transaction service proof of concept only. No transaction was applied.");
  return G_SOURCE_REMOVE;
}

static void
emit_transaction_progress(TransactionSession *session, const std::string &line)
{
  if (!session || !session->service || !session->service->connection) {
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
emit_transaction_finished(TransactionSession *session, bool success, const std::string &details)
{
  if (!session || !session->service || !session->service->connection || session->finished) {
    return;
  }

  session->finished = true;
  g_dbus_connection_emit_signal(session->service->connection,
                                nullptr,
                                session->object_path.c_str(),
                                kTransactionInterface,
                                "Finished",
                                g_variant_new("(bs)", success, details.c_str()),
                                nullptr);
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

  if (g_strcmp0(interface_name, kTransactionInterface) != 0 || g_strcmp0(method_name, "Cancel") != 0) {
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown method.");
    return;
  }

  DNF_UI_TRACE("Transaction service cancel path=%s", object_path);
  if (!session->finished) {
    emit_transaction_progress(session, "Transaction was cancelled before apply.");
    emit_transaction_finished(session, false, "Transaction was cancelled before apply.");
  }

  g_dbus_method_invocation_return_value(invocation, nullptr);
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
  // Queue the proof of concept completion after the request object is returned to the caller.
  g_idle_add_full(G_PRIORITY_DEFAULT, finish_transaction_poc, session, nullptr);
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
