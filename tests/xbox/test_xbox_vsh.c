// SPDX-License-Identifier: MIT
// wl/xbox/vsh: NV2A vertex programs decoded and interpreted (the synthetic programs of
// vsh_programs.inc, with whole-number inputs so that every result is exact), the analysis of what
// each output depends on, the hash that finds a compiled function, and the arithmetic the compiled
// functions share with the interpreter (vsh_native.h).
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "greatest.h"
#include "wl/xbox/nv2a_pipeline.h"
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
        {wl_xbox_vsh_hash(words, count), count + 1, wl_xbox_vsh_check(words, count), native_run,
         native_colors, NULL, NULL, NULL, 0},
        {wl_xbox_vsh_hash(words, count), count, wl_xbox_vsh_check(words, count), native_run,
         native_colors, NULL, NULL, NULL, 0},
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

// program 0 with another constant in its first instruction, and the word its last instruction
// leaves unused (word 0) chosen so that the two have one FNV-1a hash: two programs, one hash
static unsigned long colliding_programs(uint32_t a[WL_XBOX_VSH_MAX_INSTRUCTIONS * 4],
                                        uint32_t b[WL_XBOX_VSH_MAX_INSTRUCTIONS * 4]) {
    unsigned long count, i;
    const uint32_t *words = program_words(0, &count);
    uint32_t ha = 2166136261u, hb = 2166136261u;
    memcpy(a, words, count * 16);
    memcpy(b, words, count * 16);
    b[1] ^= 1u << 13; // (c[0] read as c[1])
    for (i = 0; i < (count - 1) * 4; i++) {
        ha = (ha ^ a[i]) * 16777619u;
        hb = (hb ^ b[i]) * 16777619u;
    }
    b[i] = a[i] ^ ha ^ hb;
    return count;
}

TEST another_program_of_the_same_hash_is_not_taken_for_it(void) {
    static uint32_t a[WL_XBOX_VSH_MAX_INSTRUCTIONS * 4], b[WL_XBOX_VSH_MAX_INSTRUCTIONS * 4];
    static WlNv2aKelvin k;
    static WlNv2aPipeline pipeline;
    unsigned long count = colliding_programs(a, b);
    ASSERT_EQ(wl_xbox_vsh_hash(a, count), wl_xbox_vsh_hash(b, count));
    ASSERT(wl_xbox_vsh_check(a, count) != wl_xbox_vsh_check(b, count));
    // the compiled functions: a's entry is not b's
    WlXboxVshNative natives[1] = {{wl_xbox_vsh_hash(a, count), count, wl_xbox_vsh_check(a, count),
                                   native_run, native_colors, NULL, NULL, NULL, 0}};
    wl_xbox_vsh_compile(a, count, natives, 1, &g_program);
    ASSERT(g_program.native == native_run);
    wl_xbox_vsh_compile(b, count, natives, 1, &g_program);
    ASSERT(g_program.native == NULL);
    ASSERT_EQ(97u, g_program.instructions[0].constant);
    // the pipeline's programs: b compiled, not a's found
    wl_nv2a_pipeline_init(&pipeline, NULL, 0);
    memset(&k, 0, sizeof k);
    k.raw[(WL_NV2A_K_SET_TRANSFORM_EXECUTION_MODE & (WL_NV2A_K_METHODS - 1)) >> 2] =
        WL_NV2A_XF_PROGRAM;
    memcpy(k.program, a, count * 16);
    k.program_generation = 1;
    ASSERT(wl_nv2a_pipeline_begin(&pipeline, &k));
    ASSERT_EQ(96u, pipeline.program->instructions[0].constant);
    const WlXboxVshProgram *first = pipeline.program;
    const uint32_t first_serial = first->serial;
    memcpy(k.program, b, count * 16);
    k.program_generation = 2;
    ASSERT(wl_nv2a_pipeline_begin(&pipeline, &k));
    ASSERT_EQ(97u, pipeline.program->instructions[0].constant);
    ASSERT_EQ(2u, pipeline.compiled);
    ASSERT(pipeline.program != first || pipeline.program->serial != first_serial);
    // and a again: found
    memcpy(k.program, a, count * 16);
    k.program_generation = 3;
    ASSERT(wl_nv2a_pipeline_begin(&pipeline, &k));
    ASSERT_EQ(96u, pipeline.program->instructions[0].constant);
    ASSERT_EQ(2u, pipeline.compiled);
    ASSERT_EQ(1u, pipeline.found);
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
    ASSERT(fabsf(out[2] - exp2f(3.5f)) <= 2.0e-7f * exp2f(3.5f));
    ASSERT_EQ(1.0f, out[3]);
    // LOG: the exponent, the mantissa, log2 |x|, 1; of 0, -1e30 and 1
    wl_xbox_vsh_ilu(6, (const float[4]){-12.0f, 0, 0, 0}, out);
    ASSERT_EQ(3.0f, out[0]);
    ASSERT_EQ(1.5f, out[1]);
    ASSERT(fabsf(out[2] - log2f(12.0f)) <= 4.0e-7f);
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
    // (n.h at or below 0: 0 whatever the power, as Direct3D's lit; not powf(0, power), 1 for a
    // power of 0 and infinity for a negative one)
    for (int i = 0; i < 3; i++) {
        const float power = i == 0 ? 0.0f : i == 1 ? -2.0f : 2.0f;
        wl_xbox_vsh_ilu(7, (const float[4]){0.5f, 0.0f, 0, power}, out);
        ASSERT_EQ(0.5f, out[1]);
        ASSERT_EQ(0.0f, out[2]);
        wl_xbox_vsh_ilu(7, (const float[4]){0.5f, -0.25f, 0, power}, out);
        ASSERT_EQ(0.0f, out[2]);
    }
    wl_xbox_vsh_ilu(7, (const float[4]){0.5f, 0.25f, 0, 0.0f}, out);
    ASSERT_EQ(1.0f, out[2]);
    PASS();
}

