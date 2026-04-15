// -----------------------------------------------------------------------------
// src/service/transaction_service_main.cpp
// Transaction service process entrypoint
// Parses the requested bus mode for the privileged transaction service and then
// hands control to the shared service runtime.
// -----------------------------------------------------------------------------
#include "transaction_service.hpp"

#include <cstdio>
#include <cstring>

// -----------------------------------------------------------------------------
// Transaction service main entrypoint
// -----------------------------------------------------------------------------
int
main(int argc, char **argv)
{
  TransactionServiceOptions options;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--system") == 0) {
      options.bus_type = G_BUS_TYPE_SYSTEM;
    } else if (std::strcmp(argv[i], "--session") == 0) {
      options.bus_type = G_BUS_TYPE_SESSION;
    } else if (std::strcmp(argv[i], "--help") == 0) {
      std::printf("Usage: dnfui-service [--session] [--system]\n");
      return 0;
    } else {
      std::fprintf(stderr, "Unknown option: %s\n", argv[i]);
      return 1;
    }
  }

  return transaction_service_run(options);
}
