#pragma once
#include <cstdlib>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
inline void *heap_caps_malloc(size_t size, unsigned) { return std::malloc(size); }
inline void heap_caps_free(void *ptr) { std::free(ptr); }
