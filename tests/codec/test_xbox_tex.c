// SPDX-License-Identifier: MIT
// wl/codec/xbox_tex: Xbox swizzle, DXT blocks and the Xbox formats' GX forms. Hand-computed vectors, cross
// checks against gx_tex.h, and a table of synthetic textures whose converted bytes (as CRC-32s) come from the
// OpenCE-Wii port's Python converter (tools/xbox_tex_reference.py regenerates xbox_tex_cases.inc).
// With WL_XBOX_DUMP_DIR set, the table's outputs are also written as files for a byte-for-byte comparison
// (python3 tools/xbox_tex_reference.py --compare DIR).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wl/base/hash.h"
#include "wl/codec/gx_tex.h"
#include "wl/codec/xbox_tex.h"
#include "greatest.h"

static uint32_t g_rng = 0x2468ACEu;
static uint32_t rnd(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 8;
}

// ---- swizzle -----------------------------------------------------------------------------------

TEST swizzle_masks_alternate_x_then_y(void) {
    uint32_t mx = 0;
    uint32_t my = 0;
    ASSERT(wl_xbox_swizzle_masks(4, 4, &mx, &my));
    ASSERT_EQ(0x5u, mx);
    ASSERT_EQ(0xAu, my);
    ASSERT(wl_xbox_swizzle_masks(8, 2, &mx, &my)); // the longer side continues alone
    ASSERT_EQ(0xDu, mx);
    ASSERT_EQ(0x2u, my);
    ASSERT(wl_xbox_swizzle_masks(2, 8, &mx, &my));
    ASSERT_EQ(0x1u, mx);
    ASSERT_EQ(0xEu, my);
    ASSERT(wl_xbox_swizzle_masks(1, 1, &mx, &my));
    ASSERT_EQ(0u, mx | my);
    ASSERT(!wl_xbox_swizzle_masks(12, 4, &mx, &my));
    ASSERT(!wl_xbox_swizzle_masks(0, 4, &mx, &my));
    ASSERT(!wl_xbox_swizzle_masks(4, WL_GXC_MAX_DIM * 2u, &mx, &my));
    PASS();
}

// The texel at (x, y) of a swizzled image sits at the index the masks give: x's bits spread over mask_x,
// y's over mask_y. Checked through the public calls: a linear image of unique texel numbers must land there.
TEST unswizzle_follows_the_masks_and_round_trips(void) {
    static const uint32_t sizes[][2] = {{1, 1}, {2, 1}, {1, 2}, {4, 4}, {16, 4}, {4, 16}, {32, 32}, {64, 2}, {2, 128}, {256, 128}};
    for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        uint32_t w = sizes[s][0];
        uint32_t h = sizes[s][1];
        uint32_t mx;
        uint32_t my;
        ASSERT(wl_xbox_swizzle_masks(w, h, &mx, &my));
        ASSERT_EQ(0u, mx & my);
        ASSERT_EQ((uint64_t)w * h - 1u, (uint64_t)(mx | my));
        uint32_t *swz = malloc((size_t)w * h * 4u);
        uint32_t *lin = malloc((size_t)w * h * 4u);
        uint32_t *back = malloc((size_t)w * h * 4u);
        ASSERT(swz && lin && back);
        for (uint32_t i = 0; i < w * h; i++) {
            swz[i] = i; // the swizzled index itself
        }
        // 4-byte texels written in host order: compare as values, so the test is endian neutral
        ASSERT_EQ((size_t)w * h * 4u, wl_xbox_unswizzle(swz, (size_t)w * h * 4u, w, h, 4, lin, (size_t)w * h * 4u));
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
                uint32_t idx = 0;
                uint32_t bit = 0;
                for (uint32_t k = 0; k < 32; k++) { // deposit x's and y's bits by the masks, one by one
                    if ((mx >> k & 1u) != 0) {
                        idx |= (x >> bit & 1u) << k;
                        bit++;
                    }
                }
                bit = 0;
                for (uint32_t k = 0; k < 32; k++) {
                    if ((my >> k & 1u) != 0) {
                        idx |= (y >> bit & 1u) << k;
                        bit++;
                    }
                }
                ASSERT_EQ(idx, lin[(size_t)y * w + x]);
            }
        }
        ASSERT_EQ((size_t)w * h * 4u, wl_xbox_swizzle(lin, (size_t)w * h * 4u, w, h, 4, back, (size_t)w * h * 4u));
        ASSERT_EQ(0, memcmp(swz, back, (size_t)w * h * 4u));
        free(swz);
        free(lin);
        free(back);
    }
    PASS();
}

// ---- hand-computed DXT vectors -----------------------------------------------------------------

// One DXT1 block: c0 = 0xF800 (red), c1 = 0x001F (blue), texels 0..3 of row 0 use indices 0..3,
// row 1 uses 3,2,1,0, rows 2 and 3 all index 1.
static const uint8_t k_dxt1_block[8] = {0x00, 0xF8, 0x1F, 0x00, 0xE4, 0x1B, 0x55, 0x55};

