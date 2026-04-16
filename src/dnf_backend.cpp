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
#include "debug_trace.hpp"

#include <algorithm>
#include <atomic>
#include <ctime>
#include <iomanip>
#include <map>
#include <mutex>
#include <memory>
#include <set>
#include <sstream>

#include <gio/gio.h>

#include <libdnf5/base/base.hpp>
#include <libdnf5/base/goal.hpp>
#include <libdnf5/base/transaction.hpp>
#include <libdnf5/base/transaction_package.hpp>
#include <libdnf5/repo/download_callbacks.hpp>
#include <libdnf5/rpm/package_query.hpp>
#include <libdnf5/transaction/transaction_item_action.hpp>

// -----------------------------------------------------------------------------
// Global state used by UI highlighting and query filters
// -----------------------------------------------------------------------------
std::set<std::string> g_installed_nevras;            // Cached NEVRAs of installed packages for UI highlighting
std::mutex g_installed_mutex;                        // Mutex for thread-safe access to global installed-package cache
std::atomic<bool> g_search_in_description { false }; // Global flag: include description field in search
std::atomic<bool> g_exact_match { false };           // Global flag: match package name or description exactly
static std::map<std::string, PackageRow> g_installed_rows_by_name_arch; // Cached installed rows keyed by name and arch

struct SearchOptions {
  bool search_in_description = false;
  bool exact_match = false;
};

// Keep the newest installed row for one package name and architecture tuple.
static void
remember_installed_row(std::map<std::string, PackageRow> &rows_by_name_arch, const PackageRow &row)
{
  auto [it, inserted] = rows_by_name_arch.emplace(row.name_arch_key(), row);
  if (!inserted && libdnf5::rpm::evrcmp(row, it->second) > 0) {
    it->second = row;
  }
}

// -----------------------------------------------------------------------------
// Helper: Convert a libdnf5 package to the structured UI row model
// -----------------------------------------------------------------------------
static PackageRow
make_package_row(const libdnf5::rpm::Package &pkg,
                 PackageRepoCandidateRelation repo_candidate_relation = PackageRepoCandidateRelation::UNKNOWN)
{
  PackageRow row;
  row.nevra = pkg.get_nevra();
  row.name = pkg.get_name();
  row.epoch = pkg.get_epoch();
  row.version = pkg.get_version();
  row.release = pkg.get_release();
  row.arch = pkg.get_arch();
  row.repo = pkg.get_repo_id();
  row.summary = pkg.get_summary();
  row.repo_candidate_relation = repo_candidate_relation;

  if (row.summary.empty()) {
    row.summary = "(no summary)";
  }

  return row;
}

// Format package size in a way that is easy to read in the details pane.
static std::string
format_package_size(unsigned long long size_bytes)
{
  char *formatted = g_format_size(size_bytes);
  std::string text = formatted ? formatted : "Unknown";
  g_free(formatted);
  return text;
}

// Return true when the active package query task was cancelled by the UI.
static bool
package_query_cancelled(GCancellable *cancellable)
{
  return cancellable && g_cancellable_is_cancelled(cancellable);
}

struct InstalledQueryResult {
  std::vector<PackageRow> rows;
  std::set<std::string> nevras;
  std::map<std::string, PackageRow> rows_by_name_arch;
};

using AvailableRowsProvider = std::function<std::map<std::string, PackageRow>(GCancellable *)>;

// Fold UTF-8 package search text before comparing it against libdnf5 metadata
// fields. This keeps manual name/description matching aligned with GTK's
// case-insensitive text handling for non-ASCII package summaries.
static std::string
utf8_casefold_copy(const std::string &text)
{
  char *folded = g_utf8_casefold(text.c_str(), -1);
  std::string result = folded ? folded : "";
  g_free(folded);
  return result;
}

// Return true when one package matches the active search term using the same
// name/description flag semantics as the main UI search controls.
static bool
package_matches_search(const libdnf5::rpm::Package &pkg,
                       const std::string &pattern_lower,
                       const SearchOptions &search_options)
{
  std::string name = utf8_casefold_copy(pkg.get_name());
  if (search_options.exact_match) {
    return name == pattern_lower;
  }

  if (name.find(pattern_lower) != std::string::npos) {
    return true;
  }

  if (!search_options.search_in_description) {
    return false;
  }

  std::string description = utf8_casefold_copy(pkg.get_description());
  return description.find(pattern_lower) != std::string::npos;
}

