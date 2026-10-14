// SPDX-License-Identifier: MIT
// wl/xbox/vsh's interpreter (decoded steps, a batch of vertices an instruction at a time, the
// instructions that reach no live output skipped) against the plain one: below, a vertex at a time,
// each instruction's fields read as it runs, everything computed, which is the definition. Random
// programs (every field of every word random, so every operation, register file, swizzle, mask and
// output, relative constants and the odd encodings among them) and the synthetic ones of
// vsh_programs.inc, random constants and inputs (now and then 0, -0, an infinity or a NaN), random
// sets of live outputs and batch sizes: every live output the same, bit for bit (two NaNs count as
// the same: the C library's square root picks its NaN). The inputs wl_xbox_vsh_inputs_read leaves out
// are set to garbage, which must change nothing.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "greatest.h"
#include "wl/xbox/vsh_native.h"

static const uint32_t synthetic[] = {
#include "vsh_programs.inc"
};

static uint32_t g_rng = 0x2545F491u;
static uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static float random_float(void) {
    uint32_t r = rnd() % 64;
    if (r == 0) {
        return 0.0f;
    }
    if (r == 1) {
        return -0.0f;
    }
    if (r == 2) {
        return (rnd() & 1) ? INFINITY : -INFINITY;
    }
    if (r == 3) {
        return NAN;
    }
    if (r < 12) {
        return (float)(int)(rnd() % 200) - 20.0f; // (an index's)
    }
    if (r < 16) {
        // far from 1: overflows and underflows on the way
        return ldexpf((float)(rnd() % 65536) / 65536.0f, (int)(rnd() % 200) - 100) * ((rnd() & 1) ? 1.0f : -1.0f);
    }
    return (float)((int)(rnd() % 65537) - 32768) / 4096.0f;
}

// ---- the definition: the interpreter as it was, a vertex at a time ---------------------------------

enum { MAC_NOP, MAC_MOV, MAC_MUL, MAC_ADD, MAC_MAD, MAC_DP3, MAC_DPH, MAC_DP4, MAC_DST, MAC_MIN, MAC_MAX, MAC_SLT,
       MAC_SGE, MAC_ARL };
enum { ILU_NOP, ILU_MOV, ILU_RCP, ILU_RCC, ILU_RSQ, ILU_EXP, ILU_LOG, ILU_LIT };
enum { MUX_TEMPORARY = 1, MUX_INPUT, MUX_CONSTANT };

struct registers {
    float r[12][4];
    float position[4];
    const float (*inputs)[4];
    const float (*constants)[4];
    int a0;
};

static void read_operand(const struct registers *registers, const WlXboxVshInstruction *instruction,
                         const WlXboxVshOperand *operand, float out[4]) {
    static const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const float *source = zero;

    switch (operand->mux) {
    case MUX_TEMPORARY:
        source = operand->index == 12 ? registers->position : registers->r[operand->index < 12 ? operand->index : 0];
        break;
    case MUX_INPUT:
        source = registers->inputs[instruction->input];
        break;
    case MUX_CONSTANT: {
        int constant = instruction->constant + (instruction->relative ? registers->a0 : 0);
        if (constant < 0) {
            constant = 0;
        }
        if (constant >= WL_XBOX_VSH_CONSTANT_COUNT) {
            constant = WL_XBOX_VSH_CONSTANT_COUNT - 1;
        }
        source = registers->constants[constant];
        break;
    }
    default:
        break;
    }
    for (int component = 0; component < 4; component++) {
        out[component] = operand->negate ? -source[operand->swizzle[component]] : source[operand->swizzle[component]];
    }
}

static void write_masked(float destination[4], const float value[4], unsigned mask) {
    if (mask & 8) {
        destination[0] = value[0];
    }
    if (mask & 4) {
        destination[1] = value[1];
    }
    if (mask & 2) {
        destination[2] = value[2];
    }
    if (mask & 1) {
        destination[3] = value[3];
    }
}

