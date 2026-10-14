// SPDX-License-Identifier: MIT
// wl/xbox/vsh: NV2A vertex programs decoded and interpreted (the synthetic programs of
// vsh_programs.inc, with whole-number inputs so that every result is exact), the analysis of what
// each output depends on, the hash that finds a compiled function, and the arithmetic the compiled
// functions share with the interpreter (vsh_native.h).
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "greatest.h"
#include "wl/xbox/vsh_native.h"

static const uint32_t microcode[] = {
#include "vsh_programs.inc"
};

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

static WlXboxVshProgram g_program;
static WlXboxVshDependencies g_dependencies;
static float g_constants[WL_XBOX_VSH_CONSTANT_COUNT][4];
static float g_inputs[WL_XBOX_VSH_INPUT_COUNT][4];

static void compile(int n) {
    unsigned long count;
    const uint32_t *words = program_words(n, &count);
    wl_xbox_vsh_compile(words, count, NULL, 0, &g_program);
}

static void set4(float out[4], float x, float y, float z, float w) {
    out[0] = x;
    out[1] = y;
    out[2] = z;
    out[3] = w;
}

// c[n] (n from -96)
static float *constant(int n) {
    return g_constants[n + WL_XBOX_VSH_CONSTANT_BIAS];
}

static void run(WlXboxVshOutput *output) {
    wl_xbox_vsh_run(&g_program, (const float (*)[4])g_constants, (const float (*)[4])g_inputs, 3.0f,
                    output);
}

// ---- decoding and the hash ---------------------------------------------------------------------

TEST compile_decodes_up_to_the_end_bit(void) {
    unsigned long count;
    const uint32_t *words = program_words(0, &count);
    ASSERT_EQ(9ul, count);
    // (more words given than the program has: it ends at its last instruction's end bit)
    wl_xbox_vsh_compile(words, count + 5, NULL, 0, &g_program);
    ASSERT_EQ(9ul, g_program.count);
    ASSERT(g_program.native == NULL && g_program.native_colors == NULL &&
           g_program.native_positions == NULL);
    // dp4 oPos.x, v0, c[0]
    const WlXboxVshInstruction *dp4 = &g_program.instructions[0];
    ASSERT_EQ(8u, dp4->output_mask);
    ASSERT_EQ(1u, dp4->output_is_register);
    ASSERT_EQ(0u, dp4->output_address);
    ASSERT_EQ(0u, dp4->input);
    ASSERT_EQ(96u, dp4->constant);
    ASSERT_EQ(2u, dp4->operands[0].mux);
    ASSERT_EQ(3u, dp4->operands[1].mux);
    for (int c = 0; c < 4; c++) {
        ASSERT_EQ((unsigned)c, dp4->operands[0].swizzle[c]);
    }
    PASS();
}

TEST the_hash_is_fnv1a_of_the_words(void) {
    unsigned long count;
    const uint32_t *words = program_words(1, &count);
    uint32_t hash = 2166136261u;
    for (unsigned long i = 0; i < count * 4; i++) {
        hash = (hash ^ words[i]) * 16777619u;
    }
    ASSERT_EQ((unsigned long)hash, wl_xbox_vsh_hash(words, count));
    wl_xbox_vsh_compile(words, count, NULL, 0, &g_program);
    ASSERT_EQ((unsigned long)hash, g_program.hash);
    PASS();
}

static void native_run(const float (*constants)[4], const float (*inputs)[4],
                       WlXboxVshOutput *output, unsigned live) {
    (void)constants;
    (void)inputs;
    (void)live;
    output->fog = 42.0f;
}

static void native_colors(const float (*constants)[4], const float (*inputs)[4],
                          WlXboxVshOutput *output) {
    (void)constants;
    (void)inputs;
    output->d0[0] = 7.0f;
}