// Collect the newest visible repo candidate for each name+arch tuple.
// When a search term is provided, apply the same name/description filtering as
// the main search flow before deduplicating the results.
static std::map<std::string, PackageRow>
collect_available_rows_by_name_arch(libdnf5::Base &base,
                                    GCancellable *cancellable,
                                    const SearchOptions &search_options,
                                    const std::string *pattern = nullptr)
{
  libdnf5::rpm::PackageQuery query(base);
  query.filter_available();
  query.filter_latest_evr();

  if (pattern && !search_options.search_in_description) {
    if (search_options.exact_match) {
      query.filter_name(*pattern, libdnf5::sack::QueryCmp::EQ);
    } else {
      query.filter_name(*pattern, libdnf5::sack::QueryCmp::CONTAINS);
    }
  }

  const std::string pattern_lower = pattern ? utf8_casefold_copy(*pattern) : "";
  std::map<std::string, PackageRow> rows_by_name_arch;

  for (auto pkg : query) {
    if (package_query_cancelled(cancellable)) {
      rows_by_name_arch.clear();
      return rows_by_name_arch;
    }

    if (pattern && search_options.search_in_description &&
        !package_matches_search(pkg, pattern_lower, search_options)) {
      continue;
    }

    // Provenance is UNKNOWN until compared against the installed set
    // visible_rows_from_maps or annotate_installed_row_with_repo_candidate
    // will resolve it when the installed map is available.
    PackageRow row = make_package_row(pkg, PackageRepoCandidateRelation::UNKNOWN);
    rows_by_name_arch[row.name_arch_key()] = row;
  }

  return rows_by_name_arch;
}

// Collect installed package rows and the corresponding exact-NEVRA/name+arch
// caches in one pass. When a search term is provided, filter the installed list
// with the same search semantics used for repo-backed rows.
static InstalledQueryResult
collect_installed_rows(libdnf5::Base &base,
                       GCancellable *cancellable,
                       const SearchOptions &search_options,
                       const std::string *pattern = nullptr)
{
  InstalledQueryResult result;
  const std::string pattern_lower = pattern ? utf8_casefold_copy(*pattern) : "";

  libdnf5::rpm::PackageQuery query(base);
  query.filter_installed();

  for (auto pkg : query) {
    if (package_query_cancelled(cancellable)) {
      result.rows.clear();
      result.nevras.clear();
      result.rows_by_name_arch.clear();
      return result;
    }

    if (pattern && !package_matches_search(pkg, pattern_lower, search_options)) {
      continue;
    }

    PackageRow row = make_package_row(pkg);
    result.nevras.insert(row.nevra);
    remember_installed_row(result.rows_by_name_arch, row);
    result.rows.push_back(row);
  }

  return result;
}

// Compare one installed row against the newest visible repo candidate for the
// same name+arch tuple and annotate the row with the resolved relationship.
static void
annotate_installed_row_with_repo_candidate(PackageRow &installed_row,
                                           const std::map<std::string, PackageRow> &available_rows)
{
  auto it = available_rows.find(installed_row.name_arch_key());
  if (it == available_rows.end()) {
    installed_row.repo_candidate_relation = PackageRepoCandidateRelation::NONE;
    return;
  }

  int cmp = libdnf5::rpm::evrcmp(it->second, installed_row);
  if (cmp > 0) {
    installed_row.repo_candidate_relation = PackageRepoCandidateRelation::NEWER;
  } else if (cmp < 0) {
    installed_row.repo_candidate_relation = PackageRepoCandidateRelation::OLDER;
  } else {
    installed_row.repo_candidate_relation = PackageRepoCandidateRelation::SAME;
  }
}

// Best-effort repo annotation for installed rows. Installed queries should keep
// working from the local rpmdb even when repository metadata is unavailable, so
// failures here only leave repo provenance as UNKNOWN.
static void
annotate_installed_rows_with_repo_candidates_best_effort(std::vector<PackageRow> &installed_rows,
                                                         GCancellable *cancellable,
                                                         const AvailableRowsProvider &available_rows_provider)
{
  if (installed_rows.empty()) {
    return;
  }

  try {
    auto available_rows = available_rows_provider(cancellable);
    if (package_query_cancelled(cancellable)) {
      return;
    }

    for (auto &row : installed_rows) {
      annotate_installed_row_with_repo_candidate(row, available_rows);
    }
  } catch (const std::exception &e) {
    DNFUI_TRACE("Installed row repo annotation skipped: %s", e.what());
  }
}

// Build the merged package view used by search and browse: start with the
// visible repo-backed candidates, then add installed-only rows for name+arch
// tuples that are missing from enabled repositories. If an installed package is
// newer than the repo candidate, keep the installed row so the UI can surface
// that state directly.
//
// Note on repo_candidate_relation in the returned rows:
// Installed rows that are promoted into the map (LOCAL_ONLY, OLDER, or the installed>repo case)
// carry a fully resolved relation.
// Available rows that stay in the map without a matching installed entry keep repo_candidate_relation = UNKNOWN
// because no installed counterpart was found during this pass.
// get_package_install_state handles this correctly through its EVR-comparison fallback path.
// Code that reads repo_candidate_relation directly should treat UNKNOWN on a non-installed row as "no installed
// counterpart known".
static std::vector<PackageRow>
visible_rows_from_maps(std::map<std::string, PackageRow> available_rows,
                       std::map<std::string, PackageRow> installed_rows)
{
  for (auto &[key, installed_row] : installed_rows) {
    annotate_installed_row_with_repo_candidate(installed_row, available_rows);

    auto visible_it = available_rows.find(key);
    if (visible_it == available_rows.end()) {
      available_rows.emplace(key, installed_row);
      continue;
    }

    if (libdnf5::rpm::evrcmp(installed_row, visible_it->second) > 0) {
      visible_it->second = installed_row;
    }
  }

  std::vector<PackageRow> rows;
  rows.reserve(available_rows.size());
  for (auto &[key, row] : available_rows) {
    rows.push_back(row);
  }
  return rows;
}

