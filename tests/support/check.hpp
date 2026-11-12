#pragma once
#include <cstdio>
#include <cstdlib>
#define CHECK(...)                                                           \
  do {                                                                       \
    if (!(__VA_ARGS__)) {                                                    \
      std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__); \
      std::abort();                                                          \
    }                                                                        \
  } while (false)
