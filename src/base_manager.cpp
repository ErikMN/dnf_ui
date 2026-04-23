// -----------------------------------------------------------------------------
// src/base_manager.cpp
// BaseManager: Provides cached access to a libdnf5::Base instance
// - Ensures thread-safe creation and reuse of libdnf5 Base objects
// - Supports manual rebuilds when repositories are refreshed
// -----------------------------------------------------------------------------
#include "base_manager.hpp"
#include "debug_trace.hpp"

#include <libdnf5/conf/const.hpp>
#include <libdnf5/repo/repo.hpp>

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

enum class RepoLoadMode {
  FULL,
  SYSTEM_ONLY,
};

// Build one fully configured Base before any repo metadata is loaded.
static std::shared_ptr<libdnf5::Base>
create_configured_base()
{
  DNFUI_TRACE("BaseManager initialize start");

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

  auto repo_sack = base->get_repo_sack();
  DNFUI_TRACE("BaseManager create repos start");
  repo_sack->create_repos_from_system_configuration();
  DNFUI_TRACE("BaseManager create repos done");

  return base;
}

// Load repository data for one already configured Base. Startup fallback should
// only trigger when this step fails for the full repo set.
static void
load_repo_data(libdnf5::Base &base, RepoLoadMode mode)
{
  auto repo_sack = base.get_repo_sack();

#ifdef DNFUI_BUILD_TESTS
  if (mode == RepoLoadMode::FULL) {
    const char *force_failure = std::getenv("DNFUI_TEST_FORCE_FULL_REPO_LOAD_FAILURE");
    if (force_failure && std::string(force_failure) == "1") {
      throw std::runtime_error("forced full repo load failure");
    }
  }
#endif

  DNFUI_TRACE("BaseManager load repos start");
  if (mode == RepoLoadMode::SYSTEM_ONLY) {
    repo_sack->load_repos(libdnf5::repo::Repo::Type::SYSTEM);
  } else {
    repo_sack->load_repos();
  }
  DNFUI_TRACE("BaseManager load repos done");
}

} // namespace

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
  // last known-good repo-backed Base.
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
  auto base = create_configured_base();
  try {
    load_repo_data(*base, RepoLoadMode::FULL);
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
    auto base = create_configured_base();
    try {
      load_repo_data(*base, RepoLoadMode::FULL);
      DNFUI_TRACE("BaseManager initialize done");
      base_ptr = std::move(base);
    } catch (const std::exception &repo_error) {
      std::cerr << "Warning: repo load failed: " << repo_error.what() << std::endl;
      DNFUI_TRACE("BaseManager load repos failed: %s", repo_error.what());
      std::cerr << "Warning: startup repo load failed, falling back to installed-package-only mode: "
                << repo_error.what() << std::endl;
      DNFUI_TRACE("BaseManager startup full repo load failed, trying system-only fallback: %s", repo_error.what());

      auto fallback_base = create_configured_base();
      try {
        load_repo_data(*fallback_base, RepoLoadMode::SYSTEM_ONLY);
        DNFUI_TRACE("BaseManager initialize done");
        base_ptr = std::move(fallback_base);
      } catch (const std::exception &fallback_error) {
        throw std::runtime_error(
            "DNF backend initialization failed after repo load error: " + std::string(repo_error.what()) +
            "; system-only fallback failed: " + fallback_error.what());
      }
    }
  }
}

#ifdef DNFUI_BUILD_TESTS
void
BaseManager::reset_for_tests()
{
  std::unique_lock<std::shared_mutex> unique(base_mutex);
  base_ptr.reset();
  generation.store(0, std::memory_order_relaxed);
}
#endif

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
