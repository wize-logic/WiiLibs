// SPDX-License-Identifier: MIT
// wl_platform_backtrace on the hosts (backtrace.h). On the big-endian ppc-be test build, which has
// the Wii's frame layout, it walks the real chain, trusting any address within a window above the
// stack pointer (the chain of a healthy program ends at 0 inside the thread's stack). On the other
// hosts, whose frames are not PowerPC's, there is no backtrace: it returns 0.
#include "wl/platform/backtrace.h"

#if defined(__powerpc__) && !defined(__powerpc64__)

#define WINDOW 0x1000000u // 16 MiB

static bool read_word(void *user, uint32_t address, uint32_t *value) {
    uint32_t base = *(const uint32_t *)user;

    if ((address & 3u) != 0 || address < base || address - base >= WINDOW - 4u) {
        return false;
    }
    *value = *(const uint32_t *)(uintptr_t)address;
    return true;
}

uint32_t wl_platform_backtrace_from(uint32_t sp, uint32_t lr, uint32_t *out, uint32_t max) {
    return wl_backtrace_walk(read_word, &sp, sp, lr, 0, out, max);
}

__attribute__((noinline)) uint32_t wl_platform_backtrace(uint32_t *out, uint32_t max, uint32_t skip) {
    uint32_t sp;

    __asm__ volatile("mr %0, 1" : "=r"(sp));
    uint32_t count = wl_backtrace_walk(read_word, &sp, sp, 0, skip, out, max);

    __asm__ volatile("" : "+r"(count)); // (not a tail call: this function's frame must be there to read)
    return count;
}

#else

uint32_t wl_platform_backtrace_from(uint32_t sp, uint32_t lr, uint32_t *out, uint32_t max) {
    (void)sp;
    (void)lr;
    (void)out;
    (void)max;
    return 0;
}

uint32_t wl_platform_backtrace(uint32_t *out, uint32_t max, uint32_t skip) {
    (void)out;
    (void)max;
    (void)skip;
    return 0;
}

#endif