TEST dxt1_to_cmpr_reorders_without_changing_a_bit_of_meaning(void) {
    uint8_t out[32];
    ASSERT_EQ(32u, wl_xbox_dxt_to_cmpr(WL_XBOX_DXT1, k_dxt1_block, 8, 4, 4, out, sizeof out));
    static const uint8_t want[8] = {0xF8, 0x00, 0x00, 0x1F, 0x1B, 0xE4, 0x55, 0x55}; // 0xAA would be index 2
    // a 4x4 image is one block in the first quarter of an 8x8 tile; the other three are padding
    ASSERT_MEM_EQ(want, out, 8);
    for (int i = 8; i < 32; i++) {
        ASSERT_EQ(0, out[i]);
    }
    // CMPR reads the same four colours for the endpoints and, c0 > c1, the same block structure
    uint8_t px[16][4];
    uint8_t ref[16][4];
    wl_gxc_cmpr_decode_block(out, px);
    wl_xbox_dxt_decode_block(WL_XBOX_DXT1, k_dxt1_block, ref);
    for (int i = 0; i < 16; i++) {
        int idx = (k_dxt1_block[4 + i / 4] >> (2 * (i % 4))) & 3;
        if (idx < 2) { // endpoints are the same colour in both (the inner two differ by design: 5:3 vs 2:1)
            ASSERT_MEM_EQ(ref[i], px[i], 4);
        }
    }
    PASS();
}

TEST dxt1_three_colour_blocks_keep_their_transparent_index(void) {
    // c0 = 0x001F < c1 = 0xF800: three colours and index 3 clear; texel 0 index 0, the rest index 3
    const uint8_t b[8] = {0x1F, 0x00, 0x00, 0xF8, 0xFC, 0xFF, 0xFF, 0xFF};
    uint8_t out[32];
    ASSERT_EQ(32u, wl_xbox_dxt_to_cmpr(WL_XBOX_DXT1, b, 8, 4, 4, out, sizeof out));
    ASSERT_EQ(0x00, out[0]);
    ASSERT_EQ(0x1F, out[1]);
    ASSERT_EQ(0xF8, out[2]);
    ASSERT_EQ(0x00, out[3]);
    ASSERT_EQ(0x3F, out[4]);
    for (int i = 5; i < 8; i++) {
        ASSERT_EQ(0xFF, out[i]);
    }
    uint8_t px[16][4];
    wl_gxc_cmpr_decode_block(out, px);
    ASSERT_EQ(255, px[0][2]);
    ASSERT_EQ(0, px[5][3]);
    PASS();
}

