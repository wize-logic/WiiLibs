// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>

// libogc's WGPipe in the part the loops use
typedef union {
    volatile uint16_t U16;
    volatile uint32_t U32;
    volatile double F64; // (the pipe takes 8-byte stores too: the "64" forms)
} BenchPipe;

extern BenchPipe *const bench_pipe;