// -----------------------------------------------------------------------------
// Search merged repo and installed-only package rows and stop early when the
// task cancellable is set.
// Returns the same merged view model as browse/list-packages, but filtered
// through the active search term and search flags.
// -----------------------------------------------------------------------------
std::vector<PackageRow>
dnf_backend_search_package_rows_interruptible(const std::string &pattern, GCancellable *cancellable)
{
  const SearchOptions search_options {
    .search_in_description = g_search_in_description.load(),
    .exact_match = g_exact_match.load(),
  };

  auto [base, guard, generation] = BaseManager::instance().acquire_read();
  auto available_rows = collect_available_rows_by_name_arch(base, cancellable, search_options, &pattern);
  if (package_query_cancelled(cancellable)) {
    return {};
  }

  InstalledQueryResult installed = collect_installed_rows(base, cancellable, search_options, &pattern);
  if (package_query_cancelled(cancellable)) {
    return {};
  }

  return visible_rows_from_maps(std::move(available_rows), std::move(installed.rows_by_name_arch));
}

// -----------------------------------------------------------------------------
// Helper: Refresh global installed package NEVRA (Name Epoch Version Release Architecture) cache
// Clears and repopulates g_installed_nevras by querying all currently installed
// packages through libdnf5. This should be called whenever the UI needs to
// update its installed-package highlighting or when transactions have modified
// the system package set.
//
// Thread-safety:
//   The Base read lock (base_mutex) and g_installed_mutex must never be held
//   simultaneously, as any future caller may acquire g_installed_mutex before
//   calling into BaseManager, which would produce a deadlock.
//   Rows are collected into local sets while the Base lock is held, then the
//   Base lock is released before g_installed_mutex is acquired to publish the
//   new snapshot.
// -----------------------------------------------------------------------------
void
dnf_backend_refresh_installed_nevras()
{
  InstalledQueryResult installed;
  {
    auto [base, guard, generation] = BaseManager::instance().acquire_read();
    const SearchOptions search_options {};
    installed = collect_installed_rows(base, nullptr, search_options);
  } // Base read lock released before acquiring g_installed_mutex

  std::lock_guard<std::mutex> lock(g_installed_mutex);
  g_installed_nevras.swap(installed.nevras);
  g_installed_rows_by_name_arch.swap(installed.rows_by_name_arch);
}

// -----------------------------------------------------------------------------
// Return true only when the queried row exactly matches an installed NEVRA in
// the cached installed snapshot.
// -----------------------------------------------------------------------------
bool
dnf_backend_is_package_installed_exact(const PackageRow &row)
{
  std::lock_guard<std::mutex> lock(g_installed_mutex);
  return g_installed_nevras.count(row.nevra) > 0;
}

// -----------------------------------------------------------------------------
// Return whether one package row is available, upgradeable, installed exactly,
// installed from a local-only RPM, or newer than the repo candidate.
// Exact-installed rows prefer the explicit repo provenance stored on the row.
// Non-exact rows fall back to the installed name+arch cache so available rows
// can still show upgrade-state information without requiring duplicate rows.
// -----------------------------------------------------------------------------
PackageInstallState
dnf_backend_get_package_install_state(const PackageRow &row)
{
  std::lock_guard<std::mutex> lock(g_installed_mutex);
  if (g_installed_nevras.count(row.nevra) > 0) {
    switch (row.repo_candidate_relation) {
    case PackageRepoCandidateRelation::UNKNOWN:
      // Annotation was not run or failed (best-effort path). The package is
      // known-installed but we cannot distinguish LOCAL_ONLY from INSTALLED
      // without a successful repo query. Fall back to INSTALLED so the UI
      // does not misrepresent the package state.
    case PackageRepoCandidateRelation::SAME:
      return PackageInstallState::INSTALLED;
    case PackageRepoCandidateRelation::NONE:
      return PackageInstallState::LOCAL_ONLY;
    case PackageRepoCandidateRelation::NEWER:
      return PackageInstallState::UPGRADEABLE;
    case PackageRepoCandidateRelation::OLDER:
      return PackageInstallState::INSTALLED_NEWER_THAN_REPO;
    default:
      return PackageInstallState::INSTALLED;
    }
  }

  auto it = g_installed_rows_by_name_arch.find(row.name_arch_key());
  if (it == g_installed_rows_by_name_arch.end()) {
    return PackageInstallState::AVAILABLE;
  }

  if (libdnf5::rpm::evrcmp(row, it->second) > 0) {
    return PackageInstallState::UPGRADEABLE;
  }

  return PackageInstallState::INSTALLED_NEWER_THAN_REPO;
}

// -----------------------------------------------------------------------------
// Return true only when the exact installed NEVRA is also available from the
// current package sources and can therefore be reinstalled through libdnf5.
// -----------------------------------------------------------------------------
bool
dnf_backend_can_reinstall_package(const PackageRow &row)
{
  if (!dnf_backend_is_package_installed_exact(row)) {
    return false;
  }

  auto [base, guard, generation] = BaseManager::instance().acquire_read();
  libdnf5::rpm::PackageQuery query(base);
  query.filter_nevra(row.nevra);
  query.filter_available();
  return !query.empty();
}

