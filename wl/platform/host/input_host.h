// SPDX-License-Identifier: MIT
// Host stub of the input part of platform.h: test control. No real devices exist on the host.
#pragma once

#include "wl/platform/platform.h"

// The state every following wl_platform_input_poll returns (until set again).
void wl_host_input_set(const WlRawInput *raw);
// The last rumble request for a remote or a GameCube pad.
bool wl_host_remote_rumble(uint8_t remote);
bool wl_host_pad_rumble(uint8_t pad);
// Whether the console's rumble setting allows rumble (wl_platform_rumble_enabled); true at start.
void wl_host_rumble_allowed(bool allowed);
// What the console's sound setting is (wl_platform_sound_mode); stereo at start.
void wl_host_sound_mode(WlSoundMode mode);
