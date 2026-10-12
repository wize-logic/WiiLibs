// SPDX-License-Identifier: MIT
// The Xbox's vertex programs (NV2A vertex microcode) run on the processor (vsh.h). Each program is
// decoded once into its instructions' fields, then run for each vertex: the MAC (vector) and ILU
// (scalar) units read their operands A, B and C, then write their results (the ILU's to r1 when
// both run), r12 is the position output read back, a0 indexes the constants relatively.
//
// Programs known ahead of time can also be compiled to C (tools/xbox_vsh_codegen.c, which decodes
// them with this file): a program found among them (wl_xbox_vsh_hash) runs as its function.

#include "wl/xbox/vsh_native.h"

#include <math.h>
#include <string.h>

enum {
    _mac_nop,
    _mac_mov,
    _mac_mul,
    _mac_add,
    _mac_mad,
    _mac_dp3,
    _mac_dph,
    _mac_dp4,
    _mac_dst,
    _mac_min,
    _mac_max,
    _mac_slt,
    _mac_sge,
    _mac_arl,
};

enum {
    _ilu_nop,
    _ilu_mov,
    _ilu_rcp,
    _ilu_rcc,
    _ilu_rsq,
    _ilu_exp,
    _ilu_log,
    _ilu_lit,
};

enum {
    _mux_unknown,
    _mux_temporary,
    _mux_input,
    _mux_constant,
};

/* o[] addresses */
enum {
    _output_position = 0,
    _output_d0 = 3,
    _output_d1 = 4,
    _output_fog = 5,
    _output_point_size = 6,
    _output_b0 = 7,
    _output_b1 = 8,
    _output_t0 = 9,
};

static unsigned long field(const uint32_t *instruction, int word, int low_bit, int bit_count) {
    return (instruction[word] >> low_bit) & ((1UL << bit_count) - 1);
}

static void operand_fields(const uint32_t *instruction, int which, WlXboxVshOperand *operand) {
    switch (which) {
    case 0:
        operand->negate = (unsigned char)field(instruction, 1, 8, 1);
        operand->swizzle[0] = (unsigned char)field(instruction, 1, 6, 2);
        operand->swizzle[1] = (unsigned char)field(instruction, 1, 4, 2);
        operand->swizzle[2] = (unsigned char)field(instruction, 1, 2, 2);
        operand->swizzle[3] = (unsigned char)field(instruction, 1, 0, 2);
        operand->index = (unsigned char)field(instruction, 2, 28, 4);
        operand->mux = (unsigned char)field(instruction, 2, 26, 2);
        break;
    case 1:
        operand->negate = (unsigned char)field(instruction, 2, 25, 1);
        operand->swizzle[0] = (unsigned char)field(instruction, 2, 23, 2);
        operand->swizzle[1] = (unsigned char)field(instruction, 2, 21, 2);
        operand->swizzle[2] = (unsigned char)field(instruction, 2, 19, 2);
        operand->swizzle[3] = (unsigned char)field(instruction, 2, 17, 2);
        operand->index = (unsigned char)field(instruction, 2, 13, 4);
        operand->mux = (unsigned char)field(instruction, 2, 11, 2);
        break;
    default:
        operand->negate = (unsigned char)field(instruction, 2, 10, 1);
        operand->swizzle[0] = (unsigned char)field(instruction, 2, 8, 2);
        operand->swizzle[1] = (unsigned char)field(instruction, 2, 6, 2);
        operand->swizzle[2] = (unsigned char)field(instruction, 2, 4, 2);
        operand->swizzle[3] = (unsigned char)field(instruction, 2, 2, 2);
        operand->index =
            (unsigned char)((field(instruction, 2, 0, 2) << 2) | field(instruction, 3, 30, 2));
        operand->mux = (unsigned char)field(instruction, 3, 28, 2);
        break;
    }
}