TEST dxt1_clear_blocks_are_clear_black(void) {
    // all texels index 3: S3TC shows transparent black, CMPR the endpoints' mean, so the endpoints become black
    const uint8_t b[8] = {0x1F, 0x00, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t out[32];
    ASSERT_EQ(32u, wl_xbox_dxt_to_cmpr(WL_XBOX_DXT1, b, 8, 4, 4, out, sizeof out));
    uint8_t px[16][4];
    wl_gxc_cmpr_decode_block(out, px);
    for (int i = 0; i < 16; i++) {
        const uint8_t clear[4] = {0, 0, 0, 0};
        ASSERT_MEM_EQ(clear, px[i], 4);
    }
    PASS();
}

TEST dxt5_colour_is_forced_into_four_colour_mode(void) {
    // Three 4x4 blocks in a row (12x4): c0 > c1, c0 < c1, c0 == c1. The alpha half is 8 bytes of filler.
    uint8_t src[48];
    memset(src, 0x77, sizeof src);
    static const uint8_t colours[3][8] = {
        {0x00, 0xF8, 0x1F, 0x00, 0xE4, 0xE4, 0xE4, 0xE4}, // red > blue, indices 0 1 2 3
        {0x1F, 0x00, 0x00, 0xF8, 0xE4, 0xE4, 0xE4, 0xE4}, // blue < red: swapped, indices 1 0 3 2
        {0x1F, 0x00, 0x1F, 0x00, 0xE4, 0xE4, 0xE4, 0xE4}, // equal: all index 0
    };
    for (int n = 0; n < 3; n++) {
        memcpy(src + n * 16 + 8, colours[n], 8);
    }
    uint8_t out[64];
    ASSERT_EQ(64u, wl_xbox_dxt_to_cmpr(WL_XBOX_DXT5, src, sizeof src, 12, 4, out, sizeof out)); // 2 tiles wide
    static const uint8_t b0[8] = {0xF8, 0x00, 0x00, 0x1F, 0x1B, 0x1B, 0x1B, 0x1B};
    static const uint8_t b2[8] = {0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00, 0x00, 0x00};
    ASSERT_MEM_EQ(b0, out, 8);
    // block 1: the endpoints swapped (c0 = red), the indices 0xE4 ^ 0x55 = 0xB1 with their pairs reversed = 0x4E
    ASSERT_EQ(0xF8, out[8]);
    ASSERT_EQ(0x00, out[9]);
    ASSERT_EQ(0x00, out[10]);
    ASSERT_EQ(0x1F, out[11]);
    for (int i = 12; i < 16; i++) {
        ASSERT_EQ(0x4E, out[i]);
    }
    // 12 texels wide is two tiles: block 2 opens the second one; equal endpoints, every index 0
    ASSERT_MEM_EQ(b2, out + 32, 8);
    PASS();
}

TEST dxt_alpha_blocks_decode_as_s3tc_says(void) {
    uint8_t a[16];
    uint8_t d3[8];
    for (int i = 0; i < 8; i++) {
        d3[i] = (uint8_t)(((2 * i + 1) << 4) | (2 * i)); // texel k has alpha nibble k
    }
    wl_xbox_dxt_alpha_block(WL_XBOX_DXT3, d3, a);
    for (int k = 0; k < 16; k++) {
        ASSERT_EQ(k * 17, a[k]);
    }
    // DXT5, eight-value mode (a0 > a1): a0 = 255, a1 = 0, codes 0..7 then 0..7 again
    uint8_t d5[8] = {255, 0, 0, 0, 0, 0, 0, 0};
    uint64_t bits = 0;
    for (int k = 0; k < 16; k++) {
        bits |= (uint64_t)(k & 7) << (3 * k);
    }
    for (int k = 0; k < 6; k++) {
        d5[2 + k] = (uint8_t)(bits >> (8 * k));
    }
    wl_xbox_dxt_alpha_block(WL_XBOX_DXT5, d5, a);
    static const uint8_t eight[8] = {255, 0, 218, 182, 145, 109, 72, 36};
    for (int k = 0; k < 16; k++) {
        ASSERT_EQ(eight[k & 7], a[k]);
    }
    // six-value mode (a0 <= a1): a0 = 0, a1 = 100, codes 4..7 are (1/5 steps), 0 and 255
    d5[0] = 0;
    d5[1] = 100;
    wl_xbox_dxt_alpha_block(WL_XBOX_DXT5, d5, a);
    static const uint8_t six[8] = {0, 100, 20, 40, 60, 80, 0, 255};
    for (int k = 0; k < 16; k++) {
        ASSERT_EQ(six[k & 7], a[k]);
    }
    PASS();
}

TEST dxt_alpha_i4_takes_the_top_nibble_in_gx_tiles(void) {
    // 8x4: two DXT3 blocks; block 0 has alpha nibble k at texel k, block 1 all 0xF.
    uint8_t src[32];
    memset(src, 0, sizeof src);
    for (int i = 0; i < 8; i++) {
        src[i] = (uint8_t)(((2 * i + 1) << 4) | (2 * i));
        src[16 + i] = 0xFF;
    }
    uint8_t out[32];
    ASSERT_EQ(32u, wl_xbox_dxt_alpha_i4(WL_XBOX_DXT3, src, sizeof src, 8, 4, out, sizeof out));
    uint8_t px[8 * 8 * 4];
    ASSERT_EQ(32u, wl_gxc_decode(WL_GXC_I4, out, sizeof out, 8, 8, px));
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 8; x++) {
            int want = x < 4 ? (y * 4 + x) * 17 : 255;
            ASSERT_EQ(want, px[(y * 8 + x) * 4]);
        }
    }
    for (int y = 4; y < 8; y++) { // below the image: padding is zero
        ASSERT_EQ(0, px[(y * 8) * 4]);
    }
    ASSERT_EQ(0u, wl_xbox_dxt_alpha_i4(WL_XBOX_DXT1, src, sizeof src, 8, 4, out, sizeof out));
    PASS();
}

// ---- DXT decode, against an independent reference ----------------------------------------------

static void ref_dxt1_pixel(const uint8_t *b, int x, int y, int rgba[4]) {
    uint32_t c0 = (uint32_t)b[0] | ((uint32_t)b[1] << 8);
    uint32_t c1 = (uint32_t)b[2] | ((uint32_t)b[3] << 8);
    int p[4][4];
    int r0 = (int)((c0 >> 11) & 31);
    int g0 = (int)((c0 >> 5) & 63);
    int b0 = (int)(c0 & 31);
    int r1 = (int)((c1 >> 11) & 31);
    int g1 = (int)((c1 >> 5) & 63);
    int b1 = (int)(c1 & 31);
    // widen by replicating the top bits: 5 bits -> v*8 + v/4, 6 bits -> v*4 + v/16
    int w0[3] = {r0 * 8 + r0 / 4, g0 * 4 + g0 / 16, b0 * 8 + b0 / 4};
    int w1[3] = {r1 * 8 + r1 / 4, g1 * 4 + g1 / 16, b1 * 8 + b1 / 4};
    for (int k = 0; k < 3; k++) {
        p[0][k] = w0[k];
        p[1][k] = w1[k];
        p[2][k] = c0 > c1 ? (2 * w0[k] + w1[k]) / 3 : (w0[k] + w1[k]) / 2;
        p[3][k] = c0 > c1 ? (w0[k] + 2 * w1[k]) / 3 : 0;
    }
    p[0][3] = p[1][3] = p[2][3] = 255;
    p[3][3] = c0 > c1 ? 255 : 0;
    int idx = (b[4 + y] >> (2 * x)) & 3;
    for (int k = 0; k < 4; k++) {
        rgba[k] = p[idx][k];
    }
}

