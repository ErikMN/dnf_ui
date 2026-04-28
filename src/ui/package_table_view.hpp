// src/ui/package_table_view.hpp
// Public package table view entry points
//
// Owns the package table population, current row selection lookup, and visible
// status refresh used after pending transaction changes.
#pragma once

#include "dnf_backend/dnf_backend.hpp"

#include <vector>

struct SearchWidgets;

// -----------------------------------------------------------------------------
// package_table_get_selected_package_row
// -----------------------------------------------------------------------------
bool package_table_get_selected_package_row(SearchWidgets *widgets, PackageRow &out_pkg);
// -----------------------------------------------------------------------------
// package_table_fill_package_view
// -----------------------------------------------------------------------------
void package_table_fill_package_view(SearchWidgets *widgets, const std::vector<PackageRow> &items);
// -----------------------------------------------------------------------------
// package_table_refresh_statuses
// -----------------------------------------------------------------------------
void package_table_refresh_statuses(SearchWidgets *widgets);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