// -----------------------------------------------------------------------------
// Helper: Query installed packages via libdnf5
// Returns a list of all installed packages in full NEVRA format (e.g., pkg-1.0-1.fc38.x86_64).
// Also updates the global set of installed package NEVRAs (g_installed_nevras).
//
// Thread-safety:
//   The query result is collected in local sets first, then published under
//   g_installed_mutex once the scan completes. This avoids partial cache
//   updates if the worker task is cancelled midway through the scan.
// -----------------------------------------------------------------------------
std::vector<PackageRow>
dnf_backend_get_installed_package_rows_interruptible(GCancellable *cancellable)
{
  InstalledQueryResult installed;
  {
    auto [base, guard, generation] = BaseManager::instance().acquire_read();
    const SearchOptions search_options {};
    installed = collect_installed_rows(base, cancellable, search_options);
    if (package_query_cancelled(cancellable)) {
      return {};
    }

    annotate_installed_rows_with_repo_candidates_best_effort(
        installed.rows, cancellable, [&base](GCancellable *annotation_cancellable) {
          const SearchOptions annotation_search_options {};
          return collect_available_rows_by_name_arch(base, annotation_cancellable, annotation_search_options);
        });
  }

  // Publish the new installed-package cache only after a complete uncancelled scan.
  std::lock_guard<std::mutex> lock(g_installed_mutex);
  g_installed_nevras.swap(installed.nevras);
  g_installed_rows_by_name_arch.swap(installed.rows_by_name_arch);

  return installed.rows;
}

// -----------------------------------------------------------------------------
// Helper: Query the combined browse view via libdnf5
// Returns the newest available candidate for each package stream and merges in
// installed-only local RPMs that are missing from enabled repositories.
//
// Thread-safety:
//   This function reads package data directly from libdnf5 and does not modify
//   shared installed-package caches. No locking is required here.
// -----------------------------------------------------------------------------
std::vector<PackageRow>
dnf_backend_get_browse_package_rows_interruptible(GCancellable *cancellable)
{
  const SearchOptions search_options {};

  auto [base, guard, generation] = BaseManager::instance().acquire_read();
  auto available_rows = collect_available_rows_by_name_arch(base, cancellable, search_options);
  if (package_query_cancelled(cancellable)) {
    return {};
  }

  InstalledQueryResult installed = collect_installed_rows(base, cancellable, search_options);
  if (package_query_cancelled(cancellable)) {
    return {};
  }

  return visible_rows_from_maps(std::move(available_rows), std::move(installed.rows_by_name_arch));
}

// -----------------------------------------------------------------------------
// Return installed package rows that exactly match one NEVRA.
// Repo provenance is annotated on a best-effort basis so pending-action
// navigation can still show local-only or newer-than-repo status when possible.
// -----------------------------------------------------------------------------
std::vector<PackageRow>
dnf_backend_get_installed_package_rows_by_nevra(const std::string &pkg_nevra)
{
  std::vector<PackageRow> packages;

  auto [base, guard, generation] = BaseManager::instance().acquire_read();
  libdnf5::rpm::PackageQuery query(base);
  query.filter_nevra(pkg_nevra);
  query.filter_installed();

  for (auto pkg : query) {
    packages.push_back(make_package_row(pkg));
  }

  // Scope the annotation query to the package name so we load only the one
  // relevant name+arch entry instead of the entire available package set.
  const std::string annotation_pattern = packages.empty() ? "" : packages[0].name;
  annotate_installed_rows_with_repo_candidates_best_effort(
      packages, nullptr, [&base, &annotation_pattern](GCancellable *annotation_cancellable) {
        const SearchOptions search_options {};
        return collect_available_rows_by_name_arch(
            base, annotation_cancellable, search_options, annotation_pattern.empty() ? nullptr : &annotation_pattern);
      });

  return packages;
}

#ifdef DNFUI_BUILD_TESTS
bool
dnf_backend_testonly_annotation_fallback_leaves_rows_unknown(std::vector<PackageRow> &rows)
{
  for (auto &row : rows) {
    row.repo_candidate_relation = PackageRepoCandidateRelation::UNKNOWN;
  }

  annotate_installed_rows_with_repo_candidates_best_effort(
      rows, nullptr, [](GCancellable *) -> std::map<std::string, PackageRow> {
        throw std::runtime_error("forced annotation failure");
      });

  return std::all_of(rows.begin(), rows.end(), [](const PackageRow &row) {
    return row.repo_candidate_relation == PackageRepoCandidateRelation::UNKNOWN;
  });
}
#endif

// -----------------------------------------------------------------------------
// Return available package rows that exactly match one NEVRA.
// This helper stays repo-only and is used for install-side pending-action
// navigation and details loading.
// -----------------------------------------------------------------------------
std::vector<PackageRow>
dnf_backend_get_available_package_rows_by_nevra(const std::string &pkg_nevra)
{
  std::vector<PackageRow> packages;

  auto [base, guard, generation] = BaseManager::instance().acquire_read();
  libdnf5::rpm::PackageQuery query(base);
  query.filter_nevra(pkg_nevra);
  query.filter_available();

  for (auto pkg : query) {
    packages.push_back(make_package_row(pkg));
  }

  return packages;
}

