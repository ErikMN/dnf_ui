#pragma once

#include <cstdio>

#ifdef DNFUI_DEBUG_TRACE
#define DNFUI_TRACE(...)               \
  do {                                 \
    std::fprintf(stderr, "[trace] ");  \
    std::fprintf(stderr, __VA_ARGS__); \
    std::fprintf(stderr, "\n");        \
    std::fflush(stderr);               \
  } while (0)
#else
#define DNFUI_TRACE(...) \
  do {                   \
  } while (0)
#endif
