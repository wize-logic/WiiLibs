// SPDX-License-Identifier: MIT
// The Xbox's vertex programs (NV2A vertex microcode) run on the processor (vsh.h). Each program is
// decoded once into its instructions' fields, then run for each vertex: the MAC (vector) and ILU
// (scalar) units read their operands A, B and C, then write their results (the ILU's to r1 when
// both run), r12 is the position output read back, a0 indexes the constants relatively.
//
// The interpreter runs the steps compile resolved from the fields (WlXboxVshStep: each operand's
// register, swizzle and sign, each write's register and mask, the outputs an instruction reaches),
// an instruction at a time for a batch of vertices, so that what an instruction is is decided once
// for all of them, and skips the instructions that reach no output the draw reads. Each vertex sees
// the same operations in the same order as a run of the program for it alone.
//
// Programs known ahead of time can also be compiled to C (tools/xbox_vsh_codegen.c, which decodes
// them with this file): a program found among them (wl_xbox_vsh_hash) runs as its function.

#include "wl/xbox/vsh_native.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
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

static unsigned temporary_index(unsigned temporary) {
    return temporary == 12 ? 12 : temporary < 12 ? temporary : 0;
}

/* the outputs each instruction's results reach (WL_XBOX_VSH_LIVE_...), found
backwards from the end a register's component at a time: a draw that reads
none of them skips the instruction, and what it runs is the same, so the
outputs it reads are too */
void wl_xbox_vsh_live_needs(const WlXboxVshProgram *program, unsigned *needs) {
    unsigned need[13][4], need_a0 = 0;
    long index;
    int component, which;

    memset(need, 0, sizeof(need));
    /* (the position is p as the program ends) */
    for (component = 0; component < 4; component++)
        need[12][component] = WL_XBOX_VSH_LIVE_POSITION;
    /* (a lit program's normal, x, y and z as it ends, is d0's: GX lights it in
    a lit form, whose unlit copy writes none of d0's r, g and b) */
    if (program->light.count)
        for (component = 0; component < 3; component++)
            need[temporary_index(program->light.normal)][component] = WL_XBOX_VSH_LIVE_D0;
    for (index = (long)program->count - 1; index >= 0; index--) {
        const WlXboxVshInstruction *instruction = &program->instructions[index];
        unsigned reaches = 0, kills[13] = {0};

        /* what its writes reach */
        if (instruction->mac == _mac_arl)
            reaches |= need_a0;
        else if (instruction->mac != _mac_nop && instruction->mac_mask)
            kills[temporary_index(instruction->temporary)] |= instruction->mac_mask;
        if (instruction->ilu != _ilu_nop && instruction->ilu_mask)
            kills[temporary_index(instruction->mac != _mac_nop ? 1 : instruction->temporary)] |=
                instruction->ilu_mask;
        if (instruction->output_mask && instruction->output_is_register &&
            (instruction->output_from_ilu ? instruction->ilu : instruction->mac) != 0) {
            switch (instruction->output_address) {
            case _output_position:
                kills[12] |= instruction->output_mask;
                break;
            case _output_d0:
                reaches |= WL_XBOX_VSH_LIVE_D0;
                break;
            case _output_d1:
                reaches |= WL_XBOX_VSH_LIVE_D1;
                break;
            case _output_b0:
            case _output_b1:
                reaches |= WL_XBOX_VSH_LIVE_BACK;
                break;
            case _output_fog:
                if (instruction->output_mask & 8)
                    reaches |= WL_XBOX_VSH_LIVE_FOG;
                break;
            case _output_point_size:
                if (instruction->output_mask & 8)
                    reaches |= WL_XBOX_VSH_LIVE_POINT_SIZE;
                break;
            default:
                if (instruction->output_address >= _output_t0 &&
                    instruction->output_address < _output_t0 + 4)
                    reaches |= WL_XBOX_VSH_LIVE_T0 << (instruction->output_address - _output_t0);
                break;
            }
        }
        for (which = 0; which < 13; which++)
            for (component = 0; component < 4; component++)
                if (kills[which] & (8u >> component)) {
                    reaches |= need[which][component];
                    need[which][component] = 0;
                }
        if (instruction->mac == _mac_arl)
            need_a0 = 0;
        /* (the clip-space position: p before this instruction's writes) */
        if (instruction->captures_clip) {
            reaches |= WL_XBOX_VSH_LIVE_POSITION;
            for (component = 0; component < 4; component++)
                need[12][component] |= WL_XBOX_VSH_LIVE_POSITION;
        }
        /* what it reads: its operands, whole */
        if (reaches) {
            int reads[3] = {0, 0, 0};

            if (instruction->mac != _mac_nop) {
                reads[0] = 1;
                if (instruction->mac != _mac_mov && instruction->mac != _mac_arl &&
                    instruction->mac != _mac_add)
                    reads[1] = 1;
            }
            if (instruction->ilu != _ilu_nop || instruction->mac == _mac_add ||
                instruction->mac == _mac_mad)
                reads[2] = 1;
            for (which = 0; which < 3; which++) {
                const WlXboxVshOperand *operand = &instruction->operands[which];

                if (!reads[which])
                    continue;
                if (operand->mux == _mux_temporary)
                    for (component = 0; component < 4; component++)
                        need[temporary_index(operand->index)][operand->swizzle[component] & 3] |=
                            reaches;
                else if (operand->mux == _mux_constant && instruction->relative)
                    need_a0 |= reaches;
            }
        }
        needs[index] = reaches;
    }
}

/* the operands the interpreter reads: A for every MAC operation, B for those
of two vectors, C for ADD, MAD and every ILU operation */
static int step_reads(const WlXboxVshInstruction *instruction, int which) {
    switch (which) {
    case 0:
        return instruction->mac >= _mac_mov && instruction->mac <= _mac_arl;
    case 1:
        return instruction->mac == _mac_mul ||
               (instruction->mac >= _mac_mad && instruction->mac <= _mac_sge);
    default:
        return instruction->mac == _mac_add || instruction->mac == _mac_mad ||
               instruction->ilu != _ilu_nop;
    }
}

