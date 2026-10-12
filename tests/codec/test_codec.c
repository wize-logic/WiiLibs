// SPDX-License-Identifier: MIT
// wl/codec: GX textures (CMPR palette weights and round trip, the other formats' layouts) and DSP-ADPCM
// (a hand-computed frame, clamping, a straightforward reference decoder).
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "wl/codec/dsp_adpcm.h"
#include "wl/codec/gx_tex.h"
#include "greatest.h"

static uint32_t g_rng = 0x1234567u;
static uint32_t rnd(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 8;
}

// ---- GX textures -------------------------------------------------------------------------------

TEST cmpr_palette_uses_hardware_weights(void) {
    uint8_t pal[4][4];
    wl_gxc_cmpr_palette(0xF800u, 0x0000u, pal); // red (255,0,0) and black, four-colour mode
    ASSERT_EQ(255, pal[0][0]);
    ASSERT_EQ(0, pal[1][0]);
    ASSERT_EQ((5 * 255) >> 3, pal[2][0]); // 159, not S3TC's 170
    ASSERT_EQ((3 * 255) >> 3, pal[3][0]); // 95, not 85
    ASSERT_EQ(255, pal[2][3]);
    ASSERT_EQ(255, pal[3][3]);
    wl_gxc_cmpr_palette(0x0000u, 0xF800u, pal); // c0 <= c1: three colours, index 3 transparent
    ASSERT_EQ(127, pal[2][0]);
    ASSERT_EQ(0, pal[3][3]);
    PASS();
}

// The colours a CMPR sub-block shows, rebuilt from its 8 raw bytes by hand.
static void ref_block_pixel(const uint8_t b[8], int x, int y, uint8_t out[4]) {
    uint8_t pal[4][4];
    uint32_t c0 = ((uint32_t)b[0] << 8) | b[1];
    uint32_t c1 = ((uint32_t)b[2] << 8) | b[3];
    wl_gxc_cmpr_palette(c0, c1, pal);
    memcpy(out, pal[(b[4 + y] >> (6 - 2 * x)) & 3u], 4);
}

TEST cmpr_roundtrip_matches_the_chosen_palette_exactly(void) {
    enum { W = 32, H = 24 };
    static uint8_t src[W * H * 4];
    static uint8_t enc[W * H];
    static uint8_t dec[W * H * 4];
    for (int trial = 0; trial < 6; trial++) {
        for (int i = 0; i < W * H; i++) {
            uint8_t *p = src + i * 4;
            int x = i % W;
            int y = i / W;
            switch (trial) {
            case 0: // smooth gradients
                p[0] = (uint8_t)(x * 8);
                p[1] = (uint8_t)(y * 10);
                p[2] = (uint8_t)(x * 3 + y * 3);
                p[3] = 255;
                break;
            case 1: // noise
                p[0] = (uint8_t)rnd();
                p[1] = (uint8_t)rnd();
                p[2] = (uint8_t)rnd();
                p[3] = 255;
                break;
            default: // noise with transparent texels
                p[0] = (uint8_t)rnd();
                p[1] = (uint8_t)rnd();
                p[2] = (uint8_t)rnd();
                p[3] = (rnd() % 5) == 0 ? 0 : 255;
                break;
            }
        }
        ASSERT_EQ((size_t)(W * H / 2), wl_gxc_encode(WL_GXC_CMPR, src, W, H, enc, sizeof enc));
        ASSERT_EQ((size_t)(W * H / 2), wl_gxc_decode(WL_GXC_CMPR, enc, sizeof enc, W, H, dec));
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                // 8x8 tile, then 2x2 sub-blocks of 4x4 texels, 8 bytes each
                const uint8_t *tile = enc + ((y / 8) * (W / 8) + (x / 8)) * 32;
                const uint8_t *blk = tile + (((y % 8) / 4) * 2 + (x % 8) / 4) * 8;
                uint8_t want[4];
                ref_block_pixel(blk, x % 4, y % 4, want);
                ASSERT_MEM_EQ(want, dec + (y * W + x) * 4, 4);
                // and it is the encoder's best choice: transparent texels map to transparent, opaque ones do not
                const uint8_t *s = src + (y * W + x) * 4;
                if (s[3] < 128) {
                    ASSERT_EQ(0, dec[(y * W + x) * 4 + 3]);
                } else if (trial < 2) {
                    ASSERT_EQ(255, dec[(y * W + x) * 4 + 3]);
                }
            }
        }
    }
    PASS();
}

