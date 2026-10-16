// SPDX-License-Identifier: MIT
// A vertex's colour as GX takes it from the floats the program or the fetch gave: the GX backend's
// (nv2a_gx.c) and the vertex cache's (nv2a_vcache.c), the same bits.
#pragma once

#include <stdint.h>
#include <string.h>

// test >= 0 ? a : b (a NaN test gives b): one fsel on PowerPC, no branch
static inline float wl_nv2a_select_ge0(float test, float a, float b) {
#if defined(__PPC__) || defined(__powerpc__)
    float r;

    __asm__("fsel %0,%1,%2,%3" : "=f"(r) : "f"(test), "f"(a), "f"(b));
    return r;
#else
    return test >= 0.0f ? a : b;
#endif
}

// a colour's component as a byte: 0 at or below 0 (and for a NaN), 255 at or above 1, else
// (int)(value * 255 + 0.5). Without branches: y = value * 255 + 0.5 is below 1 exactly where the
// byte is 0, and at or above 255 where it is 255 (every float gives the same byte as the tests on
// value would, checked over all of them)
static inline uint32_t wl_nv2a_color_byte(float value) {
    float y = value * 255.0f + 0.5f;
    float z = wl_nv2a_select_ge0(y - 1.0f, y, 0.0f);

    return (uint32_t)(int)wl_nv2a_select_ge0(z - 255.0f, 255.0f, z);
}

// the constants wl_nv2a_color_word_with reads: a loop that makes many colours takes them once
// (wl_nv2a_color_constants), so they stay in registers instead of being loaded for each colour
typedef struct WlNv2aColorConstants {
    float c255, half, zero;
    double below, magic, c256;
} WlNv2aColorConstants;

static inline WlNv2aColorConstants wl_nv2a_color_constants(void) {
    WlNv2aColorConstants k = { 255.0f, 0.5f, 0.0f, -0.5 + 0x1p-30, 0x1.8p52, 256.0 };

#if defined(__PPC__) || defined(__powerpc__)
    // (values the compiler cannot see through, so it keeps them where they are instead of loading
    // the constants again where they are used)
    __asm__("" : "+f"(k.c255), "+f"(k.half), "+f"(k.zero), "+f"(k.below), "+f"(k.magic), "+f"(k.c256));
#endif
    return k;
}

// wl_nv2a_color_byte's byte as a double, without a conversion to an integer: y clamped to 255 (a
// NaN kept) then to 0 (a NaN 0: below 1 the byte is 0 either way), so z is 0 or in [0, 255]; z - 0.5
// + 2^-30 is exact in a double and never half way between integers, so rounding it to the nearest
// (adding and taking away 1.5 * 2^52) is (int)z
static inline double wl_nv2a_color_byte_double(float value, const WlNv2aColorConstants *k) {
    float y = value * k->c255 + k->half;
    float a = wl_nv2a_select_ge0(y - k->c255, k->c255, y);
    double t = (double)wl_nv2a_select_ge0(a, a, k->zero) + k->below;

    return (t + k->magic) - k->magic;
}

// a colour as the word GX takes, 0xRRGGBBAA: wl_nv2a_color_byte's bytes put together in a double
// (exact below 2^32), its low word read by its bits after adding 1.5 * 2^52. (A float to an
// integer is a store and a load on PowerPC, one for the colour instead of one for each byte;
// every float gives wl_nv2a_color_byte's byte, checked over all of them.)
static inline uint32_t wl_nv2a_color_word_with(const float c[4], const WlNv2aColorConstants *k) {
    double word = ((wl_nv2a_color_byte_double(c[0], k) * k->c256 + wl_nv2a_color_byte_double(c[1], k)) * k->c256 +
                   wl_nv2a_color_byte_double(c[2], k)) * k->c256 + wl_nv2a_color_byte_double(c[3], k) + k->magic;
    uint64_t bits;

    memcpy(&bits, &word, sizeof bits);
    return (uint32_t)bits;
}

static inline uint32_t wl_nv2a_color_word(const float c[4]) {
    const WlNv2aColorConstants k = { 255.0f, 0.5f, 0.0f, -0.5 + 0x1p-30, 0x1.8p52, 256.0 };

    return wl_nv2a_color_word_with(c, &k);
}
