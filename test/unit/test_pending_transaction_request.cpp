// -----------------------------------------------------------------------------
// Pending transaction request tests
// Covers conversion from marked UI actions to the transaction request model.
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "transaction_request.hpp"
#include "ui/pending_transaction_request.hpp"

#include <string>
#include <vector>

TEST_CASE("Pending transaction request builder splits actions by operation type")
{
  std::vector<PendingAction> actions = {
    { PendingAction::INSTALL, "demo-install-1-1.x86_64" },
    { PendingAction::REMOVE, "demo-remove-1-1.x86_64" },
    { PendingAction::REINSTALL, "demo-reinstall-1-1.x86_64" },
    { PendingAction::INSTALL, "demo-install-libs-1-1.x86_64" },
  };

  TransactionRequest request;

  pending_transaction_build_request(actions, request);

  REQUIRE(request.install ==
          std::vector<std::string> {
              "demo-install-1-1.x86_64",
              "demo-install-libs-1-1.x86_64",
          });
  REQUIRE(request.remove ==
          std::vector<std::string> {
              "demo-remove-1-1.x86_64",
          });
  REQUIRE(request.reinstall ==
          std::vector<std::string> {
              "demo-reinstall-1-1.x86_64",
          });
}

TEST_CASE("Pending transaction request builder clears stale request data")
{
  TransactionRequest request;
  request.install.push_back("old-install");
  request.remove.push_back("old-remove");
  request.reinstall.push_back("old-reinstall");

  std::vector<PendingAction> actions = {
    { PendingAction::REMOVE, "demo-remove-1-1.x86_64" },
  };

  pending_transaction_build_request(actions, request);

  REQUIRE(request.install.empty());
  REQUIRE(request.remove ==
          std::vector<std::string> {
              "demo-remove-1-1.x86_64",
          });
  REQUIRE(request.reinstall.empty());
}

TEST_CASE("Pending transaction request validation accepts non protected requests")
{
  TransactionRequest request;
  request.install.push_back("demo-install-1-1.x86_64");
  request.remove.push_back("demo-remove-1-1.x86_64");
  request.reinstall.push_back("demo-reinstall-1-1.x86_64");
  std::string error;

  REQUIRE(pending_transaction_validate_request(request, error));
  REQUIRE(error.empty());
}
