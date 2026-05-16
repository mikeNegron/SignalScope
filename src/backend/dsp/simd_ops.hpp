#pragma once
// Vectorized free functions for the DSPPipeline per-bin hot loops.
// The implementations live in per-kernel headers under simd_ops/;
// this umbrella file pulls them in so existing call sites that
// `#include "dsp/simd_ops.hpp"` continue to work unchanged.
//
// Each kernel header defines a scalar reference (always compiled)
// plus AVX2 and NEON specializations (selected at compile time via
// SIGNALSCOPE_HAS_AVX2 / SIGNALSCOPE_HAS_NEON) and a public dispatch
// entry point. The scalar reference is the ground truth the test
// suite pins both paths against.

#include "dsp/simd_ops/magnitude_db.hpp"
#include "dsp/simd_ops/window_apply.hpp"
#include "dsp/simd_ops/max_inplace.hpp"
#include "dsp/simd_ops/min_max.hpp"
