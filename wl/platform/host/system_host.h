// SPDX-License-Identifier: MIT
// Host stub of the system part of platform.h: test control.
#pragma once

#include "wl/platform/platform.h"

// Latches WlSystemEvent bits as if the console's reset or power button had been pressed; the
// next wl_platform_system_events returns them.
void wl_host_system_raise(uint32_t events);
