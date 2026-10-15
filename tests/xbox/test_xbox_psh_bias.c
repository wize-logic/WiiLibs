// SPDX-License-Identifier: MIT
// wl/xbox/psh_bias: the combiners' signed sums made unsigned. Random colour programs (synthetic:
// half biases of the textures, the colours and the constants, the constant 1/2, signed reads of the
// registers, sums kept, halved or biased) are evaluated as the NV2A evaluates them (signed
// registers within [-1, 1]); each one written again is evaluated in the translation's model (every
// mapping and register clamped to [0, 1]) and must give the same result. Then the screen blur that
// Halo draws (the mean of four half-biased taps, the half put back), and programs it must leave
// alone.
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "greatest.h"
#include "wl/xbox/psh_bias.h"

static uint32_t g_rng;
static uint32_t rnd(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 8;
}

static float frand(void) {
    return (float)(rnd() % 256u) / 255.0f;
}

static float clamp(float x, float lo, float hi) {
    return x < lo ? lo : x > hi ? hi : x;
}

// a mapped input: the NV2A's (signed), or the translation's (clamped to [0, 1])
static float map(float x, unsigned mapping, bool clamped) {
    float p = x > 0.0f ? x : 0.0f, v;

    switch (mapping) {
    case 0x00:
        v = p;
        break;
    case 0x20:
        v = 1.0f - clamp(x, 0.0f, 1.0f);
        break;
    case 0x40:
        v = 2.0f * p - 1.0f;
        break;
    case 0x60:
        v = 1.0f - 2.0f * p;
        break;
    case 0x80:
        v = p - 0.5f;
        break;
    case 0xa0:
        v = 0.5f - p;
        break;
    case 0xc0:
        v = x;
        break;
    default:
        v = -x;
        break;
    }
    return clamped ? clamp(v, 0.0f, 1.0f) : v;
}

static float output(float x, unsigned flags, bool clamped) {
    switch (flags & 0x38u) {
    case 0x08:
        x -= 0.5f;
        break;
    case 0x10:
        x *= 2.0f;
        break;
    case 0x18:
        x = (x - 0.5f) * 2.0f;
        break;
    case 0x20:
        x *= 4.0f;
        break;
    case 0x30:
        x *= 0.5f;
        break;
    default:
        break;
    }
    return clamped ? clamp(x, 0.0f, 1.0f) : clamp(x, -1.0f, 1.0f);
}

// one channel of a program of sums (no dot products, no mux): the final combiner's D, or r0
static float evaluate(const uint32_t *s, const float *inputs, bool clamped) {
    float reg[16];
    unsigned count = s[WL_XBOX_PSH_COMBINER_COUNT] & 0xffu, stage, i;

    memcpy(reg, inputs, sizeof reg);
    for (stage = 0; stage < count; stage++) {
        uint32_t in = s[WL_XBOX_PSH_RGB_INPUTS0 + stage], out = s[WL_XBOX_PSH_RGB_OUTPUTS0 + stage];
        float v[4], next[16];
        unsigned flags = out >> 12;

        for (i = 0; i < 4; i++) {
            unsigned byte = (in >> (24 - 8 * i)) & 0xffu;

            v[i] = map(reg[byte & 0x0f], byte & 0xe0, clamped);
        }
        memcpy(next, reg, sizeof next);
        next[(out >> 4) & 0x0f] = output(v[0] * v[1], flags, clamped);
        next[out & 0x0f] = output(v[2] * v[3], flags, clamped);
        next[(out >> 8) & 0x0f] = output(v[0] * v[1] + v[2] * v[3], flags, clamped);
        next[0] = 0.0f;
        memcpy(reg, next, sizeof reg);
    }
    return clamp(reg[s[WL_XBOX_PSH_FINAL_INPUTS_ABCD] & 0x0f] > 0.0f
                     ? reg[s[WL_XBOX_PSH_FINAL_INPUTS_ABCD] & 0x0f]
                     : 0.0f,
                 0.0f, 1.0f);
}

static void inputs_random(float *inputs) {
    unsigned i;

    memset(inputs, 0, 16 * sizeof *inputs);
    for (i = 1; i <= 5; i++) {
        inputs[i] = frand(); // c0, c1, fog, v0, v1
    }
    for (i = 8; i <= 11; i++) {
        inputs[i] = frand(); // t0..t3
    }
}

