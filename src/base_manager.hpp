// src/base_manager.hpp
#pragma once

#include <atomic>
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
  explicit BaseGuard(std::shared_lock<std::shared_mutex> &&l)
      : lock(std::move(l))
  {
  }

  private:
  std::shared_lock<std::shared_mutex> lock;
};

class BaseWriteGuard {
  public:
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
  BaseRead acquire_read();
  std::pair<libdnf5::Base &, BaseWriteGuard> acquire_write();

  uint64_t current_generation() const
  {
    return generation.load(std::memory_order_relaxed);
  }

  // Force rebuild
  void rebuild();

  private:
  BaseManager() = default;
  BaseManager(const BaseManager &) = delete;
  BaseManager &operator=(const BaseManager &) = delete;

  void ensure_base_initialized();

  std::shared_ptr<libdnf5::Base> base_ptr;

  std::atomic<uint64_t> generation { 0 };

  // Shared mutex allows many readers but only one writer
  mutable std::shared_mutex base_mutex;
};

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
