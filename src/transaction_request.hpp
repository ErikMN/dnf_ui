// -----------------------------------------------------------------------------
// src/transaction_request.hpp
// Shared transaction request model
// Carries the package specs the user explicitly marked in the GUI. Dependency
// driven upgrades and downgrades are resolved later when the preview is built.
// -----------------------------------------------------------------------------
#pragma once

#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// Transaction request shared by the GUI and privileged transaction service
// -----------------------------------------------------------------------------
struct TransactionRequest {
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
    return install.empty() && remove.empty() && reinstall.empty();
  }

  // -----------------------------------------------------------------------------
  // Return the total number of explicitly marked package actions.
  // -----------------------------------------------------------------------------
  size_t item_count() const
  {
    return install.size() + remove.size() + reinstall.size();
  }

  // -----------------------------------------------------------------------------
  // Reject empty requests and empty package specs before they reach the service.
  // -----------------------------------------------------------------------------
  bool validate(std::string &error_out) const
  {
    error_out.clear();

    if (empty()) {
      error_out = "Transaction request is empty.";
      return false;
    }

    auto validate_specs = [&](const std::vector<std::string> &specs, const char *kind) {
      for (const auto &spec : specs) {
        if (spec.empty()) {
          error_out = std::string("Transaction request contains an empty ") + kind + " package spec.";
          return false;
        }
      }
      return true;
    };

    return validate_specs(install, "install") && validate_specs(remove, "remove") &&
        validate_specs(reinstall, "reinstall");
  }
};
