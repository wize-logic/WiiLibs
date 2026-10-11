// SPDX-License-Identifier: MIT
// wl_platform_backtrace on the Wii (backtrace.h): the frame chain of the running thread, or of a
// context given by its stack pointer and link register. Only the cached MEM1 and MEM2 ranges are
// read, so a chain damaged by a stack overrun ends the walk instead of raising a second fault.
#include "wl/platform/backtrace.h"

#define MEM1_BASE 0x80000000u
#define MEM1_END 0x81800000u
#define MEM2_BASE 0x90000000u
#define MEM2_END 0x94000000u

static bool read_word(void *user, uint32_t address, uint32_t *value) {
    (void)user;
    if ((address & 3u) != 0 || !((address >= MEM1_BASE && address < MEM1_END - 3u) ||
                                 (address >= MEM2_BASE && address < MEM2_END - 3u))) {
        return false;
    }
    *value = *(const volatile uint32_t *)(uintptr_t)address;
    return true;
}

uint32_t wl_platform_backtrace_from(uint32_t sp, uint32_t lr, uint32_t *out, uint32_t max) {
    return wl_backtrace_walk(read_word, NULL, sp, lr, 0, out, max);
}

// (not inlined: the walk starts at this function's own frame, whose back chain leads to the
// caller's, and the first address is the call site in that caller)
__attribute__((noinline)) uint32_t wl_platform_backtrace(uint32_t *out, uint32_t max, uint32_t skip) {
    uint32_t sp;

    __asm__ volatile("mr %0, 1" : "=r"(sp));
    uint32_t count = wl_backtrace_walk(read_word, NULL, sp, 0, skip, out, max);

    __asm__ volatile("" : "+r"(count)); // (not a tail call: this function's frame must be there to read)
    return count;
}
