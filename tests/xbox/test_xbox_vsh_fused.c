// SPDX-License-Identifier: MIT
// wl/xbox/vsh: a fused batch (tools/xbox_vsh_pairs.c's, the Wii's), emulated on the host by
// vsh_ps_emulate.py as Broadway runs it (frsqrte's table estimate, stfs cutting a double's
// mantissa), gives the general path's words to the bit: here a sphere map's stage against
// wl_nv2a_sphere_coordinates. A sphere stage that stored its doubles without frsp came out one
// ulp low on the Wii for about half its vertices (Dolphin, 2026-10-16).
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "greatest.h"
#include "wl/xbox/nv2a_texture.h"
#include "wl/xbox/vsh_native.h"

extern const WlXboxVshNative test_vsh_fused_natives[];
extern const unsigned long test_vsh_fused_natives_count;

enum { VERTICES = 64, ROUNDS = 64, WORDS = 5 };

static uint32_t g_seed = 0x2545f491u;

static uint32_t next_random(void) {
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}

// a float in [-1, 1) scaled by 2^(-4 .. 3)
static float random_component(void) {
    float x = (float)(int32_t)next_random() / 2147483648.0f;
    return x * (float)(1u << (next_random() % 8u)) / 16.0f;
}

static void put_le_float(unsigned char *p, float x) {
    uint32_t bits;

    memcpy(&bits, &x, 4);
    p[0] = (unsigned char)bits;
    p[1] = (unsigned char)(bits >> 8);
    p[2] = (unsigned char)(bits >> 16);
    p[3] = (unsigned char)(bits >> 24);
}

static uint32_t float_bits(float x) {
    uint32_t bits;

    memcpy(&bits, &x, 4);
    return bits;
}

static const WlXboxVshFusedForm *sphere_form(void) {
    unsigned long i;

    for (i = 0; i < test_vsh_fused_natives_count; i++) {
        if (test_vsh_fused_natives[i].hash == 0x9aee98b6UL && test_vsh_fused_natives[i].fused_count == 1u) {
            return &test_vsh_fused_natives[i].fused[0];
        }
    }
    return NULL;
}

TEST fused_sphere_stage_matches_the_general_path(void) {
    static float constants[192][4];
    static WlXboxVshFused fused;
    static unsigned char positions[VERTICES * 12], directions[VERTICES * 12];
    static uint32_t elements[VERTICES], words[VERTICES][WORDS];
    static uint32_t *words_made[VERTICES];
    static const double sphere[WL_XBOX_VSH_FUSED_SPHERE] = {1.0e-24, 1.0e76, 1.5, 0.5, 2.5e-13, 2.0};
    const WlXboxVshFusedForm *form = sphere_form();
    unsigned round, v, r, differing = 0, checked = 0;
    char detail[160] = "";

    ASSERT(form != NULL);
    ASSERT_EQ(WL_XBOX_VSH_FUSED_STAGE_SPHERE, form->key.stages[0] >> 2);
    for (r = 0; r < 4; r++) {
        constants[96 + r][r] = 1.0f;
    }
    memset(&fused, 0, sizeof fused);
    memcpy(fused.sphere, sphere, sizeof sphere);
    fused.pack[WL_XBOX_VSH_FUSED_SCALE] = fused.pack[WL_XBOX_VSH_FUSED_SCALE + 1] = 1.0f;
    fused.pack[WL_XBOX_VSH_FUSED_TO_SAMPLES] = fused.pack[WL_XBOX_VSH_FUSED_TO_SAMPLES + 1] = 1.0f;
    fused.pack[WL_XBOX_VSH_FUSED_HALF] = fused.pack[WL_XBOX_VSH_FUSED_HALF + 1] = 0.5f;
    fused.pack[WL_XBOX_VSH_FUSED_DEPTH] = fused.pack[WL_XBOX_VSH_FUSED_DEPTH + 1] = 1.0f;
    fused.pack[WL_XBOX_VSH_FUSED_EDGE] = 0.5f;
    fused.pack[WL_XBOX_VSH_FUSED_EDGE + 1] = 1.0f;
    fused.pack[WL_XBOX_VSH_FUSED_ONES] = fused.pack[WL_XBOX_VSH_FUSED_ONES + 1] = 1.0f;
    for (r = 0; r < 2; r++) {
        for (v = 0; v < 16; v++) {
            fused.inputs[r][v][3] = 1.0f;
        }
    }
    fused.data[0] = positions;
    fused.stride[0] = 12;
    fused.data[9] = directions;
    fused.stride[9] = 12;
    fused.elements = elements;
    fused.words = words_made;
    for (round = 0; round < ROUNDS; round++) {
        for (v = 0; v < VERTICES; v++) {
            float d[3];

            for (r = 0; r < 3; r++) {
                put_le_float(&positions[12 * v + 4 * r], random_component());
                d[r] = random_component();
            }
            // (the edges: a direction too short, one straight behind)
            if (round == 0 && v == 1) {
                d[0] = d[1] = d[2] = 0.0f;
            } else if (round == 0 && v == 2) {
                d[0] = d[1] = 0.0f;
                d[2] = -0.75f;
            }
            for (r = 0; r < 3; r++) {
                put_le_float(&directions[12 * v + 4 * r], d[r]);
            }
            elements[v] = v;
            words_made[v] = words[v];
        }
        memset(words, 0, sizeof words);
        form->run((const float (*)[4])constants, &fused, VERTICES, form->key.live);
        for (v = 0; v < VERTICES; v++) {
            float d[3], st[2];

            for (r = 0; r < 3; r++) {
                const unsigned char *p = &directions[12 * v + 4 * r];
                uint32_t bits = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;

                memcpy(&d[r], &bits, 4);
            }
            wl_nv2a_sphere_coordinates(d, st);
            checked++;
            if (words[v][3] != float_bits(st[0]) || words[v][4] != float_bits(st[1])) {
                if (differing++ == 0) {
                    snprintf(detail, sizeof detail, "round %u vertex %u: fused %08x %08x, general %08x %08x", round, v,
                             (unsigned)words[v][3], (unsigned)words[v][4], (unsigned)float_bits(st[0]),
                             (unsigned)float_bits(st[1]));
                }
            }
        }
    }
    if (differing != 0) {
        printf("  %u of %u vertices differ; the first %s\n", differing, checked, detail);
    }
    ASSERT_EQ(0u, differing);
    PASS();
}

SUITE(xbox_vsh_fused) {
    RUN_TEST(fused_sphere_stage_matches_the_general_path);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(xbox_vsh_fused);
    GREATEST_MAIN_END();
}
