// src/dnf_backend/dnf_backend.hpp
// Public libdnf5 backend facade
//
// This header is the app-facing contract for the libdnf5 integration. It keeps
// libdnf5 types out of the GTK/controller layer by exposing small value models
// and string-based transaction specs, while the implementation owns Base access,
// rpmdb/repository queries, EVR comparison, cache publication, and transaction
// resolution.
//
// Callers should depend only on the types and functions declared here. Helpers
// under src/dnf_backend/dnf_internal.hpp are private implementation details for
// the backend translation units and may change whenever the backend internals
// are reorganized.
#pragma once

#include <functional>
#include <memory>
#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <gio/gio.h>

// -----------------------------------------------------------------------------
// libdnf5 backend helpers
// -----------------------------------------------------------------------------
// Structured package metadata used by the GTK presentation layer.
// Keeps the full NEVRA for internal selection and transactions while exposing
// friendlier fields for list and column-based views. The repo candidate relation
// describes how the installed row compares to the newest visible repo-backed
// candidate for the same name+arch tuple:
//   UNKNOWN: repo provenance was not checked or could not be resolved
//   NONE: no visible repo candidate exists for that name+arch tuple
//   SAME: installed and visible repo candidate resolve to the same EVR
//   NEWER: the visible repo candidate is newer than the installed row
//   OLDER: the installed row is newer than the visible repo candidate
// -----------------------------------------------------------------------------
enum class PackageRepoCandidateRelation {
  UNKNOWN,
  NONE,
  SAME,
  NEWER,
  OLDER,
};

// Backend-owned install reason for installed packages.
// This keeps package provenance visible to the UI without exposing libdnf5
// enums directly through the presentation model. Available-only rows keep
// UNKNOWN because the install reason is meaningful only for installed packages.
enum class PackageInstallReason {
  UNKNOWN,
  DEPENDENCY,
  USER,
  CLEAN,
  WEAK_DEPENDENCY,
  GROUP,
  EXTERNAL,
};

struct PackageRow {
  std::string nevra;
  std::string name;
  std::string epoch;
  std::string version;
  std::string release;
  std::string arch;
  std::string repo;
  std::string summary;
  PackageInstallReason install_reason = PackageInstallReason::UNKNOWN;
  PackageRepoCandidateRelation repo_candidate_relation = PackageRepoCandidateRelation::UNKNOWN;

  const std::string &get_epoch() const
  {
    return epoch;
  }
  const std::string &get_version() const
  {
    return version;
  }
  const std::string &get_release() const
  {
    return release;
  }
  std::string name_arch_key() const
  {
    return name + "\n" + arch;
  }

  std::string display_version() const
  {
    if (version.empty()) {
      return release;
    }
    if (release.empty()) {
      return version;
    }
    return version + "-" + release;
  }
};

// Backend-owned install state so the UI can reason about package actions
// without depending on libdnf5 headers or EVR comparison details.
// These values are presentation-oriented and may depend on repo provenance
// being known for the visible row.
enum class PackageInstallState {
  AVAILABLE,
  UPGRADEABLE,
  INSTALLED,
  LOCAL_ONLY,
  INSTALLED_NEWER_THAN_REPO,
};

// Resolved transaction preview used by the confirmation dialog before apply.
struct TransactionPreview {
  std::vector<std::string> install;
  std::vector<std::string> upgrade;
  std::vector<std::string> downgrade;
  std::vector<std::string> reinstall;
  std::vector<std::string> remove;
  long long disk_space_delta = 0;
};

using TransactionProgressCallback = std::function<void(const std::string &)>;

extern std::atomic<bool> g_search_in_description;
extern std::atomic<bool> g_exact_match;
extern std::mutex g_installed_mutex;
extern std::set<std::string> g_installed_nevras;

// Refresh the installed-package snapshot used by the UI for exact-installed
// checks and upgrade-state classification.
void dnf_backend_refresh_installed_nevras();

// Classify one visible package row for UI status badges and action gating.
PackageInstallState dnf_backend_get_package_install_state(const PackageRow &row);

// Return the default package-table sort priority for one package state.
// Lower values sort first and keep installed rows ahead of repo-only rows.
int dnf_backend_get_install_state_sort_rank(PackageInstallState state);

// Convert one backend-owned install reason to user-facing text.
std::string dnf_backend_install_reason_to_string(PackageInstallReason reason);

// Return true only when this exact NEVRA is installed on the current system.
bool dnf_backend_is_package_installed_exact(const PackageRow &row);

// Return true when the exact installed NEVRA can be reinstalled from currently
// available package sources. Local-only packages therefore return false.
bool dnf_backend_can_reinstall_package(const PackageRow &row);

// Return true when this installed package owns the running GUI executable and
// therefore must not be removed or reinstalled from within the app itself.
bool dnf_backend_is_package_self_protected(const PackageRow &row);

// Return true when one installed remove/reinstall spec targets the running GUI
// package and must be rejected before the transaction is previewed or applied.
bool dnf_backend_is_self_protected_transaction_spec(const std::string &spec);

// Structured package row queries used by the main package list presentation.
// Browse and search results use a merged package view: the newest repo candidate
// for each name+arch pair plus any installed-only local RPMs that are missing
// from enabled repositories. Callers that do not need cancellation can pass
// nullptr as the cancellable.
// Query all installed packages. This path remains local-first and should still
// work when repository metadata is unavailable; repo provenance is annotated
// only as a best-effort extra when possible.
std::vector<PackageRow> dnf_backend_get_installed_package_rows_interruptible(GCancellable *cancellable);

// Query the merged browse view shown by "List Packages".
std::vector<PackageRow> dnf_backend_get_browse_package_rows_interruptible(GCancellable *cancellable);

// Search the merged browse view using the current search flags.
std::vector<PackageRow> dnf_backend_search_package_rows_interruptible(const std::string &pattern,
                                                                      GCancellable *cancellable);

// Exact NEVRA helpers used by details views and pending-action navigation.
std::vector<PackageRow> dnf_backend_get_installed_package_rows_by_nevra(const std::string &pkg_nevra);
std::vector<PackageRow> dnf_backend_get_available_package_rows_by_nevra(const std::string &pkg_nevra);
std::string dnf_backend_get_package_info(const std::string &pkg_nevra);
std::string dnf_backend_get_installed_package_files(const std::string &pkg_nevra, size_t max_files_for_display = 1500);
std::string dnf_backend_get_package_deps(const std::string &pkg_nevra);
std::string dnf_backend_get_package_changelog(const std::string &pkg_nevra);
// Resolve the pending transaction and summarize the final package changes for UI review.
bool dnf_backend_preview_transaction(const std::vector<std::string> &install_nevras,
                                     const std::vector<std::string> &remove_nevras,
                                     const std::vector<std::string> &reinstall_nevras,
                                     TransactionPreview &preview,
                                     std::string &error_out,
                                     const TransactionProgressCallback &progress_cb = {});
bool dnf_backend_apply_transaction(const std::vector<std::string> &install_nevras,
                                   const std::vector<std::string> &remove_nevras,
                                   const std::vector<std::string> &reinstall_nevras,
                                   std::string &error_out,
                                   const TransactionProgressCallback &progress_cb = {});

#ifdef DNFUI_BUILD_TESTS
// Test-only hook: force the best-effort repo annotation path to fail and return
// whether all rows kept UNKNOWN repo-candidate relation afterwards.
bool dnf_backend_testonly_annotation_fallback_leaves_rows_unknown(std::vector<PackageRow> &rows);
#endif

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
