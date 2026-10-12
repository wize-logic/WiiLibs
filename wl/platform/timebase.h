// SPDX-License-Identifier: MIT
// The console's time base (a free-running 64-bit counter at 60.75 MHz, one tick every 16.46 ns)
// and its conversions to milliseconds and microseconds, which neither divide in 64 bits nor
// overflow for any count.
//
// libogc's ticks_to_microsecs(t) is t * 8 / 486: a 64-bit multiply and then a division that the
// 750CL, which has no 64-bit divide, does in the runtime's __udivdi3 (about a hundred
// instructions a call, and the game reads the clock thousands of times a frame). Here the
// division is by a constant of under 16 bits, done as three divisions of 32 bits by that constant
// (the compiler turns each into a multiply by a reciprocal and a shift: the 750CL's mulhwu), one
// for each 16-bit digit of the count after its upper half. Exact for every 64-bit count, and
// the same on every target, so a host test can compare it with the division.
#ifndef WL_PLATFORM_TIMEBASE_H
#define WL_PLATFORM_TIMEBASE_H

#include <stddef.h>
#include <stdint.h>

#define WL_TIMEBASE_HZ 60750000u

// n / d and n % d for a divisor d below 65536, which should be a constant where it is called
// (the function is inline: with d a variable the three divisions are three divide instructions,
// still no library call). Long division in base 2^16 over the upper half and then the two halves
// of the lower: each step's numerator is below d * 65536, so it fits 32 bits.
static inline uint64_t wl_udiv64_16(uint64_t n, uint32_t d, uint32_t *rem) {
    uint32_t hi = (uint32_t)(n >> 32), lo = (uint32_t)n;
    uint32_t q_hi = hi / d;
    uint32_t r = hi % d;
    uint32_t step = (r << 16) | (lo >> 16);
    uint32_t q_mid = step / d;
    r = step % d;
    step = (r << 16) | (lo & 0xFFFFu);
    uint32_t q_lo = step / d;
    if (rem != NULL) {
        *rem = step % d;
    }
    return ((uint64_t)q_hi << 32) + ((uint64_t)q_mid << 16) + q_lo;
}

// Microseconds in `ticks`, rounded down: ticks * 4 / 243 (60.75 MHz is 243 * 250 kHz), found as
// 4 * (ticks / 243) plus the remainder's 4 * r / 243, so that no product can overflow.
static inline uint64_t wl_timebase_us(uint64_t ticks) {
    uint32_t r;
    uint64_t q = wl_udiv64_16(ticks, 243u, &r);
    return q * 4u + (4u * r) / 243u;
}

// Milliseconds in `ticks`, rounded down: ticks / 60750.
static inline uint64_t wl_timebase_ms(uint64_t ticks) {
    return wl_udiv64_16(ticks, 60750u, NULL);
}

#if defined(__PPC__) && !defined(__linux__)
// The time base now (the same count libogc's gettime() returns), read as the 750CL's manual says:
// upper half, lower half, upper half again, until the upper half did not change between the reads.
static inline uint64_t wl_timebase_read(void) {
    uint32_t upper, lower, check;
    do {
        __asm__ volatile("mftbu %0" : "=r"(upper));
        __asm__ volatile("mftb %0" : "=r"(lower));
        __asm__ volatile("mftbu %0" : "=r"(check));
    } while (upper != check);
    return ((uint64_t)upper << 32) | lower;
}
#endif

#endif
