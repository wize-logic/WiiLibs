// SPDX-License-Identifier: MIT
// GX texture encoders and decoders: the tile layouts GX reads, round trips within each format's
// precision, exact formats, colour-indexed data and TLUTs, the box filter and texture sizes.
#include "wl/gfx/tex_codec.h"

#include <stdlib.h>
#include <string.h>

#include "greatest.h"


static void fill_gradient(uint8_t *px, uint32_t w, uint32_t h) {
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint8_t *p = px + ((size_t)y * w + x) * 4u;
            p[0] = (uint8_t)(x * 255u / (w - 1u));
            p[1] = (uint8_t)(y * 255u / (h - 1u));
            p[2] = (uint8_t)(((x ^ y) & 1u) * 200u + 20u);
            p[3] = (uint8_t)(255u - (x + y) * 255u / (w + h - 2u));
        }
    }
}

TEST tile_layouts_are_what_gx_reads(void) {
    uint8_t px[8 * 8 * 4];
    memset(px, 0, sizeof px);
    for (int i = 0; i < 64; i++) {
        px[i * 4 + 3] = 255;
    }
    // I4: pixel (1,0) = 0xF0 luma -> second nibble of the first byte; pixel (0,1) is byte 4.
    px[(0 * 8 + 0) * 4] = px[(0 * 8 + 0) * 4 + 1] = px[(0 * 8 + 0) * 4 + 2] = 255;
    px[(1 * 8 + 0) * 4] = px[(1 * 8 + 0) * 4 + 1] = px[(1 * 8 + 0) * 4 + 2] = 255;
    uint8_t out[512];
    ASSERT_EQ(32u, wl_tex_encode(WL_TEX_I4, px, 8, 8, out, sizeof out));
    ASSERT_EQ(0xF0, out[0]);
    ASSERT_EQ(0xF0, out[4]);
    ASSERT_EQ(0x00, out[1]);
    // RGBA8 4x4: 16 AR pairs then 16 GB pairs.
    memset(px, 0, sizeof px);
    px[0] = 0x11; px[1] = 0x22; px[2] = 0x33; px[3] = 0x44; // pixel (0,0)
    px[4 * 4] = 0xAA; px[4 * 4 + 1] = 0xBB; px[4 * 4 + 2] = 0xCC; px[4 * 4 + 3] = 0xDD; // pixel (4,0): second tile
    ASSERT_EQ(128u, wl_tex_encode(WL_TEX_RGBA8, px, 8, 4, out, sizeof out));
    ASSERT_EQ(0x44, out[0]); ASSERT_EQ(0x11, out[1]); ASSERT_EQ(0x22, out[32]); ASSERT_EQ(0x33, out[33]);
    ASSERT_EQ(0xDD, out[64]); ASSERT_EQ(0xAA, out[65]); ASSERT_EQ(0xBB, out[96]); ASSERT_EQ(0xCC, out[97]);
    // RGB565 big endian, row-major inside a 4x4 tile; the second row starts after 4 texels.
    memset(px, 0, sizeof px);
    px[(1 * 8 + 0) * 4] = 255; // (0,1) red
    ASSERT_EQ(64u, wl_tex_encode(WL_TEX_RGB565, px, 8, 4, out, sizeof out));
    ASSERT_EQ(0xF8, out[8]); ASSERT_EQ(0x00, out[9]);
    // C8 8x4 tile: byte per texel, row-major.
    uint8_t idx[8 * 4];
    for (int i = 0; i < 32; i++) {
        idx[i] = (uint8_t)i;
    }
    ASSERT_EQ(32u, wl_tex_encode_ci(WL_TEX_C8, idx, 8, 4, out, sizeof out));
    ASSERT_EQ(9, out[9]);
    // C4 8x8 tile: two texels per byte, high nibble first.
    uint8_t idx4[64];
    for (int i = 0; i < 64; i++) {
        idx4[i] = (uint8_t)(i & 15);
    }
    ASSERT_EQ(32u, wl_tex_encode_ci(WL_TEX_C4, idx4, 8, 8, out, sizeof out));
    ASSERT_EQ(0x01, out[0]);
    ASSERT_EQ(0x23, out[1]);
    ASSERT_EQ(0x89, out[4]);
    PASS();
}

