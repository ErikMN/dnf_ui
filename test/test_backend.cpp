#include <catch2/catch_test_macros.hpp>

#include "dnf_backend.hpp"
#include "base_manager.hpp"
#include "test_utils.hpp"

#include <mutex>
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

  auto list = get_installed_package_rows_interruptible(nullptr);

  std::lock_guard<std::mutex> lock(g_installed_mutex);

  REQUIRE(list.size() == g_installed_nevras.size());

  for (const auto &row : list) {
    REQUIRE(g_installed_nevras.count(row.nevra) == 1);
  }
}

TEST_CASE("refresh_installed_nevras populates global sets")
{
  reset_backend_globals();

  refresh_installed_nevras();

  std::lock_guard<std::mutex> lock(g_installed_mutex);

  REQUIRE(!g_installed_nevras.empty());
  REQUIRE(!g_installed_names.empty());
}

// -----------------------------------------------------------------------------
// Search behavior tests (read-only repo metadata)
// -----------------------------------------------------------------------------

TEST_CASE("Searching for impossible package name returns empty result")
{
  reset_backend_globals();

  g_search_in_description = false;
  g_exact_match = false;

  auto results = search_available_package_rows_interruptible("___definitely_not_a_real_package_123456___", nullptr);

  REQUIRE(results.empty());
}

TEST_CASE("Exact match results are subset of contains results")
{
  reset_backend_globals();

  g_search_in_description = false;

  g_exact_match = false;
  auto contains = search_available_package_rows_interruptible("bash", nullptr);

  g_exact_match = true;
  auto exact = search_available_package_rows_interruptible("bash", nullptr);

  REQUIRE(contains.size() >= exact.size());
}

TEST_CASE("Description search returns superset of name-only search")
{
  reset_backend_globals();

  g_exact_match = false;

  g_search_in_description = false;
  auto name_only = search_available_package_rows_interruptible("shell", nullptr);

  g_search_in_description = true;
  auto desc_search = search_available_package_rows_interruptible("shell", nullptr);

  REQUIRE(desc_search.size() >= name_only.size());
}

// -----------------------------------------------------------------------------
// Package info tests (read-only)
// -----------------------------------------------------------------------------

TEST_CASE("Invalid NEVRA returns friendly message")
{
  auto info = get_package_info("invalid-0-0.x86_64");

  REQUIRE(info.find("No details found") != std::string::npos);
}

TEST_CASE("Package info formatting contains expected fields")
{
  reset_backend_globals();

  auto results = search_available_package_rows_interruptible("bash", nullptr);
  REQUIRE(!results.empty());

  auto info = get_package_info(results.front().nevra);

  REQUIRE(info.find("Name: ") != std::string::npos);
  REQUIRE(info.find("Package ID: ") != std::string::npos);
  REQUIRE(info.find("Version: ") != std::string::npos);
  REQUIRE(info.find("Release: ") != std::string::npos);
  REQUIRE(info.find("Arch: ") != std::string::npos);
  REQUIRE(info.find("Summary:") != std::string::npos);
  REQUIRE(info.find("Description:") != std::string::npos);
}

// -----------------------------------------------------------------------------
// Structured package row metadata tests
// -----------------------------------------------------------------------------
TEST_CASE("Structured package rows expose searchable metadata")
{
  reset_backend_globals();

  auto results = search_available_package_rows_interruptible("bash", nullptr);
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

// -----------------------------------------------------------------------------
// Dependency and file list tests (read-only)
// -----------------------------------------------------------------------------

TEST_CASE("Dependency info contains expected section headers")
{
  reset_backend_globals();

  auto results = search_available_package_rows_interruptible("bash", nullptr);
  REQUIRE(!results.empty());

  auto deps = get_package_deps(results.front().nevra);

  REQUIRE(deps.find("Requires:") != std::string::npos);
  REQUIRE(deps.find("Provides:") != std::string::npos);
  REQUIRE(deps.find("Conflicts:") != std::string::npos);
  REQUIRE(deps.find("Obsoletes:") != std::string::npos);
}

TEST_CASE("File list query is safe and returns valid state")
{
  reset_backend_globals();

  auto results = search_available_package_rows_interruptible("bash", nullptr);
  REQUIRE(!results.empty());

  auto files = get_installed_package_files(results.front().nevra);

  // Either it is installed (returns file list)
  // or not installed (returns friendly message).
  bool is_not_installed_msg = files.find("File list available only for installed packages.") != std::string::npos;

  bool has_content = !files.empty();
  if (!is_not_installed_msg) {
    REQUIRE(has_content);
  }
}
