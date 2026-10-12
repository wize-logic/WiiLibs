// SPDX-License-Identifier: MIT
// Host stub of the stop screen: test control.
#pragma once

#include "wl/platform/stop.h"

// Called instead of exiting when a stop is reached, with its message and whether an earlier stop
// was already in progress. A test leaves it with longjmp, as the stop never returns. NULL: the
// stop prints the message to stderr and exits with status 1.
typedef void (*WlHostStopHook)(const char *message, bool again);
void wl_host_stop_set_hook(WlHostStopHook hook);
