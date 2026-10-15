// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>

// libogc's WGPipe in the part the loops use
typedef union {
    volatile uint16_t U16;
    volatile uint32_t U32;
} BenchPipe;

extern BenchPipe *const bench_pipe;