TEST dxt1_decode_matches_a_reference(void) {
    for (int trial = 0; trial < 200; trial++) {
        uint8_t b[8];
        for (int i = 0; i < 8; i++) {
            b[i] = (uint8_t)rnd();
        }
        uint8_t px[16][4];
        wl_xbox_dxt_decode_block(WL_XBOX_DXT1, b, px);
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) {
                int ref[4];
                ref_dxt1_pixel(b, x, y, ref);
                for (int k = 0; k < 4; k++) {
                    ASSERT_EQ(ref[k], px[y * 4 + x][k]);
                }
            }
        }
    }
    // red and black: the inner colours are 2/3 and 1/3 (170, 85), not CMPR's 5/8 and 3/8
    const uint8_t rb[8] = {0x00, 0xF8, 0x00, 0x00, 0xE4, 0xE4, 0xE4, 0xE4};
    uint8_t px[16][4];
    wl_xbox_dxt_decode_block(WL_XBOX_DXT1, rb, px);
    ASSERT_EQ(255, px[0][0]);
    ASSERT_EQ(0, px[1][0]);
    ASSERT_EQ(170, px[2][0]);
    ASSERT_EQ(85, px[3][0]);
    PASS();
}

TEST dxt_to_gx_shows_what_decode_block_decodes(void) {
    static const WlXboxFormat kinds[3] = {WL_XBOX_DXT1, WL_XBOX_DXT3, WL_XBOX_DXT5};
    static const uint32_t sizes[][2] = {{8, 8}, {13, 6}, {4, 4}, {1, 1}, {9, 17}};
    for (int k = 0; k < 3; k++) {
        for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
            uint32_t w = sizes[s][0];
            uint32_t h = sizes[s][1];
            uint32_t cols = (w + 3) / 4;
            uint32_t rows = (h + 3) / 4;
            size_t bb = kinds[k] == WL_XBOX_DXT1 ? 8 : 16;
            size_t n = cols * rows * bb;
            uint8_t *src = malloc(n);
            ASSERT(src);
            for (size_t i = 0; i < n; i++) {
                src[i] = (uint8_t)rnd();
            }
            static const WlGxcFormat targets[3] = {WL_GXC_RGBA8, WL_GXC_RGB5A3, WL_GXC_RGB565};
            for (int t = 0; t < 3; t++) {
                size_t total = wl_gxc_tex_bytes(targets[t], w, h);
                uint8_t *gx = malloc(total);
                uint8_t *img = malloc((size_t)w * h * 4);
                ASSERT(gx && img);
                ASSERT_EQ(total, wl_xbox_dxt_to_gx(kinds[k], targets[t], src, n, w, h, gx, total));
                ASSERT_EQ(total, wl_gxc_decode(targets[t], gx, total, w, h, img));
                for (uint32_t y = 0; y < h; y++) {
                    for (uint32_t x = 0; x < w; x++) {
                        uint8_t blk[16][4];
                        wl_xbox_dxt_decode_block(kinds[k], src + ((size_t)(y / 4) * cols + x / 4) * bb, blk);
                        const uint8_t *want = blk[(y % 4) * 4 + (x % 4)];
                        uint8_t q[4];
                        memcpy(q, want, 4);
                        if (targets[t] == WL_GXC_RGB5A3) {
                            wl_gxc_rgb5a3_decode(wl_gxc_rgb5a3_encode(want), q);
                        } else if (targets[t] == WL_GXC_RGB565) {
                            wl_gxc_rgb565_decode(wl_gxc_rgb565_encode(want), q);
                        }
                        ASSERT_MEM_EQ(q, img + ((size_t)y * w + x) * 4, 4);
                    }
                }
                free(gx);
                free(img);
            }
            free(src);
        }
    }
    PASS();
}

// ---- the other formats against gx_tex.h --------------------------------------------------------

