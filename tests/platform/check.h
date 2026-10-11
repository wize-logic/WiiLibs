// SPDX-License-Identifier: MIT
// Minimal check API for test suites that are plain C (they can also be compiled into an on-console
// test runner): each test executable defines both functions.
#pragma once

#include "wl/base/base.h"

// Record one case.
void wlt_check(const char *name, bool ok, const char *detail);
// Print one raw line.
void wlt_report(const char *fmt, ...) WL_PRINTF_FMT(1, 2);
