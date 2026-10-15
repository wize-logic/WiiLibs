// SPDX-License-Identifier: MIT
// The NV2A's raster state as GX takes it (nv2a_pipeline.h): compare functions, blending, culling,
// from the values themselves. Apart from the pipeline so that a renderer that has the values but no
// NV2A state (an Xbox Direct3D device, whose render states are the NV2A's values) takes these
// alone, as OpenCE-Wii's d3d8_gx.c does, its host harness linking this file without the rest of
// wl_xbox. Derived from OpenCE-Wii's d3d8_gx.c (CC0: gx_compare, gx_blend_factor, the cull mode's
// mapping).

#include "wl/xbox/nv2a_pipeline.h"

#include <stddef.h>

#include "wl/xbox/nv2a_regs.h"

WlGxCompare wl_nv2a_gx_compare(uint32_t func) {
    return func >= WL_NV2A_FUNC_NEVER && func <= WL_NV2A_FUNC_ALWAYS
               ? (WlGxCompare)(func - WL_NV2A_FUNC_NEVER)
               : WL_GX_ALWAYS;
}

// a constant factor (SET_BLEND_COLOR's ARGB, NULL unknown), which GX's blender has not: ONE or ZERO,
// exact where the constant is 1 or 0 in every channel it gives (Halo's HUD blends by a white
// constant colour), else the nearer of the two by the channels' mean (unknown: ONE)
static WlGxBlendFactor constant_factor(uint32_t factor, const uint32_t *color, bool *exact) {
    bool alpha = factor == WL_NV2A_BLEND_CONSTANT_ALPHA ||
                 factor == WL_NV2A_BLEND_ONE_MINUS_CONSTANT_ALPHA;
    bool invert = factor == WL_NV2A_BLEND_ONE_MINUS_CONSTANT_COLOR ||
                  factor == WL_NV2A_BLEND_ONE_MINUS_CONSTANT_ALPHA;
    unsigned low, high, sum;

    if (color == NULL) {
        *exact = false;
        return WL_GX_BL_ONE;
    }
    if (alpha) {
        low = high = sum = *color >> 24;
        sum *= 3u;
    } else {
        unsigned r = (*color >> 16) & 0xFFu, g = (*color >> 8) & 0xFFu, b = *color & 0xFFu;

        low = r < g ? (r < b ? r : b) : (g < b ? g : b);
        high = r > g ? (r > b ? r : b) : (g > b ? g : b);
        sum = r + g + b;
    }
    if (invert) {
        unsigned swap = 255u - low;

        low = 255u - high;
        high = swap;
        sum = 765u - sum;
    }
    if (!(high == 0u || low == 255u)) {
        *exact = false;
    }
    return sum >= 383u ? WL_GX_BL_ONE : WL_GX_BL_ZERO;
}

// GX's factor for the NV2A's (GX's colour factors are the other side's, a source's factor of the
// destination colour, a destination's of the source colour)
static WlGxBlendFactor gx_blend_factor(uint32_t factor, bool source, const uint32_t *color,
                                       bool *exact) {
    switch (factor) {
    case WL_NV2A_BLEND_ZERO:
        return WL_GX_BL_ZERO;
    case WL_NV2A_BLEND_ONE:
        return WL_GX_BL_ONE;
    case WL_NV2A_BLEND_SRC_COLOR:
        if (source) {
            *exact = false;
            return WL_GX_BL_ONE;
        }
        return WL_GX_BL_OTHERCLR;
    case WL_NV2A_BLEND_ONE_MINUS_SRC_COLOR:
        if (source) {
            *exact = false;
            return WL_GX_BL_ZERO;
        }
        return WL_GX_BL_INVOTHERCLR;
    case WL_NV2A_BLEND_SRC_ALPHA_SATURATE:
        *exact = false;
        return WL_GX_BL_SRCALPHA;
    case WL_NV2A_BLEND_SRC_ALPHA:
        return WL_GX_BL_SRCALPHA;
    case WL_NV2A_BLEND_ONE_MINUS_SRC_ALPHA:
        return WL_GX_BL_INVSRCALPHA;
    case WL_NV2A_BLEND_DST_ALPHA:
        return WL_GX_BL_DSTALPHA;
    case WL_NV2A_BLEND_ONE_MINUS_DST_ALPHA:
        return WL_GX_BL_INVDSTALPHA;
    case WL_NV2A_BLEND_DST_COLOR:
        if (!source) {
            *exact = false;
            return WL_GX_BL_ONE;
        }
        return WL_GX_BL_OTHERCLR;
    case WL_NV2A_BLEND_ONE_MINUS_DST_COLOR:
        if (!source) {
            *exact = false;
            return WL_GX_BL_ZERO;
        }
        return WL_GX_BL_INVOTHERCLR;
    case WL_NV2A_BLEND_CONSTANT_COLOR:
    case WL_NV2A_BLEND_ONE_MINUS_CONSTANT_COLOR:
    case WL_NV2A_BLEND_CONSTANT_ALPHA:
    case WL_NV2A_BLEND_ONE_MINUS_CONSTANT_ALPHA:
        return constant_factor(factor, color, exact);
    default:
        *exact = false;
        return WL_GX_BL_ONE;
    }
}

