// -----------------------------------------------------------------------------
// src/transaction_request.hpp
// Shared transaction request model
// Carries the package specs the user explicitly marked in the GUI. Dependency
// driven upgrades and downgrades are resolved later when the preview is built.
// -----------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <string>
#include <vector>

constexpr size_t kTransactionRequestMaxItems = 256;
constexpr size_t kTransactionRequestMaxSpecLength = 4096;

// -----------------------------------------------------------------------------
// Transaction request shared by the GUI and privileged transaction service
// -----------------------------------------------------------------------------
struct TransactionRequest {
  // Upgrade all installed packages.
  bool upgrade_all = false;
  // Package specs explicitly marked for install.
  std::vector<std::string> install;
  // Package specs explicitly marked for removal.
  std::vector<std::string> remove;
  // Package specs explicitly marked for reinstall.
  std::vector<std::string> reinstall;

  // -----------------------------------------------------------------------------
  // Return true when no package actions have been queued.
  // -----------------------------------------------------------------------------
  bool empty() const
  {
    return !upgrade_all && install.empty() && remove.empty() && reinstall.empty();
  }

  // -----------------------------------------------------------------------------
  // Return the total number of requested package actions.
  // -----------------------------------------------------------------------------
  size_t item_count() const
  {
    return (upgrade_all ? 1 : 0) + install.size() + remove.size() + reinstall.size();
  }

  // -----------------------------------------------------------------------------
  // Reject empty or oversized requests before they reach the service.
  // -----------------------------------------------------------------------------
  bool validate(std::string &error_out) const
  {
    error_out.clear();

    if (empty()) {
      error_out = "Transaction request is empty.";
      return false;
    }

    if (upgrade_all && (!install.empty() || !remove.empty() || !reinstall.empty())) {
      error_out = "Upgrade all cannot be combined with other package actions.";
      return false;
    }

    if (item_count() > kTransactionRequestMaxItems) {
      error_out = "Transaction request contains too many package actions.";
      return false;
    }

    auto validate_specs = [&](const std::vector<std::string> &specs, const char *kind) {
      for (const auto &spec : specs) {
        if (spec.empty()) {
          error_out = std::string("Transaction request contains an empty ") + kind + " package spec.";
          return false;
        }
        if (spec.size() > kTransactionRequestMaxSpecLength) {
          error_out = std::string("Transaction request contains a package spec that is too long.");
          return false;
        }
      }
      return true;
    };

    return validate_specs(install, "install") && validate_specs(remove, "remove") &&
        validate_specs(reinstall, "reinstall");
  }
};