void wl_xbox_vsh_compile(const uint32_t *instructions, unsigned long instruction_count,
                         const WlXboxVshNative *natives, unsigned long native_count,
                         WlXboxVshProgram *program) {
    unsigned long index;

    program->count = 0;
    for (index = 0; index < instruction_count && index < WL_XBOX_VSH_MAX_INSTRUCTIONS; index++) {
        const uint32_t *word = instructions + index * 4;
        WlXboxVshInstruction *instruction = &program->instructions[program->count++];
        int which;

        instruction->mac = (unsigned char)field(word, 1, 21, 4);
        instruction->ilu = (unsigned char)field(word, 1, 25, 3);
        instruction->mac_mask = (unsigned char)field(word, 3, 24, 4);
        instruction->temporary = (unsigned char)field(word, 3, 20, 4);
        instruction->ilu_mask = (unsigned char)field(word, 3, 16, 4);
        instruction->output_mask = (unsigned char)field(word, 3, 12, 4);
        instruction->output_is_register = (unsigned char)field(word, 3, 11, 1);
        instruction->output_address = (unsigned char)field(word, 3, 3, 8);
        instruction->output_from_ilu = (unsigned char)field(word, 3, 2, 1);
        instruction->relative = (unsigned char)field(word, 3, 1, 1);
        instruction->input = (unsigned char)field(word, 1, 9, 4);
        instruction->constant = (unsigned char)field(word, 1, 13, 8);
        for (which = 0; which < 3; which++)
            operand_fields(word, which, &instruction->operands[which]);
        instruction->captures_clip = instruction->ilu == _ilu_rcc &&
                                     instruction->operands[2].mux == _mux_temporary &&
                                     instruction->operands[2].index == 12;
        if (field(word, 3, 0, 1))
            break;
    }
    program->native = NULL;
    program->native_colors = NULL;
    program->native_positions = NULL;
    program->hash = wl_xbox_vsh_hash(instructions, instruction_count);
    {
        unsigned long hash = program->hash;

        for (index = 0; natives && index < native_count; index++) {
            if (natives[index].hash == hash && natives[index].count == instruction_count) {
                program->native = natives[index].run;
                program->native_colors = natives[index].colors;
                program->native_positions = natives[index].positions;
                break;
            }
        }
    }
}

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
    int component;

    switch (operand->mux) {
    case _mux_temporary:
        /* r12 reads back the position output */
        source = operand->index == 12 ? registers->position
                                      : registers->r[operand->index < 12 ? operand->index : 0];
        break;
    case _mux_input:
        source = registers->inputs[instruction->input];
        break;
    case _mux_constant: {
        int constant = instruction->constant + (instruction->relative ? registers->a0 : 0);

        if (constant < 0)
            constant = 0;
        if (constant >= WL_XBOX_VSH_CONSTANT_COUNT)
            constant = WL_XBOX_VSH_CONSTANT_COUNT - 1;
        source = registers->constants[constant];
        break;
    }
    default:
        break;
    }
    for (component = 0; component < 4; component++)
        out[component] = operand->negate ? -source[operand->swizzle[component]]
                                         : source[operand->swizzle[component]];
}

static void write_masked(float destination[4], const float value[4], unsigned mask) {
    /* (bit 3 is x) */
    if (mask & 8)
        destination[0] = value[0];
    if (mask & 4)
        destination[1] = value[1];
    if (mask & 2)
        destination[2] = value[2];
    if (mask & 1)
        destination[3] = value[3];
}

/* ---------- what the outputs depend on

wl_xbox_vsh_run's walk with each value a set of input registers and whether it
is still affine in them, in place of a number */

struct dependency_registers {
    unsigned long r[12][4];
    unsigned long position[4];
    unsigned long a0;
    /* the inputs taken as constants (bit n: v[n]) */
    unsigned long uniform;
    /* (wl_xbox_vsh_analyze_constants: a constant register's value carries
    WL_XBOX_VSH_READS_CONSTANT) */
    unsigned long constant;
};

