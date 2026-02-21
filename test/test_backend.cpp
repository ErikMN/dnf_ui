#include <catch2/catch_test_macros.hpp>

#include "dnf_backend.hpp"
#include "base_manager.hpp"
#include "test_utils.hpp"

TEST_CASE("Base initializes and installed packages can be queried")
{
  reset_backend_globals();

  auto installed = get_installed_packages();

  REQUIRE(installed.size() > 0);
}

TEST_CASE("Package info contains expected fields")
{
  reset_backend_globals();

  auto results = search_available_packages("bash");
  REQUIRE(results.size() > 0);

  auto info = get_package_info(results.front());

  REQUIRE(info.find("Name:") != std::string::npos);
  REQUIRE(info.find("Version:") != std::string::npos);
  REQUIRE(info.find("Summary:") != std::string::npos);
}
