#include <catch2/catch_test_macros.hpp>

#include "dnf_backend.hpp"
#include "base_manager.hpp"
#include "test_utils.hpp"

#include <map>
#include <mutex>
#include <set>
#include <string>

// -----------------------------------------------------------------------------
// BaseManager safety & generation tests
// -----------------------------------------------------------------------------

TEST_CASE("BaseManager generation increments on rebuild")
{
  auto &mgr = BaseManager::instance();

  auto before = mgr.current_generation();

  mgr.rebuild(); // metadata reload only, no system modification

  auto after = mgr.current_generation();

  REQUIRE(after > before);
}

TEST_CASE("acquire_read returns current generation snapshot")
{
  auto &mgr = BaseManager::instance();

  auto expected = mgr.current_generation();
  auto read = mgr.acquire_read();

  REQUIRE(read.generation == expected);
}

// -----------------------------------------------------------------------------
// Installed package cache consistency tests (read-only)
// -----------------------------------------------------------------------------

TEST_CASE("Installed package cache matches returned list")
{
  reset_backend_globals();

  auto list = dnf_backend_get_installed_package_rows_interruptible(nullptr);

  std::lock_guard<std::mutex> lock(g_installed_mutex);

  REQUIRE(list.size() == g_installed_nevras.size());

  for (const auto &row : list) {
    REQUIRE(g_installed_nevras.count(row.nevra) == 1);
  }
}

TEST_CASE("dnf_backend_refresh_installed_nevras populates installed NEVRA cache")
{
  reset_backend_globals();

  dnf_backend_refresh_installed_nevras();

  std::lock_guard<std::mutex> lock(g_installed_mutex);

  REQUIRE(!g_installed_nevras.empty());
}

// -----------------------------------------------------------------------------
// Search behavior tests (read-only repo metadata)
// -----------------------------------------------------------------------------

TEST_CASE("Searching for impossible package name returns empty result")
{
  reset_backend_globals();

  g_search_in_description = false;
  g_exact_match = false;

  auto results = dnf_backend_search_package_rows_interruptible("___definitely_not_a_real_package_123456___", nullptr);

  REQUIRE(results.empty());
}

TEST_CASE("Exact match results are subset of contains results")
{
  reset_backend_globals();

  g_search_in_description = false;

  g_exact_match = false;
  auto contains = dnf_backend_search_package_rows_interruptible("bash", nullptr);

  g_exact_match = true;
  auto exact = dnf_backend_search_package_rows_interruptible("bash", nullptr);

  auto contains_nevras = package_row_nevras(contains);
  REQUIRE(contains.size() >= exact.size());
  for (const auto &row : exact) {
    INFO(row.nevra);
    REQUIRE(contains_nevras.count(row.nevra) == 1);
  }
}

TEST_CASE("Description search returns superset of name-only search")
{
  reset_backend_globals();

  g_exact_match = false;

  g_search_in_description = false;
  auto name_only = dnf_backend_search_package_rows_interruptible("shell", nullptr);

  g_search_in_description = true;
  auto desc_search = dnf_backend_search_package_rows_interruptible("shell", nullptr);

  auto desc_search_nevras = package_row_nevras(desc_search);
  REQUIRE(desc_search.size() >= name_only.size());
  for (const auto &row : name_only) {
    INFO(row.nevra);
    REQUIRE(desc_search_nevras.count(row.nevra) == 1);
  }
}

TEST_CASE("Cancelled search returns no results")
{
  reset_backend_globals();

  g_search_in_description = false;
  g_exact_match = false;

  GCancellable *cancellable = g_cancellable_new();
  g_cancellable_cancel(cancellable);

  auto results = dnf_backend_search_package_rows_interruptible("bash", cancellable);

  REQUIRE(results.empty());
  g_object_unref(cancellable);
}

// -----------------------------------------------------------------------------
// Package info tests (read-only)
// -----------------------------------------------------------------------------