static unsigned long product(unsigned long x, unsigned long y) {
    /* (a product of two inputs is not affine) */
    return x | y | ((x & 0xFFFF) && (y & 0xFFFF) ? WL_XBOX_VSH_NOT_AFFINE : 0);
}

static unsigned long not_affine(unsigned long x) {
    return x ? x | WL_XBOX_VSH_NOT_AFFINE : 0;
}

static void dependency_operand(const struct dependency_registers *registers,
                               const WlXboxVshInstruction *instruction,
                               const WlXboxVshOperand *operand, WlXboxVshDependencies *dependencies,
                               unsigned long out[4]) {
    unsigned long source[4] = {0, 0, 0, 0};
    int component;

    switch (operand->mux) {
    case _mux_temporary:
        memcpy(source,
               operand->index == 12 ? registers->position
                                    : registers->r[operand->index < 12 ? operand->index : 0],
               sizeof(source));
        break;
    case _mux_input:
        if (!(registers->uniform & (1UL << (instruction->input & 15))))
            source[0] = source[1] = source[2] = source[3] = 1UL << (instruction->input & 15);
        dependencies->inputs |= 1UL << (instruction->input & 15);
        break;
    case _mux_constant:
        /* (a constant at an index an input chose) */
        if (instruction->relative) {
            source[0] = source[1] = source[2] = source[3] =
                not_affine(registers->a0) | registers->constant;
            dependencies->relative = 1;
        } else {
            source[0] = source[1] = source[2] = source[3] = registers->constant;
            if (instruction->constant < WL_XBOX_VSH_CONSTANT_COUNT)
                dependencies->constants[instruction->constant / 32] |=
                    1UL << (instruction->constant % 32);
        }
        break;
    default:
        break;
    }
    for (component = 0; component < 4; component++)
        out[component] = source[operand->swizzle[component] & 3];
}

static void dependency_write(unsigned long destination[4], const unsigned long value[4],
                             unsigned mask) {
    if (mask & 8)
        destination[0] = value[0];
    if (mask & 4)
        destination[1] = value[1];
    if (mask & 2)
        destination[2] = value[2];
    if (mask & 1)
        destination[3] = value[3];
}

void wl_xbox_vsh_analyze(const WlXboxVshProgram *program, WlXboxVshDependencies *dependencies) {
    wl_xbox_vsh_analyze_uniform(program, 0, dependencies);
}

static void analyze(const WlXboxVshProgram *program, unsigned long uniform, unsigned long constant,
                    WlXboxVshDependencies *dependencies);

/* the same with some inputs taken as constants: a draw whose vertices all
have the same values there (a skinned model's part of one node) */
void wl_xbox_vsh_analyze_uniform(const WlXboxVshProgram *program, unsigned long uniform,
                                 WlXboxVshDependencies *dependencies) {
    analyze(program, uniform, 0, dependencies);
}

/* which outputs read a constant: those that do not are the same whatever
the draw's constants, for the same vertex (a device keeps them for a
static vertex buffer) */
void wl_xbox_vsh_analyze_constants(const WlXboxVshProgram *program,
                                   WlXboxVshDependencies *dependencies) {
    analyze(program, 0, WL_XBOX_VSH_READS_CONSTANT, dependencies);
}