TEST cmpr_exact_for_two_colour_blocks_and_gradients_stay_close(void) {
    uint8_t px[16][4];
    uint8_t out[8];
    uint8_t back[16][4];
    uint8_t a[4] = {248, 124, 8, 255}; // exactly representable in 565
    uint8_t b[4] = {8, 252, 247, 255};
    for (int i = 0; i < 16; i++) {
        memcpy(px[i], ((i * 5) & 4) ? a : b, 4);
    }
    wl_gxc_cmpr_encode_block((const uint8_t(*)[4])px, out);
    wl_gxc_cmpr_decode_block(out, back);
    for (int i = 0; i < 16; i++) {
        uint8_t qa[4];
        uint8_t qb[4];
        wl_gxc_rgb565_decode(wl_gxc_rgb565_encode(a), qa);
        wl_gxc_rgb565_decode(wl_gxc_rgb565_encode(b), qb);
        ASSERT_MEM_EQ(((i * 5) & 4) ? qa : qb, back[i], 4);
    }
    // a ramp along one colour axis: the two interpolants follow it within half the widest gap of the 5/8:3/8 levels
    for (int i = 0; i < 16; i++) {
        px[i][0] = (uint8_t)(i * 16);
        px[i][1] = 128;
        px[i][2] = 64;
        px[i][3] = 255;
    }
    wl_gxc_cmpr_encode_block((const uint8_t(*)[4])px, out);
    wl_gxc_cmpr_decode_block(out, back);
    for (int i = 0; i < 16; i++) {
        int d = (int)px[i][0] - (int)back[i][0];
        ASSERT(d >= -48 && d <= 48); // levels 0, 90, 150, 240 on a 0..240 ramp: half the widest gap is 45
    }
    PASS();
}

TEST cmpr_edge_blocks_repeat_the_edge(void) {
    // A flat opaque 5x3 image: the tile padding must not turn the block transparent.
    uint8_t src[5 * 3 * 4];
    for (int i = 0; i < 15; i++) {
        src[i * 4] = 8;
        src[i * 4 + 1] = 252;
        src[i * 4 + 2] = 123;
        src[i * 4 + 3] = 255;
    }
    uint8_t enc[32];
    uint8_t dec[5 * 3 * 4];
    ASSERT_EQ(32u, wl_gxc_encode(WL_GXC_CMPR, src, 5, 3, enc, sizeof enc));
    ASSERT_EQ(32u, wl_gxc_decode(WL_GXC_CMPR, enc, sizeof enc, 5, 3, dec));
    for (int i = 0; i < 15; i++) {
        ASSERT_EQ(255, dec[i * 4 + 3]);
        ASSERT(abs(dec[i * 4] - 8) <= 8 && abs(dec[i * 4 + 1] - 252) <= 4 && abs(dec[i * 4 + 2] - 123) <= 8);
    }
    PASS();
}

