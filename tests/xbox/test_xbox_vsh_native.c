// SPDX-License-Identifier: MIT
// wl/xbox/vsh: the synthetic programs of vsh_programs.inc compiled ahead of time by
// tools/xbox_vsh_codegen.c (vsh_programs.c, built with the test) against the interpreter: for
// random constants and inputs every output the same, bit for bit, run whole, for random sets of the
// outputs a draw reads, for the colours alone, and for the position of four points at once. On
// ppc-be the tool itself runs big-endian under qemu too.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "greatest.h"
#include "wl/xbox/vsh_native.h"

extern const WlXboxVshNative test_vsh_natives[];
extern const unsigned long test_vsh_natives_count;

static const uint32_t microcode[] = {
#include "vsh_programs.inc"
};

enum { PROGRAMS = 8, TRIALS = 200 };

static uint32_t g_rng = 0x1234567u;
static uint32_t rnd(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 8;
}

// -8 to 8 in steps of 1/1024, now and then 0 or a whole number (an index's)
static float random_float(void) {
    uint32_t r = rnd();
    if (r % 16 == 0) {
        return 0.0f;
    }
    if (r % 16 == 1) {
        return (float)(int)(rnd() % 8);
    }
    return (float)((int)(rnd() % 16385) - 8192) / 1024.0f;
}

static float g_constants[WL_XBOX_VSH_CONSTANT_COUNT][4];
static float g_inputs[WL_XBOX_VSH_INPUT_COUNT][4];

static void randomize(void) {
    for (int i = 0; i < WL_XBOX_VSH_CONSTANT_COUNT; i++) {
        for (int c = 0; c < 4; c++) {
            g_constants[i][c] = random_float();
        }
    }
    for (int i = 0; i < WL_XBOX_VSH_INPUT_COUNT; i++) {
        for (int c = 0; c < 4; c++) {
            g_inputs[i][c] = random_float();
        }
    }
    // (v1.x a node's index: rows c[a0 + 10] to c[a0 + 13] within the constants)
    g_inputs[1][0] = (float)(rnd() % 40);
}

// the program at the n-th header of the microcode, its instruction count in *count
static const uint32_t *program_words(int n, unsigned long *count) {
    size_t position = 0;
    for (;;) {
        unsigned long instructions = microcode[position] >> 16;
        if (n-- == 0) {
            *count = instructions;
            return &microcode[position + 1];
        }
        position += 1 + instructions * 4;
    }
}

// the outputs in live the same in both
static bool live_same(const WlXboxVshOutput *a, const WlXboxVshOutput *b, unsigned live) {
    if ((live & WL_XBOX_VSH_LIVE_POSITION) &&
        (memcmp(a->position, b->position, sizeof a->position) ||
         memcmp(a->clip, b->clip, sizeof a->clip) || a->clip_captured != b->clip_captured)) {
        return false;
    }
    if ((live & WL_XBOX_VSH_LIVE_D0) && memcmp(a->d0, b->d0, sizeof a->d0)) {
        return false;
    }
    if ((live & WL_XBOX_VSH_LIVE_D1) && memcmp(a->d1, b->d1, sizeof a->d1)) {
        return false;
    }
    if ((live & WL_XBOX_VSH_LIVE_BACK) &&
        (memcmp(a->b0, b->b0, sizeof a->b0) || memcmp(a->b1, b->b1, sizeof a->b1))) {
        return false;
    }
    if ((live & WL_XBOX_VSH_LIVE_POINT_SIZE) &&
        memcmp(&a->point_size, &b->point_size, sizeof a->point_size)) {
        return false;
    }
    for (int stage = 0; stage < 4; stage++) {
        if ((live & ((unsigned)WL_XBOX_VSH_LIVE_T0 << stage)) &&
            memcmp(a->t[stage], b->t[stage], sizeof a->t[stage])) {
            return false;
        }
    }
    if ((live & WL_XBOX_VSH_LIVE_FOG) && memcmp(&a->fog, &b->fog, sizeof a->fog)) {
        return false;
    }
    return true;
}

static WlXboxVshProgram g_native, g_interpreted;

TEST every_program_has_its_function(void) {
    ASSERT_EQ((unsigned long)PROGRAMS, test_vsh_natives_count);
    for (int n = 0; n < PROGRAMS; n++) {
        unsigned long count;
        const uint32_t *words = program_words(n, &count);
        wl_xbox_vsh_compile(words, count, test_vsh_natives, test_vsh_natives_count, &g_native);
        wl_xbox_vsh_compile(words, count, NULL, 0, &g_interpreted);
        ASSERT_EQ(count, g_native.count);
        ASSERT(g_native.native != NULL);
        ASSERT(g_native.native_colors != NULL);
        ASSERT(g_interpreted.native == NULL);
        ASSERT_EQ(wl_xbox_vsh_hash(words, count), g_native.hash);
    }
    PASS();
}

