#include <catch2/catch_test_macros.hpp>

#include "dnf_backend.hpp"
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

  auto results = search_available_packages("bash");

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

  auto exact = search_available_packages("ba");

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
  auto name_only = search_available_packages("shell");

  g_search_in_description = true;
  auto desc_search = search_available_packages("shell");

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

  auto results = search_available_packages("___definitely_not_a_real_package_987654___");

  REQUIRE(results.empty());
}