TEST other_formats_layout_and_roundtrip(void) {
    enum { W = 10, H = 6 }; // not tile multiples
    uint8_t src[W * H * 4];
    uint8_t dec[W * H * 4];
    uint8_t enc[512];
    for (int i = 0; i < W * H; i++) {
        src[i * 4] = (uint8_t)(i * 9);
        src[i * 4 + 1] = (uint8_t)(255 - i * 3);
        src[i * 4 + 2] = (uint8_t)(i * 4);
        src[i * 4 + 3] = (uint8_t)(i * 37);
    }
    // RGBA8 is lossless
    size_t n = wl_gxc_encode(WL_GXC_RGBA8, src, W, H, enc, sizeof enc);
    ASSERT_EQ(wl_gxc_tex_bytes(WL_GXC_RGBA8, W, H), n);
    ASSERT_EQ(n, wl_gxc_decode(WL_GXC_RGBA8, enc, n, W, H, dec));
    ASSERT_MEM_EQ(src, dec, sizeof src);
    // big-endian AR / GB layout of the first texel
    ASSERT_EQ(src[3], enc[0]);
    ASSERT_EQ(src[0], enc[1]);
    ASSERT_EQ(src[1], enc[32]);
    ASSERT_EQ(src[2], enc[33]);
    // RGB565 and RGB5A3: decoded values re-encode to the same words
    for (int f = 0; f < 2; f++) {
        WlGxcFormat fmt = f == 0 ? WL_GXC_RGB565 : WL_GXC_RGB5A3;
        n = wl_gxc_encode(fmt, src, W, H, enc, sizeof enc);
        ASSERT_EQ(wl_gxc_tex_bytes(fmt, W, H), n);
        wl_gxc_decode(fmt, enc, n, W, H, dec);
        uint8_t enc2[512];
        ASSERT_EQ(n, wl_gxc_encode(fmt, dec, W, H, enc2, sizeof enc2));
        ASSERT_MEM_EQ(enc, enc2, n);
    }
    // I8 / IA8 keep the luma; I4 stays within one step
    n = wl_gxc_encode(WL_GXC_IA8, src, W, H, enc, sizeof enc);
    wl_gxc_decode(WL_GXC_IA8, enc, n, W, H, dec);
    for (int i = 0; i < W * H; i++) {
        ASSERT_EQ(src[i * 4 + 3], dec[i * 4 + 3]);
        ASSERT_EQ(dec[i * 4], dec[i * 4 + 2]);
    }
    // indexed
    uint8_t idx[W * H];
    uint8_t back[W * H];
    for (int i = 0; i < W * H; i++) {
        idx[i] = (uint8_t)(i * 7);
    }
    n = wl_gxc_encode_ci(WL_GXC_C8, idx, W, H, enc, sizeof enc);
    ASSERT_EQ(wl_gxc_tex_bytes(WL_GXC_C8, W, H), n);
    ASSERT_EQ(n, wl_gxc_decode_ci(WL_GXC_C8, enc, n, W, H, back));
    ASSERT_MEM_EQ(idx, back, sizeof idx);
    n = wl_gxc_encode_ci(WL_GXC_C4, idx, W, H, enc, sizeof enc);
    ASSERT_EQ(wl_gxc_tex_bytes(WL_GXC_C4, W, H), n);
    wl_gxc_decode_ci(WL_GXC_C4, enc, n, W, H, back);
    for (int i = 0; i < W * H; i++) {
        ASSERT_EQ(idx[i] & 15, back[i]);
    }
    PASS();
}

TEST hostile_arguments_are_refused(void) {
    uint8_t px[16 * 4] = {0};
    uint8_t out[256];
    ASSERT_EQ(0u, wl_gxc_encode(WL_GXC_CMPR, px, 4, 4, out, 31));        // cap too small (32 needed)
    ASSERT_EQ(0u, wl_gxc_encode(WL_GXC_CMPR, px, 0, 4, out, sizeof out)); // zero width
    ASSERT_EQ(0u, wl_gxc_encode(WL_GXC_C4, px, 4, 4, out, sizeof out));   // indexed through the RGBA call
    ASSERT_EQ(0u, wl_gxc_encode(WL_GXC_CMPR, NULL, 4, 4, out, sizeof out));
    ASSERT_EQ(0u, wl_gxc_encode((WlGxcFormat)99, px, 4, 4, out, sizeof out));
    ASSERT_EQ(0u, wl_gxc_tex_bytes(WL_GXC_RGBA8, WL_GXC_MAX_DIM + 1u, 4));
    ASSERT_EQ(0u, wl_gxc_decode(WL_GXC_RGBA8, out, 63, 4, 4, px)); // source shorter than the image needs (64)
    ASSERT_EQ(0u, wl_gxc_decode_ci(WL_GXC_C4, out, 31, 8, 8, px));
    PASS();
}