TEST_CASE("Invalid NEVRA returns friendly message")
{
  auto info = dnf_backend_get_package_info("invalid-0-0.x86_64");

  REQUIRE(info.find("No details found") != std::string::npos);
}

TEST_CASE("Package info formatting contains expected fields")
{
  reset_backend_globals();

  auto results = dnf_backend_get_installed_package_rows_interruptible(nullptr);
  REQUIRE(!results.empty());

  auto info = dnf_backend_get_package_info(results.front().nevra);

  REQUIRE(info.find("Name: ") != std::string::npos);
  REQUIRE(info.find("Package ID: ") != std::string::npos);
  REQUIRE(info.find("Version: ") != std::string::npos);
  REQUIRE(info.find("Release: ") != std::string::npos);
  REQUIRE(info.find("Arch: ") != std::string::npos);
  REQUIRE(info.find("Install Size: ") != std::string::npos);
  REQUIRE(info.find("Install Reason: ") != std::string::npos);
  REQUIRE(info.find("Summary:") != std::string::npos);
  REQUIRE(info.find("Description:") != std::string::npos);
}

// -----------------------------------------------------------------------------
// Structured package row metadata tests
// -----------------------------------------------------------------------------
TEST_CASE("Structured package rows expose searchable metadata")
{
  reset_backend_globals();

  auto results = dnf_backend_search_package_rows_interruptible("bash", nullptr);
  REQUIRE(!results.empty());

  const auto &row = results.front();
  REQUIRE(!row.nevra.empty());
  REQUIRE(!row.name.empty());
  REQUIRE(!row.version.empty());
  REQUIRE(!row.release.empty());
  REQUIRE(!row.arch.empty());
  REQUIRE(!row.repo.empty());
  REQUIRE(!row.display_version().empty());
}

TEST_CASE("Installed package rows expose install reason")
{
  reset_backend_globals();

  auto rows = dnf_backend_get_installed_package_rows_interruptible(nullptr);
  REQUIRE(!rows.empty());

  REQUIRE(std::any_of(rows.begin(), rows.end(), [](const PackageRow &row) {
    return row.install_reason != PackageInstallReason::UNKNOWN;
  }));
}

TEST_CASE("Search results keep one visible EVR per package name and architecture")
{
  reset_backend_globals();

  g_search_in_description = false;
  g_exact_match = false;

  auto results = dnf_backend_search_package_rows_interruptible("bash", nullptr);
  REQUIRE(!results.empty());

  std::map<std::string, std::set<std::string>> versions_by_name_arch;
  for (const auto &row : results) {
    versions_by_name_arch[row.name_arch_key()].insert(row.display_version());
  }

  for (const auto &[key, versions] : versions_by_name_arch) {
    INFO(key);
    REQUIRE(versions.size() == 1);
  }
}

// -----------------------------------------------------------------------------
// Dependency and file list tests (read-only)
// -----------------------------------------------------------------------------

TEST_CASE("Dependency info contains expected section headers")
{
  reset_backend_globals();

  auto results = dnf_backend_search_package_rows_interruptible("bash", nullptr);
  REQUIRE(!results.empty());

  auto deps = dnf_backend_get_package_deps(results.front().nevra);

  REQUIRE(deps.find("Requires:") != std::string::npos);
  REQUIRE(deps.find("Required By:") != std::string::npos);
  REQUIRE(deps.find("Provides:") != std::string::npos);
  REQUIRE(deps.find("Conflicts:") != std::string::npos);
  REQUIRE(deps.find("Obsoletes:") != std::string::npos);
}

