// SPDX-License-Identifier: MIT
// The GX state gx_direct.h's setters and wl_tev_apply last sent, so that a setter given the value GX
// holds already sends nothing (the TEV's only with WL_TEV_APPLY_CHANGES). The same bookkeeping on
// the console and the host, where nothing is skipped but the calls are counted alike
// (WlGxDirectStats.state_calls / state_sent). Forgotten wherever GX may have been set behind it:
// wl_gx_invalidate_shadow (and so wl_gx_direct_begin), and wl_gx's own shadowed setters.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wl/platform/tev.h"

typedef enum {
    WL_GX_SHADOW_PROJECTION = 0,
    WL_GX_SHADOW_VIEWPORT,
    WL_GX_SHADOW_SCISSOR,
    WL_GX_SHADOW_BLEND,
    WL_GX_SHADOW_ZMODE,
    WL_GX_SHADOW_ZEARLY,
    WL_GX_SHADOW_ALPHA_COMPARE,
    WL_GX_SHADOW_COLOR_UPDATE, // (two calls: the colour's and the alpha's)
    WL_GX_SHADOW_CULL,
    WL_GX_SHADOW_KEYS
} WlGxShadowKey;

// the largest value a key keeps (a projection: 4 x 4 floats and its kind)
#define WL_GX_SHADOW_VALUE_MAX 68u

// Whether `value` (`bytes` <= WL_GX_SHADOW_VALUE_MAX, no padding in it) is not what `key` last sent,
// or nothing is known: then remembered, and the setter sends it. false: GX holds it already.
bool wl_gx_shadow_differs(WlGxShadowKey key, const void *value, size_t bytes);

// What wl_tev_apply_unchecked_ex sends of a program: per stage, the calls (WL_TEV_SEND_*); the
// konst colours, preloaded registers and swap tables by mask; the stage count.
enum {
    WL_TEV_SEND_ORDER = 1u << 0,
    WL_TEV_SEND_SWAP = 1u << 1,
    WL_TEV_SEND_KCSEL = 1u << 2,
    WL_TEV_SEND_KASEL = 1u << 3,
    WL_TEV_SEND_COLOR_IN = 1u << 4,
    WL_TEV_SEND_COLOR_OP = 1u << 5,
    WL_TEV_SEND_ALPHA_IN = 1u << 6,
    WL_TEV_SEND_ALPHA_OP = 1u << 7,
    WL_TEV_SEND_STAGE = 0xFFu
};
typedef struct {
    uint8_t stages[WL_TEV_MAX_STAGES];
    uint8_t konst, preload, swaps;
    bool stage_count;
} WlTevSend;
// The calls for `p` (flags: wl_tev_apply's; with WL_TEV_APPLY_CHANGES only those GX does not hold
// already), the shadow then holding `p`. `p` is valid (wl_tev_validate).
void wl_tev_shadow_plan(const WlTevProgram *p, uint32_t flags, WlTevSend *out);

// Everything forgotten: the next value of each key is sent.
void wl_gx_shadow_forget(void);
// The GX calls asked for since the start, and of those the ones sent.
void wl_gx_shadow_counts(uint32_t *calls, uint32_t *sent);
