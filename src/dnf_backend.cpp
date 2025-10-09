// -----------------------------------------------------------------------------
// src/dnf_backend.cpp
// libdnf5 backend helpers
// Provides simplified helper functions for interacting with the DNF (libdnf5)
// package management backend. These wrappers abstract query logic and return
// simple std::vector or std::string types for use in the GTK UI layer.
//
// Reference:
// https://dnf5.readthedocs.io/en/latest/
// -----------------------------------------------------------------------------
#include "dnf_backend.hpp"
#include "base_manager.hpp"

#include <algorithm>
#include <iostream>
#include <set>
#include <sstream>

#include <libdnf5/base/base.hpp>
#include <libdnf5/rpm/package_query.hpp>

// -----------------------------------------------------------------------------
// Global state used by UI highlighting and query filters
// -----------------------------------------------------------------------------
std::set<std::string> g_installed_nevras; // Cached NEVRAs of installed packages for UI highlighting
std::set<std::string> g_installed_names;  // Cached package names for name-based lookups
bool g_search_in_description = false;     // Global flag: include description field in search
bool g_exact_match = false;               // Global flag: match package name/desc exactly

// -----------------------------------------------------------------------------
// Helper: Refresh global installed package NEVRA (Name Epoch Version Release Architecture) cache
// Clears and repopulates g_installed_nevras by querying all currently installed
// packages through libdnf5. This should be called whenever the UI needs to
// update its installed-package highlighting or when transactions have modified
// the system package set.
// -----------------------------------------------------------------------------
void
refresh_installed_nevras()
{
  g_installed_nevras.clear();
  g_installed_names.clear();

  auto &base = BaseManager::instance().get_base();
  libdnf5::rpm::PackageQuery query(base);
  query.filter_installed();

  for (auto pkg : query) {
    g_installed_nevras.insert(pkg.get_nevra());
    g_installed_names.insert(pkg.get_name());
  }
}

// -----------------------------------------------------------------------------
// Helper: Query installed packages via libdnf5
// Returns a list of all installed packages in full NEVRA format (e.g., pkg-1.0-1.fc38.x86_64).
// Also updates the global set of installed package NEVRAs (g_installed_nevras).
// -----------------------------------------------------------------------------
std::vector<std::string>
get_installed_packages()
{
  std::vector<std::string> packages;

  auto &base = BaseManager::instance().get_base();
  libdnf5::rpm::PackageQuery query(base);
  query.filter_installed();

  // Collect all installed packages and populate global NEVRA cache
  for (auto pkg : query) {
    std::string nevra = pkg.get_nevra();
    g_installed_nevras.insert(nevra);
    g_installed_names.insert(pkg.get_name());
    packages.push_back(nevra);
  }

  return packages;
}

// -----------------------------------------------------------------------------
// Helper: Search available packages
// Performs a name or description-based search depending on active flags.
// Supports both substring (default) and exact match modes.
//
// Search logic:
//   - If g_search_in_description == true, searches name + description manually
//     using case-insensitive substring comparison.
//   - Otherwise uses libdnf5 query filters for efficient name-only search.
// -----------------------------------------------------------------------------
std::vector<std::string>
search_available_packages(const std::string &pattern)
{
  std::vector<std::string> packages;

  auto &base = BaseManager::instance().get_base();
  libdnf5::rpm::PackageQuery query(base);
  query.filter_available();

  if (g_search_in_description) {
    // Manually match pattern in description (case-insensitive)
    std::string pattern_lower = pattern;
    std::transform(pattern_lower.begin(), pattern_lower.end(), pattern_lower.begin(), ::tolower);

    for (auto pkg : query) {
      std::string desc = pkg.get_description();
      std::string name = pkg.get_name();

      std::transform(desc.begin(), desc.end(), desc.begin(), ::tolower);
      std::transform(name.begin(), name.end(), name.begin(), ::tolower);

      if (g_exact_match) {
        if (name == pattern_lower) {
          packages.push_back(pkg.get_nevra());
        }
      } else {
        if (desc.find(pattern_lower) != std::string::npos || name.find(pattern_lower) != std::string::npos) {
          packages.push_back(pkg.get_nevra());
        }
      }
    }
  } else {
    // Efficient name-based filtering using libdnf5 QueryCmp
    if (g_exact_match) {
      query.filter_name(pattern, libdnf5::sack::QueryCmp::EQ);
    } else {
      query.filter_name(pattern, libdnf5::sack::QueryCmp::CONTAINS);
    }

    for (auto pkg : query) {
      packages.push_back(pkg.get_nevra());
    }
  }

  return packages;
}

// -----------------------------------------------------------------------------
// Helper: Retrieve detailed package information
// Fetches and formats detailed info for a single package, including:
//   - name, version, release, architecture, repo
//   - summary and description
//
// If multiple versions are available, the latest EVR (Epoch-Version-Release)
// is selected. Prefers installed packages when present.
// -----------------------------------------------------------------------------
std::string
get_package_info(const std::string &pkg_name)
{
  auto &base = BaseManager::instance().get_base();
  libdnf5::rpm::PackageQuery query(base);
  query.filter_name(pkg_name);

  if (query.empty()) {
    return "No details found for " + pkg_name;
  }

  // Prefer installed package if available
  libdnf5::rpm::PackageQuery installed(query);
  installed.filter_installed();

  // Select installed if found, otherwise use available
  libdnf5::rpm::PackageQuery best_candidate = installed.empty() ? query : installed;

  // Keep only the latest version (highest EVR)
  best_candidate.filter_latest_evr();

  auto pkg = *best_candidate.begin();

  std::ostringstream oss;
  oss << "Name: " << pkg.get_name() << "\n"
      << "Version: " << pkg.get_version() << "\n"
      << "Release: " << pkg.get_release() << "\n"
      << "Arch: " << pkg.get_arch() << "\n"
      << "Repo: " << pkg.get_repo_id() << "\n\n"
      << "Summary:\n"
      << pkg.get_summary() << "\n\n"
      << "Description:\n"
      << pkg.get_description();

  return oss.str();
}
