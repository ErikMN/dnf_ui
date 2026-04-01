// src/dnf_backend.hpp
#pragma once

#include <functional>
#include <memory>
#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <vector>

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
  std::string version;
  std::string release;
  std::string arch;
  std::string repo;
  std::string summary;

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

void refresh_installed_nevras();

// Structured package row queries used by the main package list presentation.
std::vector<PackageRow> get_installed_package_rows();
std::vector<std::string> get_installed_packages();
std::vector<PackageRow> search_available_package_rows(const std::string &pattern);
std::vector<std::string> search_available_packages(const std::string &pattern);
std::string get_package_info(const std::string &pkg_name);
std::string get_installed_package_files(const std::string &pkg_nevra);
std::string get_package_deps(const std::string &pkg_nevra);
std::string get_package_changelog(const std::string &pkg_nevra);
bool install_packages(const std::vector<std::string> &pkg_names, std::string &error_out);
bool remove_packages(const std::vector<std::string> &pkg_names, std::string &error_out);
bool reinstall_packages(const std::vector<std::string> &pkg_names, std::string &error_out);
// Resolve the pending transaction and summarize the final package changes for UI review.
bool preview_transaction(const std::vector<std::string> &install_nevras,
                         const std::vector<std::string> &remove_nevras,
                         const std::vector<std::string> &reinstall_nevras,
                         TransactionPreview &preview,
                         std::string &error_out);
bool apply_transaction(const std::vector<std::string> &install_nevras,
                       const std::vector<std::string> &remove_nevras,
                       const std::vector<std::string> &reinstall_nevras,
                       std::string &error_out,
                       const TransactionProgressCallback &progress_cb = {});

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
