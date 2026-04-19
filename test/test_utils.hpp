#pragma once

#include "dnf_backend/dnf_backend.hpp"

#include <mutex>
#include <set>
#include <vector>

inline void
reset_backend_globals()
{
  g_search_in_description = false;
  g_exact_match = false;

  std::lock_guard<std::mutex> lock(g_installed_mutex);
  g_installed_nevras.clear();
}

inline std::set<std::string>
package_row_nevras(const std::vector<PackageRow> &rows)
{
  std::set<std::string> nevras;
  for (const auto &row : rows) {
    nevras.insert(row.nevra);
  }

  return nevras;
}
