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
#include <atomic>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <memory>
#include <set>
#include <sstream>
#include <unistd.h>

#include <libdnf5/base/base.hpp>
#include <libdnf5/base/goal.hpp>
#include <libdnf5/base/transaction.hpp>
#include <libdnf5/base/transaction_package.hpp>
#include <libdnf5/repo/download_callbacks.hpp>
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
// Helper: Convert a libdnf5 package to the structured UI row model
// -----------------------------------------------------------------------------
static PackageRow
make_package_row(const libdnf5::rpm::Package &pkg)
{
  PackageRow row;
  row.nevra = pkg.get_nevra();
  row.name = pkg.get_name();
  row.version = pkg.get_version();
  row.release = pkg.get_release();
  row.arch = pkg.get_arch();
  row.repo = pkg.get_repo_id();
  row.summary = pkg.get_summary();

  if (row.summary.empty()) {
    row.summary = "(no summary)";
  }

  return row;
}

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
  auto [base, guard, generation] = BaseManager::instance().acquire_read();
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
std::vector<PackageRow>
get_installed_package_rows()
{
  std::vector<PackageRow> packages;

  auto [base, guard, generation] = BaseManager::instance().acquire_read();
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
    packages.push_back(make_package_row(pkg));
  }

  return packages;
}

// -----------------------------------------------------------------------------
// Compatibility wrapper: Installed packages as NEVRA strings
// Preserves the original string-based API for callers and tests that do not yet
// consume the structured PackageRow model.
// -----------------------------------------------------------------------------
std::vector<std::string>
get_installed_packages()
{
  std::vector<PackageRow> rows = get_installed_package_rows();
  std::vector<std::string> packages;
  packages.reserve(rows.size());

  for (const auto &row : rows) {
    packages.push_back(row.nevra);
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
std::vector<PackageRow>
search_available_package_rows(const std::string &pattern)
{
  std::vector<PackageRow> packages;

  auto [base, guard, generation] = BaseManager::instance().acquire_read();
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
          packages.push_back(make_package_row(pkg));
        }
      } else {
        if (desc.find(pattern_lower) != std::string::npos || name.find(pattern_lower) != std::string::npos) {
          packages.push_back(make_package_row(pkg));
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
      packages.push_back(make_package_row(pkg));
    }
  }

  return packages;
}