TEST compile_finds_a_function_by_hash_and_count(void) {
    unsigned long count;
    const uint32_t *words = program_words(0, &count);
    WlXboxVshNative natives[2] = {
        {wl_xbox_vsh_hash(words, count), count + 1, native_run, native_colors, NULL, NULL},
        {wl_xbox_vsh_hash(words, count), count, native_run, native_colors, NULL, NULL},
    };
    wl_xbox_vsh_compile(words, count, natives, 1, &g_program); // (the count differs)
    ASSERT(g_program.native == NULL);
    wl_xbox_vsh_compile(words, count, natives, 2, &g_program);
    ASSERT(g_program.native == native_run);
    ASSERT(g_program.native_colors == native_colors);
    WlXboxVshOutput output;
    run(&output);
    ASSERT_EQ(42.0f, output.fog);
    ASSERT_EQ(3.0f, output.point_size); // (cleared first, the point size given)
    ASSERT_EQ(1.0f, output.d0[3]);
    memset(&output, 0, sizeof output);
    wl_xbox_vsh_run_colors(&g_program, (const float (*)[4])g_constants,
                           (const float (*)[4])g_inputs, &output);
    ASSERT_EQ(7.0f, output.d0[0]);
    PASS();
}

// ---- the interpreter ---------------------------------------------------------------------------

TEST a_transform_runs_exactly(void) {
    compile(0);
    memset(g_constants, 0, sizeof g_constants);
    memset(g_inputs, 0, sizeof g_inputs);
    set4(constant(0), 1, 2, 0, 0);
    set4(constant(1), 0, 1, 0, 3);
    set4(constant(2), 0, 0, 2, 0);
    set4(constant(3), 0, 0, 0, 1);
    set4(constant(4), 2, 3, 4, 0.5f);
    set4(constant(5), 2, 4, 6, 8);
    set4(constant(6), 1, 1, 1, 1);
    set4(g_inputs[0], 3, 4, 5, 1);
    set4(g_inputs[3], 0.25f, 0.5f, 0.75f, 1);
    set4(g_inputs[9], 6, 7, 8, 9);
    WlXboxVshOutput output;
    run(&output);
    ASSERT_EQ(11.0f, output.position[0]);
    ASSERT_EQ(7.0f, output.position[1]);
    ASSERT_EQ(10.0f, output.position[2]);
    ASSERT_EQ(1.0f, output.position[3]);
    ASSERT_EQ(0, output.clip_captured);
    ASSERT_MEM_EQ(g_inputs[3], output.d0, sizeof output.d0);
    ASSERT_EQ(0.5f, output.d1[0]);
    ASSERT_EQ(1.5f, output.d1[1]);
    ASSERT_EQ(3.0f, output.d1[2]);
    ASSERT_EQ(0.5f, output.d1[3]);
    ASSERT_MEM_EQ(g_inputs[9], output.t[0], sizeof output.t[0]);
    // mad oT1.xy, v9.yx, c[5], c[6]: 7 * 2 + 1, 6 * 4 + 1; z and w as a program leaves them
    ASSERT_EQ(15.0f, output.t[1][0]);
    ASSERT_EQ(25.0f, output.t[1][1]);
    ASSERT_EQ(0.0f, output.t[1][2]);
    ASSERT_EQ(1.0f, output.t[1][3]);
    // (outputs the program leaves)
    ASSERT_EQ(1.0f, output.fog);
    ASSERT_EQ(3.0f, output.point_size);
    ASSERT_EQ(1.0f, output.b0[3]);
    PASS();
}

TEST the_screen_conversion_captures_the_clip_position(void) {
    compile(1);
    memset(g_constants, 0, sizeof g_constants);
    memset(g_inputs, 0, sizeof g_inputs);
    set4(constant(0), 1, 0, 0, 0);
    set4(constant(1), 0, 1, 0, 0);
    set4(constant(2), 0, 0, 1, 0);
    set4(constant(3), 0, 0, 0, 1);
    set4(constant(-38), 320, -240, 1, 0);
    set4(constant(-37), 320, 240, 0, 0);
    set4(g_inputs[0], 2, 4, 1, 4);
    WlXboxVshOutput output;
    run(&output);
    ASSERT_EQ(1, output.clip_captured);
    ASSERT_MEM_EQ(g_inputs[0], output.clip, sizeof output.clip);
    // x / w * 320 + 320, y / w * -240 + 240, z / w
    ASSERT_EQ(480.0f, output.position[0]);
    ASSERT_EQ(0.0f, output.position[1]);
    ASSERT_EQ(0.25f, output.position[2]);
    ASSERT_EQ(4.0f, output.position[3]);
    ASSERT_EQ(0.25f, output.fog);
    PASS();
}