TEST codecs_round_trip(void) {
    enum { W = 20, H = 12 }; // not tile multiples
    uint8_t src[W * H * 4];
    uint8_t dec[W * H * 4];
    uint8_t enc[4096];
    fill_gradient(src, W, H);
    static const WlTexFormat fmts[] = {WL_TEX_I4,  WL_TEX_I8,    WL_TEX_IA4, WL_TEX_IA8,
                                         WL_TEX_RGB565, WL_TEX_RGB5A3, WL_TEX_RGBA8, WL_TEX_CMPR};
    static const int tol[] = {9, 1, 9, 1, 4, 19, 0, 0};
    for (size_t f = 0; f < sizeof fmts / sizeof fmts[0]; f++) {
        size_t n = wl_tex_encode(fmts[f], src, W, H, enc, sizeof enc);
        ASSERT_EQ(wl_tex_bytes(fmts[f], W, H), n);
        wl_tex_decode(fmts[f], enc, W, H, dec);
        int worst = 0;
        for (int i = 0; i < W * H; i++) {
            const uint8_t *a = src + i * 4;
            const uint8_t *b = dec + i * 4;
            int ref[4] = {a[0], a[1], a[2], a[3]};
            if (fmts[f] == WL_TEX_I4 || fmts[f] == WL_TEX_I8 || fmts[f] == WL_TEX_IA4 || fmts[f] == WL_TEX_IA8) {
                int l = (a[0] * 77 + a[1] * 150 + a[2] * 29 + 128) >> 8;
                ref[0] = ref[1] = ref[2] = l;
                if (fmts[f] == WL_TEX_I4 || fmts[f] == WL_TEX_I8) {
                    ref[3] = l;
                }
            }
            if (fmts[f] == WL_TEX_RGB565) {
                ref[3] = 255;
            }
            if (fmts[f] == WL_TEX_CMPR) {
                ref[3] = a[3] < 128 ? 0 : 255; // one transparent colour: alpha is on or off
            }
            int lim = fmts[f] == WL_TEX_CMPR ? 255 : tol[f];
            for (int k = 0; k < 4; k++) {
                int d = abs(ref[k] - b[k]);
                if (fmts[f] == WL_TEX_CMPR && k < 3) {
                    continue; // lossy block fit: checked as a whole below
                }
                if (d > worst) {
                    worst = d;
                }
            }
            (void)lim;
        }
        ASSERT_LTE(worst, fmts[f] == WL_TEX_CMPR ? 0 : tol[f]);
    }
    // CMPR: smooth gradients stay close (a 4x4 block fits two endpoints and two interpolants).
    uint8_t g[16 * 16 * 4];
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            uint8_t *p = g + (y * 16 + x) * 4;
            p[0] = (uint8_t)(x * 16);
            p[1] = (uint8_t)(y * 16);
            p[2] = (uint8_t)(128 + x * 4 - y * 4);
            p[3] = 255;
        }
    }
    uint8_t gd[16 * 16 * 4];
    ASSERT_EQ(128u, wl_tex_encode(WL_TEX_CMPR, g, 16, 16, enc, sizeof enc));
    wl_tex_decode(WL_TEX_CMPR, enc, 16, 16, gd);
    int worst = 0;
    int total = 0;
    for (int i = 0; i < 16 * 16; i++) {
        for (int k = 0; k < 3; k++) {
            int d = abs(g[i * 4 + k] - gd[i * 4 + k]);
            worst = d > worst ? d : worst;
            total += d;
        }
    }
    // Two endpoints and two interpolants per 4x4 block cannot follow a 2D gradient exactly; the fit has to
    // stay on the colour line through the block.
    ASSERT_LTE(worst, 40);
    ASSERT_LTE(total / (16 * 16 * 3), 12);
    // A flat colour is stored exactly (to 565 precision).
    uint8_t flat[4 * 4 * 4];
    for (int i = 0; i < 16; i++) {
        flat[i * 4] = 8; flat[i * 4 + 1] = 252; flat[i * 4 + 2] = 123; flat[i * 4 + 3] = 255;
    }
    uint8_t fd[4 * 4 * 4];
    wl_tex_encode(WL_TEX_CMPR, flat, 4, 4, enc, sizeof enc);
    wl_tex_decode(WL_TEX_CMPR, enc, 4, 4, fd);
    for (int i = 0; i < 16; i++) {
        ASSERT_LTE(abs(flat[0] - fd[i * 4]), 8);
        ASSERT_LTE(abs(flat[1] - fd[i * 4 + 1]), 4);
        ASSERT_LTE(abs(flat[2] - fd[i * 4 + 2]), 8);
    }
    PASS();
}

