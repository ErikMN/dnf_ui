// src/package_info_controller.hpp
#pragma once

#include "dnf_backend.hpp"

struct SearchWidgets;

// -----------------------------------------------------------------------------
// Package selection and details notebook controller helpers
// -----------------------------------------------------------------------------
void clear_selected_package_state(SearchWidgets *widgets);
void load_selected_package_info(SearchWidgets *widgets, const PackageRow &selected);
void reset_package_details_view(SearchWidgets *widgets);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