// -----------------------------------------------------------------------------
// Compatibility wrapper: Search results as NEVRA strings
// Preserves the original string-based API for callers and tests that still expect
// raw identifiers while the UI migrates to PackageRow.
// -----------------------------------------------------------------------------
std::vector<std::string>
search_available_packages(const std::string &pattern)
{
  std::vector<PackageRow> rows = search_available_package_rows(pattern);
  std::vector<std::string> packages;
  packages.reserve(rows.size());

  for (const auto &row : rows) {
    packages.push_back(row.nevra);
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
get_package_info(const std::string &pkg_nevra)
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

  std::ostringstream oss;
  oss << "Name: " << pkg.get_name() << "\n"
      << "Package ID: " << pkg.get_nevra() << "\n"
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
  auto [base, guard, generation] = BaseManager::instance().acquire_read();
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
  auto [base, guard, generation] = BaseManager::instance().acquire_read();
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
// Helper: Retrieve package changelog entries
// Returns formatted changelog text or a friendly message if none available
// -----------------------------------------------------------------------------
std::string
get_package_changelog(const std::string &pkg_nevra)
{
  auto [base, guard, generation] = BaseManager::instance().acquire_read();
  libdnf5::rpm::PackageQuery query(base);

  query.filter_nevra(pkg_nevra);

  if (query.empty()) {
    return "No changelog available.";
  }

  auto pkg = *query.begin();

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
// Transaction helpers
// -----------------------------------------------------------------------------
bool
install_packages(const std::vector<std::string> &pkg_specs, std::string &error_out)
{
  return apply_transaction(pkg_specs, {}, error_out);
}

bool
remove_packages(const std::vector<std::string> &pkg_specs, std::string &error_out)
{
  return apply_transaction({}, pkg_specs, error_out);
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
// libdnf5 rpm transaction callbacks for the streaming transaction popup
// -----------------------------------------------------------------------------
class StreamingTransactionCallbacks final : public libdnf5::rpm::TransactionCallbacks {
  public:
  explicit StreamingTransactionCallbacks(TransactionProgressCallback progress_cb)
      : progress_cb(std::move(progress_cb))
  {
  }

  void before_begin(uint64_t total) override
  {
    emit_progress_line(progress_cb,
                       "Running RPM transaction for " + std::to_string(total) + " package item" +
                           (total == 1 ? "." : "s."));
  }

  void after_complete(bool success) override
  {
    emit_progress_line(progress_cb, success ? "RPM transaction completed." : "RPM transaction reported an error.");
  }

  void transaction_start(uint64_t total) override
  {
    emit_progress_line(progress_cb,
                       "Preparing transaction metadata for " + std::to_string(total) + " package item" +
                           (total == 1 ? "." : "s."));
  }

  void transaction_stop(uint64_t) override
  {
    emit_progress_line(progress_cb, "Preparation phase finished.");
  }

  void verify_start(uint64_t total) override
  {
    emit_progress_line(progress_cb,
                       "Verifying " + std::to_string(total) + " package signature" + (total == 1 ? "." : "s."));
  }

  void verify_stop(uint64_t) override
  {
    emit_progress_line(progress_cb, "Verification phase finished.");
  }

  void elem_progress(const libdnf5::base::TransactionPackage &item, uint64_t amount, uint64_t total) override
  {
    if (amount == last_reported_item) {
      return;
    }

    last_reported_item = amount;
    emit_progress_line(progress_cb,
                       "Processing " + std::to_string(amount + 1) + " of " + std::to_string(total) + ": " +
                           transaction_action_label(item.get_action()) + " " + transaction_package_label(item));
  }

  void install_start(const libdnf5::base::TransactionPackage &item, uint64_t) override
  {
    emit_progress_line(progress_cb, "Installing: " + transaction_package_label(item));
  }

  void install_stop(const libdnf5::base::TransactionPackage &item, uint64_t, uint64_t) override
  {
    emit_progress_line(progress_cb, "Installed: " + transaction_package_label(item));
  }

  void uninstall_start(const libdnf5::base::TransactionPackage &item, uint64_t) override
  {
    emit_progress_line(progress_cb, "Removing: " + transaction_package_label(item));
  }

  void uninstall_stop(const libdnf5::base::TransactionPackage &item, uint64_t, uint64_t) override
  {
    emit_progress_line(progress_cb, "Removed: " + transaction_package_label(item));
  }

  void unpack_error(const libdnf5::base::TransactionPackage &item) override
  {
    emit_progress_line(progress_cb, "Unpack failed: " + transaction_package_label(item));
  }

  void cpio_error(const libdnf5::base::TransactionPackage &item) override
  {
    emit_progress_line(progress_cb, "Payload extraction failed: " + transaction_package_label(item));
  }

  void script_start(const libdnf5::base::TransactionPackage *item, libdnf5::rpm::Nevra nevra, ScriptType type) override
  {
    emit_progress_line(progress_cb,
                       "Running " + std::string(script_type_to_string(type)) + " for " + script_owner(item, nevra));
  }

  void script_stop(const libdnf5::base::TransactionPackage *item,
                   libdnf5::rpm::Nevra nevra,
                   ScriptType type,
                   uint64_t return_code) override
  {
    if (return_code == 0) {
      return;
    }

    emit_progress_line(progress_cb,
                       std::string(script_type_to_string(type)) + " finished for " + script_owner(item, nevra) +
                           " with exit code " + std::to_string(return_code) + ".");
  }

  void script_error(const libdnf5::base::TransactionPackage *item,
                    libdnf5::rpm::Nevra nevra,
                    ScriptType type,
                    uint64_t return_code) override
  {
    emit_progress_line(progress_cb,
                       "Script failed: " + std::string(script_type_to_string(type)) + " for " +
                           script_owner(item, nevra) + " (exit code " + std::to_string(return_code) + ")");
  }

  private:
  static std::string script_owner(const libdnf5::base::TransactionPackage *item, const libdnf5::rpm::Nevra &nevra)
  {
    if (item) {
      return transaction_package_label(*item);
    }

    return libdnf5::rpm::to_nevra_string(nevra);
  }

  TransactionProgressCallback progress_cb;
  uint64_t last_reported_item = std::numeric_limits<uint64_t>::max();
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

bool
apply_transaction(const std::vector<std::string> &install_nevras,
                  const std::vector<std::string> &remove_nevras,
                  std::string &error_out,
                  const TransactionProgressCallback &progress_cb)
{
  error_out.clear();

  // FIXME: Replace with Polkit:
  if (geteuid() != 0) {
    error_out = "Must be run as root to perform transactions.";
    return false;
  }

  if (install_nevras.empty() && remove_nevras.empty()) {
    error_out = "No packages specified in transaction.";
    return false;
  }

  try {
    // Exclusive access to shared libdnf Base for transactional changes
    auto [base, guard] = BaseManager::instance().acquire_write();

    libdnf5::Goal goal(base);

    emit_progress_line(progress_cb, "Resolving dependency changes...");

    // NOTE: We pass package "specs" (currently NEVRA strings from the UI list).

    for (const auto &spec : install_nevras) {
      goal.add_rpm_install(spec);
    }

    for (const auto &spec : remove_nevras) {
      goal.add_rpm_remove(spec);
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
          << "Remove specs: " << format_specs(remove_nevras) << "\n";
      error_out = oss.str();
      emit_progress_block(progress_cb, error_out);
      return false;
    }

    emit_progress_line(progress_cb,
                       "Resolved " + std::to_string(transaction.get_transaction_packages_count()) + " package item" +
                           (transaction.get_transaction_packages_count() == 1 ? "." : "s."));

    for (const auto &item : transaction.get_transaction_packages()) {
      emit_progress_line(progress_cb,
                         transaction_action_label(item.get_action()) + ": " + transaction_package_label(item));
    }

    base.set_download_callbacks(std::make_unique<StreamingDownloadCallbacks>(progress_cb));
    DownloadCallbacksReset download_callbacks_reset(base);
    emit_progress_line(progress_cb, "Starting package downloads...");
    transaction.download();
    emit_progress_line(progress_cb, "Package downloads finished.");

    transaction.set_callbacks(std::make_unique<StreamingTransactionCallbacks>(progress_cb));

    auto run_result = transaction.run();
    if (run_result != libdnf5::base::Transaction::TransactionRunResult::SUCCESS) {
      std::ostringstream oss;
      oss << "Transaction failed (code " << static_cast<int>(run_result) << ").\n";

      for (const auto &msg : transaction.get_transaction_problems()) {
        oss << "  " << msg << "\n";
      }

      auto rpm_messages = transaction.get_rpm_messages();
      if (!rpm_messages.empty()) {
        oss << "RPM messages:\n";
        for (const auto &msg : rpm_messages) {
          oss << "  " << msg << "\n";
        }
      }

      std::string script_output = transaction.get_last_script_output();
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
