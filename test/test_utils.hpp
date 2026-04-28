#pragma once

#include "dnf_backend/dnf_backend.hpp"

#include <cstdlib>
#include <set>
#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// reset_backend_globals
// -----------------------------------------------------------------------------
inline void
reset_backend_globals()
{
  dnf_backend_set_search_options({});
  dnf_backend_testonly_clear_installed_snapshot();
}

// -----------------------------------------------------------------------------
// set_backend_search_options
// -----------------------------------------------------------------------------
inline void
set_backend_search_options(bool search_in_description, bool exact_match)
{
  dnf_backend_set_search_options({
      .search_in_description = search_in_description,
      .exact_match = exact_match,
  });
}

// -----------------------------------------------------------------------------
// package_row_nevras
// -----------------------------------------------------------------------------
inline std::set<std::string>
package_row_nevras(const std::vector<PackageRow> &rows)
{
  std::set<std::string> nevras;
  for (const auto &row : rows) {
    nevras.insert(row.nevra);
  }

  return nevras;
}

struct ScopedEnvVar {
  // -----------------------------------------------------------------------------
  // ScopedEnvVar
  // -----------------------------------------------------------------------------
  explicit ScopedEnvVar(const char *key, const char *value)
      : key(key ? key : "")
  {
    const char *existing = this->key.empty() ? nullptr : std::getenv(this->key.c_str());
    if (existing) {
      had_old_value = true;
      old_value = existing;
    }

    if (!this->key.empty()) {
      setenv(this->key.c_str(), value ? value : "", 1);
    }
  }

  // -----------------------------------------------------------------------------
  // ~ScopedEnvVar
  // -----------------------------------------------------------------------------
  ~ScopedEnvVar()
  {
    if (key.empty()) {
      return;
    }

    if (had_old_value) {
      setenv(key.c_str(), old_value.c_str(), 1);
    } else {
      unsetenv(key.c_str());
    }
  }

  std::string key;
  std::string old_value;
  bool had_old_value = false;
};
