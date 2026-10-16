// SPDX-License-Identifier: MIT
// The GX state last sent by the direct layer and the TEV (gx_shadow.h).
#include "wl/platform/gx_shadow.h"

#include <string.h>

// (stages compared as bytes: no padding in them)
_Static_assert(sizeof(WlTevHalf) == 9u && sizeof(WlTevStage) == 7u + 2u * sizeof(WlTevHalf), "WlTevStage has padding");

// the GX calls each key's setter makes
static const uint8_t k_calls[WL_GX_SHADOW_KEYS] = {1, 1, 1, 1, 1, 1, 1, 2, 1};

static struct {
    uint8_t values[WL_GX_SHADOW_KEYS][WL_GX_SHADOW_VALUE_MAX];
    uint16_t known; // bit per key
    // the TEV as the last apply left it: each stage's fields (bit per stage of stages_known), the
    // konst colours, preloads and swap tables (bit per slot), the stage count
    WlTevStage stages[WL_TEV_MAX_STAGES];
    uint16_t stages_known;
    uint32_t konst[WL_TEV_KONST_SLOTS];
    uint32_t preload[WL_TEV_REG_COUNT];
    uint8_t swap[WL_TEV_SWAP_TABLES][4];
    uint8_t konst_known, preload_known, swaps_known;
    uint8_t stage_count;
    bool stage_count_known;
    uint32_t calls, sent;
} s;

bool wl_gx_shadow_differs(WlGxShadowKey key, const void *value, size_t bytes) {
    uint16_t bit = (uint16_t)(1u << key);

    s.calls += k_calls[key];
    if ((s.known & bit) != 0 && memcmp(s.values[key], value, bytes) == 0) {
        return false;
    }
    memcpy(s.values[key], value, bytes);
    s.known |= bit;
    s.sent += k_calls[key];
    return true;
}

static bool same_half(const WlTevHalf *a, const WlTevHalf *b) {
    return a->op == b->op && a->bias == b->bias && a->scale == b->scale && a->clamp == b->clamp && a->dest == b->dest;
}

// the calls a stage's fields need, against the stage GX holds (`known`)
static uint8_t stage_calls(const WlTevStage *now, const WlTevStage *was, bool known) {
    uint8_t send = 0;

    if (!known) {
        return WL_TEV_SEND_STAGE;
    }
    // (the common case first: the stage as it was, its bytes compared at once)
    if (memcmp(now, was, sizeof *now) == 0) {
        return 0;
    }
    if (now->texcoord != was->texcoord || now->texmap != was->texmap || now->channel != was->channel) {
        send |= WL_TEV_SEND_ORDER;
    }
    if (now->ras_swap != was->ras_swap || now->tex_swap != was->tex_swap) {
        send |= WL_TEV_SEND_SWAP;
    }
    if (now->kcsel != was->kcsel) {
        send |= WL_TEV_SEND_KCSEL;
    }
    if (now->kasel != was->kasel) {
        send |= WL_TEV_SEND_KASEL;
    }
    if (memcmp(now->color.in, was->color.in, sizeof now->color.in) != 0) {
        send |= WL_TEV_SEND_COLOR_IN;
    }
    if (!same_half(&now->color, &was->color)) {
        send |= WL_TEV_SEND_COLOR_OP;
    }
    if (memcmp(now->alpha.in, was->alpha.in, sizeof now->alpha.in) != 0) {
        send |= WL_TEV_SEND_ALPHA_IN;
    }
    if (!same_half(&now->alpha, &was->alpha)) {
        send |= WL_TEV_SEND_ALPHA_OP;
    }
    return send;
}

static unsigned bits(uint32_t v) {
    return (unsigned)__builtin_popcount(v);
}

void wl_tev_shadow_plan(const WlTevProgram *p, uint32_t flags, WlTevSend *out) {
    bool changes = (flags & WL_TEV_APPLY_CHANGES) != 0;
    uint32_t swaps_used = 0;
    unsigned i, calls = 0, sent = 0;

    memset(out, 0, sizeof *out);
    for (i = 0; i < p->stage_count; i++) {
        swaps_used |= (1u << p->stages[i].ras_swap) | (1u << p->stages[i].tex_swap);
    }
    if ((flags & WL_TEV_APPLY_SKIP_SWAPS) != 0) {
        s.swaps_known = 0; // (the caller's: set behind the shadow)
    } else {
        for (i = 0; i < WL_TEV_SWAP_TABLES; i++) {
            if ((swaps_used & (1u << i)) == 0) {
                continue;
            }
            calls++;
            if (!changes || (s.swaps_known & (1u << i)) == 0 || memcmp(s.swap[i], p->swap[i], 4) != 0) {
                out->swaps |= (uint8_t)(1u << i);
                memcpy(s.swap[i], p->swap[i], 4);
                s.swaps_known |= (uint8_t)(1u << i);
            }
        }
    }
    for (i = 0; i < WL_TEV_KONST_SLOTS; i++) {
        if ((p->konst_mask & (1u << i)) == 0) {
            continue;
        }
        calls++;
        if (!changes || (s.konst_known & (1u << i)) == 0 || s.konst[i] != p->konst[i]) {
            out->konst |= (uint8_t)(1u << i);
            s.konst[i] = p->konst[i];
            s.konst_known |= (uint8_t)(1u << i);
        }
    }
    for (i = 0; i < WL_TEV_REG_COUNT; i++) {
        if ((p->preload_mask & (1u << i)) == 0) {
            continue;
        }
        calls++;
        if (!changes || (s.preload_known & (1u << i)) == 0 || s.preload[i] != p->preload[i]) {
            out->preload |= (uint8_t)(1u << i);
            s.preload[i] = p->preload[i];
            s.preload_known |= (uint8_t)(1u << i);
        }
    }
    calls++;
    if (!changes || !s.stage_count_known || s.stage_count != p->stage_count) {
        out->stage_count = true;
        s.stage_count = p->stage_count;
        s.stage_count_known = true;
    }
    for (i = 0; i < p->stage_count; i++) {
        const WlTevStage *now = &p->stages[i];

        calls += 8u;
        out->stages[i] = changes ? stage_calls(now, &s.stages[i], (s.stages_known & (1u << i)) != 0)
                                 : (uint8_t)WL_TEV_SEND_STAGE;
        s.stages[i] = *now;
        s.stages_known |= (uint16_t)(1u << i);
        sent += bits(out->stages[i]);
    }
    sent += bits(out->swaps) + bits(out->konst) + bits(out->preload) + (out->stage_count ? 1u : 0u);
    s.calls += calls;
    s.sent += sent;
}

void wl_gx_shadow_forget(void) {
    s.known = 0;
    s.stages_known = 0;
    s.konst_known = s.preload_known = s.swaps_known = 0;
    s.stage_count_known = false;
}

void wl_gx_shadow_counts(uint32_t *calls, uint32_t *sent) {
    *calls = s.calls;
    *sent = s.sent;
}
