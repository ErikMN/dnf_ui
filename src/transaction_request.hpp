#pragma once

#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// Transaction request shared by the GUI and privileged transaction service
// -----------------------------------------------------------------------------
struct TransactionRequest {
  std::vector<std::string> install;
  std::vector<std::string> remove;
  std::vector<std::string> reinstall;

  bool empty() const
  {
    return install.empty() && remove.empty() && reinstall.empty();
  }

  size_t item_count() const
  {
    return install.size() + remove.size() + reinstall.size();
  }

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
