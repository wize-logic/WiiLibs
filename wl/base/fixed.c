// SPDX-License-Identifier: MIT
#include "wl/base/fixed.h"

static WlFx32 saturate(int64_t v) {
    if (v > INT32_MAX) {
        return INT32_MAX;
    }
    if (v < INT32_MIN) {
        return INT32_MIN;
    }
    return (WlFx32)v;
}

WlFx32 wl_fx_from_int(int32_t v) {
    return saturate((int64_t)v * WL_FX32_ONE);
}

int32_t wl_fx_to_int(WlFx32 v) {
    // Floor division by 4096 without relying on the sign-extension of >> for negatives.
    int32_t q = v / WL_FX32_ONE;
    if (v % WL_FX32_ONE < 0) {
        q--;
    }
    return q;
}

// Divides n by d > 0 rounding to nearest, ties away from zero.
static int64_t div_round(int64_t n, int64_t d) {
    int64_t half = d / 2;
    return n >= 0 ? (n + half) / d : -((-n + half) / d);
}

int32_t wl_fx_to_int_round(WlFx32 v) {
    return (int32_t)div_round(v, WL_FX32_ONE);
}

WlFx32 wl_fx_mul(WlFx32 a, WlFx32 b) {
    return saturate(div_round((int64_t)a * b, WL_FX32_ONE));
}

WlFx32 wl_fx_div(WlFx32 a, WlFx32 b) {
    if (b == 0) {
        return a > 0 ? INT32_MAX : (a < 0 ? INT32_MIN : 0);
    }
    int64_t n = (int64_t)a * WL_FX32_ONE;
    int64_t d = b;
    if (d < 0) {
        n = -n;
        d = -d;
    }
    return saturate(div_round(n, d));
}
