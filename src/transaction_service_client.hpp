#pragma once

#include <functional>
#include <string>

struct TransactionRequest;

bool transaction_service_client_apply_request(const TransactionRequest &request,
                                              const std::function<void(const std::string &)> &progress_callback,
                                              std::string &error_out);
