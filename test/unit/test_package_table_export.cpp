#include <catch2/catch_test_macros.hpp>

#include "ui/package_table/package_table_export_csv.hpp"

// -----------------------------------------------------------------------------
// Verify that the CSV formatter writes simple headers and rows.
// -----------------------------------------------------------------------------
TEST_CASE("Package table CSV export writes headers and rows")
{
  std::string csv = package_table_export_csv_text({ "Name", "Version" }, { { "bash", "5.3.9" } });

  REQUIRE(csv == "Name,Version\nbash,5.3.9\n");
}

// -----------------------------------------------------------------------------
// Verify that fields with CSV control characters are quoted correctly.
// -----------------------------------------------------------------------------
TEST_CASE("Package table CSV export escapes quoted fields")
{
  std::string csv = package_table_export_csv_text({ "Name", "Summary" },
                                                  { { "demo", "Contains comma, quote \" and newline\ntext" } });

  REQUIRE(csv == "Name,Summary\ndemo,\"Contains comma, quote \"\" and newline\ntext\"\n");
}

// -----------------------------------------------------------------------------
// Verify that fields beginning with spreadsheet formula markers are protected.
// -----------------------------------------------------------------------------
TEST_CASE("Package table CSV export protects spreadsheet formula fields")
{
  std::string csv =
      package_table_export_csv_text({ "Summary" }, { { "=SUM(1,1)" }, { "+cmd" }, { "-cmd" }, { "@formula" } });

  REQUIRE(csv == "Summary\n\"'=SUM(1,1)\"\n'+cmd\n'-cmd\n'@formula\n");
}

// -----------------------------------------------------------------------------
// Verify that leading whitespace does not bypass formula protection.
// -----------------------------------------------------------------------------
TEST_CASE("Package table CSV export protects formula fields after whitespace")
{
  std::string csv = package_table_export_csv_text({ "Summary" }, { { "  =SUM(1,1)" } });

  REQUIRE(csv == "Summary\n\"'  =SUM(1,1)\"\n");
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