static void reference_run(const WlXboxVshProgram *program, const float (*constants)[4], const float (*inputs)[4],
                          float point_size, WlXboxVshOutput *output) {
    struct registers registers;

    memset(&registers, 0, sizeof(registers));
    registers.position[3] = 1.0f;
    registers.inputs = inputs;
    registers.constants = constants;
    memset(output, 0, sizeof(*output));
    output->d0[3] = output->d1[3] = output->b0[3] = output->b1[3] = 1.0f;
    for (int component = 0; component < 4; component++) {
        output->t[component][3] = 1.0f;
    }
    output->fog = 1.0f;
    output->point_size = point_size;
    for (unsigned long index = 0; index < program->count; index++) {
        const WlXboxVshInstruction *instruction = &program->instructions[index];
        float a[4], b[4], c[4], mac[4] = {0}, ilu[4] = {0};
        int component;

        read_operand(&registers, instruction, &instruction->operands[0], a);
        read_operand(&registers, instruction, &instruction->operands[1], b);
        read_operand(&registers, instruction, &instruction->operands[2], c);
        switch (instruction->mac) {
        case MAC_MOV:
        case MAC_ARL:
            memcpy(mac, a, sizeof(mac));
            break;
        case MAC_MUL:
            for (component = 0; component < 4; component++) {
                mac[component] = a[component] * b[component];
            }
            break;
        case MAC_ADD:
            for (component = 0; component < 4; component++) {
                mac[component] = a[component] + c[component];
            }
            break;
        case MAC_MAD:
            for (component = 0; component < 4; component++) {
                mac[component] = a[component] * b[component] + c[component];
            }
            break;
        case MAC_DP3:
            mac[0] = mac[1] = mac[2] = mac[3] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
            break;
        case MAC_DPH:
            mac[0] = mac[1] = mac[2] = mac[3] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + b[3];
            break;
        case MAC_DP4:
            mac[0] = mac[1] = mac[2] = mac[3] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
            break;
        case MAC_DST:
            mac[0] = 1.0f;
            mac[1] = a[1] * b[1];
            mac[2] = a[2];
            mac[3] = b[3];
            break;
        case MAC_MIN:
            for (component = 0; component < 4; component++) {
                mac[component] = a[component] < b[component] ? a[component] : b[component];
            }
            break;
        case MAC_MAX:
            for (component = 0; component < 4; component++) {
                mac[component] = a[component] > b[component] ? a[component] : b[component];
            }
            break;
        case MAC_SLT:
            for (component = 0; component < 4; component++) {
                mac[component] = a[component] < b[component] ? 1.0f : 0.0f;
            }
            break;
        case MAC_SGE:
            for (component = 0; component < 4; component++) {
                mac[component] = a[component] >= b[component] ? 1.0f : 0.0f;
            }
            break;
        default:
            break;
        }
        switch (instruction->ilu) {
        case ILU_MOV:
            memcpy(ilu, c, sizeof(ilu));
            break;
        case ILU_RCP:
            ilu[0] = ilu[1] = ilu[2] = ilu[3] = 1.0f / c[0];
            break;
        case ILU_RCC:
            ilu[0] = ilu[1] = ilu[2] = ilu[3] = wl_xbox_vsh_reciprocal_clamped(c[0]);
            break;
        case ILU_RSQ:
            ilu[0] = ilu[1] = ilu[2] = ilu[3] = 1.0f / sqrtf(fabsf(c[0]));
            break;
        case ILU_EXP:
        case ILU_LOG:
        case ILU_LIT:
            wl_xbox_vsh_ilu(instruction->ilu, c, ilu);
            break;
        default:
            break;
        }
        if (instruction->captures_clip) {
            memcpy(output->clip, registers.position, sizeof(output->clip));
            output->clip_captured = 1;
        }
        if (instruction->mac == MAC_ARL) {
            registers.a0 = (int)floorf(mac[0] + 0.001f);
        } else if (instruction->mac != MAC_NOP && instruction->mac_mask) {
            write_masked(instruction->temporary == 12
                             ? registers.position
                             : registers.r[instruction->temporary < 12 ? instruction->temporary : 0],
                         mac, instruction->mac_mask);
        }
        if (instruction->ilu != ILU_NOP && instruction->ilu_mask) {
            unsigned temporary = instruction->mac != MAC_NOP ? 1 : instruction->temporary;
            write_masked(temporary == 12 ? registers.position : registers.r[temporary < 12 ? temporary : 0], ilu,
                         instruction->ilu_mask);
        }
        if (instruction->output_mask && instruction->output_is_register &&
            (instruction->output_from_ilu ? instruction->ilu : instruction->mac) != 0) {
            const float *source = instruction->output_from_ilu ? ilu : mac;
            switch (instruction->output_address) {
            case 0:
                write_masked(registers.position, source, instruction->output_mask);
                break;
            case 3:
                write_masked(output->d0, source, instruction->output_mask);
                break;
            case 4:
                write_masked(output->d1, source, instruction->output_mask);
                break;
            case 7:
                write_masked(output->b0, source, instruction->output_mask);
                break;
            case 8:
                write_masked(output->b1, source, instruction->output_mask);
                break;
            case 5:
                if (instruction->output_mask & 8) {
                    output->fog = source[0];
                }
                break;
            case 6:
                if (instruction->output_mask & 8) {
                    output->point_size = source[0];
                }
                break;
            default:
                if (instruction->output_address >= 9 && instruction->output_address < 13) {
                    write_masked(output->t[instruction->output_address - 9], source, instruction->output_mask);
                }
                break;
            }
        }
    }
    memcpy(output->position, registers.position, sizeof(output->position));
}

