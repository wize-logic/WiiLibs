// SPDX-License-Identifier: MIT
// What the backends' wl_platform_exit shares (shutdown.c). Not for programs.
#pragma once

#include "wl/platform/platform.h"

// The wind-down every exit starts with, once: the shutdown hooks, then the audio output and the
// motors stopped. A second caller waits here for the first to leave the program.
void wl_platform_shutdown_prepare(WlExitTarget target);

// Forgets the hooks and that an exit was under way (host tests, which survive an exit).
void wl_platform_shutdown_reset(void);
