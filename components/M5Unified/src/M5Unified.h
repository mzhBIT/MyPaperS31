#pragma once

#ifdef __cplusplus

#if __has_include(<sdkconfig.h>)
#include <sdkconfig.h>
#endif
#if CONFIG_IDF_TARGET_ESP32S31
#include "M5UnifiedS31.hpp"
#else
#include "M5Unified.hpp"
#endif

#else

#error M5Unified requires a C++ compiler, please change file extension to .cc or .cpp

#endif
