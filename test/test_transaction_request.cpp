// -----------------------------------------------------------------------------
// test/test_transaction_request.cpp
// Shared transaction request contract tests
// Exercises the small validation and counting rules used by both the GUI and
// the transaction service before any D-Bus or libdnf work begins.
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "transaction_request.hpp"

#include <string>

// -----------------------------------------------------------------------------
// TransactionRequest basic state tests
// -----------------------------------------------------------------------------

TEST_CASE("Transaction request empty state and item count reflect queued actions")
{
  TransactionRequest request;

  REQUIRE(request.empty());
  REQUIRE(request.item_count() == 0);

  request.install.push_back("example-install-spec");
  request.remove.push_back("example-remove-spec");
  request.reinstall.push_back("example-reinstall-spec");

  REQUIRE_FALSE(request.empty());
  REQUIRE(request.item_count() == 3);
}

// -----------------------------------------------------------------------------
// TransactionRequest validation tests
// -----------------------------------------------------------------------------

TEST_CASE("Transaction request validation rejects an empty request")
{
  TransactionRequest request;
  std::string error = "stale";

  REQUIRE_FALSE(request.validate(error));
  REQUIRE(error == "Transaction request is empty.");
}

TEST_CASE("Transaction request validation rejects an empty install package spec")
{
  TransactionRequest request;
  std::string error;

  request.install.push_back("");

  REQUIRE_FALSE(request.validate(error));
  REQUIRE(error == "Transaction request contains an empty install package spec.");
}

TEST_CASE("Transaction request validation rejects an empty remove package spec")
{
  TransactionRequest request;
  std::string error;

  request.remove.push_back("");

  REQUIRE_FALSE(request.validate(error));
  REQUIRE(error == "Transaction request contains an empty remove package spec.");
}

TEST_CASE("Transaction request validation rejects an empty reinstall package spec")
{
  TransactionRequest request;
  std::string error;

  request.reinstall.push_back("");

  REQUIRE_FALSE(request.validate(error));
  REQUIRE(error == "Transaction request contains an empty reinstall package spec.");
}

TEST_CASE("Transaction request validation accepts mixed non empty package specs")
{
  TransactionRequest request;
  std::string error = "stale";

  request.install.push_back("example-install-spec");
  request.remove.push_back("example-remove-spec");
  request.reinstall.push_back("example-reinstall-spec");

  REQUIRE(request.validate(error));
  REQUIRE(error.empty());
}