// whether the program written again gives the NV2A's results
static bool agrees(const uint32_t *original, const uint32_t *unbiased) {
    float inputs[16];
    int trial;

    for (trial = 0; trial < 64; trial++) {
        inputs_random(inputs);
        if (fabsf(evaluate(original, inputs, false) - evaluate(unbiased, inputs, true)) > 1.0e-4f) {
            return false;
        }
    }
    return true;
}

// an input of the family: mostly a half bias of a source, 1/2, a signed read of a register written
static unsigned input_random(unsigned written) {
    static const unsigned sources[] = {0x01, 0x02, 0x04, 0x05, 0x08, 0x09, 0x0a, 0x0b, 0x00};
    unsigned pick = rnd() % 8u;

    if (pick < 3u) {
        return 0x80u | sources[rnd() % 9u];
    }
    if (pick == 3u) {
        return 0xa0u;
    }
    if (pick < 6u && written) {
        return 0xc0u | ((written & 1u) && (!(written & 2u) || rnd() % 2u) ? 0x0cu : 0x0du);
    }
    if (pick == 6u) {
        return sources[rnd() % 8u];
    }
    return 0x00u;
}

static void program_random(uint32_t *s) {
    static const unsigned ops[] = {0x00, 0x30, 0x30, 0x08};
    unsigned count = 1u + rnd() % 6u, stage, written = 0;

    memset(s, 0, WL_XBOX_PSH_WORDS * sizeof *s);
    s[WL_XBOX_PSH_COMBINER_COUNT] = count;
    for (stage = 0; stage < count; stage++) {
        unsigned a = input_random(written), c = input_random(written),
                 dest = rnd() % 2u ? 0x0cu : 0x0du;
        unsigned b = 0x20u, d = 0x20u;

        // (now and then a product of two unsigned inputs, which it leaves as it is, or of a
        // register's signed read, which it cannot follow; or a product's own output)
        if (rnd() % 6u == 0) {
            a = rnd() % 4u == 0 && written ? 0xc0u | (written & 1u ? 0x0cu : 0x0du)
                                           : 0x08u + rnd() % 4u;
            b = 0x04u + rnd() % 2u;
        }
        s[WL_XBOX_PSH_RGB_INPUTS0 + stage] = a << 24 | b << 16 | c << 8 | d;
        if (rnd() % 5u == 0) {
            s[WL_XBOX_PSH_RGB_OUTPUTS0 + stage] = ops[rnd() % 4u] << 12 | dest << 4;
        } else {
            s[WL_XBOX_PSH_RGB_OUTPUTS0 + stage] = ops[rnd() % 4u] << 12 | dest << 8;
        }
        written |= dest == 0x0cu ? 1u : 2u;
    }
    s[WL_XBOX_PSH_FINAL_INPUTS_ABCD] = written & 1u ? 0x0cu : 0x0du;
}

TEST random_programs_written_again_agree_with_the_nv2a(void) {
    uint32_t s[WL_XBOX_PSH_WORDS], out[WL_XBOX_PSH_WORDS];
    int n, accepted = 0, differed = 0;

    g_rng = 0xB1A5u;
    for (n = 0; n < 20000; n++) {
        program_random(s);
        if (!wl_xbox_psh_unbias(s, out)) {
            continue;
        }
        accepted++;
        if (!agrees(s, out)) {
            differed++;
        }
        // (what the translation clamps the program written again does not reach)
        ASSERT(agrees(out, out));
    }
    ASSERT_EQ(0, differed);
    ASSERT(accepted > 1000);
    PASS();
}

// Halo's blur: ((t0 - 1/2) + (t1 - 1/2)) / 2 into r0, the same of t2 and t3 into r1, (r0 + r1) / 2
// signed into r0, r0 + 1/2 (a half bias negate of 0): the mean of the four
static void blur(uint32_t *s) {
    memset(s, 0, WL_XBOX_PSH_WORDS * sizeof *s);
    s[WL_XBOX_PSH_COMBINER_COUNT] = 0x00011004u;
    s[WL_XBOX_PSH_RGB_INPUTS0 + 0] = 0x88208920u;
    s[WL_XBOX_PSH_RGB_INPUTS0 + 1] = 0x8a208b20u;
    s[WL_XBOX_PSH_RGB_INPUTS0 + 2] = 0xcc20cd20u;
    s[WL_XBOX_PSH_RGB_INPUTS0 + 3] = 0xcc20a020u;
    s[WL_XBOX_PSH_RGB_OUTPUTS0 + 0] = 0x00030c00u;
    s[WL_XBOX_PSH_RGB_OUTPUTS0 + 1] = 0x00030d00u;
    s[WL_XBOX_PSH_RGB_OUTPUTS0 + 2] = 0x00030c00u;
    s[WL_XBOX_PSH_RGB_OUTPUTS0 + 3] = 0x00000c00u;
    s[WL_XBOX_PSH_FINAL_INPUTS_ABCD] = 0x0000000cu;
}

