// src/base_manager.hpp
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>

#include <libdnf5/base/base.hpp>

// -----------------------------------------------------------------------------
// Lock guard helpers for thread-safe Base access
// These small classes automatically hold a shared (read) or unique (write)
// lock on the BaseManager mutex for the duration of a backend operation.
// -----------------------------------------------------------------------------
class BaseGuard {
  public:
  // -----------------------------------------------------------------------------
  // BaseGuard
  // -----------------------------------------------------------------------------
  explicit BaseGuard(std::shared_lock<std::shared_mutex> &&l)
      : lock(std::move(l))
  {
  }

  private:
  std::shared_lock<std::shared_mutex> lock;
};

class BaseWriteGuard {
  public:
  // -----------------------------------------------------------------------------
  // BaseWriteGuard
  // -----------------------------------------------------------------------------
  explicit BaseWriteGuard(std::unique_lock<std::shared_mutex> &&l)
      : lock(std::move(l))
  {
  }

  private:
  std::unique_lock<std::shared_mutex> lock;
};

// -----------------------------------------------------------------------------
// Read access bundle (Base reference + guard + generation snapshot)
// -----------------------------------------------------------------------------
struct BaseRead {
  libdnf5::Base &base;
  BaseGuard guard;
  uint64_t generation;
};

// Result of one repository rebuild attempt.
// LIVE_METADATA means a normal online refresh succeeded.
// CACHED_METADATA means live refresh failed but cached repo metadata loaded.
// INSTALLED_ONLY means both repo-backed paths failed and only the local rpmdb remains.
enum class BaseRepoState {
  LIVE_METADATA,
  CACHED_METADATA,
  INSTALLED_ONLY,
};

// -----------------------------------------------------------------------------
// BaseManager
// Provides cached access to a libdnf5::Base instance with safe locking
// -----------------------------------------------------------------------------
class BaseManager {
  public:
  // -----------------------------------------------------------------------------
  // instance
  // -----------------------------------------------------------------------------
  static BaseManager &instance();

  // -----------------------------------------------------------------------------
  // Thread-safe guarded accessors
  // Each accessor returns a reference to Base plus a guard object that keeps
  // the appropriate mutex lock alive until the guard goes out of scope.
  // -----------------------------------------------------------------------------
  BaseRead acquire_read();
  // -----------------------------------------------------------------------------
  // acquire_write
  // -----------------------------------------------------------------------------
  std::pair<libdnf5::Base &, BaseWriteGuard> acquire_write();

  // -----------------------------------------------------------------------------
  // current_generation
  // -----------------------------------------------------------------------------
  uint64_t current_generation() const
  {
    return generation.load(std::memory_order_relaxed);
  }

  // -----------------------------------------------------------------------------
  // current_repo_state
  // -----------------------------------------------------------------------------
  BaseRepoState current_repo_state() const;

  // -----------------------------------------------------------------------------
  // Force rebuild
  // -----------------------------------------------------------------------------
  BaseRepoState rebuild();
  // -----------------------------------------------------------------------------
  // rebuild_system_only
  // -----------------------------------------------------------------------------
  void rebuild_system_only();
  // -----------------------------------------------------------------------------
  // ensure_system_only_initialized_if_needed
  // -----------------------------------------------------------------------------
  void ensure_system_only_initialized_if_needed();

#ifdef DNFUI_BUILD_TESTS
  // -----------------------------------------------------------------------------
  // reset_for_tests
  // -----------------------------------------------------------------------------
  void reset_for_tests();
#endif

  private:
  // -----------------------------------------------------------------------------
  // BaseManager
  // -----------------------------------------------------------------------------
  BaseManager() = default;
  // -----------------------------------------------------------------------------
  // BaseManager
  // -----------------------------------------------------------------------------
  BaseManager(const BaseManager &) = delete;
  // -----------------------------------------------------------------------------
  // operator=
  // -----------------------------------------------------------------------------
  BaseManager &operator=(const BaseManager &) = delete;

  // -----------------------------------------------------------------------------
  // build_initialized_system_only_base
  // -----------------------------------------------------------------------------
  std::shared_ptr<libdnf5::Base> build_initialized_system_only_base();
  // -----------------------------------------------------------------------------
  // ensure_base_initialized
  // -----------------------------------------------------------------------------
  void ensure_base_initialized();

  std::shared_ptr<libdnf5::Base> base_ptr;
  BaseRepoState repo_state = BaseRepoState::LIVE_METADATA;

  std::atomic<uint64_t> generation { 0 };

  // Shared mutex allows many readers but only one writer
  mutable std::shared_mutex base_mutex;
};

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
