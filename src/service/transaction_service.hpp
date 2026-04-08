#pragma once

#include <gio/gio.h>

// -----------------------------------------------------------------------------
// Privileged transaction service options
// -----------------------------------------------------------------------------
struct TransactionServiceOptions {
  GBusType bus_type = G_BUS_TYPE_SESSION;
};

int transaction_service_run(const TransactionServiceOptions &options);
