// -----------------------------------------------------------------------------
// test_repository_service_client.cpp
// dnf5daemon repository client tests
// Exercises strict repository-list parsing and opt-in daemon repository changes.
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "dnf5daemon_client/repository_service_client.hpp"

#include <glib.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

struct RepositoryMapSpec {
  const char *id = nullptr;
  const char *name = nullptr;
  bool include_enabled = true;
  bool enabled = false;
  bool enabled_as_string = false;
};

GVariant *
repository_maps_variant(const std::vector<RepositoryMapSpec> &specs)
{
  GVariantBuilder repositories_builder;
  g_variant_builder_init(&repositories_builder, G_VARIANT_TYPE("aa{sv}"));

  for (const auto &spec : specs) {
    GVariantBuilder repository_builder;
    g_variant_builder_init(&repository_builder, G_VARIANT_TYPE("a{sv}"));
    if (spec.id) {
      g_variant_builder_add(&repository_builder, "{sv}", "id", g_variant_new_string(spec.id));
    }
    if (spec.name) {
      g_variant_builder_add(&repository_builder, "{sv}", "name", g_variant_new_string(spec.name));
    }
    if (spec.include_enabled) {
      if (spec.enabled_as_string) {
        g_variant_builder_add(&repository_builder, "{sv}", "enabled", g_variant_new_string("true"));
      } else {
        g_variant_builder_add(&repository_builder, "{sv}", "enabled", g_variant_new_boolean(spec.enabled));
      }
    }
    g_variant_builder_add_value(&repositories_builder, g_variant_builder_end(&repository_builder));
  }

  return g_variant_ref_sink(g_variant_builder_end(&repositories_builder));
}

void
require_repository_client_test_enabled()
{
  const char *enabled = g_getenv("DNFUI_TEST_REPOSITORY_CLIENT");
  if (!enabled || std::string(enabled) != "1") {
    SKIP("Set DNFUI_TEST_REPOSITORY_CLIENT=1 to run repository client daemon tests.");
  }
}

std::vector<RepositoryInfo>
list_repositories_or_fail()
{
  std::vector<RepositoryInfo> repositories;
  std::string error;
  REQUIRE(repository_service_client_list(repositories, error));
  REQUIRE(error.empty());
  return repositories;
}

const RepositoryInfo *
find_repository(const std::vector<RepositoryInfo> &repositories, const std::string &id)
{
  auto it = std::find_if(
      repositories.begin(), repositories.end(), [&](const RepositoryInfo &repository) { return repository.id == id; });

  if (it == repositories.end()) {
    return nullptr;
  }

  return &*it;
}

} // namespace

// -----------------------------------------------------------------------------
// Verify that valid repository maps become sorted plain values.
// -----------------------------------------------------------------------------
TEST_CASE("Repository client parser accepts valid repository list")
{
  GVariant *repositories = repository_maps_variant({
      { "updates", "Fedora Updates", true, true },
      { "fedora", "Fedora", true, false },
  });

  std::vector<RepositoryInfo> parsed;
  std::string error;
  REQUIRE(repository_service_client_testonly_parse_repository_list(repositories, parsed, error));
  g_variant_unref(repositories);

  REQUIRE(error.empty());
  REQUIRE(parsed.size() == 2);
  REQUIRE(parsed[0].id == "fedora");
  REQUIRE(parsed[0].name == "Fedora");
  REQUIRE_FALSE(parsed[0].enabled);
  REQUIRE(parsed[1].id == "updates");
  REQUIRE(parsed[1].name == "Fedora Updates");
  REQUIRE(parsed[1].enabled);
}

// -----------------------------------------------------------------------------
// Verify that an empty repository name falls back to the ID.
// -----------------------------------------------------------------------------
TEST_CASE("Repository client parser falls back to ID for empty name")
{
  GVariant *repositories = repository_maps_variant({
      { "fedora", "", true, true },
  });

  std::vector<RepositoryInfo> parsed;
  std::string error;
  REQUIRE(repository_service_client_testonly_parse_repository_list(repositories, parsed, error));
  g_variant_unref(repositories);

  REQUIRE(error.empty());
  REQUIRE(parsed.size() == 1);
  REQUIRE(parsed[0].name == "fedora");
}