// EXP, LOG and LIT's power by polynomials (not newlib's exp2f, log2f, powf): within an ulp or two
// of libm's over the range, exact at whole powers of two, the exponent and mantissa exact
TEST the_ilu_operations_are_near_libms(void) {
    float out[4], x, worst_exp = 0.0f, worst_log = 0.0f, worst_pow = 0.0f;
    int i;

    for (i = -149 * 16; i < 128 * 16; i++) {
        x = (float)i / 16.0f + 0.0123f;
        wl_xbox_vsh_ilu(5, (const float[4]){x, 0, 0, 0}, out);
        ASSERT_EQ(exp2f(floorf(x)), out[0]);
        ASSERT_EQ(x - floorf(x), out[1]);
        if (exp2f(x) > 1.0e-37f && exp2f(x) < 1.0e38f) {
            float e = fabsf(out[2] - exp2f(x)) / exp2f(x);

            worst_exp = e > worst_exp ? e : worst_exp;
        }
    }
    for (i = -40; i < 40; i++) {
        wl_xbox_vsh_ilu(5, (const float[4]){(float)i, 0, 0, 0}, out);
        ASSERT_EQ(exp2f((float)i), out[2]);
    }
    wl_xbox_vsh_ilu(5, (const float[4]){200.0f, 0, 0, 0}, out);
    ASSERT_EQ(INFINITY, out[2]);
    wl_xbox_vsh_ilu(5, (const float[4]){-200.0f, 0, 0, 0}, out);
    ASSERT_EQ(0.0f, out[2]);
    for (x = 1.0e-44f; x < 1.0e38f; x *= 1.37f) {
        wl_xbox_vsh_ilu(6, (const float[4]){x, 0, 0, 0}, out);
        ASSERT_EQ(floorf(log2f(x)), out[0]);
        ASSERT_EQ(x / exp2f(floorf(log2f(x))), out[1]);
        worst_log = fabsf(out[2] - log2f(x)) > worst_log ? fabsf(out[2] - log2f(x)) : worst_log;
    }
    for (x = 1.0e-3f; x < 1.0f; x *= 1.1f) {
        for (i = 1; i < 128; i += 7) {
            float want = powf(x, (float)i), e;

            wl_xbox_vsh_ilu(7, (const float[4]){1.0f, x, 0, (float)i}, out);
            if (want > 1.0e-30f) {
                e = fabsf(out[2] - want) / want;
                worst_pow = e > worst_pow ? e : worst_pow;
            }
        }
    }
    ASSERT(worst_exp < 2.5e-7f);
    ASSERT(worst_log < 5.0e-7f);
    ASSERT(worst_pow < 1.0e-5f);
    PASS();
}

