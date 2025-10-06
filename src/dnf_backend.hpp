#pragma once

#include <memory>
#include <string>
#include <vector>

#include <libdnf5/base/base.hpp>

// -----------------------------------------------------------------------------
// libdnf5 backend helpers
// -----------------------------------------------------------------------------
std::unique_ptr<libdnf5::Base> create_fresh_base();
std::vector<std::string> get_installed_packages();
std::vector<std::string> search_available_packages(const std::string &pattern);
std::string get_package_info(const std::string &pkg_name);

extern bool g_search_in_description;
extern bool g_exact_match;
extern std::set<std::string> g_installed_names;
