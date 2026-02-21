#include <catch2/catch_test_macros.hpp>

#include "dnf_backend.hpp"
#include "test_utils.hpp"

TEST_CASE("Basic name search returns results")
{
  reset_backend_globals();

  g_search_in_description = false;
  g_exact_match = false;

  auto results = search_available_packages("bash");

  REQUIRE(results.size() > 0);
}

TEST_CASE("Exact match behaves correctly")
{
  reset_backend_globals();

  g_search_in_description = false;
  g_exact_match = true;

  auto results = search_available_packages("bash");

  REQUIRE(results.size() >= 1);
}