TEST linear_formats_agree_with_the_gx_encoders(void) {
    enum { W = 16, H = 12 };
    uint8_t xb[W * H * 4];
    uint8_t rgba[W * H * 4];
    uint8_t want[W * H * 4];
    uint8_t got[W * H * 4];
    // A8R8G8B8 (B, G, R, A) against RGBA8 from the same pixels
    for (int i = 0; i < W * H; i++) {
        for (int k = 0; k < 4; k++) {
            xb[i * 4 + k] = (uint8_t)rnd();
        }
        rgba[i * 4 + 0] = xb[i * 4 + 2];
        rgba[i * 4 + 1] = xb[i * 4 + 1];
        rgba[i * 4 + 2] = xb[i * 4 + 0];
        rgba[i * 4 + 3] = xb[i * 4 + 3];
    }
    ASSERT_EQ(wl_gxc_tex_bytes(WL_GXC_RGBA8, W, H), wl_gxc_encode(WL_GXC_RGBA8, rgba, W, H, want, sizeof want));
    ASSERT_EQ(sizeof want, wl_xbox_to_gx(WL_XBOX_A8R8G8B8, 0, 0, xb, sizeof xb, W, H, got, sizeof got));
    ASSERT_MEM_EQ(want, got, sizeof want);
    ASSERT_EQ(sizeof want, wl_xbox_to_gx(WL_XBOX_A8R8G8B8, 0, W * 4, xb, sizeof xb, W, H, got, sizeof got)); // pitch = packed
    ASSERT_MEM_EQ(want, got, sizeof want);
    // X8R8G8B8: alpha reads as 255
    for (int i = 0; i < W * H; i++) {
        rgba[i * 4 + 3] = 255;
    }
    wl_gxc_encode(WL_GXC_RGBA8, rgba, W, H, want, sizeof want);
    ASSERT_EQ(sizeof want, wl_xbox_to_gx(WL_XBOX_X8R8G8B8, 0, 0, xb, sizeof xb, W, H, got, sizeof got));
    ASSERT_MEM_EQ(want, got, sizeof want);

    // R5G6B5 words against RGB565 of their decoded colours (a 565 value survives the round trip)
    uint16_t words[W * H];
    uint8_t w8[W * H * 2];
    for (int i = 0; i < W * H; i++) {
        words[i] = (uint16_t)rnd();
        w8[i * 2] = (uint8_t)words[i];
        w8[i * 2 + 1] = (uint8_t)(words[i] >> 8);
        wl_gxc_rgb565_decode(words[i], rgba + i * 4);
    }
    ASSERT_EQ(wl_gxc_tex_bytes(WL_GXC_RGB565, W, H), wl_gxc_encode(WL_GXC_RGB565, rgba, W, H, want, sizeof want));
    ASSERT_EQ(wl_gxc_tex_bytes(WL_GXC_RGB565, W, H), wl_xbox_to_gx(WL_XBOX_R5G6B5, 0, 0, w8, sizeof w8, W, H, got, sizeof got));
    ASSERT_MEM_EQ(want, got, wl_gxc_tex_bytes(WL_GXC_RGB565, W, H));

    // P8 against the indexed encoder (both pad with 0)
    uint8_t idx[(W + 3) * H];
    for (size_t i = 0; i < sizeof idx; i++) {
        idx[i] = (uint8_t)rnd();
    }
    ASSERT_EQ(wl_gxc_tex_bytes(WL_GXC_C8, W, H), wl_gxc_encode_ci(WL_GXC_C8, idx, W, H, want, sizeof want));
    ASSERT_EQ(wl_gxc_tex_bytes(WL_GXC_C8, W, H), wl_xbox_to_gx(WL_XBOX_P8, 0, W, idx, sizeof idx, W, H, got, sizeof got));
    ASSERT_MEM_EQ(want, got, wl_gxc_tex_bytes(WL_GXC_C8, W, H));

    // a swizzled P8 equals the same image unswizzled first
    enum { S = 16 };
    uint8_t lin[S * S];
    uint8_t swz[S * S];
    for (int i = 0; i < S * S; i++) {
        lin[i] = (uint8_t)rnd();
    }
    ASSERT_EQ((size_t)S * S, wl_xbox_swizzle(lin, sizeof lin, S, S, 1, swz, sizeof swz));
    ASSERT_EQ(wl_gxc_tex_bytes(WL_GXC_C8, S, S), wl_gxc_encode_ci(WL_GXC_C8, lin, S, S, want, sizeof want));
    ASSERT_EQ(wl_gxc_tex_bytes(WL_GXC_C8, S, S), wl_xbox_to_gx(WL_XBOX_P8, 1, 0, swz, sizeof swz, S, S, got, sizeof got));
    ASSERT_MEM_EQ(want, got, wl_gxc_tex_bytes(WL_GXC_C8, S, S));
    PASS();
}

