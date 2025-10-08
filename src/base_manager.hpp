// src/base_manager.hpp
#pragma once

#include <libdnf5/base/base.hpp>
#include <memory>
#include <mutex>
#include <thread>
#include <chrono>

class BaseManager {
  public:
  static BaseManager &instance();

  // Returns the thread-local base, creating if needed
  libdnf5::Base &get_base();

  // Force rebuild
  void rebuild();

  private:
  BaseManager() = default;
  BaseManager(const BaseManager &) = delete;
  BaseManager &operator=(const BaseManager &) = delete;

  std::mutex rebuild_mutex;
  std::shared_ptr<libdnf5::Base> global_base;
  std::chrono::steady_clock::time_point last_refresh;
};
