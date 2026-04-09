// -----------------------------------------------------------------------------
// src/transaction_service_client.hpp
// GUI client helpers for the transaction service
// Declares the small D-Bus client API used by the GTK frontend to prepare,
// apply, and release service-owned transaction requests.
// -----------------------------------------------------------------------------
#pragma once

#include <functional>
#include <string>

struct TransactionRequest;
struct TransactionPreview;

// Prepare one transaction through the service and return its resolved preview.
bool transaction_service_client_preview_request(const TransactionRequest &request,
                                                TransactionPreview &preview_out,
                                                std::string &transaction_path_out,
                                                std::string &error_out);

// Apply one previously prepared transaction request and forward its progress.
bool transaction_service_client_apply_started_request(const std::string &transaction_path,
                                                      const std::function<void(const std::string &)> &progress_callback,
                                                      std::string &error_out);

// Release one finished transaction request that is no longer needed.
void transaction_service_client_release_request(const std::string &transaction_path);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
