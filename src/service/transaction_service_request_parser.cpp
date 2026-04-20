// -----------------------------------------------------------------------------
// transaction_service_request_parser.cpp
// Transaction service request parsing
// Keeps D-Bus parameter unpacking separate from manager method handling.
// -----------------------------------------------------------------------------
#include "service/transaction_service_request_parser.hpp"

#include <string>
#include <vector>

// Unpack the StartTransaction arrays in install, remove, and reinstall order.
TransactionRequest
transaction_service_request_from_variant(GVariant *parameters)
{
  gchar **install = nullptr;
  gchar **remove = nullptr;
  gchar **reinstall = nullptr;
  g_variant_get(parameters, "(^as^as^as)", &install, &remove, &reinstall);

  TransactionRequest request;

  auto append_specs = [](std::vector<std::string> &target, gchar **specs) {
    if (!specs) {
      return;
    }
    for (gchar **it = specs; *it; ++it) {
      target.emplace_back(*it);
    }
  };

  append_specs(request.install, install);
  append_specs(request.remove, remove);
  append_specs(request.reinstall, reinstall);

  g_strfreev(install);
  g_strfreev(remove);
  g_strfreev(reinstall);

  return request;
}