TEST relative_constants_follow_a0(void) {
    compile(2);
    memset(g_constants, 0, sizeof g_constants);
    memset(g_inputs, 0, sizeof g_inputs);
    for (int i = 0; i < 4; i++) {
        constant(i)[i] = 1.0f;
    }
    set4(constant(-38), 1, 1, 1, 0);
    set4(constant(7), 0, 0, 0, 1);
    // node 5's rows (c[15] to c[17]): a translation by (1, 2, 3); its normal row c[18]
    set4(constant(15), 1, 0, 0, 1);
    set4(constant(16), 0, 1, 0, 2);
    set4(constant(17), 0, 0, 1, 3);
    set4(constant(18), 0, 0, 2, 0);
    set4(g_inputs[0], 1, 1, 1, 1);
    set4(g_inputs[1], 5.0f, 2.0f, 0, 0);
    set4(g_inputs[2], 0, 0, 0.5f, 0);
    WlXboxVshOutput output;
    run(&output);
    // (1 + 1, 1 + 2, 1 + 3) by the weight 2, w 1
    ASSERT_EQ(4.0f, output.clip[0]);
    ASSERT_EQ(6.0f, output.clip[1]);
    ASSERT_EQ(8.0f, output.clip[2]);
    ASSERT_EQ(1.0f, output.clip[3]);
    ASSERT_EQ(1.0f, output.d0[0]);
    // (an index past the constants reads the last)
    set4(g_inputs[1], 1000.0f, 1.0f, 0, 0);
    run(&output);
    ASSERT_EQ(0.0f, output.clip[0]);
    PASS();
}

TEST the_ilu_operations(void) {
    float out[4];
    // EXP: 2^floor(x), x - floor(x), 2^x, 1
    wl_xbox_vsh_ilu(5, (const float[4]){3.5f, 0, 0, 0}, out);
    ASSERT_EQ(8.0f, out[0]);
    ASSERT_EQ(0.5f, out[1]);
    ASSERT_EQ(exp2f(3.5f), out[2]);
    ASSERT_EQ(1.0f, out[3]);
    // LOG: the exponent, the mantissa, log2 |x|, 1; of 0, -1e30 and 1
    wl_xbox_vsh_ilu(6, (const float[4]){-12.0f, 0, 0, 0}, out);
    ASSERT_EQ(3.0f, out[0]);
    ASSERT_EQ(1.5f, out[1]);
    ASSERT_EQ(log2f(12.0f), out[2]);
    wl_xbox_vsh_ilu(6, (const float[4]){0.0f, 0, 0, 0}, out);
    ASSERT_EQ(-1.0e30f, out[0]);
    ASSERT_EQ(1.0f, out[1]);
    // LIT: 1, max(n.l, 0), (n.h)^power where n.l > 0, 1
    wl_xbox_vsh_ilu(7, (const float[4]){0.5f, 0.25f, 0, 2.0f}, out);
    ASSERT_EQ(1.0f, out[0]);
    ASSERT_EQ(0.5f, out[1]);
    ASSERT_EQ(0.0625f, out[2]);
    wl_xbox_vsh_ilu(7, (const float[4]){-0.5f, 0.25f, 0, 2.0f}, out);
    ASSERT_EQ(0.0f, out[1]);
    ASSERT_EQ(0.0f, out[2]);
    PASS();
}

TEST shared_arithmetic(void) {
    ASSERT_EQ(1.884467e+19f, wl_xbox_vsh_reciprocal_clamped(0.0f));
    ASSERT_EQ(-1.884467e+19f, wl_xbox_vsh_reciprocal_clamped(-0.0f));
    ASSERT_EQ(5.42101e-20f, wl_xbox_vsh_reciprocal_clamped(1.0e30f));
    ASSERT_EQ(0.25f, wl_xbox_vsh_reciprocal_clamped(4.0f));
    ASSERT_EQ(0, wl_xbox_vsh_clamp(-3));
    ASSERT_EQ(WL_XBOX_VSH_CONSTANT_COUNT - 1, wl_xbox_vsh_clamp(500));
    for (int i = -4096; i <= 4096; i++) {
        float x = (float)i / 64.0f;
        ASSERT_EQ((int)floorf(x), wl_xbox_vsh_floor_index(x));
        ASSERT_EQ((int)floorf(nextafterf(x, -1.0e30f)),
                  wl_xbox_vsh_floor_index(nextafterf(x, -1.0e30f)));
        ASSERT_EQ((int)floorf(nextafterf(x, 1.0e30f)),
                  wl_xbox_vsh_floor_index(nextafterf(x, 1.0e30f)));
    }
    ASSERT_EQ(sqrtf(2.0f), wl_xbox_vsh_sqrtf(2.0f));
    PASS();
}

