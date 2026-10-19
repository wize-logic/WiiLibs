// SPDX-License-Identifier: MIT
// The correctly rounded square roots by integer arithmetic (gfx_sqrt.h's last resorts).
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

// gfx_sqrt.h: every double, scaled into [1, 4) and decided in 128-bit integers
double wl_sqrt_wide(double x) {
    if (!(x > 0.0 && x <= 1.7976931348623157e308)) {
        if (x == 0.0 || x > 0.0) {
            return x; // zero or +infinity
        }
        if (x != x || x < -1.7976931348623157e308) {
            return x * x + x; // NaN or -infinity
        }
        return (x - x) / (x - x);
    }
    int scale = 0;
    if (x < 2.2250738585072014e-308) {
        x *= 18014398509481984.0; // 2^54
        scale = -27;
    }
    uint64_t bits = wl_double_bits(x);
    int exponent = (int)(bits >> 52) - 1023;
    int half = exponent >= 0 ? exponent / 2 : -((1 - exponent) / 2); // (the floor of exponent / 2)
    double value = wl_bits_double((bits & (((uint64_t)1 << 52) - 1)) | ((uint64_t)(1023 + exponent - 2 * half) << 52));
    double root = wl_sqrt_refine(value, wl_rsqrt_estimate(value));
    return root * wl_bits_double((uint64_t)(1023 + half + scale) << 52);
}