// -----------------------------------------------------------------------------
// Helper: Retrieve detailed package information
// Fetches and formats detailed info for a single package, including:
//   - name, full package ID, version, release, architecture, repo
//   - summary and description
//
// Always performs an exact NEVRA match (the UI passes full NEVRA strings).
//
// Thread-safety:
//   This function reads package data from libdnf5 directly and does not touch
//   shared global sets. No locking required.
// -----------------------------------------------------------------------------
std::string
dnf_backend_get_package_info(const std::string &pkg_nevra)
{
  auto [base, guard, generation] = BaseManager::instance().acquire_read();
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
  PackageRow selected_row = make_package_row(pkg);

  // Show the installed version when the selected row is an update candidate.
  libdnf5::rpm::PackageQuery installed_by_name(base);
  installed_by_name.filter_name(pkg.get_name(), libdnf5::sack::QueryCmp::EQ);
  installed_by_name.filter_installed();

  PackageRow installed_row;
  bool have_installed_counterpart = false;
  for (auto installed_pkg : installed_by_name) {
    if (installed_pkg.get_arch() != pkg.get_arch()) {
      continue;
    }

    PackageRow row = make_package_row(installed_pkg);
    if (!have_installed_counterpart || libdnf5::rpm::evrcmp(row, installed_row) > 0) {
      installed_row = row;
      have_installed_counterpart = true;
    }
  }

  std::ostringstream oss;
  oss << "Name: " << pkg.get_name() << "\n"
      << "Package ID: " << pkg.get_nevra() << "\n"
      << "Version: " << pkg.get_version() << "\n"
      << "Release: " << pkg.get_release() << "\n"
      << "Arch: " << pkg.get_arch() << "\n"
      << "Repo: " << pkg.get_repo_id() << "\n"
      << "Install Size: " << format_package_size(static_cast<unsigned long long>(pkg.get_install_size())) << "\n";

  unsigned long long download_size = static_cast<unsigned long long>(pkg.get_download_size());
  if (download_size > 0) {
    oss << "Download Size: " << format_package_size(download_size) << "\n";
  }

  if (have_installed_counterpart && installed_row.nevra != selected_row.nevra) {
    oss << "Installed Version: " << installed_row.display_version() << "\n";
  }

  oss << "\n"
      << "Summary:\n"
      << pkg.get_summary() << "\n\n"
      << "Description:\n"
      << pkg.get_description();

  return oss.str();
}

// -----------------------------------------------------------------------------
// Helper: Retrieve file list for an installed package (by NEVRA)
// Returns newline-separated file list or a friendly message if none.
//
// max_files_for_display: if > 0, limits visible output and appends a truncation
// notice. Used by the UI to avoid clipboard socket overflow when copying large
// file lists. Pass 0 for the full list.
// -----------------------------------------------------------------------------
std::string
dnf_backend_get_installed_package_files(const std::string &pkg_nevra, size_t max_files_for_display)
{
  DNFUI_TRACE("Backend file list start nevra=%s max_display=%zu", pkg_nevra.c_str(), max_files_for_display);
  auto [base, guard, generation] = BaseManager::instance().acquire_read();
  libdnf5::rpm::PackageQuery query(base);

  query.filter_nevra(pkg_nevra);
  query.filter_installed();

  if (query.empty()) {
    DNFUI_TRACE("Backend file list not installed nevra=%s", pkg_nevra.c_str());
    return "File list available only for installed packages.";
  }

  query.filter_latest_evr();
  auto pkg = *query.begin();

  std::ostringstream files;
  size_t file_count = 0;
  size_t displayed_count = 0;
  const bool should_truncate = max_files_for_display > 0;

  for (const auto &f : pkg.get_files()) {
    ++file_count;
    if (!should_truncate || displayed_count < max_files_for_display) {
      files << f << "\n";
      ++displayed_count;
    }
  }

  std::string result = files.str();
  if (result.empty()) {
    result = "No files recorded for this installed package.";
  } else if (should_truncate && file_count > max_files_for_display) {
    // Append file list truncation notice:
    const size_t hidden_count = file_count - displayed_count;
    files << "\n--- " << hidden_count << " more file" << (hidden_count == 1 ? "" : "s") << " not shown ---\n"
          << "--- Use the package manager CLI for complete list ---\n";
    result = files.str();
  }

  DNFUI_TRACE("Backend file list done nevra=%s total=%zu displayed=%zu bytes=%zu",
              pkg_nevra.c_str(),
              file_count,
              displayed_count,
              result.size());

  return result;
}