TEST shared_arithmetic(void) {
    ASSERT_EQ(1.884467e+19f, wl_xbox_vsh_reciprocal_clamped(0.0f));
    ASSERT_EQ(-1.884467e+19f, wl_xbox_vsh_reciprocal_clamped(-0.0f));
    ASSERT_EQ(5.42101e-20f, wl_xbox_vsh_reciprocal_clamped(1.0e30f));
    ASSERT_EQ(0.25f, wl_xbox_vsh_reciprocal_clamped(4.0f));
    // (of an infinity, the bound of its sign: 1 / +inf is +0)
    ASSERT_EQ(5.42101e-20f, wl_xbox_vsh_reciprocal_clamped(INFINITY));
    ASSERT_EQ(-5.42101e-20f, wl_xbox_vsh_reciprocal_clamped(-INFINITY));
    ASSERT_EQ(-5.42101e-20f, wl_xbox_vsh_reciprocal_clamped(-1.0e30f));
    ASSERT(isnan(wl_xbox_vsh_reciprocal_clamped(NAN)));
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

// ---- d0's light shape --------------------------------------------------------------------------

// d0's r, g and b as the shape says they are made, from the normal n
static void shape_color(const WlXboxVshLightShape *shape, const float n[3], float color[3]) {
    unsigned l, c;

    for (c = 0; c < 3; c++) {
        color[c] = g_constants[shape->ambient][c];
    }
    for (l = 0; l < shape->count; l++) {
        const WlXboxVshLight *light = &shape->lights[l];
        float sign = light->direction_negate ? -1.0f : 1.0f, d = 0.0f, term;

        for (c = 0; c < 3; c++) {
            d += n[c] * (sign * g_constants[light->direction][c]);
        }
        term = d;
        if (light->has_back) {
            float back = g_constants[light->back][light->back_component] * (light->back_negate ? -1.0f : 1.0f);

            term = fmaxf(d, back * d);
        }
        term = fmaxf(term, 0.0f);
        for (c = 0; c < 3; c++) {
            color[c] += g_constants[light->color][c] * term;
        }
    }
}

TEST the_light_shape_of_a_lit_program(void) {
    const WlXboxVshLightShape *shape = &g_program.light;
    WlXboxVshProgram unlit;
    WlXboxVshOutput output;
    unsigned round, c, r;
    const int bias = WL_XBOX_VSH_CONSTANT_BIAS;

    compile(7); // lit: two lights (one with a back term) and an ambient
    ASSERT_EQ(2, shape->count);
    ASSERT_EQ(11, shape->normal);
    ASSERT_EQ(33 + bias, shape->ambient);
    ASSERT_EQ(3, shape->zero);
    ASSERT_EQ(2, shape->zero_component);
    ASSERT_EQ(30 + bias, shape->lights[0].direction);
    ASSERT_EQ(0, shape->lights[0].direction_negate);
    ASSERT_EQ(35 + bias, shape->lights[0].color);
    ASSERT_EQ(1, shape->lights[0].has_back);
    ASSERT_EQ(32 + bias, shape->lights[0].back);
    ASSERT_EQ(1, shape->lights[0].back_component);
    ASSERT_EQ(1, shape->lights[0].back_negate);
    ASSERT_EQ(31 + bias, shape->lights[1].direction);
    ASSERT_EQ(1, shape->lights[1].direction_negate);
    ASSERT_EQ(34 + bias, shape->lights[1].color);
    ASSERT_EQ(0, shape->lights[1].has_back);
    // (the shape's colour is the interpreter's, back terms of either sign among them)
    for (round = 0; round < 64; round++) {
        float n[3], expected[3];

        for (r = 30; r <= 36; r++) {
            for (c = 0; c < 4; c++) {
                constant((int)r)[c] = (float)((int)((round * 7u + r * 13u + c * 5u) % 17u) - 8) / 4.0f;
            }
        }
        constant(12)[3] = 0.5f;
        for (c = 0; c < 3; c++) {
            g_inputs[1][c] = (float)((int)((round * 11u + c * 3u) % 9u) - 4);
            n[c] = g_inputs[1][c] * 0.5f;
        }
        g_inputs[3][2] = 0.0f;
        run(&output);
        shape_color(shape, n, expected);
        for (c = 0; c < 3; c++) {
            ASSERT_IN_RANGE(expected[c], output.d0[c], 1.0e-4f * (1.0f + fabsf(expected[c])));
        }
    }
    // (its unlit copy writes d0's alpha alone)
    wl_xbox_vsh_light_unlit(&g_program, &unlit);
    for (r = 0; r < unlit.count; r++) {
        if (unlit.steps[r].output == WL_XBOX_VSH_TO_D0) {
            ASSERT_EQ(0u, unlit.steps[r].output_mask & 0xEu);
        }
    }
    compile(3); // lighting with the specular power: not of the shape
    ASSERT_EQ(0, g_program.light.count);
    PASS();
}

// ---- the constants the outputs read (wl_xbox_vsh_live_constants) --------------------------------
// (assembled by tests/xbox/vsh_programs.py's assemble(); the instructions in the comments)

// each constant into a component of its own, each output reading one of them
static const uint32_t g_split_words[] = {
    0x00000000, 0x00EC001B, 0x0836186C, 0x00008800, // dp4 oPos.x, v0, c[0]
    0x00000000, 0x00EC201B, 0x0836186C, 0x00004800, // dp4 oPos.y, v0, c[1]
    0x00000000, 0x00EC401B, 0x0836186C, 0x00002800, // dp4 oPos.z, v0, c[2]
    0x00000000, 0x00EC601B, 0x0836186C, 0x00001800, // dp4 oPos.w, v0, c[3]
    0x00000000, 0x002E801B, 0x0C36006C, 0x08200000, // mov r2.x, c[20]
    0x00000000, 0x002EA01B, 0x0C36006C, 0x04200000, // mov r2.y, c[21]
    0x00000000, 0x002EC01B, 0x0C36006C, 0x08300000, // mov r3.x, c[22]
    0x00000000, 0x002EE01B, 0x0C36006C, 0x04300000, // mov r3.y, c[23]
    0x00000000, 0x00200000, 0x2436006C, 0x00008848, // mov oT0.x, r2.x
    0x00000000, 0x00200055, 0x2436006C, 0x00008818, // mov oD0.x, r2.y
    0x00000000, 0x00200055, 0x3436006C, 0x00008851, // mov oT1.x, r3.y
};

// skinned: the position by a bone's rows (c[a0+60]); T0 = (c13, c14) . v4 written by an ILU mov beside the MAC's
// dp3 of the bone's row
static const uint32_t g_skinned_words[] = {
    0x00000000, 0x01A00200, 0x0836006C, 0x00000000, // arl a0.x, v1.x
    0x00000000, 0x0033801B, 0x0C36006C, 0x0F700002, // mov r7, c[a0+60]
    0x00000000, 0x00EDA81B, 0x0836186C, 0x08100000, // dp4 r1.x, v4, c[13]
    0x00000000, 0x00EDC81B, 0x0836186C, 0x04100000, // dp4 r1.y, v4, c[14]
    0x00000000, 0x02A0001B, 0x0836E854, 0x58B0C84C, // dp3 r11.x, v0, r7 : imov oT0.xy, r1.xy
    0x00000000, 0x002000FF, 0x0836006C, 0x01B00000, // mov r11.w, v0.w
    0x00000000, 0x00EC001B, 0xB436186C, 0x00008800, // dp4 oPos.x, r11, c[0]
    0x00000000, 0x00EC201B, 0xB436186C, 0x00004800, // dp4 oPos.y, r11, c[1]
    0x00000000, 0x00EC401B, 0xB436186C, 0x00002800, // dp4 oPos.z, r11, c[2]
    0x00000000, 0x00EC601B, 0xB436186C, 0x00001801, // dp4 oPos.w, r11, c[3]
};

// the live constants of `live` as a set of c[n] (n from -96): a bit each of c[0] to c[31]; ~0 for -1, 0xDEAD
// for one out of that range or listed twice, 0xBAD for a register written past the count
static uint32_t live_set(const uint32_t *words, unsigned long count, unsigned live, int most, int *n) {
    unsigned char registers[16];
    uint32_t set = 0;
    memset(registers, 0xEE, sizeof registers);
    wl_xbox_vsh_compile(words, count, NULL, 0, &g_program);
    *n = wl_xbox_vsh_live_constants(&g_program, live, registers, most);
    if (*n < 0) {
        return ~0u;
    }
    for (int i = 0; i < *n; i++) {
        int c = registers[i] - WL_XBOX_VSH_CONSTANT_BIAS;
        if (c < 0 || c >= 32 || (set & 1u << c)) {
            return 0xDEADu;
        }
        set |= 1u << c;
    }
    return registers[*n] == 0xEE ? set : 0xBADu;
}

TEST live_constants_a_component_at_a_time(void) {
    const unsigned long count = sizeof g_split_words / 16;
    int n;
    // r2.x alone reaches T0, r2.y alone D0, r3.y alone T1 (a walk a register at a time takes both of each)
    ASSERT_EQ(1u << 20, live_set(g_split_words, count, WL_XBOX_VSH_LIVE_T0, 16, &n));
    ASSERT_EQ(1, n);
    ASSERT_EQ(1u << 21, live_set(g_split_words, count, WL_XBOX_VSH_LIVE_D0, 16, &n));
    ASSERT_EQ(1u << 23, live_set(g_split_words, count, WL_XBOX_VSH_LIVE_T0 << 1, 16, &n));
    ASSERT_EQ(1u << 20 | 1u << 23,
              live_set(g_split_words, count, WL_XBOX_VSH_LIVE_T0 | WL_XBOX_VSH_LIVE_T0 << 1, 16, &n));
    ASSERT_EQ(2, n);
    ASSERT_EQ(0xFu, live_set(g_split_words, count, WL_XBOX_VSH_LIVE_POSITION, 16, &n));
    ASSERT_EQ(4, n);
    // an output nothing writes reads none; more than `most` is -1
    ASSERT_EQ(0u, live_set(g_split_words, count, WL_XBOX_VSH_LIVE_FOG, 16, &n));
    ASSERT_EQ(0, n);
    ASSERT_EQ(0xFu | 1u << 20 | 1u << 21 | 1u << 23, live_set(g_split_words, count, WL_XBOX_VSH_LIVE_ALL, 7, &n));
    ASSERT_EQ(~0u, live_set(g_split_words, count, WL_XBOX_VSH_LIVE_ALL, 6, &n));
    ASSERT_EQ(-1, n);
    PASS();
}

TEST live_constants_a_unit_at_a_time(void) {
    const unsigned long count = sizeof g_skinned_words / 16;
    int n;
    // the texture coordinates read c[13] and c[14], not the bone the MAC's dp3 beside the ILU's mov reads
    ASSERT_EQ(1u << 13 | 1u << 14, live_set(g_skinned_words, count, WL_XBOX_VSH_LIVE_T0, 16, &n));
    ASSERT_EQ(2, n);
    // the position reads a bone by a0: no list
    ASSERT_EQ(~0u, live_set(g_skinned_words, count, WL_XBOX_VSH_LIVE_POSITION, 16, &n));
    ASSERT_EQ(~0u, live_set(g_skinned_words, count, WL_XBOX_VSH_LIVE_POSITION | WL_XBOX_VSH_LIVE_T0, 16, &n));
    PASS();
}

SUITE(vsh_suite) {
    RUN_TEST(compile_decodes_up_to_the_end_bit);
    RUN_TEST(the_hash_is_fnv1a_of_the_words);
    RUN_TEST(compile_finds_a_function_by_hash_and_count);
    RUN_TEST(another_program_of_the_same_hash_is_not_taken_for_it);
    RUN_TEST(a_transform_runs_exactly);
    RUN_TEST(the_screen_conversion_captures_the_clip_position);
    RUN_TEST(relative_constants_follow_a0);
    RUN_TEST(the_ilu_operations);
    RUN_TEST(the_ilu_operations_are_near_libms);
    RUN_TEST(shared_arithmetic);
    RUN_TEST(the_analysis_of_a_transform);
    RUN_TEST(the_analysis_finds_what_is_not_affine);
    RUN_TEST(the_light_shape_of_a_lit_program);
    RUN_TEST(live_constants_a_component_at_a_time);
    RUN_TEST(live_constants_a_unit_at_a_time);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(vsh_suite);
    GREATEST_MAIN_END();
}
