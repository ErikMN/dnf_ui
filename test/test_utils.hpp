#pragma once

#include <mutex>
#include "dnf_backend.hpp"

inline void
reset_backend_globals()
{
  g_search_in_description = false;
  g_exact_match = false;

  std::lock_guard<std::mutex> lock(g_installed_mutex);
  g_installed_nevras.clear();
  g_installed_names.clear();
}
