// src/dnf_backend.hpp
#pragma once

#include <memory>
#include <string>
#include <vector>
#include <set>
#include <mutex>
#include <atomic>

// -----------------------------------------------------------------------------
// libdnf5 backend helpers
// -----------------------------------------------------------------------------
extern std::atomic<bool> g_search_in_description;
extern std::atomic<bool> g_exact_match;
extern std::mutex g_installed_mutex;
extern std::set<std::string> g_installed_nevras;
extern std::set<std::string> g_installed_names;

void refresh_installed_nevras();
std::vector<std::string> get_installed_packages();
std::vector<std::string> search_available_packages(const std::string &pattern);
std::string get_package_info(const std::string &pkg_name);
std::string get_installed_package_files(const std::string &pkg_nevra);
std::string get_package_deps(const std::string &pkg_nevra);
std::string get_package_changelog(const std::string &pkg_nevra);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
