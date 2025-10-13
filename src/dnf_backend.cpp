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
#include <mutex>
#include <atomic>

#include <libdnf5/base/base.hpp>
#include <libdnf5/rpm/package_query.hpp>

// -----------------------------------------------------------------------------
// Global state used by UI highlighting and query filters
// -----------------------------------------------------------------------------
std::set<std::string> g_installed_nevras;            // Cached NEVRAs of installed packages for UI highlighting
std::set<std::string> g_installed_names;             // Cached package names for name-based lookups
std::mutex g_installed_mutex;                        // Mutex for thread-safe access to global sets
std::atomic<bool> g_search_in_description { false }; // Global flag: include description field in search
std::atomic<bool> g_exact_match { false };           // Global flag: match package name/desc exactly

// -----------------------------------------------------------------------------
// Helper: Refresh global installed package NEVRA (Name Epoch Version Release Architecture) cache
// Clears and repopulates g_installed_nevras by querying all currently installed
// packages through libdnf5. This should be called whenever the UI needs to
// update its installed-package highlighting or when transactions have modified
// the system package set.
//
// Thread-safety:
//   This function takes g_installed_mutex for the entire duration of clearing
//   and repopulating g_installed_nevras and g_installed_names to prevent
//   concurrent reads or writes from GTK worker threads or periodic refresh
//   timers.
// -----------------------------------------------------------------------------
void
refresh_installed_nevras()
{
  auto [base, guard] = BaseManager::instance().acquire_read();
  libdnf5::rpm::PackageQuery query(base);
  query.filter_installed();

  // Acquire exclusive lock before modifying global sets.
  std::lock_guard<std::mutex> lock(g_installed_mutex);
  g_installed_nevras.clear();
  g_installed_names.clear();

  for (auto pkg : query) {
    g_installed_nevras.insert(pkg.get_nevra());
    g_installed_names.insert(pkg.get_name());
  }
}

// -----------------------------------------------------------------------------
// Helper: Query installed packages via libdnf5
// Returns a list of all installed packages in full NEVRA format (e.g., pkg-1.0-1.fc38.x86_64).
// Also updates the global set of installed package NEVRAs (g_installed_nevras).
//
// Thread-safety:
//   The same g_installed_mutex is held during cache update to synchronize with
//   other background refreshes and avoid data races with UI access.
// -----------------------------------------------------------------------------
std::vector<std::string>
get_installed_packages()
{
  std::vector<std::string> packages;

  auto [base, guard] = BaseManager::instance().acquire_read();
  libdnf5::rpm::PackageQuery query(base);
  query.filter_installed();

  // Collect all installed packages and populate global NEVRA cache.
  // Lock ensures atomic clear and repopulate sequence.
  std::lock_guard<std::mutex> lock(g_installed_mutex);
  g_installed_nevras.clear();
  g_installed_names.clear();

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
//
// Thread-safety:
//   This function performs only reads and does not touch global caches, so no
//   locking is required here.
// -----------------------------------------------------------------------------
std::vector<std::string>
search_available_packages(const std::string &pattern)
{
  std::vector<std::string> packages;

  auto [base, guard] = BaseManager::instance().acquire_read();
  libdnf5::rpm::PackageQuery query(base);
  query.filter_available();

  if (g_search_in_description.load()) {
    // Manually match pattern in description (case-insensitive)
    std::string pattern_lower = pattern;
    std::transform(pattern_lower.begin(), pattern_lower.end(), pattern_lower.begin(), ::tolower);

    for (auto pkg : query) {
      std::string desc = pkg.get_description();
      std::string name = pkg.get_name();

      std::transform(desc.begin(), desc.end(), desc.begin(), ::tolower);
      std::transform(name.begin(), name.end(), name.begin(), ::tolower);

      if (g_exact_match.load()) {
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
    if (g_exact_match.load()) {
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
// Always performs an exact NEVRA match (the UI passes full NEVRA strings).
//
// Thread-safety:
//   This function reads package data from libdnf5 directly and does not touch
//   shared global sets. No locking required.
// -----------------------------------------------------------------------------
std::string
get_package_info(const std::string &pkg_nevra)
{
  auto [base, guard] = BaseManager::instance().acquire_read();
  libdnf5::rpm::PackageQuery query(base);

  // Exact NEVRA match only
  query.filter_nevra(pkg_nevra);

  if (query.empty()) {
    return "No details found for " + pkg_nevra;
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

// -----------------------------------------------------------------------------
// Helper: Retrieve file list for an installed package (by NEVRA)
// Returns newline-separated file list or a friendly message if none.
// -----------------------------------------------------------------------------
std::string
get_installed_package_files(const std::string &pkg_nevra)
{
  auto [base, guard] = BaseManager::instance().acquire_read();
  libdnf5::rpm::PackageQuery query(base);

  query.filter_nevra(pkg_nevra);
  query.filter_installed();

  if (query.empty()) {
    return "File list available only for installed packages.";
  }

  query.filter_latest_evr();
  auto pkg = *query.begin();

  std::ostringstream files;
  for (const auto &f : pkg.get_files()) {
    files << f << "\n";
  }

  std::string result = files.str();
  if (result.empty()) {
    result = "No files recorded for this installed package.";
  }

  return result;
}

// -----------------------------------------------------------------------------
// Helper: Retrieve dependency information for a package (Requires/Provides/etc.)
// Returns a formatted string for display in the "Dependencies" tab
// -----------------------------------------------------------------------------
std::string
get_package_deps(const std::string &pkg_nevra)
{
  auto [base, guard] = BaseManager::instance().acquire_read();
  libdnf5::rpm::PackageQuery query(base);

  query.filter_nevra(pkg_nevra);

  if (query.empty()) {
    return "No dependency information found for this package.";
  }

  auto pkg = *query.begin();

  std::ostringstream out;

  auto list_field = [&](const char *title, const auto &items) {
    out << title << ":\n";
    if (items.empty()) {
      out << "  (none)\n\n";
      return;
    }
    for (const auto &i : items) {
      out << "  " << i.to_string() << "\n";
    }
    out << "\n";
  };

  list_field("Requires", pkg.get_requires());
  list_field("Provides", pkg.get_provides());
  list_field("Conflicts", pkg.get_conflicts());
  list_field("Obsoletes", pkg.get_obsoletes());

  return out.str();
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