// ---- DSP-ADPCM ---------------------------------------------------------------------------------

TEST texel_reads_agree_with_the_whole_image_decode(void) {
    enum { W = 21, H = 13 };
    static uint8_t src[W * H * 4];
    static uint8_t enc[4096];
    static uint8_t dec[W * H * 4];
    static uint8_t idx[W * H];
    static uint8_t back[W * H];
    for (int i = 0; i < W * H * 4; i++) {
        src[i] = (uint8_t)rnd();
    }
    for (int i = 0; i < W * H; i++) {
        idx[i] = (uint8_t)rnd();
    }
    for (int f = WL_GXC_I4; f <= WL_GXC_CMPR; f++) {
        size_t n = wl_gxc_encode((WlGxcFormat)f, src, W, H, enc, sizeof enc);
        ASSERT(n > 0);
        ASSERT_EQ(n, wl_gxc_decode((WlGxcFormat)f, enc, n, W, H, dec));
        for (uint32_t y = 0; y < H; y++) {
            for (uint32_t x = 0; x < W; x++) {
                uint8_t px[4];
                uint32_t raw;
                ASSERT(wl_gxc_texel_read((WlGxcFormat)f, enc, n, W, H, x, y, px));
                ASSERT_MEM_EQ(dec + (y * W + x) * 4u, px, 4);
                ASSERT(wl_gxc_texel_raw((WlGxcFormat)f, enc, n, W, H, x, y, &raw));
                switch (f) {
                case WL_GXC_I8:
                    ASSERT_EQ(px[0], raw);
                    break;
                case WL_GXC_IA8:
                    ASSERT_EQ(((uint32_t)px[3] << 8) | px[0], raw);
                    break;
                case WL_GXC_RGB565: {
                    uint8_t c[4];
                    wl_gxc_rgb565_decode(raw, c);
                    ASSERT_MEM_EQ(px, c, 4);
                    break;
                }
                case WL_GXC_RGB5A3: {
                    uint8_t c[4];
                    wl_gxc_rgb5a3_decode(raw, c);
                    ASSERT_MEM_EQ(px, c, 4);
                    break;
                }
                case WL_GXC_RGBA8:
                    ASSERT_EQ(((uint32_t)px[3] << 24) | ((uint32_t)px[0] << 16) | ((uint32_t)px[1] << 8) | px[2], raw);
                    break;
                case WL_GXC_I4:
                    ASSERT_EQ(px[0], raw * 17u);
                    break;
                default:
                    ASSERT(raw < 256u);
                    break;
                }
            }
        }
    }
    // CMPR: the raw index picks the palette entry the decoder shows
    {
        size_t n = wl_gxc_encode(WL_GXC_CMPR, src, W, H, enc, sizeof enc);
        for (uint32_t y = 0; y < H; y++) {
            for (uint32_t x = 0; x < W; x++) {
                uint32_t raw;
                uint8_t px[4];
                const uint8_t *tile = enc + ((y / 8u) * 3u + (x / 8u)) * 32u;
                const uint8_t *blk = tile + (((y % 8u) / 4u) * 2u + ((x % 8u) / 4u)) * 8u;
                uint8_t pal[4][4];
                wl_gxc_cmpr_palette(((uint32_t)blk[0] << 8) | blk[1], ((uint32_t)blk[2] << 8) | blk[3], pal);
                ASSERT(wl_gxc_texel_raw(WL_GXC_CMPR, enc, n, W, H, x, y, &raw));
                ASSERT(wl_gxc_texel_read(WL_GXC_CMPR, enc, n, W, H, x, y, px));
                ASSERT_MEM_EQ(pal[raw], px, 4);
            }
        }
    }
    // the colour-indexed formats: raw is the index, read refuses them
    for (int f = WL_GXC_C4; f <= WL_GXC_C8; f++) {
        size_t n = wl_gxc_encode_ci((WlGxcFormat)f, idx, W, H, enc, sizeof enc);
        ASSERT(n > 0);
        ASSERT_EQ(n, wl_gxc_decode_ci((WlGxcFormat)f, enc, n, W, H, back));
        for (uint32_t y = 0; y < H; y++) {
            for (uint32_t x = 0; x < W; x++) {
                uint32_t raw;
                uint8_t px[4];
                ASSERT(wl_gxc_texel_raw((WlGxcFormat)f, enc, n, W, H, x, y, &raw));
                ASSERT_EQ(back[y * W + x], raw);
                ASSERT(!wl_gxc_texel_read((WlGxcFormat)f, enc, n, W, H, x, y, px));
            }
        }
    }
    PASS();
}