// ---- the comparison ---------------------------------------------------------------------------

static bool same_floats(const float *a, const float *b, int count) {
    for (int i = 0; i < count; i++) {
        if (memcmp(&a[i], &b[i], sizeof(float)) != 0 && !(a[i] != a[i] && b[i] != b[i])) {
            return false;
        }
    }
    return true;
}

static bool live_same(const WlXboxVshOutput *a, const WlXboxVshOutput *b, unsigned live) {
    if ((live & WL_XBOX_VSH_LIVE_POSITION) &&
        (!same_floats(a->position, b->position, 4) || !same_floats(a->clip, b->clip, 4) ||
         a->clip_captured != b->clip_captured)) {
        return false;
    }
    if ((live & WL_XBOX_VSH_LIVE_D0) && !same_floats(a->d0, b->d0, 4)) {
        return false;
    }
    if ((live & WL_XBOX_VSH_LIVE_D1) && !same_floats(a->d1, b->d1, 4)) {
        return false;
    }
    if ((live & WL_XBOX_VSH_LIVE_BACK) && (!same_floats(a->b0, b->b0, 4) || !same_floats(a->b1, b->b1, 4))) {
        return false;
    }
    if ((live & WL_XBOX_VSH_LIVE_POINT_SIZE) && !same_floats(&a->point_size, &b->point_size, 1)) {
        return false;
    }
    for (int stage = 0; stage < 4; stage++) {
        if ((live & ((unsigned)WL_XBOX_VSH_LIVE_T0 << stage)) && !same_floats(a->t[stage], b->t[stage], 4)) {
            return false;
        }
    }
    if ((live & WL_XBOX_VSH_LIVE_FOG) && !same_floats(&a->fog, &b->fog, 1)) {
        return false;
    }
    return true;
}

enum { VERTICES = 37 };

static WlXboxVshProgram g_program;
static float g_constants[WL_XBOX_VSH_CONSTANT_COUNT][4];
static float g_inputs[VERTICES][WL_XBOX_VSH_INPUT_COUNT][4];
static WlXboxVshOutput g_want[VERTICES], g_got[VERTICES];
static WlXboxVshWork g_work;

static void randomize(void) {
    for (int i = 0; i < WL_XBOX_VSH_CONSTANT_COUNT; i++) {
        for (int c = 0; c < 4; c++) {
            g_constants[i][c] = random_float();
        }
    }
    for (int v = 0; v < VERTICES; v++) {
        for (int i = 0; i < WL_XBOX_VSH_INPUT_COUNT; i++) {
            for (int c = 0; c < 4; c++) {
                g_inputs[v][i][c] = random_float();
            }
        }
    }
}

