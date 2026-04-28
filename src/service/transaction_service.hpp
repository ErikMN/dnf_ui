// -----------------------------------------------------------------------------
// src/service/transaction_service.hpp
// Shared transaction service declarations
// Declares the bus selection options and the service runtime entrypoint used by
// the standalone transaction service process.
// -----------------------------------------------------------------------------
#pragma once

#include <gio/gio.h>

// -----------------------------------------------------------------------------
// Privileged transaction service options
// -----------------------------------------------------------------------------
struct TransactionServiceOptions {
  GBusType bus_type = G_BUS_TYPE_SESSION;
};

// -----------------------------------------------------------------------------
// Run the transaction service on the requested D-Bus bus until it exits.
// -----------------------------------------------------------------------------
int transaction_service_run(const TransactionServiceOptions &options);