TEST_CASE("File list query is safe and returns valid state")
{
  reset_backend_globals();

  auto results = dnf_backend_search_package_rows_interruptible("bash", nullptr);
  REQUIRE(!results.empty());

  auto files = dnf_backend_get_installed_package_files(results.front().nevra);

  // Either it is installed (returns file list)
  // or not installed (returns friendly message).
  bool is_not_installed_msg = files.find("File list available only for installed packages.") != std::string::npos;

  bool has_content = !files.empty();
  if (!is_not_installed_msg) {
    REQUIRE(has_content);
  }
}

TEST_CASE("Exact installed rows distinguish local-only and repo-backed states")
{
  reset_backend_globals();

  PackageRow row;
  row.nevra = "demo-1-1.x86_64";
  row.name = "demo";
  row.arch = "x86_64";

  {
    std::lock_guard<std::mutex> lock(g_installed_mutex);
    g_installed_nevras.insert(row.nevra);
  }

  row.repo_candidate_relation = PackageRepoCandidateRelation::UNKNOWN;
  REQUIRE(dnf_backend_get_package_install_state(row) == PackageInstallState::INSTALLED);

  row.repo_candidate_relation = PackageRepoCandidateRelation::NONE;
  REQUIRE(dnf_backend_get_package_install_state(row) == PackageInstallState::LOCAL_ONLY);

  row.repo_candidate_relation = PackageRepoCandidateRelation::SAME;
  REQUIRE(dnf_backend_get_package_install_state(row) == PackageInstallState::INSTALLED);

  row.repo_candidate_relation = PackageRepoCandidateRelation::NEWER;
  REQUIRE(dnf_backend_get_package_install_state(row) == PackageInstallState::UPGRADEABLE);

  row.repo_candidate_relation = PackageRepoCandidateRelation::OLDER;
  REQUIRE(dnf_backend_get_package_install_state(row) == PackageInstallState::INSTALLED_NEWER_THAN_REPO);
}

TEST_CASE("Default install state sort keeps installed rows first")
{
  REQUIRE(dnf_backend_get_install_state_sort_rank(PackageInstallState::INSTALLED) <
          dnf_backend_get_install_state_sort_rank(PackageInstallState::LOCAL_ONLY));
  REQUIRE(dnf_backend_get_install_state_sort_rank(PackageInstallState::LOCAL_ONLY) <
          dnf_backend_get_install_state_sort_rank(PackageInstallState::UPGRADEABLE));
  REQUIRE(dnf_backend_get_install_state_sort_rank(PackageInstallState::UPGRADEABLE) <
          dnf_backend_get_install_state_sort_rank(PackageInstallState::AVAILABLE));
}

TEST_CASE("Exact installed checks use the cached installed NEVRA snapshot")
{
  reset_backend_globals();

  PackageRow exact_row;
  exact_row.nevra = "demo-1.0-1.x86_64";
  exact_row.name = "demo";
  exact_row.arch = "x86_64";

  PackageRow different_row = exact_row;
  different_row.nevra = "demo-2.0-1.x86_64";

  {
    std::lock_guard<std::mutex> lock(g_installed_mutex);
    g_installed_nevras.clear();
    g_installed_nevras.insert(exact_row.nevra);
  }

  REQUIRE(dnf_backend_is_package_installed_exact(exact_row));
  REQUIRE_FALSE(dnf_backend_is_package_installed_exact(different_row));
}

TEST_CASE("Annotation fallback keeps installed rows usable when repo lookup fails")
{
  reset_backend_globals();

  PackageRow row;
  row.nevra = "demo-1.0-1.x86_64";
  row.name = "demo";
  row.arch = "x86_64";

  {
    std::lock_guard<std::mutex> lock(g_installed_mutex);
    g_installed_nevras.clear();
    g_installed_nevras.insert(row.nevra);
  }

  std::vector<PackageRow> rows { row };
  REQUIRE(dnf_backend_testonly_annotation_fallback_leaves_rows_unknown(rows));
  REQUIRE(rows.size() == 1);
  REQUIRE(dnf_backend_get_package_install_state(rows.front()) == PackageInstallState::INSTALLED);
}
