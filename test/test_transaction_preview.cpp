// -----------------------------------------------------------------------------
// test/test_transaction_preview.cpp
// Backend transaction preview tests
// Covers the public preview API that prepares transaction summaries before the
// service or GUI proceeds to the apply step.
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "dnf_backend.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace {

// -----------------------------------------------------------------------------
// Return true when one progress line contains the expected text
// -----------------------------------------------------------------------------
bool
progress_contains(const std::vector<std::string> &lines, const std::string &needle)
{
  for (const auto &line : lines) {
    if (line.find(needle) != std::string::npos) {
      return true;
    }
  }

  return false;
}

} // namespace

// -----------------------------------------------------------------------------
// Transaction preview request validation tests
// -----------------------------------------------------------------------------

TEST_CASE("Transaction preview rejects an empty request")
{
  reset_backend_globals();

  TransactionPreview preview;
  preview.install.push_back("stale");
  preview.disk_space_delta = 123;
  std::string error;

  bool ok = dnf_backend_preview_transaction({}, {}, {}, preview, error);

  REQUIRE_FALSE(ok);
  REQUIRE(error == "No packages specified in transaction.");
  REQUIRE(preview.install.empty());
  REQUIRE(preview.upgrade.empty());
  REQUIRE(preview.downgrade.empty());
  REQUIRE(preview.reinstall.empty());
  REQUIRE(preview.remove.empty());
  REQUIRE(preview.disk_space_delta == 0);
}

TEST_CASE("Transaction preview reports a friendly resolve error for an impossible package")
{
  reset_backend_globals();

  TransactionPreview preview;
  std::string error;
  std::vector<std::string> progress_lines;

  // clang-format off
  bool ok = dnf_backend_preview_transaction({"___definitely_not_a_real_package_246810___"},
                                            {},
                                            {},
                                            preview,
                                            error,
                                            [&](const std::string &line) { progress_lines.push_back(line); });
  // clang-format on

  REQUIRE_FALSE(ok);
  REQUIRE(error.find("Unable to resolve transaction.") != std::string::npos);
  REQUIRE(progress_contains(progress_lines, "Resolving dependency changes..."));
}

// -----------------------------------------------------------------------------
// Transaction preview success path tests
// -----------------------------------------------------------------------------

TEST_CASE("Transaction preview resolves a reinstall request for an installed package")
{
  reset_backend_globals();

  auto installed_rows = dnf_backend_get_installed_package_rows_interruptible(nullptr);
  REQUIRE_FALSE(installed_rows.empty());
  const PackageRow &installed_row = installed_rows.front();

  TransactionPreview preview;
  std::string error;
  std::vector<std::string> progress_lines;

  // clang-format off
  bool ok = dnf_backend_preview_transaction({},
                                            {},
                                            {installed_row.nevra},
                                            preview,
                                            error,
                                            [&](const std::string &line) { progress_lines.push_back(line); });
  // clang-format on

  INFO(error);
  REQUIRE(ok);
  REQUIRE(error.empty());
  REQUIRE(progress_contains(progress_lines, "Resolving dependency changes..."));
  REQUIRE_FALSE(preview.reinstall.empty());
  REQUIRE(std::any_of(preview.reinstall.begin(), preview.reinstall.end(), [&](const std::string &label) {
    return label.find(installed_row.name + "-") != std::string::npos;
  }));
}
