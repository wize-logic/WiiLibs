// SPDX-License-Identifier: MIT
// Host stub of the TEV apply step (declarations: wl/platform/tev.h, test control: tev_host.h).
#include "wl/platform/host/tev_host.h"

#include "wl/platform/gx_shadow.h"

static WlTevProgram s_last;
static uint32_t s_applies;
static uint32_t s_flags;

void wl_tev_apply_unchecked_ex(const WlTevProgram *p, uint32_t flags) {
    WlTevSend send;

    wl_tev_shadow_plan(p, flags, &send); // (the console's calls counted: WlGxDirectStats.state_*)
    s_last = *p;
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