/* the interpreter's steps from the instructions' fields */
static void steps_make(WlXboxVshProgram *program) {
    unsigned needs[WL_XBOX_VSH_MAX_INSTRUCTIONS], written[13] = {0};
    unsigned long index;
    int which, component;

    program->exposed = 0;
    program->captures_clip = 0;
    wl_xbox_vsh_live_needs(program, needs);
    for (index = 0; index < program->count; index++) {
        const WlXboxVshInstruction *instruction = &program->instructions[index];
        WlXboxVshStep *step = &program->steps[index];

        memset(step, 0, sizeof(*step));
        step->mac = instruction->mac;
        step->ilu = instruction->ilu;
        step->reaches = (unsigned short)needs[index];
        if (instruction->captures_clip) {
            step->flags |= WL_XBOX_VSH_STEP_CAPTURES_CLIP;
            program->captures_clip = 1;
        }
        for (which = 0; which < 3; which++) {
            const WlXboxVshOperand *operand = &instruction->operands[which];
            WlXboxVshStepOperand *to = &step->operands[which];

            if (!step_reads(instruction, which))
                continue;
            switch (operand->mux) {
            case _mux_temporary:
                to->file = WL_XBOX_VSH_FILE_TEMPORARY;
                to->index = (unsigned char)temporary_index(operand->index);
                break;
            case _mux_input:
                to->file = WL_XBOX_VSH_FILE_INPUT;
                to->index = (unsigned char)(instruction->input & 15);
                break;
            case _mux_constant:
                if (instruction->relative) {
                    to->file = WL_XBOX_VSH_FILE_RELATIVE;
                    to->index = instruction->constant;
                } else {
                    to->file = WL_XBOX_VSH_FILE_CONSTANT;
                    to->index = (unsigned char)(instruction->constant < WL_XBOX_VSH_CONSTANT_COUNT
                                                    ? instruction->constant
                                                    : WL_XBOX_VSH_CONSTANT_COUNT - 1);
                }
                break;
            default:
                to->file = WL_XBOX_VSH_FILE_ZERO;
                break;
            }
            to->swizzle = (unsigned char)((operand->swizzle[0] & 3) | (operand->swizzle[1] & 3) << 2 |
                                          (operand->swizzle[2] & 3) << 4 |
                                          (operand->swizzle[3] & 3) << 6);
            /* (a temporary read before any instruction writes it: 0) */
            if (to->file == WL_XBOX_VSH_FILE_TEMPORARY && to->index < 12)
                for (component = 0; component < 4; component++)
                    if (!(written[to->index] & (8u >> (operand->swizzle[component] & 3))))
                        program->exposed |= (unsigned short)(1u << to->index);
            if (operand->negate)
                to->file |= WL_XBOX_VSH_NEGATE;
        }
        if (instruction->mac == _mac_arl) {
            step->flags |= WL_XBOX_VSH_STEP_ARL;
        } else if (instruction->mac != _mac_nop && instruction->mac_mask) {
            step->mac_register = (unsigned char)temporary_index(instruction->temporary);
            step->mac_mask = instruction->mac_mask;
            written[step->mac_register] |= step->mac_mask;
        }
        if (instruction->ilu != _ilu_nop && instruction->ilu_mask) {
            step->ilu_register = (unsigned char)temporary_index(
                instruction->mac != _mac_nop ? 1 : instruction->temporary);
            step->ilu_mask = instruction->ilu_mask;
            written[step->ilu_register] |= step->ilu_mask;
        }
        if (instruction->output_mask && instruction->output_is_register &&
            (instruction->output_from_ilu ? instruction->ilu : instruction->mac) != 0) {
            unsigned char output = WL_XBOX_VSH_TO_NONE, mask = instruction->output_mask;

            switch (instruction->output_address) {
            case _output_position:
                output = WL_XBOX_VSH_TO_POSITION;
                break;
            case _output_d0:
                output = WL_XBOX_VSH_TO_D0;
                break;
            case _output_d1:
                output = WL_XBOX_VSH_TO_D1;
                break;
            case _output_b0:
                output = WL_XBOX_VSH_TO_B0;
                break;
            case _output_b1:
                output = WL_XBOX_VSH_TO_B1;
                break;
            case _output_fog:
                output = WL_XBOX_VSH_TO_FOG;
                mask &= 8;
                break;
            case _output_point_size:
                output = WL_XBOX_VSH_TO_POINT_SIZE;
                mask &= 8;
                break;
            default:
                if (instruction->output_address >= _output_t0 &&
                    instruction->output_address < _output_t0 + 4)
                    output = (unsigned char)(WL_XBOX_VSH_TO_T0 + instruction->output_address -
                                             _output_t0);
                break;
            }
            if (output != WL_XBOX_VSH_TO_NONE && mask) {
                step->output = output;
                step->output_mask = mask;
                if (instruction->output_from_ilu)
                    step->flags |= WL_XBOX_VSH_STEP_FROM_ILU;
            }
        }
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
    memset(&program->light, 0, sizeof program->light);
    steps_make(program);
    wl_xbox_vsh_light_shape(program, &program->light);
    wl_xbox_vsh_world_position(program, &program->world);
    program->native = NULL;
    program->native_colors = NULL;
    program->native_positions = NULL;
    program->native_batch = NULL;
    program->native_fused = NULL;
    program->native_fused_count = 0;
    program->hash = wl_xbox_vsh_hash(instructions, instruction_count);
    program->serial = 0;
    {
        unsigned long hash = program->hash;

        for (index = 0; natives && index < native_count; index++) {
            if (natives[index].hash == hash && natives[index].count == instruction_count &&
                natives[index].check == wl_xbox_vsh_check(instructions, instruction_count)) {
                program->native = natives[index].run;
                program->native_colors = natives[index].colors;
                program->native_positions = natives[index].positions;
                program->native_batch = natives[index].batch;
                program->native_fused = natives[index].fused;
                program->native_fused_count = natives[index].fused_count;
                break;
            }
        }
    }
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

static uint32_t rotate_left(uint32_t x, unsigned n) {
    return (x << n) | (x >> (32u - n));
}

unsigned long wl_xbox_vsh_check(const uint32_t *instructions, unsigned long instruction_count) {
    uint32_t hash = 0x9E3779B9u, k;
    unsigned long index;

    for (index = 0; index < instruction_count * 4; index++) {
        k = rotate_left(instructions[index] * 0xCC9E2D51u, 15) * 0x1B873593u;
        hash = rotate_left(hash ^ k, 13) * 5u + 0xE6546B64u;
    }
    hash ^= (uint32_t)(instruction_count * 16u);
    hash ^= hash >> 16;
    hash *= 0x85EBCA6Bu;
    hash ^= hash >> 13;
    hash *= 0xC2B2AE35u;
    hash ^= hash >> 16;
    return hash;
}

/* floorf for the ILU, without newlib's call: 2^23 added and taken away
rounds to a whole number (wl_xbox_vsh_floor_index's way), 1 less where it
rounded up; from 2^23 on (and a NaN) every float is its own floor */
static float ilu_floor(float x) {
    float whole;

    if (!(x > -8388608.0f && x < 8388608.0f))
        return x;
    whole = x >= 0.0f ? (x + 8388608.0f) - 8388608.0f : (x - 8388608.0f) + 8388608.0f;
    return whole > x ? whole - 1.0f : whole;
}

/* 2^n for a whole n in -149..127 (below, 0; above, infinity), made from its
bits */
static float ilu_power_of_two(int n) {
    union {
        uint32_t u;
        float f;
    } bits;

    if (n > 127)
        return INFINITY;
    if (n < -149)
        return 0.0f;
    if (n < -126) {
        bits.u = (uint32_t)(n + 64 + 127) << 23;
        return bits.f * 5.42101086e-20f; /* (2^-64) */
    }
    bits.u = (uint32_t)(n + 127) << 23;
    return bits.f;
}

/* 2^x without newlib's exp2f (which is its powf, on the Wii most of a
heavy program's time): x split into a whole n and f in [-1/2, 1/2], 2^f a
polynomial fitted to it (a relative error of 2e-9 before the float's
rounding: within an ulp or two of exp2f), 2^n from its bits. The NV2A's
own EXP and LOG are approximations too. */
static float ilu_exp2(float x) {
    float n, f, p;

    if (!(x < 128.0f))
        return x != x ? x : INFINITY;
    if (!(x > -150.0f))
        return 0.0f;
    n = (x + 12582912.0f) - 12582912.0f; /* (1.5 * 2^23: x rounded to the nearest) */
    f = x - n;
    p = 1.53375775e-4f;
    p = p * f + 1.33998599e-3f;
    p = p * f + 9.61851981e-3f;
    p = p * f + 5.55032901e-2f;
    p = p * f + 2.40226462e-1f;
    p = p * f + 6.93147182e-1f;
    p = p * f + 1.0f;
    if (n < -126.0f)
        return p * ilu_power_of_two((int)n + 64) * 5.42101086e-20f;
    return p * ilu_power_of_two((int)n);
}

/* log2 of a finite x > 0, its exponent e (floor(log2(x)), exactly) and
mantissa m (x / 2^e in [1, 2), exactly) from its bits; the logarithm e +
log2(m), log2(m) as t P(t^2) with t = (m - 1) / (m + 1), m taken to
[sqrt(1/2), sqrt(2)) (an error of 6e-12 before the float's rounding) */
static float ilu_log2(float x, float *exponent, float *mantissa) {
    union {
        uint32_t u;
        float f;
    } bits;
    int e;
    float m, t, t2, p;

    bits.f = x;
    e = -127;
    if ((bits.u & 0x7F800000u) == 0) {
        bits.f = x * 8388608.0f; /* (a subnormal: 2^23 times it) */
        e -= 23;
    }
    e += (int)((bits.u >> 23) & 0xFFu);
    bits.u = (bits.u & 0x007FFFFFu) | 0x3F800000u;
    m = bits.f;
    *exponent = (float)e;
    *mantissa = m;
    if (m >= 1.41421356f) {
        m *= 0.5f;
        e++;
    }
    t = (m - 1.0f) / (m + 1.0f);
    t2 = t * t;
    p = 3.39634150e-1f;
    p = p * t2 + 4.11730886e-1f;
    p = p * t2 + 5.77082634e-1f;
    p = p * t2 + 9.61796701e-1f;
    p = p * t2 + 2.88539004e+0f;
    return (float)e + t * p;
}

/* the ILU's exponent, logarithm and lighting (from C into I) */
void wl_xbox_vsh_ilu(int ilu, const float c[4], float out[4]) {
    switch (ilu) {
    case _ilu_exp: {
        float whole = ilu_floor(c[0]);

        out[0] = whole > -150.0f && whole < 128.0f ? ilu_power_of_two((int)whole) : ilu_exp2(whole);
        out[1] = c[0] - whole;
        out[2] = ilu_exp2(c[0]);
        out[3] = 1.0f;
        break;
    }
    case _ilu_log: {
        float x = fabsf(c[0]);

        if (x == 0.0f) {
            out[0] = out[2] = -1.0e30f;
            out[1] = out[3] = 1.0f;
        } else if (!(x < INFINITY)) {
            /* (infinity or a NaN: as libm's) */
            out[0] = floorf(log2f(x));
            out[1] = x / exp2f(out[0]);
            out[2] = log2f(x);
            out[3] = 1.0f;
        } else {
            out[2] = ilu_log2(x, &out[0], &out[1]);
            out[3] = 1.0f;
        }
        break;
    }
    case _ilu_lit: {
        float power = c[3] < -127.9961f ? -127.9961f : c[3] > 127.9961f ? 127.9961f : c[3];
        float exponent, mantissa;

        out[0] = 1.0f;
        out[1] = c[0] > 0.0f ? c[0] : 0.0f;
        /* (0 unless both are above 0, as Direct3D's lit: never 0 to the power,
        which is 1 for a power of 0 and infinity for one below; c[1] to the power
        as 2 to the power times its logarithm) */
        out[2] = c[0] > 0.0f && c[1] > 0.0f
                     ? (c[1] < INFINITY ? ilu_exp2(power * ilu_log2(c[1], &exponent, &mantissa)) : powf(c[1], power))
                     : 0.0f;
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


/* ---------- the interpreter

Each instruction runs for a batch of vertices (the registers of vertex n are
r[n], a0[n]) before the next: what it reads and writes is decided once for
all of them. For each vertex the operations are those of a run for it alone,
in the same order (its operands read before its results are written), so
the results are the same to the bit.

An operand is read through a view: vertex n's four components, swizzled and
signed, at base + n stride bytes. A constant's (or 0's) is made once for the
batch (stride 0); a temporary or an input read as it is, in place; any other
(swizzled, negated, or a constant by a0) is gathered first into the scratch. */

typedef struct vsh_view {
    const unsigned char *base;
    size_t stride;
} vsh_view;

#define VSH_AT(view, n) ((const float *)(const void *)((view).base + (size_t)(n) * (view).stride))

/* (the identity swizzle: x y z w) */
enum { VSH_XYZW = 0 | 1 << 2 | 2 << 4 | 3 << 6 };

static void view_make(vsh_view *view, const WlXboxVshStepOperand *operand, float (*r)[13][4],
                      const float (*inputs)[WL_XBOX_VSH_INPUT_COUNT][4],
                      const float (*constants)[4], const int *a0, unsigned count,
                      float uniform[4], float (*gathered)[4]) {
    unsigned x = operand->swizzle & 3u, y = (operand->swizzle >> 2) & 3u,
             z = (operand->swizzle >> 4) & 3u, w = (operand->swizzle >> 6) & 3u, n;
    int negate = (operand->file & WL_XBOX_VSH_NEGATE) != 0;
    const unsigned char *base;
    size_t stride;

    switch (operand->file & ~WL_XBOX_VSH_NEGATE) {
    case WL_XBOX_VSH_FILE_TEMPORARY:
        base = (const unsigned char *)r[0][operand->index];
        stride = sizeof(r[0]);
        break;
    case WL_XBOX_VSH_FILE_INPUT:
        base = (const unsigned char *)inputs[0][operand->index];
        stride = sizeof(inputs[0]);
        break;
    case WL_XBOX_VSH_FILE_RELATIVE:
        for (n = 0; n < count; n++) {
            const float *p = constants[wl_xbox_vsh_clamp(operand->index + a0[n])];

            if (negate) {
                gathered[n][0] = -p[x];
                gathered[n][1] = -p[y];
                gathered[n][2] = -p[z];
                gathered[n][3] = -p[w];
            } else {
                gathered[n][0] = p[x];
                gathered[n][1] = p[y];
                gathered[n][2] = p[z];
                gathered[n][3] = p[w];
            }
        }
        view->base = (const unsigned char *)gathered;
        view->stride = sizeof(gathered[0]);
        return;
    default: {
        /* a constant, or 0: the same for every vertex */
        const float *p = (operand->file & ~WL_XBOX_VSH_NEGATE) == WL_XBOX_VSH_FILE_CONSTANT
                             ? constants[operand->index]
                             : wl_xbox_vsh_zero;

        if (negate) {
            uniform[0] = -p[x];
            uniform[1] = -p[y];
            uniform[2] = -p[z];
            uniform[3] = -p[w];
        } else {
            uniform[0] = p[x];
            uniform[1] = p[y];
            uniform[2] = p[z];
            uniform[3] = p[w];
        }
        view->base = (const unsigned char *)uniform;
        view->stride = 0;
        return;
    }
    }
    if (operand->swizzle == VSH_XYZW && !negate) {
        view->base = base;
        view->stride = stride;
        return;
    }
    for (n = 0; n < count; n++) {
        const float *p = (const float *)(const void *)(base + n * stride);

        if (negate) {
            gathered[n][0] = -p[x];
            gathered[n][1] = -p[y];
            gathered[n][2] = -p[z];
            gathered[n][3] = -p[w];
        } else {
            gathered[n][0] = p[x];
            gathered[n][1] = p[y];
            gathered[n][2] = p[z];
            gathered[n][3] = p[w];
        }
    }
    view->base = (const unsigned char *)gathered;
    view->stride = sizeof(gathered[0]);
}

/* the x component alone (the ILU's scalar operations), each vertex's at
base + n stride bytes, negated or not; a constant by a0 is gathered */
static void scalar_view(vsh_view *view, int *negate, const WlXboxVshStepOperand *operand,
                        float (*r)[13][4], const float (*inputs)[WL_XBOX_VSH_INPUT_COUNT][4],
                        const float (*constants)[4], const int *a0, unsigned count,
                        float uniform[4], float (*gathered)[4]) {
    unsigned x = operand->swizzle & 3u, n;

    *negate = (operand->file & WL_XBOX_VSH_NEGATE) != 0;
    switch (operand->file & ~WL_XBOX_VSH_NEGATE) {
    case WL_XBOX_VSH_FILE_TEMPORARY:
        view->base = (const unsigned char *)&r[0][operand->index][x];
        view->stride = sizeof(r[0]);
        return;
    case WL_XBOX_VSH_FILE_INPUT:
        view->base = (const unsigned char *)&inputs[0][operand->index][x];
        view->stride = sizeof(inputs[0]);
        return;
    case WL_XBOX_VSH_FILE_RELATIVE:
        for (n = 0; n < count; n++)
            gathered[n][0] = constants[wl_xbox_vsh_clamp(operand->index + a0[n])][x];
        view->base = (const unsigned char *)gathered;
        view->stride = sizeof(gathered[0]);
        return;
    case WL_XBOX_VSH_FILE_CONSTANT:
        uniform[0] = constants[operand->index][x];
        break;
    default:
        uniform[0] = 0.0f;
        break;
    }
    view->base = (const unsigned char *)uniform;
    view->stride = 0;
}

/* a result's masked components (bit 3 x) into each vertex's register or
output, stride bytes apart */
static void write_batch(unsigned char *destination, size_t stride, const float (*value)[4],
                        unsigned count, unsigned mask) {
    unsigned n;

    if (mask == 15) {
        for (n = 0; n < count; n++, destination += stride) {
            float *d = (float *)(void *)destination;
            float x = value[n][0], y = value[n][1], z = value[n][2], w = value[n][3];

            d[0] = x;
            d[1] = y;
            d[2] = z;
            d[3] = w;
        }
        return;
    }
    for (n = 0; n < count; n++, destination += stride) {
        float *d = (float *)(void *)destination;

        if (mask & 8)
            d[0] = value[n][0];
        if (mask & 4)
            d[1] = value[n][1];
        if (mask & 2)
            d[2] = value[n][2];
        if (mask & 1)
            d[3] = value[n][3];
    }
}

/* the registers of a batch (WlXboxVshWork's, or one vertex's on the stack) */
typedef struct vsh_scratch {
    float (*r)[13][4];
    float (*mac)[4], (*ilu)[4];
    float (*operands[3])[4];
    int *a0;
} vsh_scratch;

static void interpret(const WlXboxVshProgram *program, const float (*constants)[4],
                      const float (*inputs)[WL_XBOX_VSH_INPUT_COUNT][4], unsigned count,
                      float point_size, WlXboxVshOutput *outputs, unsigned live,
                      const vsh_scratch *scratch) {
    float (*r)[13][4] = scratch->r, (*mac)[4] = scratch->mac, (*ilu)[4] = scratch->ilu;
    int *a0 = scratch->a0;
    unsigned long index;
    unsigned n, t;

    /* the registers, and the outputs' parts no instruction may write */
    for (n = 0; n < count; n++) {
        WlXboxVshOutput *o = &outputs[n];

        a0[n] = 0;
        r[n][12][0] = r[n][12][1] = r[n][12][2] = 0.0f;
        r[n][12][3] = 1.0f;
        for (t = 0; t < 12; t++)
            if (program->exposed & (1u << t))
                r[n][t][0] = r[n][t][1] = r[n][t][2] = r[n][t][3] = 0.0f;
        if (live & WL_XBOX_VSH_LIVE_POSITION) {
            o->clip[0] = o->clip[1] = o->clip[2] = o->clip[3] = 0.0f;
            o->clip_captured = 0;
        }
        if (live & WL_XBOX_VSH_LIVE_D0) {
            o->d0[0] = o->d0[1] = o->d0[2] = 0.0f;
            o->d0[3] = 1.0f;
        }
        if (live & WL_XBOX_VSH_LIVE_D1) {
            o->d1[0] = o->d1[1] = o->d1[2] = 0.0f;
            o->d1[3] = 1.0f;
        }
        if (live & WL_XBOX_VSH_LIVE_BACK) {
            o->b0[0] = o->b0[1] = o->b0[2] = 0.0f;
            o->b1[0] = o->b1[1] = o->b1[2] = 0.0f;
            o->b0[3] = o->b1[3] = 1.0f;
        }
        for (t = 0; t < 4; t++)
            if (live & (WL_XBOX_VSH_LIVE_T0 << t)) {
                o->t[t][0] = o->t[t][1] = o->t[t][2] = 0.0f;
                o->t[t][3] = 1.0f;
            }
        if (live & WL_XBOX_VSH_LIVE_FOG)
            o->fog = 1.0f;
        if (live & WL_XBOX_VSH_LIVE_POINT_SIZE)
            o->point_size = point_size;
    }

    for (index = 0; index < program->count; index++) {
        const WlXboxVshStep *step = &program->steps[index];
        float uniform[3][4];
        vsh_view a, b, c;
        int scalar = step->ilu == _ilu_rcp || step->ilu == _ilu_rcc || step->ilu == _ilu_rsq;

        if (!(step->reaches & live))
            continue;
        if (step->mac >= _mac_mov && step->mac <= _mac_arl)
            view_make(&a, &step->operands[0], r, inputs, constants, a0, count, uniform[0],
                      scratch->operands[0]);
        if (step->mac == _mac_mul || (step->mac >= _mac_mad && step->mac <= _mac_sge))
            view_make(&b, &step->operands[1], r, inputs, constants, a0, count, uniform[1],
                      scratch->operands[1]);
        if (step->mac == _mac_add || step->mac == _mac_mad ||
            (step->ilu != _ilu_nop && !scalar))
            view_make(&c, &step->operands[2], r, inputs, constants, a0, count, uniform[2],
                      scratch->operands[2]);

        /* the MAC's result for each vertex */
        switch (step->mac) {
        case _mac_nop:
            break;
        case _mac_mov:
        case _mac_arl:
            for (n = 0; n < count; n++) {
                const float *x = VSH_AT(a, n);
                float x0 = x[0], x1 = x[1], x2 = x[2], x3 = x[3];

                mac[n][0] = x0;
                mac[n][1] = x1;
                mac[n][2] = x2;
                mac[n][3] = x3;
            }
            break;
        case _mac_mul:
            for (n = 0; n < count; n++) {
                const float *x = VSH_AT(a, n), *y = VSH_AT(b, n);
                float x0 = x[0], x1 = x[1], x2 = x[2], x3 = x[3];
                float y0 = y[0], y1 = y[1], y2 = y[2], y3 = y[3];

                mac[n][0] = x0 * y0;
                mac[n][1] = x1 * y1;
                mac[n][2] = x2 * y2;
                mac[n][3] = x3 * y3;
            }
            break;
        case _mac_add:
            for (n = 0; n < count; n++) {
                const float *x = VSH_AT(a, n), *z = VSH_AT(c, n);
                float x0 = x[0], x1 = x[1], x2 = x[2], x3 = x[3];
                float z0 = z[0], z1 = z[1], z2 = z[2], z3 = z[3];

                mac[n][0] = x0 + z0;
                mac[n][1] = x1 + z1;
                mac[n][2] = x2 + z2;
                mac[n][3] = x3 + z3;
            }
            break;
        case _mac_mad:
            for (n = 0; n < count; n++) {
                const float *x = VSH_AT(a, n), *y = VSH_AT(b, n), *z = VSH_AT(c, n);
                float x0 = x[0], x1 = x[1], x2 = x[2], x3 = x[3];
                float y0 = y[0], y1 = y[1], y2 = y[2], y3 = y[3];
                float z0 = z[0], z1 = z[1], z2 = z[2], z3 = z[3];

                mac[n][0] = x0 * y0 + z0;
                mac[n][1] = x1 * y1 + z1;
                mac[n][2] = x2 * y2 + z2;
                mac[n][3] = x3 * y3 + z3;
            }
            break;
        case _mac_dp3:
            for (n = 0; n < count; n++) {
                const float *x = VSH_AT(a, n), *y = VSH_AT(b, n);
                float d = x[0] * y[0] + x[1] * y[1] + x[2] * y[2];

                mac[n][0] = mac[n][1] = mac[n][2] = mac[n][3] = d;
            }
            break;
        case _mac_dph:
            for (n = 0; n < count; n++) {
                const float *x = VSH_AT(a, n), *y = VSH_AT(b, n);
                float d = x[0] * y[0] + x[1] * y[1] + x[2] * y[2] + y[3];

                mac[n][0] = mac[n][1] = mac[n][2] = mac[n][3] = d;
            }
            break;
        case _mac_dp4:
            for (n = 0; n < count; n++) {
                const float *x = VSH_AT(a, n), *y = VSH_AT(b, n);
                float d = x[0] * y[0] + x[1] * y[1] + x[2] * y[2] + x[3] * y[3];

                mac[n][0] = mac[n][1] = mac[n][2] = mac[n][3] = d;
            }
            break;
        case _mac_dst:
            for (n = 0; n < count; n++) {
                const float *x = VSH_AT(a, n), *y = VSH_AT(b, n);
                float m1 = x[1] * y[1], m2 = x[2], m3 = y[3];

                mac[n][0] = 1.0f;
                mac[n][1] = m1;
                mac[n][2] = m2;
                mac[n][3] = m3;
            }
            break;
        case _mac_min:
            for (n = 0; n < count; n++) {
                const float *x = VSH_AT(a, n), *y = VSH_AT(b, n);
                float x0 = x[0], x1 = x[1], x2 = x[2], x3 = x[3];
                float y0 = y[0], y1 = y[1], y2 = y[2], y3 = y[3];

                mac[n][0] = x0 < y0 ? x0 : y0;
                mac[n][1] = x1 < y1 ? x1 : y1;
                mac[n][2] = x2 < y2 ? x2 : y2;
                mac[n][3] = x3 < y3 ? x3 : y3;
            }
            break;
        case _mac_max:
            for (n = 0; n < count; n++) {
                const float *x = VSH_AT(a, n), *y = VSH_AT(b, n);
                float x0 = x[0], x1 = x[1], x2 = x[2], x3 = x[3];
                float y0 = y[0], y1 = y[1], y2 = y[2], y3 = y[3];

                mac[n][0] = x0 > y0 ? x0 : y0;
                mac[n][1] = x1 > y1 ? x1 : y1;
                mac[n][2] = x2 > y2 ? x2 : y2;
                mac[n][3] = x3 > y3 ? x3 : y3;
            }
            break;
        case _mac_slt:
            for (n = 0; n < count; n++) {
                const float *x = VSH_AT(a, n), *y = VSH_AT(b, n);
                float x0 = x[0], x1 = x[1], x2 = x[2], x3 = x[3];
                float y0 = y[0], y1 = y[1], y2 = y[2], y3 = y[3];

                mac[n][0] = x0 < y0 ? 1.0f : 0.0f;
                mac[n][1] = x1 < y1 ? 1.0f : 0.0f;
                mac[n][2] = x2 < y2 ? 1.0f : 0.0f;
                mac[n][3] = x3 < y3 ? 1.0f : 0.0f;
            }
            break;
        case _mac_sge:
            for (n = 0; n < count; n++) {
                const float *x = VSH_AT(a, n), *y = VSH_AT(b, n);
                float x0 = x[0], x1 = x[1], x2 = x[2], x3 = x[3];
                float y0 = y[0], y1 = y[1], y2 = y[2], y3 = y[3];

                mac[n][0] = x0 >= y0 ? 1.0f : 0.0f;
                mac[n][1] = x1 >= y1 ? 1.0f : 0.0f;
                mac[n][2] = x2 >= y2 ? 1.0f : 0.0f;
                mac[n][3] = x3 >= y3 ? 1.0f : 0.0f;
            }
            break;
        default:
            /* (an operation the MAC does not have: 0) */
            for (n = 0; n < count; n++)
                mac[n][0] = mac[n][1] = mac[n][2] = mac[n][3] = 0.0f;
            break;
        }

        /* the ILU's */
        if (scalar) {
            vsh_view s;
            int negate;

            scalar_view(&s, &negate, &step->operands[2], r, inputs, constants, a0, count, uniform[2],
                        scratch->operands[2]);
            for (n = 0; n < count; n++) {
                float x = *VSH_AT(s, n), i;

                if (negate)
                    x = -x;
                i = step->ilu == _ilu_rcp   ? 1.0f / x
                    : step->ilu == _ilu_rcc ? wl_xbox_vsh_reciprocal_clamped(x)
                                            : 1.0f / wl_xbox_vsh_sqrtf(fabsf(x));
                ilu[n][0] = ilu[n][1] = ilu[n][2] = ilu[n][3] = i;
            }
        } else if (step->ilu == _ilu_mov) {
            for (n = 0; n < count; n++) {
                const float *z = VSH_AT(c, n);
                float z0 = z[0], z1 = z[1], z2 = z[2], z3 = z[3];

                ilu[n][0] = z0;
                ilu[n][1] = z1;
                ilu[n][2] = z2;
                ilu[n][3] = z3;
            }
        } else if (step->ilu != _ilu_nop) {
            for (n = 0; n < count; n++) {
                float z[4];

                memcpy(z, VSH_AT(c, n), sizeof(z));
                wl_xbox_vsh_ilu(step->ilu, z, ilu[n]);
            }
        }

        /* the screen-space conversion: the position before it */
        if (step->flags & WL_XBOX_VSH_STEP_CAPTURES_CLIP)
            for (n = 0; n < count; n++) {
                memcpy(outputs[n].clip, r[n][12], sizeof(outputs[n].clip));
                outputs[n].clip_captured = 1;
            }

        /* results written once both units have read their inputs */
        if (step->flags & WL_XBOX_VSH_STEP_ARL) {
            for (n = 0; n < count; n++)
                a0[n] = wl_xbox_vsh_floor_index(mac[n][0] + 0.001f);
        } else if (step->mac_mask) {
            write_batch((unsigned char *)r[0][step->mac_register], sizeof(r[0]),
                        (const float (*)[4])mac, count, step->mac_mask);
        }
        if (step->ilu_mask)
            write_batch((unsigned char *)r[0][step->ilu_register], sizeof(r[0]),
                        (const float (*)[4])ilu, count, step->ilu_mask);
        if (step->output != WL_XBOX_VSH_TO_NONE) {
            const float (*value)[4] =
                (const float (*)[4])(step->flags & WL_XBOX_VSH_STEP_FROM_ILU ? ilu : mac);
            unsigned char *destination = (unsigned char *)outputs;
            size_t stride = sizeof(outputs[0]);

            switch (step->output) {
            case WL_XBOX_VSH_TO_POSITION:
                destination = (unsigned char *)r[0][12];
                stride = sizeof(r[0]);
                break;
            case WL_XBOX_VSH_TO_D0:
                destination += offsetof(WlXboxVshOutput, d0);
                break;
            case WL_XBOX_VSH_TO_D1:
                destination += offsetof(WlXboxVshOutput, d1);
                break;
            case WL_XBOX_VSH_TO_B0:
                destination += offsetof(WlXboxVshOutput, b0);
                break;
            case WL_XBOX_VSH_TO_B1:
                destination += offsetof(WlXboxVshOutput, b1);
                break;
            case WL_XBOX_VSH_TO_FOG:
                destination += offsetof(WlXboxVshOutput, fog);
                break;
            case WL_XBOX_VSH_TO_POINT_SIZE:
                destination += offsetof(WlXboxVshOutput, point_size);
                break;
            default:
                destination += offsetof(WlXboxVshOutput, t) +
                               sizeof(float[4]) * (size_t)(step->output - WL_XBOX_VSH_TO_T0);
                break;
            }
            write_batch(destination, stride, value, count, step->output_mask);
        }
    }
    for (n = 0; n < count; n++)
        memcpy(outputs[n].position, r[n][12], sizeof(outputs[n].position));
}

/* one vertex, its registers on the stack */
static void interpret_one(const WlXboxVshProgram *program, const float (*constants)[4],
                          const float (*inputs)[4], float point_size, WlXboxVshOutput *output,
                          unsigned live) {
    float r[1][13][4], mac[1][4], ilu[1][4], operands[3][1][4];
    int a0[1];
    vsh_scratch scratch;

    scratch.r = r;
    scratch.mac = mac;
    scratch.ilu = ilu;
    scratch.operands[0] = operands[0];
    scratch.operands[1] = operands[1];
    scratch.operands[2] = operands[2];
    scratch.a0 = a0;
    interpret(program, constants, (const float (*)[WL_XBOX_VSH_INPUT_COUNT][4])inputs, 1,
              point_size, output, live, &scratch);
}

void wl_xbox_vsh_run(const WlXboxVshProgram *program, const float (*constants)[4],
                     const float (*inputs)[4], float point_size, WlXboxVshOutput *output) {
    if (program->native) {
        output_clear(output, point_size);
        program->native(constants, inputs, output, WL_XBOX_VSH_LIVE_ALL);
        return;
    }
    interpret_one(program, constants, inputs, point_size, output, WL_XBOX_VSH_LIVE_ALL);
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
    interpret_one(program, constants, inputs, point_size, output, live);
}

void wl_xbox_vsh_run_batch(const WlXboxVshProgram *program, const float (*constants)[4],
                           const float (*inputs)[WL_XBOX_VSH_INPUT_COUNT][4], unsigned count,
                           float point_size, WlXboxVshOutput *outputs, unsigned live,
                           WlXboxVshWork *work) {
    vsh_scratch scratch;
    unsigned done, n;

    if (program->native && program->native_batch) {
        if (live & WL_XBOX_VSH_LIVE_POINT_SIZE)
            for (done = 0; done < count; done++)
                outputs[done].point_size = point_size;
        program->native_batch(constants, inputs, outputs, count, live);
        return;
    }
    if (program->native) {
        for (done = 0; done < count; done++)
            wl_xbox_vsh_run_live(program, constants, inputs[done], point_size, &outputs[done], live);
        return;
    }
    scratch.r = work->r;
    scratch.mac = work->mac;
    scratch.ilu = work->ilu;
    scratch.operands[0] = work->operands[0];
    scratch.operands[1] = work->operands[1];
    scratch.operands[2] = work->operands[2];
    scratch.a0 = work->a0;
    for (done = 0; done < count; done += n) {
        n = count - done < WL_XBOX_VSH_BATCH ? count - done : WL_XBOX_VSH_BATCH;
        interpret(program, constants, inputs + done, n, point_size, outputs + done, live, &scratch);
    }
}

unsigned long wl_xbox_vsh_inputs_read(const WlXboxVshProgram *program, unsigned live) {
    unsigned long reads = 0, index;
    int which;

    for (index = 0; index < program->count; index++) {
        const WlXboxVshStep *step = &program->steps[index];

        if (!(step->reaches & live))
            continue;
        for (which = 0; which < 3; which++)
            if ((step->operands[which].file & ~WL_XBOX_VSH_NEGATE) == WL_XBOX_VSH_FILE_INPUT)
                reads |= 1UL << step->operands[which].index;
    }
    return reads;
}

int wl_xbox_vsh_run_positions(const WlXboxVshProgram *program, const float (*constants)[4],
                              const float (*inputs)[4], const float (*points)[4], float (*clips)[4],
                              float (*positions)[4]) {
    if (!program->native_positions)
        return 0;
    program->native_positions(constants, inputs, points, clips, positions);
    return 1;
}

/* ---------- the clip-space position's shape (wl_xbox_vsh_world_position) */

/* the last instruction before `before` that writes the position's component
(-1: none) */
static long world_writer(const WlXboxVshProgram *program, long before, unsigned component) {
    long index;

    for (index = before - 1; index >= 0; index--) {
        const WlXboxVshInstruction *in = &program->instructions[index];

        if (in->output_is_register && in->output_address == _output_position && (in->output_mask & (8u >> component)) &&
            (in->output_from_ilu ? in->ilu : in->mac) != 0)
            return index;
    }
    return -1;
}

int wl_xbox_vsh_world_position(const WlXboxVshProgram *program, WlXboxVshWorldPosition *world) {
    long capture = -1, first = (long)program->count, index;
    unsigned c, j;

    memset(world, 0, sizeof(*world));
    for (index = 0; index < (long)program->count && capture < 0; index++)
        if (program->instructions[index].captures_clip)
            capture = index;
    if (capture < 0)
        return 0;
    for (c = 0; c < 4; c++) {
        long at = world_writer(program, capture, c);
        const WlXboxVshInstruction *in;
        const WlXboxVshOperand *r, *k;

        if (at < 0)
            return 0;
        in = &program->instructions[at];
        r = &in->operands[0];
        k = &in->operands[1];
        if (in->output_from_ilu || in->mac != _mac_dph || in->relative || r->mux != _mux_temporary || r->negate ||
            r->index >= 12 || k->mux != _mux_constant || k->negate || (c != 0 && r->index != world->temporary))
            return 0;
        for (j = 0; j < 4; j++)
            if ((j < 3 && r->swizzle[j] != j) || k->swizzle[j] != j)
                return 0;
        world->temporary = r->index;
        world->constant[c] = in->constant;
        first = at < first ? at : first;
    }
    /* (the temporary's x, y and z the same from the first dph to the capture, and to the end) */
    world->to_end = 1;
    for (index = first; index < (long)program->count; index++) {
        const WlXboxVshInstruction *in = &program->instructions[index];

        if ((in->mac != _mac_nop && in->mac != _mac_arl && (in->mac_mask & 0xEu) && in->temporary == world->temporary) ||
            (in->ilu != _ilu_nop && (in->ilu_mask & 0xEu) &&
             (in->mac != _mac_nop ? 1u : in->temporary) == world->temporary)) {
            if (index <= capture) {
                memset(world, 0, sizeof(*world));
                return 0;
            }
            world->to_end = 0;
        }
    }
    world->found = 1;
    return 1;
}

/* ---------- d0's light shape (wl_xbox_vsh_light_shape)

A walk back from the instructions that write d0's r, g and b through the
values they read, each a MAC's result at one component (a dot product's the
same at all four) */

typedef struct {
    long at;
    unsigned component;
} LightValue;

/* the instruction before `before` whose MAC writes temporary t's component
(-1: none; -2: the ILU wrote it last, which the walk does not follow) */
static long light_writer(const WlXboxVshProgram *program, long before, unsigned t, unsigned component) {
    long index;

    for (index = before - 1; index >= 0; index--) {
        const WlXboxVshInstruction *in = &program->instructions[index];
        unsigned bit = 8u >> component;

        if (in->ilu != _ilu_nop && (in->ilu_mask & bit) && (in->mac != _mac_nop ? 1u : in->temporary) == t)
            return -2;
        if (in->mac != _mac_nop && in->mac != _mac_arl && (in->mac_mask & bit) && in->temporary == t)
            return index;
    }
    return -1;
}

/* operand `which` of the instruction at `at`, for its result's component:
1 a temporary's MAC value (*value), 2 an input, 3 a constant (*index, and
*read the component read), 0 what the walk does not take; *negate its sign */
static int light_operand(const WlXboxVshProgram *program, long at, unsigned which, unsigned component,
                         LightValue *value, unsigned *index, unsigned *read, int *negate) {
    const WlXboxVshInstruction *in = &program->instructions[at];
    const WlXboxVshOperand *o = &in->operands[which];

    *negate = o->negate != 0;
    *read = o->swizzle[component];
    switch (o->mux) {
    case _mux_temporary:
        if (o->index >= 12)
            return 0;
        value->component = *read;
        value->at = light_writer(program, at, o->index, *read);
        *index = o->index;
        return value->at >= 0 ? 1 : 0;
    case _mux_input:
        *index = in->input;
        return 2;
    case _mux_constant:
        if (in->relative)
            return 0;
        *index = in->constant;
        return 3;
    default:
        return 0;
    }
}

/* a dot product of the normal and a constant's xyz at `at`: into *light,
the normal's temporary checked (or set) */
static int light_dot(const WlXboxVshProgram *program, long at, WlXboxVshLightShape *shape, WlXboxVshLight *light) {
    const WlXboxVshInstruction *in = &program->instructions[at];
    unsigned which, c;

    if (in->mac != _mac_dp3 || in->relative)
        return 0;
    for (which = 0; which < 2; which++) {
        const WlXboxVshOperand *n = &in->operands[which], *d = &in->operands[1 - which];

        if (n->mux != _mux_temporary || n->negate || n->index >= 12 || d->mux != _mux_constant)
            continue;
        for (c = 0; c < 3; c++)
            if (n->swizzle[c] != c || d->swizzle[c] != c)
                break;
        if (c < 3)
            continue;
        /* (the normal as the program ends: its writers the last ones) */
        for (c = 0; c < 3; c++) {
            long writer = light_writer(program, at, n->index, c);

            if (writer < 0 || writer != light_writer(program, (long)program->count, n->index, c))
                return 0;
        }
        if (shape->normal != 0xFF && shape->normal != n->index)
            return 0;
        shape->normal = n->index;
        light->direction = in->constant;
        light->direction_negate = d->negate != 0;
        return 1;
    }
    return 0;
}

/* one light's term at a value: max(t, zero) of a dot product, or of the
maximum of one and its multiple by a constant (a back term) */
static int light_term(const WlXboxVshProgram *program, LightValue value, WlXboxVshLightShape *shape,
                      WlXboxVshLight *light) {
    const WlXboxVshInstruction *in = &program->instructions[value.at];
    LightValue v[2];
    unsigned index[2], read[2], which;
    int kind[2], negate[2];

    if (in->mac != _mac_max)
        return 0;
    for (which = 0; which < 2; which++)
        kind[which] = light_operand(program, value.at, which, value.component, &v[which], &index[which], &read[which],
                                    &negate[which]);
    for (which = 0; which < 2; which++) {
        const WlXboxVshInstruction *t;

        if (kind[1 - which] != 2 || negate[1 - which] || read[1 - which] > 2 || kind[which] != 1 || negate[which])
            continue;
        if (shape->zero != 0xFF && (shape->zero != index[1 - which] || shape->zero_component != read[1 - which]))
            return 0;
        shape->zero = (unsigned char)index[1 - which];
        shape->zero_component = (unsigned char)read[1 - which];
        light->has_back = 0;
        if (light_dot(program, v[which].at, shape, light))
            return 1;
        /* (max(d, b d): the dot product one operand, the other its multiple by a constant) */
        t = &program->instructions[v[which].at];
        if (t->mac == _mac_max) {
            LightValue u[2], w[2];
            unsigned ui[2], ur[2], wi[2], wr[2], j, m;
            int uk[2], un[2], wk[2], wn[2];

            for (j = 0; j < 2; j++)
                uk[j] = light_operand(program, v[which].at, j, v[which].component, &u[j], &ui[j], &ur[j], &un[j]);
            for (j = 0; j < 2; j++) {
                const WlXboxVshInstruction *mul;

                if (uk[j] != 1 || uk[1 - j] != 1 || un[j] || !light_dot(program, u[j].at, shape, light))
                    continue;
                mul = &program->instructions[u[1 - j].at];
                if (mul->mac != _mac_mul)
                    return 0;
                for (m = 0; m < 2; m++)
                    wk[m] = light_operand(program, u[1 - j].at, m, u[1 - j].component, &w[m], &wi[m], &wr[m], &wn[m]);
                for (m = 0; m < 2; m++) {
                    if (wk[m] == 1 && w[m].at == u[j].at && wk[1 - m] == 3) {
                        light->has_back = 1;
                        light->back = (unsigned char)wi[1 - m];
                        light->back_component = (unsigned char)wr[1 - m];
                        light->back_negate = (unsigned char)((un[1 - j] + wn[0] + wn[1]) & 1);
                        return 1;
                    }
                }
                return 0;
            }
        }
        return 0;
    }
    return 0;
}

/* d0's component (0..2) at value: the ambient, and each term a colour's
component times a light's term */
static int light_sum(const WlXboxVshProgram *program, LightValue value, unsigned component, WlXboxVshLightShape *shape,
                     WlXboxVshLight *lights, unsigned *count, int *ambient) {
    for (;;) {
        const WlXboxVshInstruction *in = &program->instructions[value.at];
        LightValue v[3];
        unsigned index[3], read[3], which;
        int kind[3], negate[3];

        for (which = 0; which < 3; which++)
            kind[which] = light_operand(program, value.at, which, value.component, &v[which], &index[which],
                                        &read[which], &negate[which]);
        if (in->mac == _mac_mov) {
            if (kind[0] == 3 && !negate[0] && read[0] == component) {
                *ambient = (int)index[0];
                return 1;
            }
            if (kind[0] != 1 || negate[0])
                return 0;
            value = v[0];
            continue;
        }
        if (in->mac != _mac_mad || *count >= WL_XBOX_VSH_LIGHTS)
            return 0;
        for (which = 0; which < 2; which++) {
            if (kind[which] == 3 && !negate[which] && read[which] == component && kind[1 - which] == 1 &&
                !negate[1 - which])
                break;
        }
        if (which == 2)
            return 0;
        memset(&lights[*count], 0, sizeof lights[*count]);
        lights[*count].color = (unsigned char)index[which];
        if (!light_term(program, v[1 - which], shape, &lights[*count]))
            return 0;
        (*count)++;
        if (kind[2] == 3 && !negate[2] && read[2] == component) {
            *ambient = (int)index[2];
            return 1;
        }
        if (kind[2] != 1 || negate[2])
            return 0;
        value = v[2];
    }
}

int wl_xbox_vsh_light_shape(const WlXboxVshProgram *program, WlXboxVshLightShape *shape) {
    WlXboxVshLight lights[3][WL_XBOX_VSH_LIGHTS];
    unsigned counts[3], component, n;
    int ambients[3];

    memset(shape, 0, sizeof *shape);
    memset(lights, 0, sizeof lights);
    shape->normal = shape->zero = 0xFF;
    for (component = 0; component < 3; component++) {
        LightValue value = {-1, component};
        long index;

        /* (the last instruction to write d0's component: a MAC's result) */
        for (index = (long)program->count - 1; index >= 0; index--) {
            const WlXboxVshInstruction *in = &program->instructions[index];

            if (in->output_mask && in->output_is_register && in->output_address == _output_d0 &&
                (in->output_mask & (8u >> component)) && (in->output_from_ilu ? in->ilu : in->mac) != 0) {
                if (!in->output_from_ilu)
                    value.at = index;
                break;
            }
        }
        counts[component] = 0;
        ambients[component] = -1;
        if (value.at < 0 || !light_sum(program, value, component, shape, lights[component], &counts[component],
                                       &ambients[component]))
            goto none;
    }
    /* (the same lights and ambient for r, g and b) */
    for (component = 1; component < 3; component++) {
        if (counts[component] != counts[0] || ambients[component] != ambients[0])
            goto none;
        for (n = 0; n < counts[0]; n++)
            if (memcmp(&lights[component][n], &lights[0][n], sizeof lights[0][n]) != 0)
                goto none;
    }
    if (counts[0] == 0 || shape->normal == 0xFF || shape->zero == 0xFF)
        goto none;
    shape->count = (unsigned char)counts[0];
    shape->ambient = (unsigned char)ambients[0];
    memcpy(shape->lights, lights[0], sizeof shape->lights);
    return 1;
none:
    memset(shape, 0, sizeof *shape);
    return 0;
}

void wl_xbox_vsh_light_unlit(const WlXboxVshProgram *program, WlXboxVshProgram *unlit) {
    unsigned long index;

    *unlit = *program;
    for (index = 0; index < unlit->count; index++) {
        WlXboxVshInstruction *in = &unlit->instructions[index];

        if (in->output_is_register && in->output_address == _output_d0)
            in->output_mask &= 1u;
    }
    steps_make(unlit);
}

/* ---------- fused forms as text */

static const char g_fused_kinds[] = "NLPQSGHUM";

static const char *fused_format_name(unsigned format, char name[8]) {
    static const char *const names[] = {"-", "F1", "F2", "F3", "F4", "D3D", "CMP"};

    if (format < WL_XBOX_VSH_FUSED_S1)
        return names[format];
    name[0] = format < WL_XBOX_VSH_FUSED_UB || format >= WL_XBOX_VSH_FUSED_S32K ? 'S' : 'U';
    name[1] = format < WL_XBOX_VSH_FUSED_UB ? '1' : format < WL_XBOX_VSH_FUSED_S32K ? 'B' : 'K';
    name[2] = '.';
    name[3] = (char)('1' + (format - WL_XBOX_VSH_FUSED_S1) % 4u);
    name[4] = 0;
    return name;
}

int wl_xbox_vsh_texgen_source(const WlXboxVshDependencies *dependencies, const WlXboxVshFusedKey *key,
                              unsigned stage) {
    unsigned long all = dependencies->t[stage & 3u][0] | dependencies->t[stage & 3u][1];
    unsigned format;
    int r = 0;

    all &= ~(unsigned long)WL_XBOX_VSH_READS_CONSTANT;
    if ((all & WL_XBOX_VSH_NOT_AFFINE) || (all & 0xFFFFu) == 0 || (all & (all - 1u)) != 0)
        return -1;
    while (!(all & 1u)) {
        all >>= 1;
        r++;
    }
    /* (a register of the form's arrays, of one or two components: (s, t) as GX takes them, w 1) */
    format = key->format[r];
    if (r == 0 || format == WL_XBOX_VSH_FUSED_NONE ||
        !(format == WL_XBOX_VSH_FUSED_F1 || format == WL_XBOX_VSH_FUSED_F2 ||
          (format >= WL_XBOX_VSH_FUSED_S1 && format < WL_XBOX_VSH_FUSED_S1 + 2u) ||
          (format >= WL_XBOX_VSH_FUSED_UB && format < WL_XBOX_VSH_FUSED_UB + 2u) ||
          (format >= WL_XBOX_VSH_FUSED_S32K && format < WL_XBOX_VSH_FUSED_S32K + 2u)))
        return -1;
    return r;
}

/* what an operand's components read (`need[c]`: the outputs component c of its value reaches) into
the backward walk's registers, and a constant's register into the list when one of them is live */
struct live_walk {
    unsigned need[13][4], need_a0, live;
    unsigned char seen[WL_XBOX_VSH_CONSTANT_COUNT];
    unsigned char *registers;
    int count, most;
};

static void live_operand(struct live_walk *walk, const WlXboxVshInstruction *instruction, int which,
                         const unsigned need[4]) {
    const WlXboxVshOperand *operand = &instruction->operands[which];
    unsigned all = need[0] | need[1] | need[2] | need[3], n;
    int component;

    if (!all)
        return;
    switch (operand->mux) {
    case _mux_temporary:
        for (component = 0; component < 4; component++)
            walk->need[temporary_index(operand->index)][operand->swizzle[component] & 3] |= need[component];
        break;
    case _mux_constant:
        if (!(all & walk->live))
            break;
        if (instruction->relative) {
            walk->count = -1;
            break;
        }
        n = instruction->constant < WL_XBOX_VSH_CONSTANT_COUNT ? instruction->constant
                                                               : WL_XBOX_VSH_CONSTANT_COUNT - 1;
        if (walk->count < 0 || walk->seen[n])
            break;
        if (walk->count >= walk->most) {
            walk->count = -1;
            break;
        }
        walk->seen[n] = 1;
        walk->registers[walk->count++] = (unsigned char)n;
        break;
    default:
        break;
    }
}

/* (wl_xbox_vsh_live_needs' walk a unit and a component at a time: what each of an instruction's two
units makes, and each component of it, reaches; each operand's component read for those alone. A
skinned program's T0 = c13, c14 . v4 written by an ILU mov beside the MAC's dp3 of the blended
position reads no bone) */
int wl_xbox_vsh_live_constants(const WlXboxVshProgram *program, unsigned live, unsigned char *registers, int most) {
    struct live_walk walk;
    long index;
    int component;

    memset(&walk, 0, sizeof(walk));
    walk.live = live;
    walk.registers = registers;
    walk.most = most;
    for (component = 0; component < 4; component++)
        walk.need[12][component] = WL_XBOX_VSH_LIVE_POSITION;
    if (program->light.count)
        for (component = 0; component < 3; component++)
            walk.need[temporary_index(program->light.normal)][component] = WL_XBOX_VSH_LIVE_D0;
    for (index = (long)program->count - 1; index >= 0 && walk.count >= 0; index--) {
        const WlXboxVshInstruction *instruction = &program->instructions[index];
        unsigned mac[4] = {0, 0, 0, 0}, ilu[4] = {0, 0, 0, 0}, a[4] = {0, 0, 0, 0}, b[4] = {0, 0, 0, 0},
                 c[4] = {0, 0, 0, 0}, kills[13] = {0}, sum, *out;
        int which;

        /* what each unit's writes reach */
        if (instruction->mac == _mac_arl) {
            mac[0] = walk.need_a0;
            walk.need_a0 = 0;
        } else if (instruction->mac != _mac_nop && instruction->mac_mask) {
            unsigned t = temporary_index(instruction->temporary);

            for (component = 0; component < 4; component++)
                if (instruction->mac_mask & (8u >> component))
                    mac[component] |= walk.need[t][component];
            kills[t] |= instruction->mac_mask;
        }
        if (instruction->ilu != _ilu_nop && instruction->ilu_mask) {
            unsigned t = temporary_index(instruction->mac != _mac_nop ? 1 : instruction->temporary);

            for (component = 0; component < 4; component++)
                if (instruction->ilu_mask & (8u >> component))
                    ilu[component] |= walk.need[t][component];
            kills[t] |= instruction->ilu_mask;
        }
        if (instruction->output_mask && instruction->output_is_register &&
            (instruction->output_from_ilu ? instruction->ilu : instruction->mac) != 0) {
            unsigned to = 0;

            out = instruction->output_from_ilu ? ilu : mac;
            switch (instruction->output_address) {
            case _output_position:
                for (component = 0; component < 4; component++)
                    if (instruction->output_mask & (8u >> component))
                        out[component] |= walk.need[12][component];
                kills[12] |= instruction->output_mask;
                break;
            case _output_d0:
                to = WL_XBOX_VSH_LIVE_D0;
                break;
            case _output_d1:
                to = WL_XBOX_VSH_LIVE_D1;
                break;
            case _output_b0:
            case _output_b1:
                to = WL_XBOX_VSH_LIVE_BACK;
                break;
            case _output_fog:
                out[0] |= (instruction->output_mask & 8) ? WL_XBOX_VSH_LIVE_FOG : 0u;
                break;
            case _output_point_size:
                out[0] |= (instruction->output_mask & 8) ? WL_XBOX_VSH_LIVE_POINT_SIZE : 0u;
                break;
            default:
                if (instruction->output_address >= _output_t0 && instruction->output_address < _output_t0 + 4)
                    to = (unsigned)WL_XBOX_VSH_LIVE_T0 << (instruction->output_address - _output_t0);
                break;
            }
            for (component = 0; to && component < 4; component++)
                if (instruction->output_mask & (8u >> component))
                    out[component] |= to;
        }
        for (which = 0; which < 13; which++)
            for (component = 0; component < 4; component++)
                if (kills[which] & (8u >> component))
                    walk.need[which][component] = 0;
        /* (the clip-space position: p before this instruction's writes) */
        if (instruction->captures_clip)
            for (component = 0; component < 4; component++)
                walk.need[12][component] |= WL_XBOX_VSH_LIVE_POSITION;
        /* what the units read for it */
        switch (instruction->mac) {
        case _mac_mov:
        case _mac_arl:
            memcpy(a, mac, sizeof(a));
            break;
        case _mac_mul:
        case _mac_min:
        case _mac_max:
        case _mac_slt:
        case _mac_sge:
            memcpy(a, mac, sizeof(a));
            memcpy(b, mac, sizeof(b));
            break;
        case _mac_add:
            memcpy(a, mac, sizeof(a));
            memcpy(c, mac, sizeof(c));
            break;
        case _mac_mad:
            memcpy(a, mac, sizeof(a));
            memcpy(b, mac, sizeof(b));
            memcpy(c, mac, sizeof(c));
            break;
        case _mac_dp3:
        case _mac_dph:
        case _mac_dp4:
            sum = mac[0] | mac[1] | mac[2] | mac[3];
            a[0] = a[1] = a[2] = b[0] = b[1] = b[2] = sum;
            if (instruction->mac == _mac_dph)
                b[3] = sum;
            else if (instruction->mac == _mac_dp4)
                a[3] = b[3] = sum;
            break;
        case _mac_dst:
            a[1] = b[1] = mac[1];
            a[2] = mac[2];
            b[3] = mac[3];
            break;
        default:
            break;
        }
        switch (instruction->ilu) {
        case _ilu_mov:
            for (component = 0; component < 4; component++)
                c[component] |= ilu[component];
            break;
        case _ilu_rcp:
        case _ilu_rcc:
        case _ilu_rsq:
        case _ilu_exp:
        case _ilu_log:
            c[0] |= ilu[0] | ilu[1] | ilu[2] | ilu[3];
            break;
        case _ilu_lit:
            sum = ilu[0] | ilu[1] | ilu[2] | ilu[3];
            c[0] |= sum;
            c[1] |= sum;
            c[3] |= sum;
            break;
        default:
            break;
        }
        /* (a constant by a0: a0 needed for what reads it) */
        if (instruction->relative)
            for (which = 0; which < 3; which++)
                if (instruction->operands[which].mux == _mux_constant)
                    for (component = 0; component < 4; component++)
                        walk.need_a0 |= (which == 0 ? a : which == 1 ? b : c)[component];
        live_operand(&walk, instruction, 0, a);
        live_operand(&walk, instruction, 1, b);
        live_operand(&walk, instruction, 2, c);
    }
    return walk.count;
}

int wl_xbox_vsh_fused_text(const WlXboxVshFusedKey *key, char *text, unsigned long size) {
    char stages[32] = "-", formats[160] = "-", name[8];
    unsigned n, at = 0;

    for (n = 0; n < key->stage_count && n < 4; n++)
        at += (unsigned)snprintf(stages + at, sizeof(stages) - at, "%s%u%c", n ? "," : "", key->stages[n] & 3u,
                                 g_fused_kinds[(key->stages[n] >> 2) % 9u]);
    at = 0;
    for (n = 0; n < 16; n++)
        if (key->format[n] != WL_XBOX_VSH_FUSED_NONE)
            at += (unsigned)snprintf(formats + at, sizeof(formats) - at, "%s%u:%s", at ? "," : "", n,
                                     fused_format_name(key->format[n] % WL_XBOX_VSH_FUSED_FORMATS, name));
    return snprintf(text, size, "%x %c %u %u%s%s %s %s", key->live,
                    key->perspective == WL_XBOX_VSH_FUSED_WORLD ? 'w'
                    : key->perspective == WL_XBOX_VSH_FUSED_XF  ? 'x'
                    : key->perspective                          ? 'p'
                                                                : 'o',
                    key->colors,
                    key->channels, key->lit ? "L" : "", key->interleaved ? "I" : "", stages, formats);
}

/* a number at *p (base 10 or 16), past it */
static int fused_number(const char **p, unsigned base, unsigned *value) {
    const char *s = *p;
    unsigned v = 0;

    while (*s == ' ')
        s++;
    if (!((*s >= '0' && *s <= '9') || (base == 16 && ((*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F')))))
        return 0;
    for (;; s++) {
        unsigned d = *s >= '0' && *s <= '9'   ? (unsigned)(*s - '0')
                     : base == 16 && *s >= 'a' && *s <= 'f' ? (unsigned)(*s - 'a' + 10)
                     : base == 16 && *s >= 'A' && *s <= 'F' ? (unsigned)(*s - 'A' + 10)
                                                             : 99u;
        if (d >= base)
            break;
        if (v > 0xFFFFu)
            return 0;
        v = v * base + d;
    }
    *value = v;
    *p = s;
    return 1;
}

int wl_xbox_vsh_fused_parse(const char *text, WlXboxVshFusedKey *key) {
    const char *p = text;
    unsigned live, colors, channels, n, format;
    char name[8];

    memset(key, 0, sizeof(*key));
    if (!fused_number(&p, 16, &live) || live > 0xFFFFu || p[0] != ' ' || (p[1] != 'p' && p[1] != 'o' && p[1] != 'x' && p[1] != 'w'))
        return 0;
    key->live = (unsigned short)live;
    key->perspective = p[1] == 'w' ? WL_XBOX_VSH_FUSED_WORLD : p[1] == 'x' ? WL_XBOX_VSH_FUSED_XF : p[1] == 'p';
    p += 2;
    if (!fused_number(&p, 10, &colors) || colors > 2 || !fused_number(&p, 10, &channels) || channels > 3)
        return 0;
    key->colors = (unsigned char)colors;
    key->channels = (unsigned char)channels;
    if (*p == 'L') {
        key->lit = 1;
        p++;
    }
    if (*p == 'I') {
        key->interleaved = 1;
        p++;
    }
    if (*p++ != ' ')
        return 0;
    if (*p == '-') {
        p++;
    } else {
        do {
            const char *kind = *p >= '0' && *p <= '3' && p[1] ? strchr(g_fused_kinds, p[1]) : NULL;

            if (!kind || key->stage_count >= 4)
                return 0;
            key->stages[key->stage_count++] = (unsigned char)((unsigned)(*p - '0') | (unsigned)(kind - g_fused_kinds) << 2);
            p += 2;
        } while (*p == ',' && *++p);
    }
    if (*p++ != ' ')
        return 0;
    if (*p == '-')
        return (int)(p + 1 - text);
    do {
        if (!fused_number(&p, 10, &n) || n > 15 || *p++ != ':')
            return 0;
        for (format = 1; format < WL_XBOX_VSH_FUSED_FORMATS; format++) {
            const char *s = fused_format_name(format, name);
            size_t length = strlen(s);

            if (strncmp(p, s, length) == 0 && (p[length] == ',' || p[length] == 0 || p[length] == ' ' ||
                                               p[length] == '\n' || p[length] == '\r')) {
                p += length;
                break;
            }
        }
        if (format == WL_XBOX_VSH_FUSED_FORMATS)
            return 0;
        key->format[n] = (unsigned char)format;
    } while (*p == ',' && *++p);
    return (int)(p - text);
}
