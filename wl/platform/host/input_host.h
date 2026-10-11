// SPDX-License-Identifier: MIT
// Host stub of the input part of platform.h: test control. No real devices exist on the host.
#pragma once

#include "wl/platform/platform.h"

// The state every following wl_platform_input_poll returns (until set again).
void wl_host_input_set(const WlRawInput *raw);
// The last rumble request for a remote or a GameCube pad.
bool wl_host_remote_rumble(uint8_t remote);
bool wl_host_pad_rumble(uint8_t pad);
