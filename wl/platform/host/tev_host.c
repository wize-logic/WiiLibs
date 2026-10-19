// SPDX-License-Identifier: MIT
// Host stub of the TEV apply step (declarations: wl/platform/tev.h, test control: tev_host.h).
#include "wl/platform/host/tev_host.h"

#include "wl/platform/gx_shadow.h"

static WlTevProgram s_last;
static uint32_t s_applies;
static uint32_t s_flags;
// the last program's stages' maps as applied (logical: gx_shadow.h's texture maps as a cache); s_last's
// stages read the maps those went to, as the console's
static uint8_t s_logical[WL_TEV_MAX_STAGES];

void wl_tev_maps_follow(void) {
    if (!wl_gx_shadow_maps_moved()) {
        return;
    }
    wl_gx_shadow_maps_followed();
    for (uint8_t i = 0; i < s_last.stage_count && i < WL_TEV_MAX_STAGES; i++) {
        WlTevStage *st = &s_last.stages[i];

        if (s_logical[i] < WL_TEV_TEXMAPS) {
            st->texmap = (uint8_t)wl_gx_shadow_map_of(s_logical[i]);
            (void)wl_tev_shadow_order_moved(i, st->texcoord, st->texmap, st->channel); // (counted)
        }
    }
}

void wl_tev_apply_unchecked_ex(const WlTevProgram *p, uint32_t flags) {
    WlTevSend send;

    wl_tev_shadow_plan(p, flags, &send); // (the console's calls counted: WlGxDirectStats.state_*)
    s_last = *p;
    for (uint8_t i = 0; i < p->stage_count && i < WL_TEV_MAX_STAGES; i++) {
        s_logical[i] = p->stages[i].texmap;
        if (s_logical[i] < WL_TEV_TEXMAPS) {
            s_last.stages[i].texmap = (uint8_t)wl_gx_shadow_map_of(s_logical[i]);
        }
    }
    s_flags = flags;
    s_applies++;
}

const WlTevProgram *wl_host_tev_last(void) {
    return s_applies != 0 ? &s_last : NULL;
}

uint32_t wl_host_tev_last_flags(void) {
    return s_flags;
}

uint32_t wl_host_tev_applies(void) {
    return s_applies;
}

void wl_host_tev_reset(void) {
    s_applies = 0;
    s_flags = 0;
}
