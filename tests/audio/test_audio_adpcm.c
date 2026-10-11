// SPDX-License-Identifier: MIT
// DSP-ADPCM and IMA-ADPCM decoders (wl/audio/adpcm.c) against reference implementations written
// from the format text in this file, plus synthetic encodings.
#include <stdint.h>
#include <string.h>

#include "wl/audio/adpcm.h"
#include "greatest.h"
#include "tests/audio/dsp_fixture.h"

// ---- reference DSP-ADPCM (adpcm.h), straight from the text, 64-bit throughout ------------

static int16_t ref_clamp(int64_t v) {
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

static void ref_dsp_frame(const int16_t coef[16], int16_t *h1, int16_t *h2, const uint8_t f[8], int16_t out[14]) {
    const int pred = (f[0] >> 4) & 7;
    const int scale = f[0] & 15;
    for (int i = 0; i < 14; i++) {
        int nib = (i % 2 == 0) ? (f[1 + i / 2] >> 4) : (f[1 + i / 2] & 15);
        if (nib >= 8) {
            nib -= 16;
        }
        const int64_t v = (int64_t)nib * ((int64_t)2048 << scale) + 1024 + (int64_t)coef[2 * pred] * *h1 +
                          (int64_t)coef[2 * pred + 1] * *h2;
        out[i] = ref_clamp(v >> 11);
        *h2 = *h1;
        *h1 = out[i];
    }
}

static uint32_t g_rng = 12345;
static uint32_t rnd(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 8;
}

TEST dsp_matches_reference_on_random_frames(void) {
    // Random coefficients (including large ones that overflow 32-bit sums), random headers with every
    // scale 0..15 and predictor 0..7, random nibbles: 20000 frames.
    for (int trial = 0; trial < 20; trial++) {
        WlDspState st;
        for (int k = 0; k < 16; k++) {
            st.coef[k] = (int16_t)(trial < 10 ? (int)(rnd() % 8192) - 4096 : (int)(rnd() % 65536) - 32768);
        }
        st.h1 = (int16_t)rnd();
        st.h2 = (int16_t)rnd();
        WlDspState ref = st;
        for (int f = 0; f < 1000; f++) {
            uint8_t fr[8];
            for (int i = 0; i < 8; i++) {
                fr[i] = (uint8_t)rnd();
            }
            int16_t a[14], b[14];
            wl_dsp_decode_frame_from(&st, fr[0], fr, 0, a);
            ref_dsp_frame(ref.coef, &ref.h1, &ref.h2, fr, b);
            ASSERT_MEM_EQ(b, a, sizeof a);
            ASSERT_EQ(ref.h1, st.h1);
            ASSERT_EQ(ref.h2, st.h2);
        }
    }
    PASS();
}

TEST dsp_decode_many_frames_matches_fixture_decoder(void) {
    int16_t pcm[1400], dec[1400], ref[1400];
    fx_sine_pcm(pcm, 1400);
    uint8_t enc[100 * 8];
    fx_dsp_encode(pcm, 1400, FX_DSP_COEF, enc);
    WlDspState st;
    memcpy(st.coef, FX_DSP_COEF, sizeof st.coef);
    st.h1 = st.h2 = 0;
    wl_dsp_decode(&st, enc, 100, dec);
    int16_t hist[2] = {0, 0};
    fx_dsp_decode(enc, 100, FX_DSP_COEF, ref, hist);
    ASSERT_MEM_EQ(ref, dec, sizeof dec);
    // The encoding is a faithful (lossy) version of the sine: peak error stays small.
    int32_t worst = 0;
    for (int i = 0; i < 1400; i++) {
        int32_t d = dec[i] - pcm[i];
        worst = d < 0 ? worst < -d ? -d : worst : worst < d ? d : worst;
    }
    ASSERT(worst < 400);
    PASS();
}

TEST dsp_partial_frame_start_resumes_mid_frame(void) {
    // A loop that starts mid-frame decodes nibbles first..13 with the supplied history: the result
    // equals the tail of a full decode when the history is the two samples before `first`.
    int16_t pcm[280], dec[280];
    fx_sine_pcm(pcm, 280);
    uint8_t enc[20 * 8];
    fx_dsp_encode(pcm, 280, FX_DSP_COEF, enc);
    WlDspState st;
    memcpy(st.coef, FX_DSP_COEF, sizeof st.coef);
    st.h1 = st.h2 = 0;
    wl_dsp_decode(&st, enc, 20, dec);
    for (uint32_t first = 1; first < 14; first++) {
        const uint32_t frame = 7;
        const uint32_t at = frame * 14 + first;
        WlDspState r;
        memcpy(r.coef, FX_DSP_COEF, sizeof r.coef);
        r.h1 = dec[at - 1];
        r.h2 = dec[at - 2];
        int16_t out[14];
        memset(out, 0x55, sizeof out);
        wl_dsp_decode_frame_from(&r, enc[frame * 8], enc + frame * 8, first, out);
        for (uint32_t i = 0; i < first; i++) {
            ASSERT_EQ((int16_t)0x5555, out[i]); // untouched
        }
        for (uint32_t i = first; i < 14; i++) {
            ASSERT_EQ(dec[frame * 14 + i], out[i]);
        }
        ASSERT_EQ(dec[frame * 14 + 13], r.h1);
    }
    PASS();
}

TEST dsp_saturates_instead_of_wrapping(void) {
    WlDspState st;
    memset(&st, 0, sizeof st);
    st.coef[2] = 2047 * 2; // pair 1: c0 ~ 2.0: doubles the history every sample
    st.h1 = 30000;
    uint8_t fr[8] = {0x1F, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77}; // pair 1, scale 15, nibble 7
    int16_t out[14];
    wl_dsp_decode_frame_from(&st, fr[0], fr, 0, out);
    ASSERT_EQ(32767, out[0]);
    ASSERT_EQ(32767, out[13]);
    uint8_t fn[8] = {0x1F, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88}; // nibble -8
    st.h1 = -30000;
    wl_dsp_decode_frame_from(&st, fn[0], fn, 0, out);
    ASSERT_EQ(-32768, out[0]);
    PASS();
}

TEST dsp_predictor_nibble_is_masked_to_three_bits(void) {
    WlDspState st;
    memset(&st, 0, sizeof st);
    for (int k = 0; k < 16; k++) {
        st.coef[k] = (int16_t)(100 * (k + 1));
    }
    st.h1 = 1000;
    st.h2 = 500;
    WlDspState st2 = st;
    uint8_t a[8] = {0x31, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE};
    uint8_t b[8] = {0xB1, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE}; // predictor 11 -> 3
    int16_t oa[14], ob[14];
    wl_dsp_decode_frame_from(&st, a[0], a, 0, oa);
    wl_dsp_decode_frame_from(&st2, b[0], b, 0, ob);
    ASSERT_MEM_EQ(oa, ob, sizeof oa);
    PASS();
}

// ---- IMA-ADPCM ------------------------------------------------------------------------------

// Reference decoder: the textbook algorithm with its own copy of the tables.
static const int ref_steps[89] = {
    7,    8,    9,    10,   11,   12,   13,   14,   16,   17,   19,   21,   23,    25,    28,    31,    34,
    37,   41,   45,   50,   55,   60,   66,   73,   80,   88,   97,   107,  118,   130,   143,   157,   173,
    190,  209,  230,  253,  279,  307,  337,  371,  408,  449,  494,  544,  598,   658,   724,   796,   876,
    963,  1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,  3327,  3660,  4026,  4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385,
    24623, 27086, 29794, 32767};

static int ref_ima(int *pred, int *index, int code) {
    const int step = ref_steps[*index];
    int vpdiff = step >> 3;
    if (code & 4) {
        vpdiff += step;
    }
    if (code & 2) {
        vpdiff += step >> 1;
    }
    if (code & 1) {
        vpdiff += step >> 2;
    }
    *pred += (code & 8) ? -vpdiff : vpdiff;
    *pred = *pred > 32767 ? 32767 : *pred < -32768 ? -32768 : *pred;
    static const int adj[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
    *index += adj[code & 7];
    *index = *index < 0 ? 0 : *index > 88 ? 88 : *index;
    return *pred;
}

TEST ima_known_values(void) {
    // Hand-derived from the algorithm: pred 0, index 0 (step 7).
    WlImaState st;
    wl_ima_init(&st, 0, 0);
    ASSERT_EQ(11, wl_ima_step(&st, 7)); // 0 + (7>>3 = 0) + 7 + 3 + 1; index 0 + 8
    ASSERT_EQ(8, st.index);
    ASSERT_EQ(13, wl_ima_step(&st, 0)); // + (16>>3 = 2); index 8 - 1
    ASSERT_EQ(7, st.index);
    ASSERT_EQ(-12, wl_ima_step(&st, 15)); // step 14: 13 - (1 + 14 + 7 + 3); index 7 + 8
    ASSERT_EQ(15, st.index);
    PASS();
}

TEST ima_matches_reference_and_clamps_the_index(void) {
    for (int trial = 0; trial < 50; trial++) {
        WlImaState st;
        const int p0 = (int)(rnd() % 65536) - 32768;
        const int i0 = (int)(rnd() % 89);
        wl_ima_init(&st, (int16_t)p0, (uint32_t)i0);
        int pred = p0, index = i0;
        uint8_t data[512];
        for (unsigned i = 0; i < sizeof data; i++) {
            data[i] = (uint8_t)rnd();
        }
        int16_t out[1024];
        wl_ima_decode(&st, data, 1024, out);
        for (int i = 0; i < 1024; i++) {
            const int code = (i & 1) ? (data[i / 2] >> 4) : (data[i / 2] & 15); // low nibble first
            ASSERT_EQ(ref_ima(&pred, &index, code), out[i]);
        }
        ASSERT_EQ(index, st.index);
    }
    WlImaState st;
    wl_ima_init(&st, 0, 500);
    ASSERT_EQ(88, st.index);
    PASS();
}

TEST ima_roundtrip_of_a_sine(void) {
    // Encode with the textbook encoder, decode, expect a close copy.
    int16_t pcm[2048];
    fx_sine_pcm(pcm, 2048);
    uint8_t enc[1024];
    int pred = 0, index = 0;
    for (int i = 0; i < 2048; i++) {
        const int step = ref_steps[index];
        int diff = pcm[i] - pred;
        int code = 0;
        if (diff < 0) {
            code = 8;
            diff = -diff;
        }
        int t = step;
        if (diff >= t) {
            code |= 4;
            diff -= t;
        }
        t >>= 1;
        if (diff >= t) {
            code |= 2;
            diff -= t;
        }
        t >>= 1;
        if (diff >= t) {
            code |= 1;
        }
        ref_ima(&pred, &index, code);
        if (i & 1) {
            enc[i / 2] = (uint8_t)(enc[i / 2] | (code << 4));
        } else {
            enc[i / 2] = (uint8_t)code;
        }
    }
    WlImaState st;
    wl_ima_init(&st, 0, 0);
    int16_t dec[2048];
    wl_ima_decode(&st, enc, 2048, dec);
    int32_t worst = 0;
    for (int i = 64; i < 2048; i++) { // skip the step-size ramp-up
        int32_t d = dec[i] - pcm[i];
        d = d < 0 ? -d : d;
        worst = d > worst ? d : worst;
    }
    ASSERT(worst < 600);
    PASS();
}

SUITE(adpcm_suite) {
    RUN_TEST(dsp_matches_reference_on_random_frames);
    RUN_TEST(dsp_decode_many_frames_matches_fixture_decoder);
    RUN_TEST(dsp_partial_frame_start_resumes_mid_frame);
    RUN_TEST(dsp_saturates_instead_of_wrapping);
    RUN_TEST(dsp_predictor_nibble_is_masked_to_three_bits);
    RUN_TEST(ima_known_values);
    RUN_TEST(ima_matches_reference_and_clamps_the_index);
    RUN_TEST(ima_roundtrip_of_a_sine);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(adpcm_suite);
    GREATEST_MAIN_END();
}
