// src/base_manager.hpp
#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <shared_mutex>

#include <libdnf5/base/base.hpp>

class BaseManager {
  public:
  static BaseManager &instance();

  // Returns the shared base instance, thread-safe for concurrent access
  libdnf5::Base &get_base();

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