static void analyze(const WlXboxVshProgram *program, unsigned long uniform, unsigned long constant,
                    WlXboxVshDependencies *dependencies) {
    struct dependency_registers registers;
    unsigned long index;
    int component;

    memset(&registers, 0, sizeof(registers));
    registers.uniform = uniform;
    registers.constant = constant;
    memset(dependencies, 0, sizeof(*dependencies));
    for (index = 0; index < program->count; index++) {
        const WlXboxVshInstruction *instruction = &program->instructions[index];
        unsigned long a[4], b[4], c[4], mac[4] = {0, 0, 0, 0}, ilu[4] = {0, 0, 0, 0}, sum;

        dependency_operand(&registers, instruction, &instruction->operands[0], dependencies, a);
        dependency_operand(&registers, instruction, &instruction->operands[1], dependencies, b);
        dependency_operand(&registers, instruction, &instruction->operands[2], dependencies, c);
        switch (instruction->mac) {
        case _mac_mov:
        case _mac_arl:
            memcpy(mac, a, sizeof(mac));
            break;
        case _mac_mul:
            for (component = 0; component < 4; component++)
                mac[component] = product(a[component], b[component]);
            break;
        case _mac_add:
            for (component = 0; component < 4; component++)
                mac[component] = a[component] | c[component];
            break;
        case _mac_mad:
            for (component = 0; component < 4; component++)
                mac[component] = product(a[component], b[component]) | c[component];
            break;
        case _mac_dp3:
        case _mac_dph:
        case _mac_dp4:
            sum = product(a[0], b[0]) | product(a[1], b[1]) | product(a[2], b[2]);
            if (instruction->mac == _mac_dph)
                sum |= b[3];
            else if (instruction->mac == _mac_dp4)
                sum |= product(a[3], b[3]);
            mac[0] = mac[1] = mac[2] = mac[3] = sum;
            break;
        case _mac_dst:
            mac[0] = 0;
            mac[1] = product(a[1], b[1]);
            mac[2] = a[2];
            mac[3] = b[3];
            break;
        case _mac_min:
        case _mac_max:
        case _mac_slt:
        case _mac_sge:
            for (component = 0; component < 4; component++)
                mac[component] = not_affine(a[component] | b[component]);
            break;
        default:
            break;
        }
        switch (instruction->ilu) {
        case _ilu_mov:
            memcpy(ilu, c, sizeof(ilu));
            break;
        case _ilu_rcp:
        case _ilu_rcc:
        case _ilu_rsq:
        case _ilu_exp:
        case _ilu_log:
            ilu[0] = ilu[1] = ilu[2] = ilu[3] = not_affine(c[0]);
            break;
        case _ilu_lit:
            ilu[0] = ilu[1] = ilu[2] = ilu[3] = not_affine(c[0] | c[1] | c[3]);
            break;
        default:
            break;
        }
        if (instruction->captures_clip) {
            memcpy(dependencies->clip, registers.position, sizeof(dependencies->clip));
            dependencies->clip_captured = 1;
        }
        if (instruction->mac == _mac_arl)
            registers.a0 = mac[0];
        else if (instruction->mac != _mac_nop && instruction->mac_mask)
            dependency_write(
                instruction->temporary == 12
                    ? registers.position
                    : registers.r[instruction->temporary < 12 ? instruction->temporary : 0],
                mac, instruction->mac_mask);
        if (instruction->ilu != _ilu_nop && instruction->ilu_mask) {
            unsigned temporary = instruction->mac != _mac_nop ? 1 : instruction->temporary;

            dependency_write(temporary == 12 ? registers.position
                                             : registers.r[temporary < 12 ? temporary : 0],
                             ilu, instruction->ilu_mask);
        }
        if (instruction->output_mask && instruction->output_is_register &&
            (instruction->output_from_ilu ? instruction->ilu : instruction->mac) != 0) {
            const unsigned long *source = instruction->output_from_ilu ? ilu : mac;

            switch (instruction->output_address) {
            case _output_position:
                dependency_write(registers.position, source, instruction->output_mask);
                break;
            case _output_d0:
                dependency_write(dependencies->d0, source, instruction->output_mask);
                break;
            case _output_d1:
                dependency_write(dependencies->d1, source, instruction->output_mask);
                break;
            case _output_b0:
                dependency_write(dependencies->b0, source, instruction->output_mask);
                break;
            case _output_b1:
                dependency_write(dependencies->b1, source, instruction->output_mask);
                break;
            case _output_fog:
                if (instruction->output_mask & 8)
                    dependencies->fog = source[0];
                break;
            case _output_point_size:
                if (instruction->output_mask & 8)
                    dependencies->point_size = source[0];
                break;
            default:
                if (instruction->output_address >= _output_t0 &&
                    instruction->output_address < _output_t0 + 4)
                    dependency_write(dependencies->t[instruction->output_address - _output_t0],
                                     source, instruction->output_mask);
                break;
            }
        }
    }
    memcpy(dependencies->position, registers.position, sizeof(dependencies->position));
}

