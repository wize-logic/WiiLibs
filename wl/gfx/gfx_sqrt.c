// SPDX-License-Identifier: MIT
// The correctly rounded square root by integer arithmetic (gfx_sqrt.h's last resort).
#include "wl/gfx/gfx_sqrt.h"

#include <math.h>

float wl_sqrtf_integer(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    uint32_t biased = (bits >> 23) & 0xFFu;
    uint32_t mantissa = bits & 0x7FFFFFu;
    int exponent;
    // value = mantissa * 2^exponent
    if (biased == 0) {
        exponent = -149;
    } else {
        mantissa |= 0x800000u;
        exponent = (int)biased - 150;
    }
    // (a subnormal's mantissa made 24 bits, and the exponent even, so the root's exponent is whole)
    while (mantissa < 0x800000u) {
        mantissa <<= 1;
        exponent--;
    }
    if (exponent & 1) {
        mantissa <<= 1;
        exponent--;
    }
    // sqrt(mantissa) * 2^14 has 26 or 27 bits: the floor of it, and what is left over
    uint64_t rest = (uint64_t)mantissa << 28;
    uint64_t root = 0, bit = (uint64_t)1 << 52;
    while (bit > rest) {
        bit >>= 2;
    }
    while (bit) {
        if (rest >= root + bit) {
            rest -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    // round to 24 bits, to nearest, a tie to even (rest != 0 puts the true root above the floor)
    int drop = root >= ((uint64_t)1 << 26) ? 3 : 2;
    uint64_t low = root & (((uint64_t)1 << drop) - 1), half = (uint64_t)1 << (drop - 1);
    uint64_t kept = root >> drop;
    if (low > half || (low == half && (rest != 0 || (kept & 1)))) {
        kept++;
    }
    // (kept is at most 2^24: exact in a float, and the result is never subnormal)
    return ldexpf((float)kept, exponent / 2 - 14 + drop);
}