// -----------------------------------------------------------------------------
// Verify that malformed daemon repository objects are rejected.
// -----------------------------------------------------------------------------
TEST_CASE("Repository client parser rejects missing identity and enabled state")
{
  std::vector<RepositoryInfo> parsed;
  std::string error;

  GVariant *missing_id = repository_maps_variant({
      { nullptr, "No ID", true, true },
  });
  REQUIRE_FALSE(repository_service_client_testonly_parse_repository_list(missing_id, parsed, error));
  g_variant_unref(missing_id);
  REQUIRE(error.find("ID") != std::string::npos);
  REQUIRE(parsed.empty());

  GVariant *empty_id = repository_maps_variant({
      { "", "Empty ID", true, true },
  });
  REQUIRE_FALSE(repository_service_client_testonly_parse_repository_list(empty_id, parsed, error));
  g_variant_unref(empty_id);
  REQUIRE(error.find("ID") != std::string::npos);
  REQUIRE(parsed.empty());

  GVariant *missing_enabled = repository_maps_variant({
      { "fedora", "Fedora", false, false },
  });
  REQUIRE_FALSE(repository_service_client_testonly_parse_repository_list(missing_enabled, parsed, error));
  g_variant_unref(missing_enabled);
  REQUIRE(error.find("enabled") != std::string::npos);
  REQUIRE(parsed.empty());

  GVariant *wrong_enabled = repository_maps_variant({
      { "fedora", "Fedora", true, false, true },
  });
  REQUIRE_FALSE(repository_service_client_testonly_parse_repository_list(wrong_enabled, parsed, error));
  g_variant_unref(wrong_enabled);
  REQUIRE(error.find("enabled") != std::string::npos);
  REQUIRE(parsed.empty());
}

// -----------------------------------------------------------------------------
// Verify that duplicate repository IDs are rejected.
// -----------------------------------------------------------------------------
TEST_CASE("Repository client parser rejects duplicate repository IDs")
{
  GVariant *repositories = repository_maps_variant({
      { "fedora", "Fedora", true, true },
      { "fedora", "Fedora Copy", true, false },
  });

  std::vector<RepositoryInfo> parsed;
  std::string error;
  REQUIRE_FALSE(repository_service_client_testonly_parse_repository_list(repositories, parsed, error));
  g_variant_unref(repositories);

  REQUIRE(error.find("duplicate") != std::string::npos);
  REQUIRE(parsed.empty());
}

// -----------------------------------------------------------------------------
// Verify that repository enable and disable changes persist through fresh daemon sessions.
// -----------------------------------------------------------------------------
TEST_CASE("Repository client applies changes through fresh daemon sessions", "[dnf5daemon]")
{
  require_repository_client_test_enabled();

  const char *repo_id_env = g_getenv("DNFUI_TEST_REPOSITORY_CLIENT_REPO_ID");
  const std::string repo_id = repo_id_env && *repo_id_env ? repo_id_env : "dnfui-repository-client-test";

  std::vector<RepositoryInfo> initial = list_repositories_or_fail();
  const RepositoryInfo *initial_repo = find_repository(initial, repo_id);
  REQUIRE(initial_repo);
  REQUIRE_FALSE(initial_repo->enabled);

  RepositoryWriteResult enable_result = repository_service_client_apply_changes({ repo_id }, {});
  REQUIRE(enable_result.enable_attempted);
  REQUIRE(enable_result.enable_succeeded);
  REQUIRE_FALSE(enable_result.disable_attempted);
  REQUIRE(enable_result.disable_succeeded);
  REQUIRE(enable_result.error.empty());

  std::vector<RepositoryInfo> after_enable = list_repositories_or_fail();
  const RepositoryInfo *enabled_repo = find_repository(after_enable, repo_id);
  REQUIRE(enabled_repo);
  REQUIRE(enabled_repo->enabled);

  RepositoryWriteResult disable_result = repository_service_client_apply_changes({}, { repo_id });
  REQUIRE_FALSE(disable_result.enable_attempted);
  REQUIRE(disable_result.enable_succeeded);
  REQUIRE(disable_result.disable_attempted);
  REQUIRE(disable_result.disable_succeeded);
  REQUIRE(disable_result.error.empty());

  std::vector<RepositoryInfo> after_disable = list_repositories_or_fail();
  const RepositoryInfo *disabled_repo = find_repository(after_disable, repo_id);
  REQUIRE(disabled_repo);
  REQUIRE_FALSE(disabled_repo->enabled);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