TEST sixteen_bit_alpha_formats_pack_bit_for_bit(void) {
    uint8_t out[32];
    // A1R5G5B5: opaque 0xFC1F-ish value passes through, clear drops to 0RRRRGGGGBBBB
    const uint8_t a1[2] = {0x1F, 0xFC}; // 0xFC1F: a=1, r=31, g=0, b=31
    ASSERT_EQ(32u, wl_xbox_to_gx(WL_XBOX_A1R5G5B5, 0, 0, a1, 2, 1, 1, out, sizeof out));
    ASSERT_EQ(0xFC, out[0]);
    ASSERT_EQ(0x1F, out[1]);
    const uint8_t a1c[2] = {0x3F, 0x7C}; // 0x7C3F: a=0, r=31, g=1, b=31
    ASSERT_EQ(32u, wl_xbox_to_gx(WL_XBOX_A1R5G5B5, 0, 0, a1c, 2, 1, 1, out, sizeof out));
    ASSERT_EQ(0x0F, out[0]); // r top 4 bits = 15
    ASSERT_EQ(0x0F, out[1]); // g = 1 >> 1 = 0, b = 15
    // A4R4G4B4: 0xFA5C opaque, colours widened to five bits; 0x7A5C clear, alpha 7 >> 1 = 3
    const uint8_t a4[2] = {0x5C, 0xFA};
    ASSERT_EQ(32u, wl_xbox_to_gx(WL_XBOX_A4R4G4B4, 0, 0, a4, 2, 1, 1, out, sizeof out));
    uint32_t r = 0xA, g = 0x5, b = 0xC;
    uint32_t want = 0x8000u | (((r << 1) | (r >> 3)) << 10) | (((g << 1) | (g >> 3)) << 5) | ((b << 1) | (b >> 3));
    ASSERT_EQ(want >> 8, out[0]);
    ASSERT_EQ(want & 255u, out[1]);
    const uint8_t a4c[2] = {0x5C, 0x7A};
    ASSERT_EQ(32u, wl_xbox_to_gx(WL_XBOX_A4R4G4B4, 0, 0, a4c, 2, 1, 1, out, sizeof out));
    ASSERT_EQ(0x3A, out[0]); // alpha 7 >> 1 = 3, r = 0xA
    ASSERT_EQ(0x5C, out[1]);
    // A8L8: the word is alpha << 8 | luminance; IA8 stores alpha first
    const uint8_t al[2] = {0x12, 0xAB};
    ASSERT_EQ(32u, wl_xbox_to_gx(WL_XBOX_A8L8, 0, 0, al, 2, 1, 1, out, sizeof out));
    ASSERT_EQ(0xAB, out[0]);
    ASSERT_EQ(0x12, out[1]);
    PASS();
}

TEST palette_becomes_a_tlut(void) {
    // D3DCOLOR B, G, R, A entries
    const uint8_t pal[3 * 4] = {0x00, 0x00, 0xFF, 0xFF,  // opaque red
                                0xFF, 0x00, 0x00, 0x00,  // clear blue
                                0x80, 0x80, 0x80, 0xFF}; // grey
    uint8_t t[6];
    uint8_t rgba[3 * 4];
    for (int f = 0; f < 3; f++) {
        wl_xbox_palette_to_tlut((WlGxcTlutFormat)f, pal, 3, t);
        wl_gxc_tlut_decode((WlGxcTlutFormat)f, t, 3, rgba);
        uint8_t red[4] = {255, 0, 0, 255};
        uint8_t want[4];
        wl_gxc_tlut_encode((WlGxcTlutFormat)f, red, 1, want);
        ASSERT_MEM_EQ(want, t, 2); // entry 0 is RGBA (255, 0, 0, 255) encoded
    }
    wl_xbox_palette_to_tlut(WL_GXC_TLUT_RGB5A3, pal, 3, t);
    ASSERT_EQ(0xFC, t[0]); // 1 11111 00000 00000
    ASSERT_EQ(0x00, t[1]);
    ASSERT_EQ(0x00, t[2]); // alpha 0, blue 15: 0000 0000 0000 1111
    ASSERT_EQ(0x0F, t[3]);
    PASS();
}

