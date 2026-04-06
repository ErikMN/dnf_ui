// src/dnf_backend.hpp
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
// friendlier fields for list and column-based views.
// -----------------------------------------------------------------------------
struct PackageRow {
  std::string nevra;
  std::string name;
  std::string epoch;
  std::string version;
  std::string release;
  std::string arch;
  std::string repo;
  std::string summary;

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
enum class PackageInstallState {
  AVAILABLE,
  UPGRADEABLE,
  INSTALLED,
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
extern std::set<std::string> g_installed_names;

void dnf_backend_refresh_installed_nevras();
PackageInstallState dnf_backend_get_package_install_state(const PackageRow &row);

// Structured package row queries used by the main package list presentation.
// Callers that do not need cancellation can pass nullptr as the cancellable.
std::vector<PackageRow> dnf_backend_get_installed_package_rows_interruptible(GCancellable *cancellable);
std::vector<PackageRow> dnf_backend_get_available_package_rows_interruptible(GCancellable *cancellable);
std::vector<PackageRow> dnf_backend_search_available_package_rows_interruptible(const std::string &pattern,
                                                                                GCancellable *cancellable);
std::vector<PackageRow> dnf_backend_get_installed_package_rows_by_nevra(const std::string &pkg_nevra);
std::vector<PackageRow> dnf_backend_get_available_package_rows_by_nevra(const std::string &pkg_nevra);
std::string dnf_backend_get_package_info(const std::string &pkg_name);
std::string dnf_backend_get_installed_package_files(const std::string &pkg_nevra);
std::string dnf_backend_get_package_deps(const std::string &pkg_nevra);
std::string dnf_backend_get_package_changelog(const std::string &pkg_nevra);
// Resolve the pending transaction and summarize the final package changes for UI review.
bool dnf_backend_preview_transaction(const std::vector<std::string> &install_nevras,
                                     const std::vector<std::string> &remove_nevras,
                                     const std::vector<std::string> &reinstall_nevras,
                                     TransactionPreview &preview,
                                     std::string &error_out);
bool dnf_backend_apply_transaction(const std::vector<std::string> &install_nevras,
                                   const std::vector<std::string> &remove_nevras,
                                   const std::vector<std::string> &reinstall_nevras,
                                   std::string &error_out,
                                   const TransactionProgressCallback &progress_cb = {});

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
