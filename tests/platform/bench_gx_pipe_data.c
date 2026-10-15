// SPDX-License-Identifier: MIT
// The pipe of bench_gx_pipe.c, defined where its users cannot see the address (libogc's wgPipe is
// likewise a pointer defined in another unit), so that the plain loops reread it as they would there.
#include "bench_gx_pipe.h"

static BenchPipe sink;

BenchPipe *const bench_pipe = &sink;
