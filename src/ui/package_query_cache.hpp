// -----------------------------------------------------------------------------
// src/ui/package_query_cache.hpp
// Package query result cache
//
// Owns cached search result storage so the package query controller does not
// need to manage cache keys, generations, or locking directly.
// -----------------------------------------------------------------------------
#pragma once

#include "dnf_backend/dnf_backend.hpp"

#include <cstdint>
#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// Build the cache key from the current search options and search term.
// -----------------------------------------------------------------------------
std::string package_query_cache_key_for(const std::string &term);
// -----------------------------------------------------------------------------
// Clear all cached package query results.
// -----------------------------------------------------------------------------
void package_query_cache_clear();
// -----------------------------------------------------------------------------
// Look up cached package rows for one key and generation.
// -----------------------------------------------------------------------------
bool package_query_cache_lookup(const std::string &key, uint64_t generation, std::vector<PackageRow> &out_packages);
// -----------------------------------------------------------------------------
// Store package rows for one key and generation.
// -----------------------------------------------------------------------------
void package_query_cache_store(const std::string &key, uint64_t generation, const std::vector<PackageRow> &packages);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