unsigned long wl_xbox_vsh_live_inputs(const WlXboxVshDependencies *dependencies, unsigned live) {
    unsigned long reads = 0;
    int component, stage;

    for (component = 0; component < 4; component++) {
        if (live & WL_XBOX_VSH_LIVE_POSITION)
            reads |= dependencies->clip[component] | dependencies->position[component];
        if (live & WL_XBOX_VSH_LIVE_D0)
            reads |= dependencies->d0[component];
        if (live & WL_XBOX_VSH_LIVE_D1)
            reads |= dependencies->d1[component];
        if (live & WL_XBOX_VSH_LIVE_BACK)
            reads |= dependencies->b0[component] | dependencies->b1[component];
        for (stage = 0; stage < 4; stage++)
            if (live & (WL_XBOX_VSH_LIVE_T0 << stage))
                reads |= dependencies->t[stage][component];
    }
    if (live & WL_XBOX_VSH_LIVE_FOG)
        reads |= dependencies->fog;
    if (live & WL_XBOX_VSH_LIVE_POINT_SIZE)
        reads |= dependencies->point_size;
    return reads & 0xFFFF;
}

unsigned long wl_xbox_vsh_hash(const uint32_t *instructions, unsigned long instruction_count) {
    unsigned long hash = 2166136261UL, index;

    for (index = 0; index < instruction_count * 4; index++)
        hash = ((hash ^ instructions[index]) * 16777619UL) & 0xFFFFFFFFUL;
    return hash;
}

/* the ILU's exponent, logarithm and lighting (from C into I) */
void wl_xbox_vsh_ilu(int ilu, const float c[4], float out[4]) {
    switch (ilu) {
    case _ilu_exp:
        out[0] = exp2f(floorf(c[0]));
        out[1] = c[0] - floorf(c[0]);
        out[2] = exp2f(c[0]);
        out[3] = 1.0f;
        break;
    case _ilu_log: {
        float x = fabsf(c[0]);

        if (x == 0.0f) {
            out[0] = out[2] = -1.0e30f;
            out[1] = out[3] = 1.0f;
        } else {
            float exponent = floorf(log2f(x));

            out[0] = exponent;
            out[1] = x / exp2f(exponent);
            out[2] = log2f(x);
            out[3] = 1.0f;
        }
        break;
    }
    case _ilu_lit: {
        float power = c[3] < -127.9961f ? -127.9961f : c[3] > 127.9961f ? 127.9961f : c[3];

        out[0] = 1.0f;
        out[1] = c[0] > 0.0f ? c[0] : 0.0f;
        out[2] = c[0] > 0.0f ? powf(c[1] > 0.0f ? c[1] : 0.0f, power) : 0.0f;
        out[3] = 1.0f;
        break;
    }
    default:
        break;
    }
}

/* a vertex's colours (d0, d1) and fog alone: the program's colour-only
function where it has one, else all of it */
void wl_xbox_vsh_run_colors(const WlXboxVshProgram *program, const float (*constants)[4],
                            const float (*inputs)[4], WlXboxVshOutput *output) {
    if (!program->native_colors) {
        wl_xbox_vsh_run(program, constants, inputs, 1.0f, output);
        return;
    }
    output->d0[0] = output->d0[1] = output->d0[2] = 0.0f;
    output->d1[0] = output->d1[1] = output->d1[2] = 0.0f;
    output->d0[3] = output->d1[3] = 1.0f;
    output->fog = 1.0f;
    program->native_colors(constants, inputs, output);
}

