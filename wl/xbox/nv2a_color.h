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

// wl_nv2a_color_byte's byte plus 1.5 * 2^52 as a double, without a conversion to an integer: y
// clamped to 255 (a NaN kept) then to 0 (a NaN 0: below 1 the byte is 0 either way), so z is 0 or in
// [0, 255]; z - 0.5 + 2^-30 is exact in a double and never half way between integers, so adding 1.5 *
// 2^52 rounds it to the nearest, (int)z
static inline double wl_nv2a_color_byte_magic(float value, const WlNv2aColorConstants *k) {
    float y = value * k->c255 + k->half;
    float a = wl_nv2a_select_ge0(y - k->c255, k->c255, y);
    double t = (double)wl_nv2a_select_ge0(a, a, k->zero) + k->below;

    return t + k->magic;
}

// the byte itself as a double
static inline double wl_nv2a_color_byte_double(float value, const WlNv2aColorConstants *k) {
    return wl_nv2a_color_byte_magic(value, k) - k->magic;
}

// b * 256 + c for whole numbers below 2^53, exact: on PowerPC one fmadd (its one rounding, of an
// exact result, the same bits as the product's and the sum's)
static inline double wl_nv2a_color_shift_add(double b, double c, const WlNv2aColorConstants *k) {
#if defined(__PPC__) || defined(__powerpc__)
    return __builtin_fma(b, k->c256, c);
#else
    return b * k->c256 + c;
#endif
}

// a colour as the word GX takes, 0xRRGGBBAA: wl_nv2a_color_byte's bytes put together in a double
// (exact below 2^32), its low word read by its bits after adding 1.5 * 2^52 (the last byte's own,
// all of them whole numbers and exact). (A float to an integer is a store and a load on PowerPC, one
// for the colour instead of one for each byte; every float gives wl_nv2a_color_byte's byte, checked
// over all of them.)
static inline uint32_t wl_nv2a_color_word_with(const float c[4], const WlNv2aColorConstants *k) {
    double word = wl_nv2a_color_shift_add(
        wl_nv2a_color_shift_add(wl_nv2a_color_shift_add(wl_nv2a_color_byte_double(c[0], k),
                                                        wl_nv2a_color_byte_double(c[1], k), k),
                                wl_nv2a_color_byte_double(c[2], k), k),
        wl_nv2a_color_byte_magic(c[3], k), k);
    uint64_t bits;

    memcpy(&bits, &word, sizeof bits);
    return (uint32_t)bits;
}

// wl_nv2a_color_byte as the Gekko's paired singles make it (wl_nv2a_color_words_paired): y = value *
// 255 + 0.5 in two roundings (ps_mul, ps_add, as the plain C's fmuls and fadds), a NaN or y below 0
// made 0 (ps_sel), then the quantized store's u8: clamped to [0, 255] and cut down. The same byte as
// wl_nv2a_color_byte for every float (the unit test checks them all): y below 1 cuts down to 0, and
// from 255 on the clamp gives 255. Plain C, the model the console's code is checked by.
static inline uint32_t wl_nv2a_color_byte_paired_model(float value) {
    float y = value * 255.0f + 0.5f;
    float z = y >= 0.0f ? y : 0.0f; // (ps_sel: a NaN fails the test)

    z = z > 255.0f ? 255.0f : z;
    return (uint32_t)z;
}

#if defined(__wii__)
#define WL_NV2A_COLOR_PAIRED 1
// The colours of `count` vertices as GX's words, 0xRRGGBBAA, each from four floats at `src` (the
// next `stride` bytes on) into the word at byte `offset` of the vertex `out[n]` points to: two pairs
// of paired singles a colour, their bytes stored by GQR6 set to u8 (saved and restored, as the fused
// forms do; GQR0 the floats' own). About 15 instructions a colour against wl_nv2a_color_word's 45.
// (f1-f5 named: a callee-saved FPR's second slot is not saved by the prologue's stfd)
static inline void wl_nv2a_color_words_paired(const void *src, uint32_t stride, void *const *out, uint32_t offset,
                                              uint32_t count) {
    static const float k[6] __attribute__((aligned(8))) = {255.0f, 255.0f, 0.5f, 0.5f, 0.0f, 0.0f};
    uint32_t saved, at;

    if (count == 0) {
        return;
    }
    __asm__ volatile("mfspr %[saved], 918\n\t"
                     "lis %[at], 4\n\t"
                     "ori %[at], %[at], 4\n\t"
                     "mtspr 918, %[at]\n\t"
                     "isync\n\t"
                     "psq_l 1, 0(%[k]), 0, 0\n\t"
                     "psq_l 2, 8(%[k]), 0, 0\n\t"
                     "psq_l 3, 16(%[k]), 0, 0\n\t"
                     "mtctr %[count]\n"
                     "1:\n\t"
                     "lwz %[at], 0(%[out])\n\t"
                     "psq_l 4, 0(%[src]), 0, 0\n\t"
                     "psq_l 5, 8(%[src]), 0, 0\n\t"
                     "addi %[out], %[out], 4\n\t"
                     "add %[src], %[src], %[stride]\n\t"
                     "ps_mul 4, 4, 1\n\t"
                     "ps_mul 5, 5, 1\n\t"
                     "ps_add 4, 4, 2\n\t"
                     "ps_add 5, 5, 2\n\t"
                     "ps_sel 4, 4, 4, 3\n\t"
                     "ps_sel 5, 5, 5, 3\n\t"
                     "add %[at], %[at], %[offset]\n\t"
                     "psq_st 4, 0(%[at]), 0, 6\n\t"
                     "psq_st 5, 2(%[at]), 0, 6\n\t"
                     "bdnz 1b\n\t"
                     "mtspr 918, %[saved]\n\t"
                     "isync"
                     : [saved] "=&r"(saved), [at] "=&b"(at), [src] "+b"(src), [out] "+b"(out)
                     : [k] "b"(k), [count] "r"(count), [stride] "r"(stride), [offset] "r"(offset)
                     : "fr1", "fr2", "fr3", "fr4", "fr5", "ctr", "memory");
}
#else
#define WL_NV2A_COLOR_PAIRED 0
#endif

static inline uint32_t wl_nv2a_color_word(const float c[4]) {
    const WlNv2aColorConstants k = { 255.0f, 0.5f, 0.0f, -0.5 + 0x1p-30, 0x1.8p52, 256.0 };

    return wl_nv2a_color_word_with(c, &k);
}