// the program run every way against the definition, for a few sets of constants and inputs
static int check_program(void) {
    for (int trial = 0; trial < 6; trial++) {
        unsigned live = trial == 0 ? (unsigned)WL_XBOX_VSH_LIVE_ALL : rnd() & WL_XBOX_VSH_LIVE_ALL;
        unsigned count = 1 + rnd() % VERTICES;
        unsigned long reads;

        randomize();
        for (unsigned v = 0; v < count; v++) {
            reference_run(&g_program, (const float (*)[4])g_constants, (const float (*)[4])g_inputs[v], 3.5f, &g_want[v]);
        }
        // whole, a vertex at a time
        for (unsigned v = 0; v < count; v++) {
            memset(&g_got[v], 0x5A, sizeof g_got[v]);
            wl_xbox_vsh_run(&g_program, (const float (*)[4])g_constants, (const float (*)[4])g_inputs[v], 3.5f,
                            &g_got[v]);
            ASSERTm("run", live_same(&g_want[v], &g_got[v], WL_XBOX_VSH_LIVE_ALL));
            memset(&g_got[v], 0x5A, sizeof g_got[v]);
            wl_xbox_vsh_run_live(&g_program, (const float (*)[4])g_constants, (const float (*)[4])g_inputs[v], 3.5f,
                                 &g_got[v], live);
            ASSERTm("run_live", live_same(&g_want[v], &g_got[v], live));
        }
        // the inputs not read made garbage, then the batch
        reads = wl_xbox_vsh_inputs_read(&g_program, live);
        for (unsigned v = 0; v < count; v++) {
            for (int i = 0; i < WL_XBOX_VSH_INPUT_COUNT; i++) {
                if (!(reads & (1ul << i))) {
                    for (int c = 0; c < 4; c++) {
                        g_inputs[v][i][c] = (rnd() & 1) ? NAN : 1.0e30f;
                    }
                }
            }
        }
        // (the scratch's garbage, a register the program reads before writing it must not see)
        memset(g_got, 0x5A, sizeof g_got);
        memset(&g_work, (rnd() & 1) ? 0x5A : 0xFF, sizeof g_work);
        wl_xbox_vsh_run_batch(&g_program, (const float (*)[4])g_constants,
                              (const float (*)[WL_XBOX_VSH_INPUT_COUNT][4])g_inputs, count, 3.5f, g_got, live, &g_work);
        for (unsigned v = 0; v < count; v++) {
            ASSERTm("run_batch", live_same(&g_want[v], &g_got[v], live));
        }
    }
    PASS();
}

TEST random_programs_match_the_definition(void) {
    static uint32_t words[WL_XBOX_VSH_MAX_INSTRUCTIONS * 4];

    for (int n = 0; n < 3000; n++) {
        unsigned long count = 1 + rnd() % (n % 4 == 0 ? WL_XBOX_VSH_MAX_INSTRUCTIONS : 24);
        for (unsigned long i = 0; i < count * 4; i++) {
            words[i] = rnd();
        }
        for (unsigned long i = 0; i < count; i++) {
            words[i * 4 + 3] = (words[i * 4 + 3] & ~1u) | (i + 1 == count ? 1u : 0u);
            // (mostly temporaries and inputs written and read, as programs do)
            if (rnd() % 4 != 0) {
                words[i * 4 + 3] |= 1u << 11;
            }
        }
        wl_xbox_vsh_compile(words, count, NULL, 0, &g_program);
        ASSERT_EQ(count, g_program.count);
        CHECK_CALL(check_program());
    }
    PASS();
}

TEST the_synthetic_programs_match_the_definition(void) {
    size_t position = 0;
    int programs = 0;

    while (position < sizeof synthetic / sizeof synthetic[0]) {
        unsigned long count = synthetic[position] >> 16;
        wl_xbox_vsh_compile(&synthetic[position + 1], count, NULL, 0, &g_program);
        for (int repeat = 0; repeat < 50; repeat++) {
            CHECK_CALL(check_program());
        }
        position += 1 + count * 4;
        programs++;
    }
    ASSERT(programs >= 5);
    PASS();
}

SUITE(vsh_batch_suite) {
    RUN_TEST(random_programs_match_the_definition);
    RUN_TEST(the_synthetic_programs_match_the_definition);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(vsh_batch_suite);
    GREATEST_MAIN_END();
}
