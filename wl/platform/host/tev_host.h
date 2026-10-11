// SPDX-License-Identifier: MIT
// Host stub of wl_tev_apply: nothing is written anywhere; the last program applied is kept for tests.
#pragma once

#include "wl/platform/tev.h"

// The program of the last wl_tev_apply* call and the number of calls since the last reset;
// NULL until one was made.
const WlTevProgram *wl_host_tev_last(void);
uint32_t wl_host_tev_applies(void);
uint32_t wl_host_tev_last_flags(void); // the WL_TEV_APPLY_* flags of the last call
void wl_host_tev_reset(void);
