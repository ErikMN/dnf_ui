// src/dnf_backend.hpp
#pragma once

#include <memory>
#include <string>
#include <vector>

#include <libdnf5/base/base.hpp>

// -----------------------------------------------------------------------------
// libdnf5 backend helpers
// -----------------------------------------------------------------------------
extern bool g_search_in_description;
extern bool g_exact_match;
extern std::set<std::string> g_installed_nevras;
extern std::set<std::string> g_installed_names;

void refresh_installed_nevras();
std::vector<std::string> get_installed_packages();
std::vector<std::string> search_available_packages(const std::string &pattern);
std::string get_package_info(const std::string &pkg_name);
