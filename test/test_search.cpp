#include <catch2/catch_test_macros.hpp>

#include "dnf_backend/dnf_backend.hpp"
#include "test_utils.hpp"

#include <string>

// -----------------------------------------------------------------------------
// Contains search (basic positive case)
// -----------------------------------------------------------------------------

TEST_CASE("Search contains mode returns results for common package")
{
  reset_backend_globals();

  g_search_in_description = false;
  g_exact_match = false;

  auto results = dnf_backend_search_package_rows_interruptible("bash", nullptr);

  REQUIRE(!results.empty());
}

// -----------------------------------------------------------------------------
// Exact match should not match substrings
// -----------------------------------------------------------------------------

TEST_CASE("Search exact mode does not match partial substring")
{
  reset_backend_globals();

  g_search_in_description = false;
  g_exact_match = true;

  auto exact = dnf_backend_search_package_rows_interruptible("ba", nullptr);

  REQUIRE(exact.empty());
}

// -----------------------------------------------------------------------------
// Description search should expand or equal name-only results
// -----------------------------------------------------------------------------

TEST_CASE("Search description mode expands or equals name-only results")
{
  reset_backend_globals();

  g_exact_match = false;

  g_search_in_description = false;
  auto name_only = dnf_backend_search_package_rows_interruptible("shell", nullptr);

  g_search_in_description = true;
  auto desc_search = dnf_backend_search_package_rows_interruptible("shell", nullptr);

  if (desc_search.size() < name_only.size()) {
    FAIL("Description search returned fewer results than name-only search");
  }
}

// -----------------------------------------------------------------------------
// Negative search case
// -----------------------------------------------------------------------------

TEST_CASE("Search returns empty for impossible package name")
{
  reset_backend_globals();

  g_search_in_description = false;
  g_exact_match = false;

  auto results = dnf_backend_search_package_rows_interruptible("___definitely_not_a_real_package_987654___", nullptr);

  REQUIRE(results.empty());
}
