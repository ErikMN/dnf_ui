#pragma once

#include <cstdio>

#ifdef DNF_UI_DEBUG_TRACE
#define DNF_UI_TRACE(...)     \
  do {                        \
    std::printf("[trace] ");  \
    std::printf(__VA_ARGS__); \
    std::printf("\n");        \
    std::fflush(stdout);      \
  } while (0)
#else
#define DNF_UI_TRACE(...) \
  do {                    \
  } while (0)
#endif
