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
static bool g_rumble_allowed = true;
static WlSoundMode g_sound_mode = WL_SOUND_STEREO;

WlResult wl_platform_input_init(void) {
    g_started = true;
    return WL_OK;
}

void wl_platform_input_shutdown(void) {
    wl_platform_input_stop_rumble();
    g_started = false;
}

bool wl_platform_rumble_enabled(void) {
    return g_rumble_allowed;
}

WlSoundMode wl_platform_sound_mode(void) {
    return g_sound_mode;
}

void wl_host_sound_mode(WlSoundMode mode) {
    g_sound_mode = mode;
}

void wl_platform_input_stop_rumble(void) {
    memset(g_remote_rumble, 0, sizeof g_remote_rumble);
    memset(g_pad_rumble, 0, sizeof g_pad_rumble);
}

void wl_platform_input_poll(WlRawInput *out) {
    if (!g_started) {
        (void)wl_platform_input_init();
    }
    *out = g_next;
}

void wl_platform_rumble(uint8_t remote, bool on) {
    if (remote < WL_INPUT_MAX_REMOTES) {
        g_remote_rumble[remote] = on && g_rumble_allowed;
    }
}

void wl_platform_pad_rumble(uint8_t pad, bool on) {
    if (pad < WL_INPUT_MAX_PADS) {
        g_pad_rumble[pad] = on && g_rumble_allowed;
    }
}

// ---- test hooks (input_host.h) ----------------------------------------------------------------

void wl_host_input_set(const WlRawInput *raw) {
    g_next = *raw;
}

void wl_host_rumble_allowed(bool allowed) {
    g_rumble_allowed = allowed;
}

bool wl_host_remote_rumble(uint8_t remote) {
    return remote < WL_INPUT_MAX_REMOTES && g_remote_rumble[remote];
}

bool wl_host_pad_rumble(uint8_t pad) {
    return pad < WL_INPUT_MAX_PADS && g_pad_rumble[pad];
}
