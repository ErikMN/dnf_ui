// -----------------------------------------------------------------------------
// libdnf5 backend helpers
// -----------------------------------------------------------------------------
#include "dnf_backend.hpp"
#include <algorithm>
#include <iostream>
#include <set>
#include <sstream>

#include <libdnf5/base/base.hpp>
#include <libdnf5/rpm/package_query.hpp>

std::set<std::string> g_installed_names;

bool g_search_in_description = false;
bool g_exact_match = false;

// -----------------------------------------------------------------------------
// Helper: create fresh libdnf5::Base (thread-safe per-thread)
// -----------------------------------------------------------------------------
std::unique_ptr<libdnf5::Base>
create_fresh_base()
{
  auto base = std::make_unique<libdnf5::Base>();
  base->load_config();
  base->setup();
  auto repo_sack = base->get_repo_sack();
  repo_sack->create_repos_from_system_configuration();
  repo_sack->load_repos();

  return base;
}

// -----------------------------------------------------------------------------
// Helper: Query installed packages via libdnf5
// -----------------------------------------------------------------------------
std::vector<std::string>
get_installed_packages()
{
  std::vector<std::string> packages;

  auto base = create_fresh_base();

  libdnf5::rpm::PackageQuery query(*base);
  query.filter_installed();
  for (auto pkg : query) {
    g_installed_names.insert(pkg.get_name());
    packages.push_back(pkg.get_name() + "-" + pkg.get_evr());
  }

  return packages;
}

// -----------------------------------------------------------------------------
// Helper: Search available packages by substring or exact match
// -----------------------------------------------------------------------------

std::vector<std::string>
search_available_packages(const std::string &pattern)
{
  std::vector<std::string> packages;

  auto base = create_fresh_base();

  libdnf5::rpm::PackageQuery query(*base);
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
          packages.push_back(pkg.get_name() + "-" + pkg.get_evr());
        }
      } else {
        if (desc.find(pattern_lower) != std::string::npos || name.find(pattern_lower) != std::string::npos) {
          packages.push_back(pkg.get_name() + "-" + pkg.get_evr());
        }
      }
    }
  } else {
    if (g_exact_match) {
      query.filter_name(pattern, libdnf5::sack::QueryCmp::EQ);
    } else {
      query.filter_name(pattern, libdnf5::sack::QueryCmp::CONTAINS);
    }

    for (auto pkg : query) {
      packages.push_back(pkg.get_name() + "-" + pkg.get_evr());
    }
  }

  return packages;
}

// -----------------------------------------------------------------------------
// Helper: get detailed info about a single package
// -----------------------------------------------------------------------------
std::string
get_package_info(const std::string &pkg_name)
{
  auto base = create_fresh_base();

  libdnf5::rpm::PackageQuery query(*base);
  query.filter_name(pkg_name);

  if (query.empty()) {
    return "No details found for " + pkg_name;
  }

  // Prefer installed package if available
  libdnf5::rpm::PackageQuery installed(query);
  installed.filter_installed();

  libdnf5::rpm::PackageQuery best_candidate = installed.empty() ? query : installed;
  // choose latest version if multiple
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
