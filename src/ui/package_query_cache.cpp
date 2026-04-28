// -----------------------------------------------------------------------------
// src/ui/package_query_cache.cpp
// Package query result cache
// Keeps cached search result storage and invalidation rules separate from the
// package query controller.
// -----------------------------------------------------------------------------
#include "package_query_cache.hpp"

#include <map>
#include <mutex>

// Cache one visible result set per search term and search option combination.
// Entries are tied to the BaseManager generation that produced them so a Base
// rebuild cannot serve outdated package metadata back into the UI.
struct CachedSearchResults {
  uint64_t generation;
  std::vector<PackageRow> packages;
};

static std::map<std::string, CachedSearchResults> g_search_cache;
static std::mutex g_cache_mutex; // Protects g_search_cache

// -----------------------------------------------------------------------------
// Build a unique cache key from search options and the search term.
// -----------------------------------------------------------------------------
std::string
package_query_cache_key_for(const std::string &term)
{
  const DnfBackendSearchOptions options = dnf_backend_get_search_options();
  std::string key = (options.search_in_description ? "desc:" : "name:");
  key += (options.exact_match ? "exact:" : "contains:");
  key += term;

  return key;
}

// -----------------------------------------------------------------------------
// Clear cached search results.
// Used by the Clear Cache button, repository refresh, and transaction refresh.
// -----------------------------------------------------------------------------
void
package_query_cache_clear()
{
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  g_search_cache.clear();
}

// -----------------------------------------------------------------------------
// Look up cached rows before starting a new backend query.
// Reuse only results produced from the current Base generation so refreshes
// and transaction rebuilds cannot surface outdated package metadata.
// -----------------------------------------------------------------------------
bool
package_query_cache_lookup(const std::string &key, uint64_t generation, std::vector<PackageRow> &out_packages)
{
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  auto it = g_search_cache.find(key);
  if (it == g_search_cache.end()) {
    return false;
  }

  if (it->second.generation != generation) {
    g_search_cache.erase(it);
    return false;
  }

  out_packages = it->second.packages;
  return true;
}

// -----------------------------------------------------------------------------
// Save rows so the same search can be shown faster next time.
// Search results are only reusable while the backend Base generation stays
// the same, otherwise repo state may have changed underneath the cache.
// -----------------------------------------------------------------------------
void
package_query_cache_store(const std::string &key, uint64_t generation, const std::vector<PackageRow> &packages)
{
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  g_search_cache[key] = CachedSearchResults { generation, packages };
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