TEST hostile_arguments_are_refused(void) {
    uint8_t buf[256] = {0};
    uint8_t out[256];
    ASSERT_EQ(0u, wl_xbox_to_gx(WL_XBOX_DXT1, 0, 0, buf, sizeof buf, 4, 4, out, sizeof out)); // DXT has its own calls
    ASSERT_EQ(0u, wl_xbox_to_gx(WL_XBOX_A8R8G8B8, 1, 0, buf, sizeof buf, 12, 4, out, sizeof out)); // not a power of two
    ASSERT_EQ(0u, wl_xbox_to_gx(WL_XBOX_A8R8G8B8, 0, 0, buf, 63, 4, 4, out, sizeof out));         // source short
    ASSERT_EQ(0u, wl_xbox_to_gx(WL_XBOX_A8R8G8B8, 0, 0, buf, sizeof buf, 4, 4, out, 63));          // cap short
    ASSERT_EQ(0u, wl_xbox_to_gx(WL_XBOX_A8R8G8B8, 0, 15, buf, sizeof buf, 4, 4, out, sizeof out)); // pitch < row
    ASSERT_EQ(0u, wl_xbox_to_gx(WL_XBOX_A8R8G8B8, 0, 0, NULL, sizeof buf, 4, 4, out, sizeof out));
    ASSERT_EQ(0u, wl_xbox_to_gx(WL_XBOX_A8R8G8B8, 0, 0, buf, sizeof buf, 0, 4, out, sizeof out));
    ASSERT_EQ(0u, wl_xbox_to_gx((WlXboxFormat)99, 0, 0, buf, sizeof buf, 4, 4, out, sizeof out));
    ASSERT_EQ(0u, wl_xbox_to_gx(WL_XBOX_A8R8G8B8, 0, 0, buf, sizeof buf, WL_GXC_MAX_DIM + 1u, 1, out, sizeof out));
    ASSERT_EQ(0u, wl_xbox_dxt_to_cmpr(WL_XBOX_DXT1, buf, 7, 4, 4, out, sizeof out));    // 8 bytes needed
    ASSERT_EQ(0u, wl_xbox_dxt_to_cmpr(WL_XBOX_DXT1, buf, 8, 4, 4, out, 31));           // 32 needed
    ASSERT_EQ(0u, wl_xbox_dxt_to_cmpr(WL_XBOX_P8, buf, 8, 4, 4, out, sizeof out));      // not a DXT format
    ASSERT_EQ(0u, wl_xbox_dxt_to_cmpr(WL_XBOX_DXT3, NULL, 16, 4, 4, out, sizeof out));
    ASSERT_EQ(0u, wl_xbox_dxt_alpha_i4(WL_XBOX_DXT3, buf, 15, 4, 4, out, sizeof out));
    ASSERT_EQ(0u, wl_xbox_dxt_to_gx(WL_XBOX_DXT3, WL_GXC_CMPR, buf, sizeof buf, 4, 4, out, sizeof out)); // target
    ASSERT_EQ(0u, wl_xbox_dxt_to_gx(WL_XBOX_DXT3, WL_GXC_RGBA8, buf, sizeof buf, 4, 4, out, 63));
    ASSERT_EQ(0u, wl_xbox_unswizzle(buf, sizeof buf, 3, 4, 1, out, sizeof out));
    ASSERT_EQ(0u, wl_xbox_unswizzle(buf, 15, 4, 4, 1, out, sizeof out));
    ASSERT_EQ(0u, wl_xbox_unswizzle(buf, sizeof buf, 4, 4, 17, out, sizeof out));
    ASSERT_EQ(0u, wl_xbox_level_bytes(WL_XBOX_A8R8G8B8, 4, 4, 15));
    ASSERT_EQ(16u, wl_xbox_level_bytes(WL_XBOX_DXT3, 4, 4, 0));
    ASSERT_EQ(8u * 3 * 2, wl_xbox_level_bytes(WL_XBOX_DXT1, 9, 5, 0));
    ASSERT_EQ(5u * 3 * 4 * 2, wl_xbox_level_bytes(WL_XBOX_A8R8G8B8, 5, 3, 40));
    PASS();
}

// ---- the port's Python converter ----------------------------------------------------------------

typedef struct {
    uint8_t fmt;
    uint16_t w, h;
    uint8_t swizzled;
    uint16_t pitch;
    uint32_t seed;
    uint32_t out_bytes, out_crc, alpha_bytes, alpha_crc;
} Case;

static const Case k_cases[] = {
#include "xbox_tex_cases.inc"
};

// The synthetic texture of tools/xbox_tex_reference.py's make_input: an LCG byte stream; in DXT textures, every
// fifth block has equal endpoints and every fifth+1 block an endpoint just under c0, so each ordering occurs.
static void make_input(WlXboxFormat fmt, uint8_t *data, size_t size, uint32_t seed) {
    uint32_t x = seed;
    for (size_t i = 0; i < size; i++) {
        x = x * 1664525u + 1013904223u;
        data[i] = (uint8_t)(x >> 16);
    }
    if (fmt == WL_XBOX_DXT1 || fmt == WL_XBOX_DXT3 || fmt == WL_XBOX_DXT5) {
        size_t block = fmt == WL_XBOX_DXT1 ? 8 : 16;
        size_t colour = fmt == WL_XBOX_DXT1 ? 0 : 8;
        for (size_t n = 0; n < size / block; n++) {
            uint8_t *b = data + n * block + colour;
            if (n % 5 == 0) {
                b[2] = b[0];
                b[3] = b[1];
            } else if (n % 5 == 1) {
                b[1] = b[3];
                b[0] = (uint8_t)(b[2] + 1u);
            }
        }
    }
}

static void dump(const char *dir, size_t index, const char *suffix, const void *data, size_t len) {
    char path[512];
    snprintf(path, sizeof path, "%s/case%03u%s.bin", dir, (unsigned)index, suffix);
    FILE *f = fopen(path, "wb");
    if (f != NULL) {
        fwrite(data, 1, len, f);
        fclose(f);
    }
}

