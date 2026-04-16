// -----------------------------------------------------------------------------
// src/base_manager.cpp
// BaseManager: Provides cached access to a libdnf5::Base instance
// - Ensures thread-safe creation and reuse of libdnf5 Base objects
// - Supports manual rebuilds when repositories are refreshed
// -----------------------------------------------------------------------------
#include "base_manager.hpp"
#include "debug_trace.hpp"

#include <libdnf5/conf/const.hpp>

#include <iostream>
#include <stdexcept>

// -----------------------------------------------------------------------------
// Singleton: BaseManager instance accessor
// -----------------------------------------------------------------------------
BaseManager &
BaseManager::instance()
{
  static BaseManager mgr;
  return mgr;
}

// -----------------------------------------------------------------------------
// Thread-safe read accessor
// -----------------------------------------------------------------------------
BaseRead
BaseManager::acquire_read()
{
  // Fast path: Base already exists
  {
    std::shared_lock<std::shared_mutex> shared(base_mutex);
    if (base_ptr) {
      return { *base_ptr, BaseGuard(std::move(shared)), generation.load(std::memory_order_relaxed) };
    }
  }

  // Initialize under exclusive lock
  {
    std::unique_lock<std::shared_mutex> unique(base_mutex);
    if (!base_ptr) {
      ensure_base_initialized();
    }
    if (!base_ptr) {
      // Never return a null Base reference.
      throw std::runtime_error("DNF backend not initialized (Base is null).");
    }
  }

  // Re-acquire shared lock for the returned guard
  std::shared_lock<std::shared_mutex> shared(base_mutex);
  if (!base_ptr) {
    // base_ptr should not become null while we hold the mutex
    throw std::runtime_error("DNF backend not initialized (Base is null).");
  }
  return { *base_ptr, BaseGuard(std::move(shared)), generation.load(std::memory_order_relaxed) };
}

// -----------------------------------------------------------------------------
// Thread-safe write accessor (exclusive)
// -----------------------------------------------------------------------------
std::pair<libdnf5::Base &, BaseWriteGuard>
BaseManager::acquire_write()
{
  std::unique_lock<std::shared_mutex> write_lock(base_mutex);
  if (!base_ptr) {
    ensure_base_initialized();
  }
  if (!base_ptr) {
    // Never return a null Base reference.
    throw std::runtime_error("DNF backend not initialized (Base is null).");
  }

  return { *base_ptr, BaseWriteGuard(std::move(write_lock)) };
}

// -----------------------------------------------------------------------------
// Force rebuild of cached Base (used when user requests "Refresh Repositories")
// -----------------------------------------------------------------------------
void
BaseManager::rebuild()
{
  // Lock to ensure only one rebuild runs at a time
  std::unique_lock lock(base_mutex);

  // Build the replacement first so a repo-refresh failure does not discard the
  // last known-good Base. This keeps local rpmdb-backed operations available
  // even when repository metadata refresh fails.
  auto rebuilt_base = build_initialized_base();
  if (!rebuilt_base) {
    throw std::runtime_error("Repository rebuild failed (Base is null).");
  }

  base_ptr = rebuilt_base;

  // Publish the generation change only after the new Base is ready so readers
  // never drop their cached results without a replacement snapshot to use.
  generation.fetch_add(1, std::memory_order_relaxed);
}

// -----------------------------------------------------------------------------
// Internal helper: build one fully initialized Base instance without touching
// the shared cache until initialization succeeds.
// -----------------------------------------------------------------------------
std::shared_ptr<libdnf5::Base>
BaseManager::build_initialized_base()
{
  DNFUI_TRACE("BaseManager initialize start");

  // Create and fully initialize a new libdnf5::Base
  auto base = std::make_shared<libdnf5::Base>();
  DNFUI_TRACE("BaseManager load config start");
  base->load_config();
  DNFUI_TRACE("BaseManager load config done");

  // Changelog lookups for available packages need repo "other" metadata.
  base->get_config().get_optional_metadata_types_option().add_item(libdnf5::Option::Priority::RUNTIME,
                                                                   libdnf5::METADATA_TYPE_OTHER);
  DNFUI_TRACE("BaseManager setup start");
  base->setup();
  DNFUI_TRACE("BaseManager setup done");

  // Load system repositories
  auto repo_sack = base->get_repo_sack();
  DNFUI_TRACE("BaseManager create repos start");
  repo_sack->create_repos_from_system_configuration();
  DNFUI_TRACE("BaseManager create repos done");
  // Force load all enabled repos; skip disabled safely
  try {
    DNFUI_TRACE("BaseManager load repos start");
    repo_sack->load_repos();
    DNFUI_TRACE("BaseManager load repos done");
  } catch (const std::exception &e) {
    std::cerr << "Warning: repo load failed: " << e.what() << std::endl;
    DNFUI_TRACE("BaseManager load repos failed: %s", e.what());
    throw;
  }

  DNFUI_TRACE("BaseManager initialize done");
  return base;
}

// -----------------------------------------------------------------------------
// Internal helper: ensure_base_initialized()
// Called under unique_lock to create the shared Base when it does not exist.
// -----------------------------------------------------------------------------
void
BaseManager::ensure_base_initialized()
{
  if (!base_ptr) {
    base_ptr = build_initialized_base();
  }
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
