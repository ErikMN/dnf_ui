// src/base_manager.hpp
#pragma once

#include <chrono>
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
  explicit BaseGuard(std::shared_mutex &m)
      : lock(m)
  {
  }

  private:
  std::shared_lock<std::shared_mutex> lock;
};

class BaseWriteGuard {
  public:
  explicit BaseWriteGuard(std::shared_mutex &m)
      : lock(m)
  {
  }

  private:
  std::unique_lock<std::shared_mutex> lock;
};

// -----------------------------------------------------------------------------
// BaseManager
// Provides cached access to a libdnf5::Base instance with safe locking
// -----------------------------------------------------------------------------
class BaseManager {
  public:
  static BaseManager &instance();

  // Thread-safe guarded accessors
  // Each accessor returns a reference to Base plus a guard object that keeps
  // the appropriate mutex lock alive until the guard goes out of scope.
  std::pair<libdnf5::Base &, BaseGuard> acquire_read();
  std::pair<libdnf5::Base &, BaseWriteGuard> acquire_write();

  // Force rebuild
  void rebuild();

  private:
  BaseManager() = default;
  BaseManager(const BaseManager &) = delete;
  BaseManager &operator=(const BaseManager &) = delete;

  void ensure_base_initialized();

  std::shared_ptr<libdnf5::Base> base_ptr;
  std::chrono::steady_clock::time_point last_refresh;

  // Shared mutex allows many readers but only one writer
  mutable std::shared_mutex base_mutex;
};