/* an output as a program leaves what it doesn't write */
static void output_clear(WlXboxVshOutput *output, float point_size) {
    int component;

    memset(output, 0, sizeof(*output));
    output->d0[3] = output->d1[3] = output->b0[3] = output->b1[3] = 1.0f;
    for (component = 0; component < 4; component++)
        output->t[component][3] = 1.0f;
    output->fog = 1.0f;
    output->point_size = point_size;
}

static void wl_xbox_vsh_interpret(const WlXboxVshProgram *program, const float (*constants)[4],
                                  const float (*inputs)[4], float point_size,
                                  WlXboxVshOutput *output);

void wl_xbox_vsh_run(const WlXboxVshProgram *program, const float (*constants)[4],
                     const float (*inputs)[4], float point_size, WlXboxVshOutput *output) {
    if (program->native) {
        output_clear(output, point_size);
        program->native(constants, inputs, output, WL_XBOX_VSH_LIVE_ALL);
        return;
    }
    wl_xbox_vsh_interpret(program, constants, inputs, point_size, output);
}

void wl_xbox_vsh_run_live(const WlXboxVshProgram *program, const float (*constants)[4],
                          const float (*inputs)[4], float point_size, WlXboxVshOutput *output,
                          unsigned live) {
    /* (the function sets what the program leaves of the outputs it reads,
    but the point size) */
    if (program->native) {
        if (live & WL_XBOX_VSH_LIVE_POINT_SIZE)
            output->point_size = point_size;
        program->native(constants, inputs, output, live);
        return;
    }
    wl_xbox_vsh_run(program, constants, inputs, point_size, output);
}

int wl_xbox_vsh_run_positions(const WlXboxVshProgram *program, const float (*constants)[4],
                              const float (*inputs)[4], const float (*points)[4], float (*clips)[4],
                              float (*positions)[4]) {
    if (!program->native_positions)
        return 0;
    program->native_positions(constants, inputs, points, clips, positions);
    return 1;
}

