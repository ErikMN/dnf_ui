// -----------------------------------------------------------------------------
// transaction_service_preview_formatter.hpp
// Transaction service preview text formatting
// Formats resolved transaction previews into readable status text for D-Bus
// result details.
// -----------------------------------------------------------------------------
#pragma once

#include <string>

struct TransactionPreview;

// Format the full resolved transaction preview as a readable summary string.
std::string format_transaction_preview_details(const TransactionPreview &preview);
