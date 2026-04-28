// -----------------------------------------------------------------------------
// test/test_transaction_service_client.cpp
// Transaction service client integration tests
// Covers the GUI-side D-Bus client behavior around request lifecycle failures
// that need a live service process and a private session bus.
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "dnf_backend/dnf_backend.hpp"
#include "transaction_request.hpp"
#include "transaction_service_client.hpp"

#include <gio/gio.h>
#include <glib/gstdio.h>

#include <chrono>
#include <cstdio>
#include <future>
#include <string>
#include <thread>

#ifndef DNFUI_TEST_SERVICE_BIN
#define DNFUI_TEST_SERVICE_BIN ""
#endif

namespace {

struct PreviewClientResult {
  bool ok = false;
  TransactionPreview preview;
  std::string transaction_path;
  std::string error;
};

// Save one environment variable and restore its previous state on scope exit.
struct ScopedEnvironmentOverride {
  // -----------------------------------------------------------------------------
  // Save the original environment variable value.
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
  // Restore the original environment variable value.
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

// -----------------------------------------------------------------------------
// Return true when the preview worker writes its started marker file.
// -----------------------------------------------------------------------------
static bool
wait_for_file(const std::string &path, int timeout_ms)
{
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

  while (std::chrono::steady_clock::now() < deadline) {
    if (g_file_test(path.c_str(), G_FILE_TEST_EXISTS)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  return false;
}

} // namespace

TEST_CASE("Transaction service client reports an error when the service disappears while waiting")
{
  REQUIRE(std::string(DNFUI_TEST_SERVICE_BIN).size() > 0);

  // Start one private session bus so the test does not depend on the user's
  // real desktop bus state.
  GTestDBus *test_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  REQUIRE(test_bus != nullptr);
  g_test_dbus_up(test_bus);

  ScopedEnvironmentOverride session_bus_address_env("DBUS_SESSION_BUS_ADDRESS");
  ScopedEnvironmentOverride transaction_bus_env("DNFUI_TRANSACTION_BUS");

  const char *bus_address = g_test_dbus_get_bus_address(test_bus);
  REQUIRE(bus_address != nullptr);
  REQUIRE(g_setenv("DBUS_SESSION_BUS_ADDRESS", bus_address, TRUE));
  REQUIRE(g_setenv("DNFUI_TRANSACTION_BUS", "session", TRUE));

  GError *error = nullptr;
  gchar *temp_dir = g_dir_make_tmp("dnfui-service-client-XXXXXX", &error);
  std::string error_text = error && error->message ? error->message : "";
  INFO(error_text);
  REQUIRE(temp_dir != nullptr);

  // Start the service with a test-only preview delay so the client has time to
  // enter its wait path before the service is stopped.
  std::string started_file = std::string(temp_dir) + "/preview-started";

  GSubprocessLauncher *launcher = g_subprocess_launcher_new(
      static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE));
  REQUIRE(launcher != nullptr);
  g_subprocess_launcher_setenv(launcher, "DBUS_SESSION_BUS_ADDRESS", bus_address, TRUE);
  g_subprocess_launcher_setenv(launcher, "DNFUI_TEST_PREVIEW_STARTED_FILE", started_file.c_str(), TRUE);
  g_subprocess_launcher_setenv(launcher, "DNFUI_TEST_PREVIEW_DELAY_MS", "10000", TRUE);

  const char *service_argv[] = {
    DNFUI_TEST_SERVICE_BIN,
    "--session",
    nullptr,
  };
  GSubprocess *service = g_subprocess_launcher_spawnv(launcher, service_argv, &error);
  error_text = error && error->message ? error->message : "";
  INFO(error_text);
  REQUIRE(service != nullptr);
  g_object_unref(launcher);

  GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  error_text = error && error->message ? error->message : "";
  INFO(error_text);
  REQUIRE(connection != nullptr);
  REQUIRE(wait_for_bus_name_owner(connection, "com.fedora.Dnfui.Transaction1", 5000));

  // Start one normal preview request through the GUI-side transaction client.
  TransactionRequest request;
  request.install.push_back("bash");

  auto future = std::async(std::launch::async, [request]() {
    PreviewClientResult result;
    result.ok =
        transaction_service_client_preview_request(request, result.preview, result.transaction_path, result.error);
    return result;
  });

  // Stop the service only after the preview worker has started so the test can
  // verify the client behavior while it is already waiting for the result.
  REQUIRE(wait_for_file(started_file, 5000));

  g_subprocess_force_exit(service);

  // The client should report a normal error instead of staying blocked.
  auto wait_status = future.wait_for(std::chrono::seconds(10));
  REQUIRE(wait_status == std::future_status::ready);

  PreviewClientResult result = future.get();
  REQUIRE_FALSE(result.ok);
  REQUIRE(result.error == "Transaction service disappeared while waiting for the result.");

  transaction_service_client_reset_for_tests();
  g_object_unref(connection);
  g_object_unref(service);
  g_remove(started_file.c_str());
  g_rmdir(temp_dir);
  g_free(temp_dir);
  g_test_dbus_down(test_bus);
  g_object_unref(test_bus);
}
