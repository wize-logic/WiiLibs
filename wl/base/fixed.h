// SPDX-License-Identifier: MIT
// Helpers for WlFx32, the DS 20.12 fixed-point number (1.0 = 4096; see base.h). Integer
// arithmetic only, so simulations stay deterministic across CPUs. Implementation: wl/base/fixed.c.
//
// Rounding: mul and div round to nearest, ties away from zero. All results saturate to
// [INT32_MIN, INT32_MAX] instead of wrapping. Division by zero saturates by the sign of the
// dividend (0 / 0 = 0); it is not an assert because values can come from external data.
#pragma once

#include "wl/base/base.h"

#define WL_FX32_FRAC_BITS 12
#define WL_FX32_MAX INT32_MAX
#define WL_FX32_MIN INT32_MIN

// Integer to fixed; saturates outside [-524288, 524287].
WlFx32 wl_fx_from_int(int32_t v);
// Fixed to integer, rounding toward negative infinity (floor): -0.5 -> -1, 1.9 -> 1.
int32_t wl_fx_to_int(WlFx32 v);
// Fixed to integer, rounding to nearest, ties away from zero: -0.5 -> -1, 0.5 -> 1, 1.4 -> 1.
int32_t wl_fx_to_int_round(WlFx32 v);

WlFx32 wl_fx_mul(WlFx32 a, WlFx32 b);
WlFx32 wl_fx_div(WlFx32 a, WlFx32 b);
