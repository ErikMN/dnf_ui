#pragma once

#include <cstdio>

#ifdef DNFUI_DEBUG_TRACE
#define DNFUI_TRACE(...)      \
  do {                        \
    std::printf("[trace] ");  \
    std::printf(__VA_ARGS__); \
    std::printf("\n");        \
    std::fflush(stdout);      \
  } while (0)
#else
#define DNFUI_TRACE(...) \
  do {                   \
  } while (0)
#endif
