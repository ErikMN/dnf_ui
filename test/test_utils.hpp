#pragma once

#include "dnf_backend/dnf_backend.hpp"

#include <set>
#include <vector>

inline void
reset_backend_globals()
{
  dnf_backend_set_search_options({});
  dnf_backend_testonly_clear_installed_snapshot();
}

inline void
set_backend_search_options(bool search_in_description, bool exact_match)
{
  dnf_backend_set_search_options({
      .search_in_description = search_in_description,
      .exact_match = exact_match,
  });
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
