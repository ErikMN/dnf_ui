// -----------------------------------------------------------------------------
// test/test_offline.cpp
// Offline behavior tests
// Covers cached repo search without network and remove-only transaction preview
// behavior when remote repo refresh is unavailable.
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "base_manager.hpp"
#include "dnf_backend/dnf_backend.hpp"
#include "test_utils.hpp"
#include "transaction_request.hpp"
#include "transaction_service_client.hpp"

#include <gio/gio.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>

#ifndef DNFUI_TEST_SERVICE_BIN
#define DNFUI_TEST_SERVICE_BIN ""
#endif

namespace {

// Save one environment variable and restore its previous state on scope exit.
struct ScopedEnvironmentOverride {
  // -----------------------------------------------------------------------------
  // ScopedEnvironmentOverride
  // -----------------------------------------------------------------------------
  explicit ScopedEnvironmentOverride(const char *variable_name)
      : name(variable_name ? variable_name : "")
  {
    const char *current_value = g_getenv(name.c_str());
    if (current_value) {
      had_value = true;
      value = current_value;
    }
  }

  // -----------------------------------------------------------------------------
  // ~ScopedEnvironmentOverride
  // -----------------------------------------------------------------------------
  ~ScopedEnvironmentOverride()
  {
    if (had_value) {
      g_setenv(name.c_str(), value.c_str(), TRUE);
    } else {
      g_unsetenv(name.c_str());
    }
  }

  std::string name;
  bool had_value = false;
  std::string value;
};

// -----------------------------------------------------------------------------
// Return true when the private test bus reports that the service name is owned.
// -----------------------------------------------------------------------------
static bool
wait_for_bus_name_owner(GDBusConnection *connection, const char *service_name, int timeout_ms)
{
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

  while (std::chrono::steady_clock::now() < deadline) {
    GError *error = nullptr;
    GVariant *reply = g_dbus_connection_call_sync(connection,
                                                  "org.freedesktop.DBus",
                                                  "/org/freedesktop/DBus",
                                                  "org.freedesktop.DBus",
                                                  "NameHasOwner",
                                                  g_variant_new("(s)", service_name),
                                                  G_VARIANT_TYPE("(b)"),
                                                  G_DBUS_CALL_FLAGS_NONE,
                                                  -1,
                                                  nullptr,
                                                  &error);
    if (reply) {
      gboolean has_owner = FALSE;
      g_variant_get(reply, "(b)", &has_owner);
      g_variant_unref(reply);
      if (has_owner) {
        return true;
      }
    }

    g_clear_error(&error);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  return false;
}

} // namespace

TEST_CASE("Offline cached search finds a repo package", "[offline]")
{
  const char *repo_spec = g_getenv("DNFUI_TEST_OFFLINE_REPO_SPEC");
  if (!repo_spec || !*repo_spec) {
    SKIP("Set DNFUI_TEST_OFFLINE_REPO_SPEC to run the offline cached-search test.");
  }

  reset_backend_globals();

  auto &mgr = BaseManager::instance();
  mgr.reset_for_tests();
  set_backend_search_options(false, true);

  auto installed_rows = dnf_backend_get_installed_package_rows_interruptible(nullptr);
  REQUIRE_FALSE(std::any_of(
      installed_rows.begin(), installed_rows.end(), [&](const PackageRow &row) { return row.name == repo_spec; }));

  auto results = dnf_backend_search_package_rows_interruptible(repo_spec, nullptr);

  INFO(repo_spec);
  REQUIRE(std::any_of(results.begin(), results.end(), [&](const PackageRow &row) { return row.name == repo_spec; }));

  mgr.reset_for_tests();
}

TEST_CASE("Offline remove-only service preview stays local-first", "[offline]")
{
  REQUIRE(std::string(DNFUI_TEST_SERVICE_BIN).size() > 0);

  reset_backend_globals();

  auto installed_rows = dnf_backend_get_installed_package_rows_interruptible(nullptr);
  REQUIRE_FALSE(installed_rows.empty());
  const PackageRow &installed_row = installed_rows.front();

  GTestDBus *test_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  REQUIRE(test_bus != nullptr);
  g_test_dbus_up(test_bus);

  ScopedEnvironmentOverride session_bus_address_env("DBUS_SESSION_BUS_ADDRESS");
  ScopedEnvironmentOverride transaction_bus_env("DNFUI_TRANSACTION_BUS");

  const char *bus_address = g_test_dbus_get_bus_address(test_bus);
  REQUIRE(bus_address != nullptr);
  REQUIRE(g_setenv("DBUS_SESSION_BUS_ADDRESS", bus_address, TRUE));
  REQUIRE(g_setenv("DNFUI_TRANSACTION_BUS", "session", TRUE));

  GSubprocessLauncher *launcher = g_subprocess_launcher_new(
      static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE));
  REQUIRE(launcher != nullptr);
  g_subprocess_launcher_setenv(launcher, "DBUS_SESSION_BUS_ADDRESS", bus_address, TRUE);
  g_subprocess_launcher_setenv(launcher, "DNFUI_TEST_FORCE_FULL_REPO_LOAD_FAILURE", "1", TRUE);
  g_subprocess_launcher_setenv(launcher, "DNFUI_TEST_FORCE_CACHEONLY_REPO_LOAD_FAILURE", "1", TRUE);

  GError *error = nullptr;
  const char *service_argv[] = {
    DNFUI_TEST_SERVICE_BIN,
    "--session",
    nullptr,
  };
  GSubprocess *service = g_subprocess_launcher_spawnv(launcher, service_argv, &error);
  std::string error_text = error && error->message ? error->message : "";
  INFO(error_text);
  REQUIRE(service != nullptr);
  g_object_unref(launcher);

  GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  error_text = error && error->message ? error->message : "";
  INFO(error_text);
  REQUIRE(connection != nullptr);
  REQUIRE(wait_for_bus_name_owner(connection, "com.fedora.Dnfui.Transaction1", 5000));

  TransactionRequest request;
  request.remove.push_back(installed_row.nevra);

  TransactionPreview preview;
  std::string transaction_path;
  std::string preview_error;
  bool ok = transaction_service_client_preview_request(request, preview, transaction_path, preview_error);

  INFO(preview_error);
  REQUIRE(ok);
  REQUIRE(std::any_of(preview.remove.begin(), preview.remove.end(), [&](const std::string &label) {
    return label == installed_row.nevra;
  }));

  transaction_service_client_release_request(transaction_path);
  transaction_service_client_reset_for_tests();
  g_object_unref(connection);
  g_subprocess_force_exit(service);
  g_object_unref(service);
  g_test_dbus_down(test_bus);
  g_object_unref(test_bus);
}
