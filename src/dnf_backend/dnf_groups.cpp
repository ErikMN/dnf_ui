// -----------------------------------------------------------------------------
// dnf_groups.cpp
// DNF package group queries
//
// Translates libdnf5 comps groups into the small backend models used by the GTK
// layer. Package rows returned for a group use the same merged row model as the
// main browse view.
// -----------------------------------------------------------------------------
#include "dnf_backend/dnf_internal.hpp"

#include "base_manager.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <libdnf5/comps/group/query.hpp>
#include <libdnf5/rpm/package_query.hpp>

namespace {

struct OrderedPackageGroup {
  PackageGroup group;
  int order = 0;
};

// -----------------------------------------------------------------------------
// Keep the newest row for one package name and architecture tuple.
// -----------------------------------------------------------------------------
static void
remember_newest_group_row(std::map<std::string, PackageRow> &rows_by_name_arch, const PackageRow &row)
{
  auto [it, inserted] = rows_by_name_arch.emplace(row.name_arch_key(), row);
  if (!inserted && libdnf5::rpm::evrcmp(row, it->second) > 0) {
    it->second = row;
  }
}

// -----------------------------------------------------------------------------
// Return distinct package names attached to one comps group.
// -----------------------------------------------------------------------------
static std::vector<std::string>
group_package_names(libdnf5::comps::Group &group)
{
  std::set<std::string> names;
  for (auto package : group.get_packages()) {
    std::string name = package.get_name();
    if (!name.empty()) {
      names.insert(std::move(name));
    }
  }

  return std::vector<std::string>(names.begin(), names.end());
}

// -----------------------------------------------------------------------------
// Collect newest available rows for the package names attached to a group.
// -----------------------------------------------------------------------------
static std::map<std::string, PackageRow>
collect_available_group_rows_by_name_arch(libdnf5::Base &base,
                                          const std::vector<std::string> &package_names,
                                          GCancellable *cancellable)
{
  std::map<std::string, PackageRow> rows_by_name_arch;
  if (package_names.empty()) {
    return rows_by_name_arch;
  }

  libdnf5::rpm::PackageQuery query(base);
  query.filter_available();
  query.filter_name(package_names);
  query.filter_latest_evr();

  for (auto pkg : query) {
    if (dnf_backend_internal::package_query_cancelled(cancellable)) {
      rows_by_name_arch.clear();
      return rows_by_name_arch;
    }

    remember_newest_group_row(rows_by_name_arch, dnf_backend_internal::make_package_row(pkg));
  }

  return rows_by_name_arch;
}

// -----------------------------------------------------------------------------
// Collect installed rows for the package names attached to a group.
// -----------------------------------------------------------------------------
static dnf_backend_internal::InstalledQueryResult
collect_installed_group_rows(libdnf5::Base &base,
                             const std::vector<std::string> &package_names,
                             GCancellable *cancellable)
{
  dnf_backend_internal::InstalledQueryResult result;
  if (package_names.empty()) {
    return result;
  }

  libdnf5::rpm::PackageQuery query(base);
  query.filter_installed();
  query.filter_name(package_names);

  for (auto pkg : query) {
    if (dnf_backend_internal::package_query_cancelled(cancellable)) {
      result.rows.clear();
      result.nevras.clear();
      result.rows_by_name_arch.clear();
      return result;
    }

    PackageRow row = dnf_backend_internal::make_package_row(pkg);
    result.nevras.insert(row.nevra);
    remember_newest_group_row(result.rows_by_name_arch, row);
    result.rows.push_back(std::move(row));
  }

  return result;
}

} // namespace

// -----------------------------------------------------------------------------
// Return user-visible DNF package groups from repository comps metadata.
// -----------------------------------------------------------------------------
std::vector<PackageGroup>
dnf_backend_get_package_groups_interruptible(GCancellable *cancellable)
{
  std::vector<OrderedPackageGroup> ordered_groups;

  if (dnf_backend_internal::package_query_cancelled(cancellable)) {
    return {};
  }

  auto [base, guard, generation] = BaseManager::instance().acquire_read();
  libdnf5::comps::GroupQuery query(base);
  query.filter_uservisible(true);

  for (auto group : query) {
    if (dnf_backend_internal::package_query_cancelled(cancellable)) {
      return {};
    }

    PackageGroup package_group;
    package_group.id = group.get_groupid();
    package_group.name = group.get_translated_name();
    package_group.description = group.get_translated_description();
    package_group.package_count = group_package_names(group).size();

    if (package_group.id.empty()) {
      continue;
    }
    if (package_group.name.empty()) {
      package_group.name = package_group.id;
    }

    OrderedPackageGroup ordered_group;
    ordered_group.group = std::move(package_group);
    ordered_group.order = group.get_order_int();
    ordered_groups.push_back(std::move(ordered_group));
  }

  std::sort(ordered_groups.begin(), ordered_groups.end(), [](const auto &left, const auto &right) {
    if (left.order != right.order) {
      return left.order < right.order;
    }
    if (left.group.name != right.group.name) {
      return left.group.name < right.group.name;
    }
    return left.group.id < right.group.id;
  });

  std::vector<PackageGroup> groups;
  groups.reserve(ordered_groups.size());
  for (auto &ordered_group : ordered_groups) {
    groups.push_back(std::move(ordered_group.group));
  }

  return groups;
}

// -----------------------------------------------------------------------------
// Return the merged package rows belonging to one DNF package group.
// -----------------------------------------------------------------------------
std::vector<PackageRow>
dnf_backend_get_package_group_package_rows_interruptible(const std::string &group_id, GCancellable *cancellable)
{
  if (group_id.empty() || dnf_backend_internal::package_query_cancelled(cancellable)) {
    return {};
  }

  auto [base, guard, generation] = BaseManager::instance().acquire_read();

  libdnf5::comps::GroupQuery group_query(base);
  group_query.filter_groupid(group_id);

  std::vector<std::string> package_names;
  for (auto group : group_query) {
    package_names = group_package_names(group);
    break;
  }

  if (dnf_backend_internal::package_query_cancelled(cancellable) || package_names.empty()) {
    return {};
  }

  auto available_rows = collect_available_group_rows_by_name_arch(base, package_names, cancellable);
  if (dnf_backend_internal::package_query_cancelled(cancellable)) {
    return {};
  }

  dnf_backend_internal::InstalledQueryResult installed = collect_installed_group_rows(base, package_names, cancellable);
  if (dnf_backend_internal::package_query_cancelled(cancellable)) {
    return {};
  }

  return dnf_backend_internal::visible_rows_from_maps(std::move(available_rows),
                                                      std::move(installed.rows_by_name_arch));
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
