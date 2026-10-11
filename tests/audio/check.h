// SPDX-License-Identifier: MIT
// The check-and-report API of the audio test suites (suite_sseq.c, audio_synth.c). Each test
// executable defines both functions (counting passes and failures for its greatest assertions).
#pragma once

#include "wl/base/base.h"

// Record one case: a failing case is printed with its detail.
void wlt_check(const char *name, bool ok, const char *detail);
// Print one raw line.
void wlt_report(const char *fmt, ...) WL_PRINTF_FMT(1, 2);