TEST rgba8_and_565_are_exact_for_representable_values(void) {
    uint8_t src[8 * 8 * 4];
    uint8_t dec[8 * 8 * 4];
    uint8_t enc[1024];
    for (int i = 0; i < 64; i++) {
        src[i * 4] = (uint8_t)(i * 3);
        src[i * 4 + 1] = (uint8_t)(255 - i);
        src[i * 4 + 2] = (uint8_t)(i * 4);
        src[i * 4 + 3] = (uint8_t)(i * 4 + 1);
    }
    wl_tex_encode(WL_TEX_RGBA8, src, 8, 8, enc, sizeof enc);
    wl_tex_decode(WL_TEX_RGBA8, enc, 8, 8, dec);
    ASSERT_EQ(0, memcmp(src, dec, sizeof src));
    // IA8: alpha exact, intensity = luma.
    wl_tex_encode(WL_TEX_IA8, src, 8, 8, enc, sizeof enc);
    wl_tex_decode(WL_TEX_IA8, enc, 8, 8, dec);
    for (int i = 0; i < 64; i++) {
        ASSERT_EQ(src[i * 4 + 3], dec[i * 4 + 3]);
    }
    // Pixels already on the 565 grid survive.
    for (int i = 0; i < 64; i++) {
        src[i * 4] = (uint8_t)(((i & 31) << 3) | ((i & 31) >> 2));
        src[i * 4 + 1] = (uint8_t)((((i * 2) & 63) << 2) | (((i * 2) & 63) >> 4));
        src[i * 4 + 2] = (uint8_t)((((31 - (i & 31))) << 3) | ((31 - (i & 31)) >> 2));
        src[i * 4 + 3] = 255;
    }
    wl_tex_encode(WL_TEX_RGB565, src, 8, 8, enc, sizeof enc);
    wl_tex_decode(WL_TEX_RGB565, enc, 8, 8, dec);
    ASSERT_EQ(0, memcmp(src, dec, sizeof src));
    // RGB5A3 picks 555 opaque or 444+3-bit alpha; both decode to values the same encode reproduces.
    wl_tex_encode(WL_TEX_RGB5A3, src, 8, 8, enc, sizeof enc);
    wl_tex_decode(WL_TEX_RGB5A3, enc, 8, 8, dec);
    uint8_t enc2[1024];
    wl_tex_encode(WL_TEX_RGB5A3, dec, 8, 8, enc2, sizeof enc2);
    ASSERT_EQ(0, memcmp(enc, enc2, 128));
    for (int i = 0; i < 64; i++) {
        src[i * 4 + 3] = (uint8_t)(i * 3); // translucent
    }
    wl_tex_encode(WL_TEX_RGB5A3, src, 8, 8, enc, sizeof enc);
    wl_tex_decode(WL_TEX_RGB5A3, enc, 8, 8, dec);
    wl_tex_encode(WL_TEX_RGB5A3, dec, 8, 8, enc2, sizeof enc2);
    ASSERT_EQ(0, memcmp(enc, enc2, 128));
    PASS();
}

TEST ci_and_tlut_round_trip(void) {
    uint8_t idx[16 * 8];
    uint8_t back[16 * 8];
    uint8_t enc[512];
    for (int i = 0; i < 16 * 8; i++) {
        idx[i] = (uint8_t)((i * 7) & 255);
    }
    wl_tex_encode_ci(WL_TEX_C8, idx, 16, 8, enc, sizeof enc);
    wl_tex_decode_ci(WL_TEX_C8, enc, 16, 8, back);
    ASSERT_EQ(0, memcmp(idx, back, sizeof idx));
    for (int i = 0; i < 16 * 8; i++) {
        idx[i] &= 15;
    }
    wl_tex_encode_ci(WL_TEX_C4, idx, 16, 8, enc, sizeof enc);
    wl_tex_decode_ci(WL_TEX_C4, enc, 16, 8, back);
    ASSERT_EQ(0, memcmp(idx, back, sizeof idx));
    ASSERT_EQ(0u, wl_tex_encode(WL_TEX_C4, idx, 16, 8, enc, sizeof enc)); // wrong entry point
    ASSERT_EQ(0u, wl_tex_encode_ci(WL_TEX_I4, idx, 16, 8, enc, sizeof enc));
    ASSERT_EQ(0u, wl_tex_encode_ci(WL_TEX_C8, idx, 16, 8, enc, 10)); // buffer too small

    uint8_t pal[4 * 4] = {255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0, 40, 80, 120, 255};
    uint8_t t[8];
    uint8_t d[16];
    wl_tlut_encode(WL_TLUT_RGB565, pal, 4, t);
    ASSERT_EQ(0xF8, t[0]); ASSERT_EQ(0x00, t[1]);
    wl_tlut_decode(WL_TLUT_RGB565, t, 4, d);
    ASSERT_EQ(255, d[0]); ASSERT_EQ(0, d[1]); ASSERT_EQ(255, d[3]);
    wl_tlut_encode(WL_TLUT_RGB5A3, pal, 4, t);
    ASSERT_EQ(0xFC, t[0]); // opaque red: 1 11111 00000 00000 -> 0xFC00
    wl_tlut_decode(WL_TLUT_RGB5A3, t, 4, d);
    ASSERT_EQ(0, d[4]);
    ASSERT_EQ(255, d[5]);
    ASSERT_EQ(0x92, d[7]); // alpha 128 -> 3 bits (4) -> replicated back to 146
    ASSERT_EQ(0, d[11]);
    wl_tlut_encode(WL_TLUT_IA8, pal, 4, t);
    ASSERT_EQ(255, t[0]); // alpha first
    ASSERT_EQ(128, t[2]);
    PASS();
}