static void wl_xbox_vsh_interpret(const WlXboxVshProgram *program, const float (*constants)[4],
                                  const float (*inputs)[4], float point_size,
                                  WlXboxVshOutput *output) {
    struct registers registers;
    unsigned long index;
    int component;

    memset(&registers, 0, sizeof(registers));
    registers.position[3] = 1.0f;
    registers.inputs = inputs;
    registers.constants = constants;
    output_clear(output, point_size);

    for (index = 0; index < program->count; index++) {
        const WlXboxVshInstruction *instruction = &program->instructions[index];
        float a[4], b[4], c[4], mac[4] = {0}, ilu[4] = {0};

        read_operand(&registers, instruction, &instruction->operands[0], a);
        read_operand(&registers, instruction, &instruction->operands[1], b);
        read_operand(&registers, instruction, &instruction->operands[2], c);
        switch (instruction->mac) {
        case _mac_mov:
        case _mac_arl:
            memcpy(mac, a, sizeof(mac));
            break;
        case _mac_mul:
            for (component = 0; component < 4; component++)
                mac[component] = a[component] * b[component];
            break;
        case _mac_add:
            for (component = 0; component < 4; component++)
                mac[component] = a[component] + c[component];
            break;
        case _mac_mad:
            for (component = 0; component < 4; component++)
                mac[component] = a[component] * b[component] + c[component];
            break;
        case _mac_dp3:
            mac[0] = mac[1] = mac[2] = mac[3] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
            break;
        case _mac_dph:
            mac[0] = mac[1] = mac[2] = mac[3] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + b[3];
            break;
        case _mac_dp4:
            mac[0] = mac[1] = mac[2] = mac[3] =
                a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
            break;
        case _mac_dst:
            mac[0] = 1.0f;
            mac[1] = a[1] * b[1];
            mac[2] = a[2];
            mac[3] = b[3];
            break;
        case _mac_min:
            for (component = 0; component < 4; component++)
                mac[component] = a[component] < b[component] ? a[component] : b[component];
            break;
        case _mac_max:
            for (component = 0; component < 4; component++)
                mac[component] = a[component] > b[component] ? a[component] : b[component];
            break;
        case _mac_slt:
            for (component = 0; component < 4; component++)
                mac[component] = a[component] < b[component] ? 1.0f : 0.0f;
            break;
        case _mac_sge:
            for (component = 0; component < 4; component++)
                mac[component] = a[component] >= b[component] ? 1.0f : 0.0f;
            break;
        default:
            break;
        }
        switch (instruction->ilu) {
        case _ilu_mov:
            memcpy(ilu, c, sizeof(ilu));
            break;
        case _ilu_rcp:
            ilu[0] = ilu[1] = ilu[2] = ilu[3] = 1.0f / c[0];
            break;
        case _ilu_rcc:
            ilu[0] = ilu[1] = ilu[2] = ilu[3] = wl_xbox_vsh_reciprocal_clamped(c[0]);
            break;
        case _ilu_rsq:
            ilu[0] = ilu[1] = ilu[2] = ilu[3] = 1.0f / sqrtf(fabsf(c[0]));
            break;
        case _ilu_exp:
        case _ilu_log:
        case _ilu_lit:
            wl_xbox_vsh_ilu(instruction->ilu, c, ilu);
            break;
        default:
            break;
        }
        /* the screen-space conversion: the position before it */
        if (instruction->captures_clip) {
            memcpy(output->clip, registers.position, sizeof(output->clip));
            output->clip_captured = 1;
        }

        /* results written once both units have read their inputs */
        if (instruction->mac == _mac_arl) {
            registers.a0 = (int)floorf(mac[0] + 0.001f);
        } else if (instruction->mac != _mac_nop && instruction->mac_mask) {
            write_masked(
                instruction->temporary == 12
                    ? registers.position
                    : registers.r[instruction->temporary < 12 ? instruction->temporary : 0],
                mac, instruction->mac_mask);
        }
        if (instruction->ilu != _ilu_nop && instruction->ilu_mask) {
            unsigned temporary = instruction->mac != _mac_nop ? 1 : instruction->temporary;

            write_masked(temporary == 12 ? registers.position
                                         : registers.r[temporary < 12 ? temporary : 0],
                         ilu, instruction->ilu_mask);
        }
        if (instruction->output_mask && instruction->output_is_register &&
            (instruction->output_from_ilu ? instruction->ilu : instruction->mac) != 0) {
            const float *source = instruction->output_from_ilu ? ilu : mac;

            switch (instruction->output_address) {
            case _output_position:
                write_masked(registers.position, source, instruction->output_mask);
                break;
            case _output_d0:
                write_masked(output->d0, source, instruction->output_mask);
                break;
            case _output_d1:
                write_masked(output->d1, source, instruction->output_mask);
                break;
            case _output_b0:
                write_masked(output->b0, source, instruction->output_mask);
                break;
            case _output_b1:
                write_masked(output->b1, source, instruction->output_mask);
                break;
            case _output_fog:
                if (instruction->output_mask & 8)
                    output->fog = source[0];
                break;
            case _output_point_size:
                if (instruction->output_mask & 8)
                    output->point_size = source[0];
                break;
            default:
                if (instruction->output_address >= _output_t0 &&
                    instruction->output_address < _output_t0 + 4)
                    write_masked(output->t[instruction->output_address - _output_t0], source,
                                 instruction->output_mask);
                break;
            }
        }
    }
    memcpy(output->position, registers.position, sizeof(output->position));
}