// -----------------------------------------------------------------------------
// Helper: Retrieve dependency information for a package (Requires/Provides/etc.)
// Returns a formatted string for display in the "Dependencies" tab
// -----------------------------------------------------------------------------
std::string
dnf_backend_get_package_deps(const std::string &pkg_nevra)
{
  auto [base, guard, generation] = BaseManager::instance().acquire_read();
  libdnf5::rpm::PackageQuery query(base);

  query.filter_nevra(pkg_nevra);

  if (query.empty()) {
    return "No dependency information found for this package.";
  }

  // Prefer the installed copy: its rpmdb metadata is always present and
  // authoritative. Fall back to any available repo match if not installed.
  libdnf5::rpm::PackageQuery installed(query);
  installed.filter_installed();
  libdnf5::rpm::PackageQuery &best = installed.empty() ? query : installed;
  auto pkg = *best.begin();

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
// Helper: Retrieve package changelog entries
// Returns formatted changelog text or a friendly message if none available
// -----------------------------------------------------------------------------
std::string
dnf_backend_get_package_changelog(const std::string &pkg_nevra)
{
  auto [base, guard, generation] = BaseManager::instance().acquire_read();
  libdnf5::rpm::PackageQuery query(base);

  query.filter_nevra(pkg_nevra);

  if (query.empty()) {
    return "No changelog available.";
  }

  // Prefer the installed copy: repo metadata often omits older changelog
  // entries while the rpmdb retains the full history. Fall back to any
  // available repo match if the package is not installed.
  libdnf5::rpm::PackageQuery installed(query);
  installed.filter_installed();
  libdnf5::rpm::PackageQuery &best = installed.empty() ? query : installed;
  auto pkg = *best.begin();

  std::ostringstream out;

  auto entries = pkg.get_changelogs();
  if (entries.empty()) {
    return "No changelog entries found.";
  }

  for (const auto &entry : entries) {
    std::time_t ts = static_cast<std::time_t>(entry.get_timestamp());
    std::tm tm_buf {};
    localtime_r(&ts, &tm_buf);

    out << "Date: " << std::put_time(&tm_buf, "%Y-%m-%d") << "\n"
        << "Author: " << entry.get_author() << "\n"
        << entry.get_text() << "\n\n";
  }

  return out.str();
}

// -----------------------------------------------------------------------------
// Helper: Format a short, bounded summary of package specs for error reporting
//
// Purpose:
//   When a transaction fails or resolves to an empty set, this helper produces
//   a concise, human-readable summary of the install and remove specs involved.
//   This is intended purely for diagnostics and must not affect transaction
//   logic or behavior.
//
// Output format:
//   - "<count>" if empty
//   - "<count> (spec1, spec2, ...)" with a bounded preview if non-empty
//
// Notes:
//   - The output is intentionally truncated to avoid flooding the UI with
//     large transaction payloads.
//   - This helper is used only in failure paths.
// -----------------------------------------------------------------------------
static std::string
format_specs(const std::vector<std::string> &specs)
{
  std::ostringstream out;
  out << specs.size();

  if (!specs.empty()) {
    out << " (";

    const size_t limit = std::min<size_t>(specs.size(), 3);
    for (size_t i = 0; i < limit; ++i) {
      if (i > 0) {
        out << ", ";
      }
      out << specs[i];
    }

    if (specs.size() > limit) {
      out << ", ...";
    }

    out << ")";
  }

  return out.str();
}

// -----------------------------------------------------------------------------
// Helper: Forward one progress line to the UI callback
// -----------------------------------------------------------------------------
static void
emit_progress_line(const TransactionProgressCallback &progress_cb, const std::string &message)
{
  if (!progress_cb || message.empty()) {
    return;
  }

  progress_cb(message);
}

// -----------------------------------------------------------------------------
// Helper: Forward a multi-line message as individual progress lines
// -----------------------------------------------------------------------------
static void
emit_progress_block(const TransactionProgressCallback &progress_cb, const std::string &message)
{
  if (!progress_cb || message.empty()) {
    return;
  }

  std::istringstream stream(message);
  std::string line;

  while (std::getline(stream, line)) {
    if (!line.empty()) {
      progress_cb(line);
    }
  }
}

// -----------------------------------------------------------------------------
// Helper: Human-friendly summary label for transaction package actions
// -----------------------------------------------------------------------------
static std::string
transaction_action_label(libdnf5::base::TransactionPackage::Action action)
{
  using Action = libdnf5::base::TransactionPackage::Action;

  switch (action) {
  case Action::INSTALL:
    return "Install";
  case Action::UPGRADE:
    return "Upgrade";
  case Action::DOWNGRADE:
    return "Downgrade";
  case Action::REINSTALL:
    return "Reinstall";
  case Action::REMOVE:
    return "Remove";
  case Action::REPLACED:
    return "Replace";
  case Action::REASON_CHANGE:
    return "Reason change";
  default:
    return "Process";
  }
}

// -----------------------------------------------------------------------------
// Helper: Produce a readable package label for transaction logs
// -----------------------------------------------------------------------------
static std::string
transaction_package_label(const libdnf5::base::TransactionPackage &item)
{
  return item.get_package().get_nevra();
}

// -----------------------------------------------------------------------------
// libdnf5 download callbacks for the streaming transaction popup
// -----------------------------------------------------------------------------
class StreamingDownloadCallbacks final : public libdnf5::repo::DownloadCallbacks {
  public:
  explicit StreamingDownloadCallbacks(TransactionProgressCallback progress_cb)
      : progress_cb(std::move(progress_cb))
  {
  }

  void *add_new_download(void *, const char *description, double) override
  {
    auto *state = new DownloadState;
    state->description = description ? description : "package";
    emit_progress_line(progress_cb, "Downloading: " + state->description);
    return state;
  }

  int progress(void *user_cb_data, double total_to_download, double downloaded) override
  {
    auto *state = static_cast<DownloadState *>(user_cb_data);
    if (!state || total_to_download <= 0.0) {
      return OK;
    }

    int percent = static_cast<int>((downloaded * 100.0) / total_to_download);
    percent = std::clamp(percent, 0, 100);
    int bucket = percent / 10;

    if (bucket > state->last_reported_bucket) {
      state->last_reported_bucket = bucket;
      emit_progress_line(progress_cb,
                         "Download progress: " + state->description + " (" + std::to_string(percent) + "%)");
    }

    return OK;
  }

  int end(void *user_cb_data, TransferStatus status, const char *msg) override
  {
    std::unique_ptr<DownloadState> state(static_cast<DownloadState *>(user_cb_data));
    std::string description = state ? state->description : "package";

    switch (status) {
    case TransferStatus::SUCCESSFUL:
    case TransferStatus::ALREADYEXISTS:
      emit_progress_line(progress_cb, "Download ready: " + description);
      break;
    case TransferStatus::ERROR:
      if (msg && *msg) {
        emit_progress_line(progress_cb, "Download failed: " + description + " (" + std::string(msg) + ")");
      } else {
        emit_progress_line(progress_cb, "Download failed: " + description);
      }
      break;
    }

    return OK;
  }

  private:
  struct DownloadState {
    std::string description;
    int last_reported_bucket = -1;
  };

  TransactionProgressCallback progress_cb;
};

// -----------------------------------------------------------------------------
// Helper: Reset Base download callbacks when leaving transaction apply scope
// -----------------------------------------------------------------------------
class DownloadCallbacksReset {
  public:
  explicit DownloadCallbacksReset(libdnf5::Base &base)
      : base(base)
  {
  }

  ~DownloadCallbacksReset()
  {
    base.set_download_callbacks(std::unique_ptr<libdnf5::repo::DownloadCallbacks>());
  }

  private:
  libdnf5::Base &base;
};

// Resolve the requested transaction once so preview and apply stay in sync.
static bool
resolve_transaction_plan(libdnf5::Base &base,
                         const std::vector<std::string> &install_nevras,
                         const std::vector<std::string> &remove_nevras,
                         const std::vector<std::string> &reinstall_nevras,
                         std::string &error_out,
                         const TransactionProgressCallback &progress_cb,
                         std::unique_ptr<libdnf5::base::Transaction> &transaction_out)
{
  transaction_out.reset();

  if (install_nevras.empty() && remove_nevras.empty() && reinstall_nevras.empty()) {
    error_out = "No packages specified in transaction.";
    return false;
  }

  libdnf5::Goal goal(base);

  emit_progress_line(progress_cb, "Resolving dependency changes...");

  // NOTE: Let package removal also remove installed packages that depend on the selected package.
  if (!remove_nevras.empty()) {
    goal.set_allow_erasing(true);
  }

  // NOTE: We pass package "specs" (currently NEVRA strings from the UI list).
  for (const auto &spec : install_nevras) {
    goal.add_rpm_install(spec);
  }

  for (const auto &spec : remove_nevras) {
    goal.add_rpm_remove(spec);
  }

  for (const auto &spec : reinstall_nevras) {
    goal.add_rpm_reinstall(spec);
  }

  auto transaction = goal.resolve();

  auto goal_problem = transaction.get_problems();
  if (goal_problem != libdnf5::GoalProblem::NO_PROBLEM) {
    std::ostringstream oss;
    oss << "Unable to resolve transaction.\n";

    for (const auto &log : transaction.get_resolve_logs_as_strings()) {
      oss << "  " << log << "\n";
    }

    error_out = oss.str();
    emit_progress_block(progress_cb, error_out);
    return false;
  }

  if (transaction.get_transaction_packages().empty()) {
    std::ostringstream oss;
    oss << "No packages in transaction (nothing to do).\n"
        << "Install specs: " << format_specs(install_nevras) << "\n"
        << "Remove specs: " << format_specs(remove_nevras) << "\n"
        << "Reinstall specs: " << format_specs(reinstall_nevras) << "\n";
    error_out = oss.str();
    emit_progress_block(progress_cb, error_out);
    return false;
  }

  transaction_out = std::make_unique<libdnf5::base::Transaction>(std::move(transaction));
  return true;
}

// Add one resolved transaction item to the confirmation preview model.
static void
append_preview_item(TransactionPreview &preview, const libdnf5::base::TransactionPackage &item)
{
  using Action = libdnf5::base::TransactionPackage::Action;

  const std::string label = transaction_package_label(item);
  const long long install_size = static_cast<long long>(item.get_package().get_install_size());

  switch (item.get_action()) {
  case Action::INSTALL:
    preview.install.push_back(label);
    preview.disk_space_delta += install_size;
    break;
  case Action::UPGRADE:
    preview.upgrade.push_back(label);
    preview.disk_space_delta += install_size;
    break;
  case Action::DOWNGRADE:
    preview.downgrade.push_back(label);
    preview.disk_space_delta += install_size;
    break;
  case Action::REINSTALL:
    preview.reinstall.push_back(label);
    break;
  case Action::REMOVE:
    preview.remove.push_back(label);
    preview.disk_space_delta -= install_size;
    break;
  case Action::REPLACED:
    preview.disk_space_delta -= install_size;
    break;
  default:
    break;
  }
}

// Resolve the final transaction and group the resulting package actions for the summary dialog.
bool
dnf_backend_preview_transaction(const std::vector<std::string> &install_nevras,
                                const std::vector<std::string> &remove_nevras,
                                const std::vector<std::string> &reinstall_nevras,
                                TransactionPreview &preview,
                                std::string &error_out,
                                const TransactionProgressCallback &progress_cb)
{
  error_out.clear();
  preview = TransactionPreview();

  try {
    DNFUI_TRACE("Transaction preview start install=%zu remove=%zu reinstall=%zu",
                install_nevras.size(),
                remove_nevras.size(),
                reinstall_nevras.size());
    auto [base, guard] = BaseManager::instance().acquire_write();
    std::unique_ptr<libdnf5::base::Transaction> transaction;

    if (!resolve_transaction_plan(
            base, install_nevras, remove_nevras, reinstall_nevras, error_out, progress_cb, transaction)) {
      DNFUI_TRACE("Transaction preview resolve failed: %s", error_out.c_str());
      return false;
    }

    for (const auto &item : transaction->get_transaction_packages()) {
      append_preview_item(preview, item);
    }

    DNFUI_TRACE("Transaction preview done items=%zu", transaction->get_transaction_packages_count());
    return true;
  } catch (const std::exception &e) {
    error_out = e.what();
    DNFUI_TRACE("Transaction preview failed: %s", e.what());
    return false;
  }
}

bool
dnf_backend_apply_transaction(const std::vector<std::string> &install_nevras,
                              const std::vector<std::string> &remove_nevras,
                              const std::vector<std::string> &reinstall_nevras,
                              std::string &error_out,
                              const TransactionProgressCallback &progress_cb)
{
  error_out.clear();

  // AUTHORIZATION NOTE:
  // When called through the transaction service D-Bus interface,
  // Polkit authorization is enforced by the service before this function is invoked.
  // The service runs with elevated privileges and performs the authorization check upstream.
  //
  // Direct backend calls (tests, service-side execution) bypass authorization and ASSUME
  // the caller has already validated privileges or is running in a test environment.

  try {
    DNFUI_TRACE("Transaction apply start install=%zu remove=%zu reinstall=%zu",
                install_nevras.size(),
                remove_nevras.size(),
                reinstall_nevras.size());
    // Exclusive access to shared libdnf Base for transactional changes
    auto [base, guard] = BaseManager::instance().acquire_write();
    std::unique_ptr<libdnf5::base::Transaction> transaction;

    if (!resolve_transaction_plan(
            base, install_nevras, remove_nevras, reinstall_nevras, error_out, progress_cb, transaction)) {
      DNFUI_TRACE("Transaction apply resolve failed: %s", error_out.c_str());
      return false;
    }

    emit_progress_line(progress_cb,
                       "Resolved " + std::to_string(transaction->get_transaction_packages_count()) + " package item" +
                           (transaction->get_transaction_packages_count() == 1 ? "." : "s."));

    for (const auto &item : transaction->get_transaction_packages()) {
      emit_progress_line(progress_cb,
                         transaction_action_label(item.get_action()) + ": " + transaction_package_label(item));
    }

    base.set_download_callbacks(std::make_unique<StreamingDownloadCallbacks>(progress_cb));
    DownloadCallbacksReset download_callbacks_reset(base);
    emit_progress_line(progress_cb, "Starting package downloads...");
    DNFUI_TRACE("Transaction download start");
    transaction->download();
    DNFUI_TRACE("Transaction download done");
    emit_progress_line(progress_cb, "Package downloads finished.");

    DNFUI_TRACE("Transaction run start");
    auto run_result = transaction->run();
    DNFUI_TRACE("Transaction run done result=%d", static_cast<int>(run_result));
    if (run_result != libdnf5::base::Transaction::TransactionRunResult::SUCCESS) {
      std::ostringstream oss;
      oss << "Transaction failed: " << libdnf5::base::Transaction::transaction_result_to_string(run_result) << " (code "
          << static_cast<int>(run_result) << ").\n";

      for (const auto &msg : transaction->get_transaction_problems()) {
        oss << "  " << msg << "\n";
      }

      auto rpm_messages = transaction->get_rpm_messages();
      if (!rpm_messages.empty()) {
        oss << "RPM messages:\n";
        for (const auto &msg : rpm_messages) {
          oss << "  " << msg << "\n";
        }
      }

      std::string script_output = transaction->get_last_script_output();
      if (!script_output.empty()) {
        oss << "Last script output:\n" << script_output << "\n";
      }

      error_out = oss.str();
      emit_progress_block(progress_cb, error_out);
      return false;
    }

    emit_progress_line(progress_cb, "Transaction applied successfully.");
    return true;
  } catch (const std::exception &e) {
    error_out = e.what();
    emit_progress_line(progress_cb, error_out);
    return false;
  }
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