TEST conversions_match_the_ports_python(void) {
    const char *dump_dir = getenv("WL_XBOX_DUMP_DIR");
    for (size_t c = 0; c < sizeof k_cases / sizeof k_cases[0]; c++) {
        const Case *k = &k_cases[c];
        WlXboxFormat fmt = (WlXboxFormat)k->fmt;
        size_t in_len = wl_xbox_level_bytes(fmt, k->w, k->h, k->pitch);
        ASSERT(in_len != 0);
        uint8_t *in = malloc(in_len);
        uint8_t *out = malloc(k->out_bytes + 1u);
        uint8_t *alpha = malloc(k->alpha_bytes + 1u);
        ASSERT(in && out && alpha);
        make_input(fmt, in, in_len, k->seed);
        size_t got;
        if (fmt == WL_XBOX_DXT1 || fmt == WL_XBOX_DXT3 || fmt == WL_XBOX_DXT5) {
            got = wl_xbox_dxt_to_cmpr(fmt, in, in_len, k->w, k->h, out, k->out_bytes);
        } else {
            got = wl_xbox_to_gx(fmt, k->swizzled, k->pitch, in, in_len, k->w, k->h, out, k->out_bytes);
        }
        if (got != k->out_bytes || wl_crc32(out, got) != k->out_crc) {
            fprintf(stderr, "case %u: format %u %ux%u swizzled %u pitch %u: %zu bytes, crc %08X (want %u, %08X)\n",
                    (unsigned)c, k->fmt, k->w, k->h, k->swizzled, k->pitch, got, (unsigned)wl_crc32(out, got),
                    (unsigned)k->out_bytes, (unsigned)k->out_crc);
            FAIL();
        }
        if (k->alpha_bytes != 0) {
            size_t ga = wl_xbox_dxt_alpha_i4(fmt, in, in_len, k->w, k->h, alpha, k->alpha_bytes);
            if (ga != k->alpha_bytes || wl_crc32(alpha, ga) != k->alpha_crc) {
                fprintf(stderr, "case %u: alpha of format %u %ux%u differs\n", (unsigned)c, k->fmt, k->w, k->h);
                FAIL();
            }
        }
        if (dump_dir != NULL) {
            dump(dump_dir, c, "", out, got);
            if (k->alpha_bytes != 0) {
                dump(dump_dir, c, ".alpha", alpha, k->alpha_bytes);
            }
        }
        free(in);
        free(out);
        free(alpha);
    }
    PASS();
}

TEST packed_pitch_zero_is_the_row_width(void) {
    // the table gives every linear level an explicit pitch; pitch 0 must give the same bytes when it is the row width
    for (size_t c = 0; c < sizeof k_cases / sizeof k_cases[0]; c++) {
        const Case *k = &k_cases[c];
        WlXboxFormat fmt = (WlXboxFormat)k->fmt;
        size_t tb = wl_xbox_texel_bytes(fmt);
        if (k->swizzled || tb == 0 || k->pitch != k->w * tb) {
            continue;
        }
        size_t in_len = wl_xbox_level_bytes(fmt, k->w, k->h, 0);
        ASSERT_EQ(in_len, wl_xbox_level_bytes(fmt, k->w, k->h, k->pitch));
        uint8_t *in = malloc(in_len);
        uint8_t *out = malloc(k->out_bytes);
        ASSERT(in && out);
        make_input(fmt, in, in_len, k->seed);
        ASSERT_EQ((size_t)k->out_bytes, wl_xbox_to_gx(fmt, 0, 0, in, in_len, k->w, k->h, out, k->out_bytes));
        ASSERT_EQ(k->out_crc, wl_crc32(out, k->out_bytes));
        free(in);
        free(out);
    }
    PASS();
}

SUITE(xbox_tex_suite) {
    RUN_TEST(swizzle_masks_alternate_x_then_y);
    RUN_TEST(unswizzle_follows_the_masks_and_round_trips);
    RUN_TEST(dxt1_to_cmpr_reorders_without_changing_a_bit_of_meaning);
    RUN_TEST(dxt1_three_colour_blocks_keep_their_transparent_index);
    RUN_TEST(dxt1_clear_blocks_are_clear_black);
    RUN_TEST(dxt5_colour_is_forced_into_four_colour_mode);
    RUN_TEST(dxt_alpha_blocks_decode_as_s3tc_says);
    RUN_TEST(dxt_alpha_i4_takes_the_top_nibble_in_gx_tiles);
    RUN_TEST(dxt1_decode_matches_a_reference);
    RUN_TEST(dxt_to_gx_shows_what_decode_block_decodes);
    RUN_TEST(linear_formats_agree_with_the_gx_encoders);
    RUN_TEST(sixteen_bit_alpha_formats_pack_bit_for_bit);
    RUN_TEST(palette_becomes_a_tlut);
    RUN_TEST(hostile_arguments_are_refused);
    RUN_TEST(conversions_match_the_ports_python);
    RUN_TEST(packed_pitch_zero_is_the_row_width);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(xbox_tex_suite);
    GREATEST_MAIN_END();
}
