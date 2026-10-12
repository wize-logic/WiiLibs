// SPDX-License-Identifier: MIT
// Host stub of the system part of platform.h: test control.
#pragma once

#include "wl/platform/platform.h"

// Latches WlSystemEvent bits as if the console's reset or power button had been pressed; the
// next wl_platform_system_events returns them.
void wl_host_system_raise(uint32_t events);

// Called by wl_platform_exit in place of leaving the program, after the wind-down (shutdown
// hooks, audio and motors stopped) and the log flush, with the target. It must not return: a
// test leaves it with longjmp, or pthread_exit when the exit is made on a thread of its own (the
// system watcher's). NULL: exit(0).
typedef void (*WlHostExitHook)(WlExitTarget target);
void wl_host_exit_set_hook(WlHostExitHook hook);
// Forgets the shutdown hooks and any exit under way, and the latched events (for a test's next
// case; the hook for exit stays).
void wl_host_system_reset(void);

// Called by wl_platform_exit in place of leaving the program, after the wind-down (shutdown
// hooks, audio and motors stopped) and the log flush, with the target. It must not return: a
// test leaves it with longjmp, or pthread_exit when the exit is made on a thread of its own (the
// system watcher's). NULL: exit(0).
typedef void (*WlHostExitHook)(WlExitTarget target);
void wl_host_exit_set_hook(WlHostExitHook hook);
// Forgets the shutdown hooks and any exit under way, and the latched events (for a test's next
// case; the hook for exit stays).
void wl_host_system_reset(void);