TEST functions_match_the_interpreter(void) {
    for (int n = 0; n < PROGRAMS; n++) {
        unsigned long count;
        const uint32_t *words = program_words(n, &count);
        wl_xbox_vsh_compile(words, count, test_vsh_natives, test_vsh_natives_count, &g_native);
        wl_xbox_vsh_compile(words, count, NULL, 0, &g_interpreted);
        for (int trial = 0; trial < TRIALS; trial++) {
            WlXboxVshOutput want, got;
            randomize();
            wl_xbox_vsh_run(&g_interpreted, (const float (*)[4])g_constants,
                            (const float (*)[4])g_inputs, 2.5f, &want);
            wl_xbox_vsh_run(&g_native, (const float (*)[4])g_constants,
                            (const float (*)[4])g_inputs, 2.5f, &got);
            ASSERTm("run", live_same(&want, &got, WL_XBOX_VSH_LIVE_ALL));
            // a draw's outputs alone, the rest left as they were
            unsigned live = rnd() & WL_XBOX_VSH_LIVE_ALL;
            memset(&got, 0x7F, sizeof got);
            wl_xbox_vsh_run_live(&g_native, (const float (*)[4])g_constants,
                                 (const float (*)[4])g_inputs, 2.5f, &got, live);
            ASSERTm("run_live", live_same(&want, &got, live));
            // (the same, for three vertices at once)
            {
                static float inputs[3][WL_XBOX_VSH_INPUT_COUNT][4];
                static WlXboxVshOutput outs[3];
                static WlXboxVshWork work;

                for (int m = 0; m < 3; m++) {
                    memcpy(inputs[m], g_inputs, sizeof inputs[m]);
                    memset(&outs[m], 0x7F, sizeof outs[m]);
                }
                ASSERT(g_native.native_batch != NULL);
                wl_xbox_vsh_run_batch(&g_native, (const float (*)[4])g_constants,
                                      (const float (*)[WL_XBOX_VSH_INPUT_COUNT][4])inputs, 3, 2.5f, outs, live,
                                      &work);
                for (int m = 0; m < 3; m++) {
                    ASSERTm("run_batch", live_same(&want, &outs[m], live));
                }
            }
            // (four vertices that share the nodes and weights (v5, v6) but for the third, their
            // other inputs their own: each as the interpreter makes it, whether a skinned
            // program's prefix was run or taken from the vertex before)
            {
                static float inputs[4][WL_XBOX_VSH_INPUT_COUNT][4];
                static WlXboxVshOutput outs[4], one;
                static WlXboxVshWork work;

                for (int m = 0; m < 4; m++) {
                    memcpy(inputs[m], g_inputs, sizeof inputs[m]);
                    for (int i = 0; i < WL_XBOX_VSH_INPUT_COUNT; i++) {
                        if (i != 5 && i != 6) {
                            for (int c = 0; c < 4; c++) {
                                inputs[m][i][c] = random_float();
                            }
                        }
                    }
                    memset(&outs[m], 0x7F, sizeof outs[m]);
                }
                inputs[2][6][0] = random_float();
                wl_xbox_vsh_run_batch(&g_native, (const float (*)[4])g_constants,
                                      (const float (*)[WL_XBOX_VSH_INPUT_COUNT][4])inputs, 4, 2.5f, outs, live,
                                      &work);
                for (int m = 0; m < 4; m++) {
                    wl_xbox_vsh_run(&g_interpreted, (const float (*)[4])g_constants,
                                    (const float (*)[4])inputs[m], 2.5f, &one);
                    ASSERTm("run_batch shared nodes", live_same(&one, &outs[m], live));
                }
            }
            memset(&got, 0, sizeof got);
            wl_xbox_vsh_run_colors(&g_native, (const float (*)[4])g_constants,
                                   (const float (*)[4])g_inputs, &got);
            ASSERTm("run_colors",
                    live_same(&want, &got, WL_XBOX_VSH_LIVE_D0 | WL_XBOX_VSH_LIVE_D1));
        }
    }
    PASS();
}

TEST positions_of_four_points_match_four_runs(void) {
    int with_positions = 0;
    for (int n = 0; n < PROGRAMS; n++) {
        unsigned long count;
        const uint32_t *words = program_words(n, &count);
        wl_xbox_vsh_compile(words, count, test_vsh_natives, test_vsh_natives_count, &g_native);
        wl_xbox_vsh_compile(words, count, NULL, 0, &g_interpreted);
        float points[4][4], clips[4][4], positions[4][4];
        if (!g_native.native_positions) {
            ASSERT_EQ(0, wl_xbox_vsh_run_positions(&g_native, (const float (*)[4])g_constants,
                                                   (const float (*)[4])g_inputs,
                                                   (const float (*)[4])points, clips, positions));
            continue;
        }
        with_positions++;
        for (int trial = 0; trial < TRIALS; trial++) {
            randomize();
            for (int p = 0; p < 4; p++) {
                for (int c = 0; c < 4; c++) {
                    points[p][c] = random_float();
                }
            }
            ASSERT_EQ(1, wl_xbox_vsh_run_positions(&g_native, (const float (*)[4])g_constants,
                                                   (const float (*)[4])g_inputs,
                                                   (const float (*)[4])points, clips, positions));
            for (int p = 0; p < 4; p++) {
                WlXboxVshOutput want;
                memcpy(g_inputs[0], points[p], sizeof points[p]);
                wl_xbox_vsh_run(&g_interpreted, (const float (*)[4])g_constants,
                                (const float (*)[4])g_inputs, 1.0f, &want);
                ASSERT_MEM_EQ(want.clip, clips[p], sizeof want.clip);
                ASSERT_MEM_EQ(want.position, positions[p], sizeof want.position);
            }
        }
    }
    // (the skinned program's: constants by a0, converted to the screen)
    ASSERT_EQ(1, with_positions);
    PASS();
}

SUITE(vsh_native_suite) {
    RUN_TEST(every_program_has_its_function);
    RUN_TEST(functions_match_the_interpreter);
    RUN_TEST(positions_of_four_points_match_four_runs);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(vsh_native_suite);
    GREATEST_MAIN_END();
}