TEST texel_reads_refuse_hostile_arguments(void) {
    uint8_t enc[64] = {0};
    uint8_t px[4];
    uint32_t raw;
    ASSERT(wl_gxc_texel_read(WL_GXC_RGBA8, enc, 64, 4, 4, 3, 3, px));
    ASSERT(!wl_gxc_texel_read(WL_GXC_RGBA8, enc, 64, 4, 4, 4, 0, px));  // x out of range
    ASSERT(!wl_gxc_texel_read(WL_GXC_RGBA8, enc, 64, 4, 4, 0, 4, px));  // y out of range
    ASSERT(!wl_gxc_texel_read(WL_GXC_RGBA8, enc, 63, 4, 4, 0, 0, px));  // source shorter than the image
    ASSERT(!wl_gxc_texel_read(WL_GXC_RGBA8, NULL, 64, 4, 4, 0, 0, px));
    ASSERT(!wl_gxc_texel_read(WL_GXC_RGBA8, enc, 64, 0, 4, 0, 0, px));
    ASSERT(!wl_gxc_texel_read((WlGxcFormat)99, enc, 64, 4, 4, 0, 0, px));
    ASSERT(!wl_gxc_texel_raw((WlGxcFormat)99, enc, 64, 4, 4, 0, 0, &raw));
    ASSERT(!wl_gxc_texel_raw(WL_GXC_RGBA8, enc, 64, 4, 4, 0, 0, NULL));
    ASSERT(!wl_gxc_texel_raw(WL_GXC_CMPR, enc, 31, 8, 8, 0, 0, &raw));
    PASS();
}

TEST dsp_known_frame_gives_known_samples(void) {
    // Predictor pair 0 = (0.5, 0), scale 1 (step 4096), history 0: s = (nib*4096 + 1024 + 1024*h1) >> 11.
    WlDspState st;
    memset(&st, 0, sizeof st);
    st.coef[0] = 1024;
    const uint8_t frame[8] = {0x01, 0x12, 0x78, 0xF0, 0x00, 0x44, 0x80, 0x00};
    static const int16_t want[14] = {2, 5, 17, -7, -5, -2, -1, 0, 8, 12, -10, -5, -2, -1};
    int16_t out[14];
    wl_dsp_decode(&st, frame, 1, out);
    ASSERT_MEM_EQ(want, out, sizeof want);
    ASSERT_EQ(-1, st.h1); // the history carries the last two samples
    ASSERT_EQ(-2, st.h2);
    // a restart from nibble 10 with the header overridden uses the new scale (loop restart)
    WlDspState st2;
    memset(&st2, 0, sizeof st2);
    st2.coef[0] = 1024;
    int16_t out2[14] = {0};
    wl_dsp_decode_frame_from(&st2, 0x00, frame, 12, out2); // scale 0: step 2048; nibbles 0, 0
    ASSERT_EQ(0, out2[0]);
    ASSERT_EQ(0, out2[11]);
    ASSERT_EQ(0, out2[12]);
    ASSERT_EQ(0, out2[13]);
    PASS();
}

