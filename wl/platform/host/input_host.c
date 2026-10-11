// SPDX-License-Identifier: MIT
// Host stub of the input part of platform.h: no devices, rumble requests are
// recorded so host tests can check what the input module sent. A test may preload the next poll
// with wl_host_input_set (input_host.h; otherwise every device reads as absent).
#include <string.h>

#include "wl/platform/host/input_host.h"

static WlRawInput g_next;
static bool g_remote_rumble[WL_INPUT_MAX_REMOTES];
static bool g_pad_rumble[WL_INPUT_MAX_PADS];
static bool g_started;

WlResult wl_platform_input_init(void) {
    g_started = true;
    return WL_OK;
}

void wl_platform_input_shutdown(void) {
    g_started = false;
}

void wl_platform_input_poll(WlRawInput *out) {
    if (!g_started) {
        (void)wl_platform_input_init();
    }
    *out = g_next;
}

void wl_platform_rumble(uint8_t remote, bool on) {
    if (remote < WL_INPUT_MAX_REMOTES) {
        g_remote_rumble[remote] = on;
    }
}

void wl_platform_pad_rumble(uint8_t pad, bool on) {
    if (pad < WL_INPUT_MAX_PADS) {
        g_pad_rumble[pad] = on;
    }
}

// ---- test hooks (input_host.h) ----------------------------------------------------------------

void wl_host_input_set(const WlRawInput *raw) {
    g_next = *raw;
}

bool wl_host_remote_rumble(uint8_t remote) {
    return remote < WL_INPUT_MAX_REMOTES && g_remote_rumble[remote];
}

bool wl_host_pad_rumble(uint8_t pad) {
    return pad < WL_INPUT_MAX_PADS && g_pad_rumble[pad];
}