static bool blend_of(bool enable, uint32_t equation, uint32_t sfactor, uint32_t dfactor,
                     const uint32_t *color, WlGxBlendMode *mode, WlGxBlendFactor *source,
                     WlGxBlendFactor *destination);

bool wl_nv2a_gx_blend_of(bool enable, uint32_t equation, uint32_t sfactor, uint32_t dfactor,
                         WlGxBlendMode *mode, WlGxBlendFactor *source,
                         WlGxBlendFactor *destination) {
    return blend_of(enable, equation, sfactor, dfactor, NULL, mode, source, destination);
}

bool wl_nv2a_gx_blend_color_of(bool enable, uint32_t equation, uint32_t sfactor, uint32_t dfactor,
                               uint32_t color, WlGxBlendMode *mode, WlGxBlendFactor *source,
                               WlGxBlendFactor *destination) {
    return blend_of(enable, equation, sfactor, dfactor, &color, mode, source, destination);
}

static bool blend_of(bool enable, uint32_t equation, uint32_t sfactor, uint32_t dfactor,
                     const uint32_t *color, WlGxBlendMode *mode, WlGxBlendFactor *source,
                     WlGxBlendFactor *destination) {
    bool exact = true;

    if (!enable) {
        *mode = WL_GX_BM_NONE;
        *source = WL_GX_BL_ONE;
        *destination = WL_GX_BL_ZERO;
        return true;
    }
    *source = gx_blend_factor(sfactor, true, color, &exact);
    *destination = gx_blend_factor(dfactor, false, color, &exact);
    *mode = WL_GX_BM_BLEND;
    if (equation == WL_NV2A_BLEND_EQ_REVERSE_SUBTRACT ||
        equation == WL_NV2A_BLEND_EQ_REVERSE_SUBTRACT_SIGNED) {
        // (GX subtracts the source from the destination, its factors one)
        *mode = WL_GX_BM_SUBTRACT;
        exact = exact && *source == WL_GX_BL_ONE && *destination == WL_GX_BL_ONE;
    } else if (equation == WL_NV2A_BLEND_EQ_MIN) {
        // (min(s, d), the factors not read: s d, which darkens as it does and is it where either is
        // 0 or 1; the factors' sum would brighten)
        *source = WL_GX_BL_OTHERCLR;
        *destination = WL_GX_BL_ZERO;
        exact = false;
    } else if (equation == WL_NV2A_BLEND_EQ_MAX) {
        // (max(s, d): s + d - s d, s (1 - d) + d, as bright as the brighter or more, never over 1)
        *source = WL_GX_BL_INVOTHERCLR;
        *destination = WL_GX_BL_ONE;
        exact = false;
    } else if (equation == WL_NV2A_BLEND_EQ_SUBTRACT) {
        // (s f - d g, which GX cannot subtract that way round: s (1 - d), the source darkened where
        // the destination is bright, s - d where s is 1; of no destination, s f as it is)
        if (*destination != WL_GX_BL_ZERO) {
            *source = WL_GX_BL_INVOTHERCLR;
            *destination = WL_GX_BL_ZERO;
        }
        exact = exact && dfactor == WL_NV2A_BLEND_ZERO;
    } else if (equation != WL_NV2A_BLEND_EQ_ADD) {
        exact = false;
    }
    return exact;
}

unsigned wl_nv2a_cull_of(bool enable, uint32_t face, uint32_t front) {
    bool front_positive;

    if (!enable) {
        return WL_NV2A_CULLS_NONE;
    }
    // (CW as the screen shows it: a positive area in its y-down coordinates)
    front_positive = front == WL_NV2A_FRONT_CW;
    if (face == WL_NV2A_CULL_FRONT_AND_BACK) {
        return WL_NV2A_CULLS_ALL;
    }
    return (face == WL_NV2A_CULL_FRONT) == front_positive ? WL_NV2A_CULLS_POSITIVE
                                                          : WL_NV2A_CULLS_NEGATIVE;
}

unsigned wl_nv2a_cull_d3d(uint32_t cull_mode) {
    // (Direct3D culls the back faces, the front ones wound against the mode: D3DCULL_CCW sets the
    // front CW, so what is counterclockwise on the screen goes)
    if (cull_mode != WL_NV2A_FRONT_CW && cull_mode != WL_NV2A_FRONT_CCW) {
        return WL_NV2A_CULLS_NONE;
    }
    return wl_nv2a_cull_of(true, WL_NV2A_CULL_BACK,
                           cull_mode == WL_NV2A_FRONT_CW ? WL_NV2A_FRONT_CCW : WL_NV2A_FRONT_CW);
}

WlGxCull wl_nv2a_gx_cull(unsigned culls, WlGxCull cull_positive) {
    switch (culls) {
    case WL_NV2A_CULLS_POSITIVE:
        return cull_positive;
    case WL_NV2A_CULLS_NEGATIVE:
        return cull_positive == WL_GX_CULL_BACK ? WL_GX_CULL_FRONT : WL_GX_CULL_BACK;
    case WL_NV2A_CULLS_ALL:
        return WL_GX_CULL_ALL;
    default:
        return WL_GX_CULL_NONE;
    }
}
