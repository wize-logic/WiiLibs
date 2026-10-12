// SPDX-License-Identifier: MIT
// Wii entropy: the Wii has no entropy device a program can reach, so the bytes are the time base
// (the processor's free-running counter, which ticks at 60.75 MHz and is read once per byte)
// put through a splitmix-style finaliser. Unpredictable to a program, not to an attacker who can
// guess when it started: see platform.h. Concurrent callers share the state without a lock; a
// race only changes what is mixed in next.
#include <stdint.h>

#include "wl/platform/platform.h"

WlResult wl_platform_random_bytes(void *buf, size_t size) {
    static uint64_t state;
    unsigned char *out = buf;

    if (buf == NULL && size != 0) {
        return WL_ERR_ARG;
    }
    for (size_t i = 0; i < size; i++) {
        uint32_t upper, lower;
        uint64_t x;

        __asm__ volatile("mftbu %0; mftb %1" : "=r"(upper), "=r"(lower));
        x = state ^ (((uint64_t)upper << 32 | lower) + 0x9E3779B97F4A7C15ULL);
        x ^= x >> 33;
        x *= 0xFF51AFD7ED558CCDULL;
        x ^= x >> 33;
        state = x;
        out[i] = (unsigned char)x;
    }
    return WL_OK;
}