TEST dsp_clamps_and_masks_the_predictor(void) {
    WlDspState st;
    memset(&st, 0, sizeof st);
    int16_t out[14];
    const uint8_t hi[8] = {0x0F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F}; // scale 15, nibble 7: far above 16 bits
    wl_dsp_decode(&st, hi, 1, out);
    ASSERT_EQ(32767, out[0]);
    const uint8_t lo[8] = {0x0F, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88};
    wl_dsp_decode(&st, lo, 1, out);
    ASSERT_EQ(-32768, out[0]);
    // a predictor index above 7 is taken & 7 (pair 1 here has coefficients, pair 0 has none)
    WlDspState a;
    memset(&a, 0, sizeof a);
    a.coef[2] = 2048;
    a.h1 = 100;
    WlDspState b = a;
    const uint8_t f1[8] = {0x10, 0, 0, 0, 0, 0, 0, 0};
    const uint8_t f9[8] = {0x90, 0, 0, 0, 0, 0, 0, 0};
    int16_t o1[14];
    int16_t o9[14];
    wl_dsp_decode(&a, f1, 1, o1);
    wl_dsp_decode(&b, f9, 1, o9);
    ASSERT_MEM_EQ(o1, o9, sizeof o1);
    ASSERT_EQ(100, o1[0]);
    PASS();
}

// A straightforward reference decoder (32-bit history copies, same arithmetic).
static void ref_decode(const uint8_t *frames, size_t nframes, const int16_t coef[16], int16_t *h1, int16_t *h2,
                             int16_t *out) {
    int32_t a = *h1, b = *h2;
    for (size_t f = 0; f < nframes; f++) {
        const uint8_t *fr = frames + f * 8;
        unsigned pi = (unsigned)(fr[0] >> 4) & 7u, scale = fr[0] & 15u;
        int64_t c0 = coef[pi * 2], c1 = coef[pi * 2 + 1];
        for (int k = 0; k < 14; k++) {
            int nib = (k & 1) ? (fr[1 + k / 2] & 15) : (fr[1 + k / 2] >> 4);
            if (nib >= 8) {
                nib -= 16;
            }
            int64_t s = ((int64_t)nib * ((int64_t)2048 << scale) + 1024 + c0 * a + c1 * b) >> 11;
            s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
            out[f * 14 + (size_t)k] = (int16_t)s;
            b = a;
            a = (int32_t)s;
        }
    }
    *h1 = (int16_t)a;
    *h2 = (int16_t)b;
}

TEST dsp_matches_a_reference_decoder(void) {
    for (int trial = 0; trial < 8; trial++) {
        WlDspState st;
        for (int k = 0; k < 16; k++) {
            st.coef[k] = (int16_t)(trial < 4 ? (int)(rnd() % 8192) - 4096 : (int)(rnd() % 65536) - 32768);
        }
        st.h1 = (int16_t)rnd();
        st.h2 = (int16_t)rnd();
        uint8_t frames[200 * 8];
        for (size_t i = 0; i < sizeof frames; i++) {
            frames[i] = (uint8_t)rnd();
        }
        int16_t h1 = st.h1, h2 = st.h2;
        int16_t a[200 * 14], b[200 * 14];
        ref_decode(frames, 200, st.coef, &h1, &h2, a);
        wl_dsp_decode(&st, frames, 200, b);
        ASSERT_MEM_EQ(a, b, sizeof a);
        ASSERT_EQ(h1, st.h1);
        ASSERT_EQ(h2, st.h2);
    }
    PASS();
}

SUITE(codec_suite) {
    RUN_TEST(cmpr_palette_uses_hardware_weights);
    RUN_TEST(cmpr_roundtrip_matches_the_chosen_palette_exactly);
    RUN_TEST(cmpr_exact_for_two_colour_blocks_and_gradients_stay_close);
    RUN_TEST(cmpr_edge_blocks_repeat_the_edge);
    RUN_TEST(other_formats_layout_and_roundtrip);
    RUN_TEST(hostile_arguments_are_refused);
    RUN_TEST(texel_reads_agree_with_the_whole_image_decode);
    RUN_TEST(texel_reads_refuse_hostile_arguments);
    RUN_TEST(dsp_known_frame_gives_known_samples);
    RUN_TEST(dsp_clamps_and_masks_the_predictor);
    RUN_TEST(dsp_matches_a_reference_decoder);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(codec_suite);
    GREATEST_MAIN_END();
}
