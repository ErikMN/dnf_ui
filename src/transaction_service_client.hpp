#pragma once

#include <functional>
#include <string>

struct TransactionRequest;
struct TransactionPreview;

bool transaction_service_client_preview_request(const TransactionRequest &request,
                                                TransactionPreview &preview_out,
                                                std::string &transaction_path_out,
                                                std::string &error_out);
bool transaction_service_client_apply_started_request(const std::string &transaction_path,
                                                      const std::function<void(const std::string &)> &progress_callback,
                                                      std::string &error_out);
void transaction_service_client_release_request(const std::string &transaction_path);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
