// -----------------------------------------------------------------------------
// transaction_service_request_parser.hpp
// Transaction service request parsing
// Converts D-Bus method parameters into backend transaction requests.
// -----------------------------------------------------------------------------
#pragma once

#include "transaction_request.hpp"

#include <glib.h>

// -----------------------------------------------------------------------------
// Unpack the StartTransaction arrays in install, remove, and reinstall order.
// -----------------------------------------------------------------------------
TransactionRequest transaction_service_request_from_variant(GVariant *parameters);
