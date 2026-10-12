// SPDX-License-Identifier: MIT
// What a backend provides the stop screen (stop.c): the wait.
#pragma once

#include "wl/base/base.h"

// The stop, after its message is said and the log flushed. `again` is true when an earlier stop
// is already in progress (the message is then not said): wait for it. Does not return.
WL_NORETURN void wl_platform_stop_wait(const char *message, bool again);