// ---- the analysis ------------------------------------------------------------------------------

TEST the_analysis_of_a_transform(void) {
    compile(0);
    wl_xbox_vsh_analyze(&g_program, &g_dependencies);
    for (int c = 0; c < 4; c++) {
        ASSERT_EQ(1ul, g_dependencies.position[c]); // v0, affinely
        ASSERT_EQ(1ul << 3, g_dependencies.d0[c]);
        ASSERT_EQ(1ul << 3, g_dependencies.d1[c]);
        ASSERT_EQ(1ul << 9, g_dependencies.t[0][c]);
    }
    ASSERT_EQ(1ul << 9, g_dependencies.t[1][0]);
    ASSERT_EQ(0ul, g_dependencies.t[1][2]); // (not written)
    ASSERT_EQ(1ul | 1ul << 3 | 1ul << 9, g_dependencies.inputs);
    ASSERT_EQ(0, g_dependencies.relative);
    ASSERT_EQ(0, g_dependencies.clip_captured);
    // c[0] to c[6] read
    ASSERT_EQ(0x7Ful << (96 % 32), g_dependencies.constants[96 / 32]);
    ASSERT_EQ(1ul << 3, wl_xbox_vsh_live_inputs(&g_dependencies, WL_XBOX_VSH_LIVE_D0));
    ASSERT_EQ(1ul | 1ul << 9,
              wl_xbox_vsh_live_inputs(&g_dependencies,
                                      WL_XBOX_VSH_LIVE_POSITION | (WL_XBOX_VSH_LIVE_T0 << 1)));
    // which outputs read constants: d0 and t0 are the vertex's alone
    wl_xbox_vsh_analyze_constants(&g_program, &g_dependencies);
    ASSERT(g_dependencies.position[0] & WL_XBOX_VSH_READS_CONSTANT);
    ASSERT(g_dependencies.d1[0] & WL_XBOX_VSH_READS_CONSTANT);
    ASSERT(!(g_dependencies.d0[0] & WL_XBOX_VSH_READS_CONSTANT));
    ASSERT(!(g_dependencies.t[0][0] & WL_XBOX_VSH_READS_CONSTANT));
    PASS();
}

TEST the_analysis_finds_what_is_not_affine(void) {
    compile(2); // skinned: rows by a0, times v1.y, divided by w
    wl_xbox_vsh_analyze(&g_program, &g_dependencies);
    ASSERT_EQ(1, g_dependencies.relative);
    ASSERT_EQ(1, g_dependencies.clip_captured);
    ASSERT(g_dependencies.position[0] & WL_XBOX_VSH_NOT_AFFINE);
    ASSERT(g_dependencies.clip[0] & WL_XBOX_VSH_NOT_AFFINE);
    // with v1 the same for every vertex (one node, one weight), the clip position is affine in v0
    wl_xbox_vsh_analyze_uniform(&g_program, 1ul << 1, &g_dependencies);
    ASSERT_EQ(1ul, g_dependencies.clip[0]);
    ASSERT_EQ(1ul << 2, g_dependencies.d0[0]);
    compile(3); // lighting: a product of two inputs
    wl_xbox_vsh_analyze(&g_program, &g_dependencies);
    ASSERT(g_dependencies.t[0][0] & WL_XBOX_VSH_NOT_AFFINE);
    ASSERT(!(g_dependencies.position[0] & WL_XBOX_VSH_NOT_AFFINE));
    PASS();
}

SUITE(vsh_suite) {
    RUN_TEST(compile_decodes_up_to_the_end_bit);
    RUN_TEST(the_hash_is_fnv1a_of_the_words);
    RUN_TEST(compile_finds_a_function_by_hash_and_count);
    RUN_TEST(a_transform_runs_exactly);
    RUN_TEST(the_screen_conversion_captures_the_clip_position);
    RUN_TEST(relative_constants_follow_a0);
    RUN_TEST(the_ilu_operations);
    RUN_TEST(shared_arithmetic);
    RUN_TEST(the_analysis_of_a_transform);
    RUN_TEST(the_analysis_finds_what_is_not_affine);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(vsh_suite);
    GREATEST_MAIN_END();
}
