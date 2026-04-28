// pending_transaction_request.hpp
// Pending transaction request helpers
//
// Converts the marked package actions into the request sent to the transaction
// service and performs request-level safety checks before preview.
#pragma once

#include "pending_transaction_state.hpp"
#include "transaction_request.hpp"

#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// pending_transaction_build_request
// -----------------------------------------------------------------------------
void pending_transaction_build_request(const std::vector<PendingAction> &actions, TransactionRequest &request);
// -----------------------------------------------------------------------------
// pending_transaction_validate_request
// -----------------------------------------------------------------------------
bool pending_transaction_validate_request(const TransactionRequest &request, std::string &error_out);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
