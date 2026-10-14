// SPDX-License-Identifier: MIT
// A vertex's colour as GX takes it from the floats the program or the fetch gave: the GX backend's
// (nv2a_gx.c) and the vertex cache's (nv2a_vcache.c), the same bits.
#pragma once

#include <stdint.h>

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

// a colour as the word GX takes, 0xRRGGBBAA
static inline uint32_t wl_nv2a_color_word(const float c[4]) {
    return wl_nv2a_color_byte(c[0]) << 24 | wl_nv2a_color_byte(c[1]) << 16 | wl_nv2a_color_byte(c[2]) << 8 | wl_nv2a_color_byte(c[3]);
}
