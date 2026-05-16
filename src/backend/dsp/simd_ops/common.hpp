#pragma once
// Shared SIMD detection macros and intrinsic includes for the
// simd_ops/ kernel headers. Per-kernel files include this and
// then guard their AVX2/NEON specializations with SIGNALSCOPE_HAS_*.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

#if defined(__AVX2__)
  #include <immintrin.h>
  #define SIGNALSCOPE_HAS_AVX2 1
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
  #include <arm_neon.h>
  #define SIGNALSCOPE_HAS_NEON 1
#endif