TEST the_screen_blur_is_the_mean_of_its_taps(void) {
    uint32_t s[WL_XBOX_PSH_WORDS], out[WL_XBOX_PSH_WORDS];
    float inputs[16];

    blur(s);
    ASSERT(wl_xbox_psh_unbias(s, out));
    // (t0 + t1) / 2, (t2 + t3) / 2, their mean, plus 0
    ASSERT_EQ_FMT(0x08200920u, out[WL_XBOX_PSH_RGB_INPUTS0 + 0], "%08x");
    ASSERT_EQ_FMT(0x0a200b20u, out[WL_XBOX_PSH_RGB_INPUTS0 + 1], "%08x");
    ASSERT_EQ_FMT(0x0c200d20u, out[WL_XBOX_PSH_RGB_INPUTS0 + 2], "%08x");
    ASSERT_EQ_FMT(0x0c200020u, out[WL_XBOX_PSH_RGB_INPUTS0 + 3], "%08x");
    ASSERT_EQ_FMT(s[WL_XBOX_PSH_RGB_OUTPUTS0 + 3], out[WL_XBOX_PSH_RGB_OUTPUTS0 + 3], "%08x");
    ASSERT(agrees(s, out));
    // a dark picture: the NV2A's mean, where the clamped half biases gave a flat 1/2
    memset(inputs, 0, sizeof inputs);
    inputs[8] = 0.1f;
    inputs[9] = 0.2f;
    inputs[10] = 0.0f;
    inputs[11] = 0.3f;
    ASSERT_IN_RANGE(0.15f, evaluate(out, inputs, true), 1.0e-4f);
    ASSERT_IN_RANGE(0.5f, evaluate(s, inputs, true), 1.0e-4f);
    PASS();
}

TEST programs_it_cannot_follow_are_left_alone(void) {
    uint32_t s[WL_XBOX_PSH_WORDS], out[WL_XBOX_PSH_WORDS];

    // no half bias: nothing to do
    blur(s);
    s[WL_XBOX_PSH_RGB_INPUTS0 + 0] = 0x08200920u;
    s[WL_XBOX_PSH_RGB_INPUTS0 + 1] = 0x0a200b20u;
    s[WL_XBOX_PSH_RGB_INPUTS0 + 3] = 0x0c200020u;
    s[WL_XBOX_PSH_RGB_INPUTS0 + 2] = 0x0c200d20u;
    ASSERT_FALSE(wl_xbox_psh_unbias(s, out));
    // the half not put back: the final combiner would read r0 below 0
    blur(s);
    s[WL_XBOX_PSH_COMBINER_COUNT] = 0x00011003u;
    ASSERT_FALSE(wl_xbox_psh_unbias(s, out));
    // a register with an offset times another input
    blur(s);
    s[WL_XBOX_PSH_RGB_INPUTS0 + 2] = 0xcc08cd20u;
    ASSERT_FALSE(wl_xbox_psh_unbias(s, out));
    // a sum of two unhalved values that may pass 1
    blur(s);
    s[WL_XBOX_PSH_RGB_OUTPUTS0 + 0] = 0x00000c00u;
    ASSERT_FALSE(wl_xbox_psh_unbias(s, out));
    // the alpha half reading a colour with an offset as its blue
    blur(s);
    s[WL_XBOX_PSH_ALPHA_INPUTS0 + 2] = 0x0c200000u;
    ASSERT_FALSE(wl_xbox_psh_unbias(s, out));
    // a dot product in a stage it writes again
    blur(s);
    s[WL_XBOX_PSH_RGB_OUTPUTS0 + 0] |= 0x2000u;
    ASSERT_FALSE(wl_xbox_psh_unbias(s, out));
    PASS();
}

SUITE(psh_bias_suite) {
    RUN_TEST(random_programs_written_again_agree_with_the_nv2a);
    RUN_TEST(the_screen_blur_is_the_mean_of_its_taps);
    RUN_TEST(programs_it_cannot_follow_are_left_alone);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(psh_bias_suite);
    GREATEST_MAIN_END();
}