TEST box_filter_halves(void) {
    uint8_t src[4 * 2 * 4];
    uint8_t out[2 * 1 * 4];
    for (int i = 0; i < 8; i++) {
        for (int k = 0; k < 4; k++) {
            src[i * 4 + k] = (uint8_t)(i < 4 ? (i % 2 ? 100 : 0) : (i % 2 ? 200 : 100));
        }
    }
    wl_tex_box_down(src, 4, 2, out);
    ASSERT_EQ(100, out[0]); // (0 + 100 + 100 + 200) / 4
    ASSERT_EQ(100, out[3]);
    PASS();
}

TEST mip_chain_sizes(void) {
    ASSERT_EQ(wl_tex_bytes(WL_TEX_RGB565, 64, 64) + wl_tex_bytes(WL_TEX_RGB565, 32, 32) +
                  wl_tex_bytes(WL_TEX_RGB565, 16, 16) + wl_tex_bytes(WL_TEX_RGB565, 8, 8) +
                  wl_tex_bytes(WL_TEX_RGB565, 4, 4) + wl_tex_bytes(WL_TEX_RGB565, 2, 2) +
                  wl_tex_bytes(WL_TEX_RGB565, 1, 1),
              wl_tex_bytes_mips(WL_TEX_RGB565, 64, 64, 7));
    ASSERT_EQ(wl_tex_bytes(WL_TEX_I4, 8, 8), wl_tex_bytes_mips(WL_TEX_I4, 8, 8, 0));
    ASSERT_EQ(32u, wl_tex_bytes(WL_TEX_C4, 8, 8));
    ASSERT_EQ(32u, wl_tex_bytes(WL_TEX_C8, 8, 4));
    ASSERT_EQ(64u, wl_tex_bytes(WL_TEX_RGBA8, 4, 4));
    ASSERT_EQ(32u, wl_tex_bytes(WL_TEX_CMPR, 8, 8));
    ASSERT_EQ(0u, wl_tex_bytes((WlTexFormat)99, 8, 8));
    // Hardware format numbers (GX_TF_*).
    ASSERT_EQ(0x0, wl_tex_gx_format(WL_TEX_I4));
    ASSERT_EQ(0x6, wl_tex_gx_format(WL_TEX_RGBA8));
    ASSERT_EQ(0xE, wl_tex_gx_format(WL_TEX_CMPR));
    ASSERT_EQ(0x8, wl_tex_gx_format(WL_TEX_C4));
    ASSERT_EQ(0x9, wl_tex_gx_format(WL_TEX_C8));
    ASSERT_EQ(-1, wl_tex_gx_format(WL_TEX_FORMAT_COUNT));
    PASS();
}

SUITE(tex_codec_suite) {
    RUN_TEST(tile_layouts_are_what_gx_reads);
    RUN_TEST(codecs_round_trip);
    RUN_TEST(rgba8_and_565_are_exact_for_representable_values);
    RUN_TEST(ci_and_tlut_round_trip);
    RUN_TEST(box_filter_halves);
    RUN_TEST(mip_chain_sizes);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(tex_codec_suite);
    GREATEST_MAIN_END();
}
