// -----------------------------------------------------------------------------
// test/unit/test_transaction_service_preview_formatter.cpp
// Transaction service preview formatter tests
// Covers the user-facing transaction summary text generated from a resolved
// backend preview before the GUI asks the user to apply package changes.
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "dnf_backend/dnf_backend.hpp"
#include "service/transaction_service_preview_formatter.hpp"

#include <string>

namespace {

// -----------------------------------------------------------------------------
// Return true when the first text appears before the second text.
// -----------------------------------------------------------------------------
bool
appears_before(const std::string &text, const std::string &first, const std::string &second)
{
  const size_t first_pos = text.find(first);
  const size_t second_pos = text.find(second);

  return first_pos != std::string::npos && second_pos != std::string::npos && first_pos < second_pos;
}

} // namespace

TEST_CASE("Transaction preview formatter reports unchanged disk space for an empty preview")
{
  TransactionPreview preview;

  const std::string summary = format_transaction_preview_details(preview);

  REQUIRE(preview.empty());
  REQUIRE(summary == "No package changes are available.\nDisk space usage will be unchanged.\n\n");
}

TEST_CASE("Transaction preview formatter uses singular and plural package count lines")
{
  TransactionPreview preview;
  preview.install = { "demo-install-1-1.x86_64" };
  preview.remove = { "demo-remove-1-1.x86_64", "demo-remove-libs-1-1.x86_64" };

  const std::string summary = format_transaction_preview_details(preview);

  REQUIRE_FALSE(preview.empty());
  REQUIRE(summary.find("1 package will be installed.") != std::string::npos);
  REQUIRE(summary.find("2 packages will be removed.") != std::string::npos);
}

TEST_CASE("Transaction preview formatter keeps transaction sections in display order")
{
  TransactionPreview preview;
  preview.install = { "demo-install-1-1.x86_64" };
  preview.upgrade = { "demo-upgrade-2-1.x86_64" };
  preview.downgrade = { "demo-downgrade-1-1.x86_64" };
  preview.reinstall = { "demo-reinstall-1-1.x86_64" };
  preview.remove = { "demo-remove-1-1.x86_64" };

  const std::string summary = format_transaction_preview_details(preview);

  REQUIRE(appears_before(summary, "To be installed:", "To be upgraded:"));
  REQUIRE(appears_before(summary, "To be upgraded:", "To be downgraded:"));
  REQUIRE(appears_before(summary, "To be downgraded:", "To be reinstalled:"));
  REQUIRE(appears_before(summary, "To be reinstalled:", "To be removed:"));

  REQUIRE(summary.find("  demo-install-1-1.x86_64\n") != std::string::npos);
  REQUIRE(summary.find("  demo-upgrade-2-1.x86_64\n") != std::string::npos);
  REQUIRE(summary.find("  demo-downgrade-1-1.x86_64\n") != std::string::npos);
  REQUIRE(summary.find("  demo-reinstall-1-1.x86_64\n") != std::string::npos);
  REQUIRE(summary.find("  demo-remove-1-1.x86_64\n") != std::string::npos);
}

TEST_CASE("Transaction preview formatter omits empty package sections")
{
  TransactionPreview preview;
  preview.install = { "demo-install-1-1.x86_64" };

  const std::string summary = format_transaction_preview_details(preview);

  REQUIRE(summary.find("To be installed:") != std::string::npos);
  REQUIRE(summary.find("To be upgraded:") == std::string::npos);
  REQUIRE(summary.find("To be downgraded:") == std::string::npos);
  REQUIRE(summary.find("To be reinstalled:") == std::string::npos);
  REQUIRE(summary.find("To be removed:") == std::string::npos);
}

TEST_CASE("Transaction preview formatter describes positive and negative disk space changes")
{
  TransactionPreview install_preview;
  install_preview.disk_space_delta = 4096;

  TransactionPreview remove_preview;
  remove_preview.disk_space_delta = -4096;

  const std::string install_summary = format_transaction_preview_details(install_preview);
  const std::string remove_summary = format_transaction_preview_details(remove_preview);

  REQUIRE(install_summary.find("extra disk space will be used.") != std::string::npos);
  REQUIRE(remove_summary.find("of disk space will be freed.") != std::string::npos);
}
