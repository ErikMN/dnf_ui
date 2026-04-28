// src/ui/package_info_controller.hpp
#pragma once

#include "dnf_backend/dnf_backend.hpp"

struct SearchWidgets;

// -----------------------------------------------------------------------------
// Package selection and details notebook controller helpers
// -----------------------------------------------------------------------------
void package_info_clear_selected_package_state(SearchWidgets *widgets);
// -----------------------------------------------------------------------------
// package_info_load_selected_package_info
// -----------------------------------------------------------------------------
void package_info_load_selected_package_info(SearchWidgets *widgets, const PackageRow &selected);
// -----------------------------------------------------------------------------
// package_info_reset_details_view
// -----------------------------------------------------------------------------
void package_info_reset_details_view(SearchWidgets *widgets);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
