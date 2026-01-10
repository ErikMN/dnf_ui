// -----------------------------------------------------------------------------
// src/base_manager.cpp
// BaseManager: Provides cached access to a libdnf5::Base instance
// - Ensures thread-safe creation and reuse of libdnf5 Base objects
// - Supports manual rebuilds when repositories are refreshed
// -----------------------------------------------------------------------------
#include "base_manager.hpp"

#include <iostream>

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
  }

  // Re-acquire shared lock for the returned guard
  std::shared_lock<std::shared_mutex> shared(base_mutex);
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

  // Bump generation epoch so in-flight async UI tasks can detect the rebuild
  // and drop stale results produced against the previous Base instance.
  generation.fetch_add(1, std::memory_order_relaxed);

  // Clear global cached Base instance to force fresh creation
  base_ptr.reset();

  // Build a new Base and reload all repository data
  ensure_base_initialized();
}

// -----------------------------------------------------------------------------
// Internal helper: ensure_base_initialized()
// Called under unique_lock to (re)create base if missing or expired
// -----------------------------------------------------------------------------
void
BaseManager::ensure_base_initialized()
{
  // Create and fully initialize a new libdnf5::Base
  auto base = std::make_shared<libdnf5::Base>();
  base->load_config();
  base->setup();

  // Load system repositories
  auto repo_sack = base->get_repo_sack();
  repo_sack->create_repos_from_system_configuration();
  // Force load all enabled repos; skip disabled safely
  try {
    repo_sack->load_repos();
  } catch (const std::exception &e) {
    std::cerr << "Warning: repo load failed: " << e.what() << std::endl;
    throw;
  }

  // Make this new Base the shared instance used by the rest of the application
  base_ptr = base;
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
