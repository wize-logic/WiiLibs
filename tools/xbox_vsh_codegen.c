// SPDX-License-Identifier: MIT
// Xbox vertex programs as C (a host tool): each program of a file of NV2A vertex microcode is
// decoded by wl/xbox/vsh.c's own decoder and written out as a function that does what
// wl_xbox_vsh_run does for it, its operands, swizzles and registers fixed, so the compiler keeps
// them in registers. The output defines a table of WlXboxVshNative (wl/xbox/vsh.h), which the
// program hands to wl_xbox_vsh_compile: that finds a program's function by a hash of its words
// (wl_xbox_vsh_hash); a program not among them runs on the interpreter. Functions and
// interpreter give the same results, to the bit, when both are compiled without fused
// multiply-adds.
//
// The microcode file is C text: hexadecimal words (0x...), comments (/* */) left out, as the
// Xbox's Direct3D takes vertex shader functions one after another: each a header word whose high
// half is its instruction count, then four words an instruction. Programs met twice are written
// once.
//
// usage: xbox_vsh_codegen microcode.inc output.c [table]
//   (table: the name of the table, default xbox_vsh_natives, and of its count, <table>_count)

#include "../wl/xbox/vsh.c"

#include <stdio.h>
#include <stdlib.h>

#define MAXIMUM_WORDS 65536
#define MAXIMUM_PROGRAMS 512

static const char component_letters[] = "xyzw";

static char components_name(int component, int count) {
    return count > 1 ? component_letters[component] : ' ';
}

/* writing a program's positions function (positions_code): the lane whose
registers an instruction's code names (R<lane>, P<lane>, a<lane>, v0 its
point), -1 for the other functions' (r, p, a0, v) */
static int lane = -1;

/* an operand's source, as read_operand chooses it */
static void source(char *text, size_t size, const WlXboxVshInstruction *instruction,
                   const WlXboxVshOperand *operand) {
    switch (operand->mux) {
    case _mux_temporary:
        if (lane >= 0) {
            if (operand->index == 12)
                snprintf(text, size, "P%d", lane);
            else
                snprintf(text, size, "R%d[%d]", lane, operand->index < 12 ? operand->index : 0);
        } else if (operand->index == 12)
            snprintf(text, size, "p");
        else
            snprintf(text, size, "r[%d]", operand->index < 12 ? operand->index : 0);
        break;
    case _mux_input:
        if (lane >= 0 && instruction->input == 0)
            snprintf(text, size, "points[%d]", lane);
        else
            snprintf(text, size, "v[%d]", instruction->input);
        break;
    case _mux_constant:
        if (instruction->relative && lane >= 0)
            snprintf(text, size, "c[wl_xbox_vsh_clamp(%d + a%d)]", instruction->constant, lane);
        else if (instruction->relative)
            snprintf(text, size, "c[wl_xbox_vsh_clamp(%d + a0)]", instruction->constant);
        else
            snprintf(text, size, "c[%d]",
                     instruction->constant < WL_XBOX_VSH_CONSTANT_COUNT
                         ? instruction->constant
                         : WL_XBOX_VSH_CONSTANT_COUNT - 1);
        break;
    default:
        snprintf(text, size, "wl_xbox_vsh_zero");
        break;
    }
}

static void operand(FILE *out, const char *name, const WlXboxVshInstruction *instruction,
                    const WlXboxVshOperand *value) {
    char text[64];
    int component;

    source(text, sizeof(text), instruction, value);
    for (component = 0; component < 4; component++)
        fprintf(out, "\t\t%s[%d] = %s%s[%d];\n", name, component, value->negate ? "-" : "", text,
                value->swizzle[component]);
}

static void masked(FILE *out, const char *destination, const char *value, unsigned mask) {
    int component;

    /* (bit 3 is x) */
    for (component = 0; component < 4; component++) {
        if (mask & (8u >> component))
            fprintf(out, "\t\t%s[%d] = %s[%d];\n", destination, component, value, component);
    }
}

static const char *temporary_name(unsigned temporary, char *text, size_t size) {
    if (lane >= 0 && temporary == 12)
        snprintf(text, size, "P%d", lane);
    else if (lane >= 0)
        snprintf(text, size, "R%d[%u]", lane, temporary < 12 ? temporary : 0);
    else if (temporary == 12)
        snprintf(text, size, "p");
    else
        snprintf(text, size, "r[%u]", temporary < 12 ? temporary : 0);
    return text;
}

/* writing a program's colour-only function: its texture coordinates, back
colours, point size and clip-space position are left out, and the compiler
drops what only they needed (for draws whose colours alone are the
processor's) */
static int colors_only;

static void instruction_code(FILE *out, const WlXboxVshInstruction *instruction,
                             unsigned long index) {
    char text[32];
    int component;

    fprintf(out, "\t/* %lu */\n\t{\n", index);
    if (instruction->mac != _mac_nop) {
        operand(out, "A", instruction, &instruction->operands[0]);
        if (instruction->mac != _mac_mov && instruction->mac != _mac_arl &&
            instruction->mac != _mac_add)
            operand(out, "B", instruction, &instruction->operands[1]);
    }
    if (instruction->ilu != _ilu_nop || instruction->mac == _mac_add ||
        instruction->mac == _mac_mad)
        operand(out, "C", instruction, &instruction->operands[2]);
    switch (instruction->mac) {
    case _mac_mov:
    case _mac_arl:
        for (component = 0; component < 4; component++)
            fprintf(out, "\t\tM[%d] = A[%d];\n", component, component);
        break;
    case _mac_mul:
    case _mac_add:
    case _mac_mad:
    case _mac_min:
    case _mac_max:
    case _mac_slt:
    case _mac_sge:
        for (component = 0; component < 4; component++) {
            switch (instruction->mac) {
            case _mac_mul:
                fprintf(out, "\t\tM[%d] = A[%d] * B[%d];\n", component, component, component);
                break;
            case _mac_add:
                fprintf(out, "\t\tM[%d] = A[%d] + C[%d];\n", component, component, component);
                break;
            case _mac_mad:
                fprintf(out, "\t\tM[%d] = A[%d] * B[%d] + C[%d];\n", component, component,
                        component, component);
                break;
            case _mac_min:
                fprintf(out, "\t\tM[%d] = A[%d] < B[%d] ? A[%d] : B[%d];\n", component, component,
                        component, component, component);
                break;
            case _mac_max:
                fprintf(out, "\t\tM[%d] = A[%d] > B[%d] ? A[%d] : B[%d];\n", component, component,
                        component, component, component);
                break;
            case _mac_slt:
                fprintf(out, "\t\tM[%d] = A[%d] < B[%d] ? 1.0f : 0.0f;\n", component, component,
                        component);
                break;
            default:
                fprintf(out, "\t\tM[%d] = A[%d] >= B[%d] ? 1.0f : 0.0f;\n", component, component,
                        component);
                break;
            }
        }
        break;
    case _mac_dp3:
        fprintf(out, "\t\tM[0] = M[1] = M[2] = M[3] = A[0] * B[0] + A[1] * B[1] + A[2] * B[2];\n");
        break;
    case _mac_dph:
        fprintf(
            out,
            "\t\tM[0] = M[1] = M[2] = M[3] = A[0] * B[0] + A[1] * B[1] + A[2] * B[2] + B[3];\n");
        break;
    case _mac_dp4:
        fprintf(out, "\t\tM[0] = M[1] = M[2] = M[3] = A[0] * B[0] + A[1] * B[1] + A[2] * B[2] + "
                     "A[3] * B[3];\n");
        break;
    case _mac_dst:
        fprintf(out, "\t\tM[0] = 1.0f; M[1] = A[1] * B[1]; M[2] = A[2]; M[3] = B[3];\n");
        break;
    default:
        /* (no MAC: its result is 0) */
        fprintf(out, "\t\tM[0] = M[1] = M[2] = M[3] = 0.0f;\n");
        break;
    }
    switch (instruction->ilu) {
    case _ilu_mov:
        fprintf(out, "\t\tI[0] = C[0]; I[1] = C[1]; I[2] = C[2]; I[3] = C[3];\n");
        break;
    case _ilu_rcp:
        fprintf(out, "\t\tI[0] = I[1] = I[2] = I[3] = 1.0f / C[0];\n");
        break;
    case _ilu_rcc:
        fprintf(out, "\t\tI[0] = I[1] = I[2] = I[3] = wl_xbox_vsh_reciprocal_clamped(C[0]);\n");
        break;
    case _ilu_rsq:
        fprintf(out, "\t\tI[0] = I[1] = I[2] = I[3] = 1.0f / wl_xbox_vsh_sqrtf(fabsf(C[0]));\n");
        break;
    case _ilu_exp:
    case _ilu_log:
    case _ilu_lit:
        fprintf(out, "\t\twl_xbox_vsh_ilu(%d, C, I);\n", instruction->ilu);
        break;
    default:
        fprintf(out, "\t\tI[0] = I[1] = I[2] = I[3] = 0.0f;\n");
        break;
    }
    if (instruction->captures_clip && lane >= 0)
        fprintf(out, "\t\tmemcpy(clips[%d], P%d, sizeof(clips[0]));\n", lane, lane);
    else if (instruction->captures_clip && !colors_only)
        fprintf(out, "\t\tmemcpy(o->clip, p, sizeof(o->clip));\n\t\to->clip_captured = 1;\n");
    if (instruction->mac == _mac_arl && lane >= 0)
        fprintf(out, "\t\ta%d = wl_xbox_vsh_floor_index(M[0] + 0.001f);\n", lane);
    else if (instruction->mac == _mac_arl)
        fprintf(out, "\t\ta0 = wl_xbox_vsh_floor_index(M[0] + 0.001f);\n");
    else if (instruction->mac != _mac_nop && instruction->mac_mask)
        masked(out, temporary_name(instruction->temporary, text, sizeof(text)), "M",
               instruction->mac_mask);
    if (instruction->ilu != _ilu_nop && instruction->ilu_mask) {
        unsigned temporary = instruction->mac != _mac_nop ? 1 : instruction->temporary;

        masked(out, temporary_name(temporary, text, sizeof(text)), "I", instruction->ilu_mask);
    }
    if (instruction->output_mask && instruction->output_is_register &&
        (instruction->output_from_ilu ? instruction->ilu : instruction->mac) != 0) {
        const char *value = instruction->output_from_ilu ? "I" : "M";

        /* (a positions function writes the position alone) */
        if (lane >= 0) {
            if (instruction->output_address == _output_position)
                masked(out, temporary_name(12, text, sizeof(text)), value,
                       instruction->output_mask);
            fprintf(out, "\t}\n");
            return;
        }
        switch (instruction->output_address) {
        case _output_position:
            masked(out, "p", value, instruction->output_mask);
            break;
        case _output_d0:
            masked(out, "o->d0", value, instruction->output_mask);
            break;
        case _output_d1:
            masked(out, "o->d1", value, instruction->output_mask);
            break;
        case _output_b0:
            if (!colors_only)
                masked(out, "o->b0", value, instruction->output_mask);
            break;
        case _output_b1:
            if (!colors_only)
                masked(out, "o->b1", value, instruction->output_mask);
            break;
        case _output_fog:
            if (instruction->output_mask & 8)
                fprintf(out, "\t\to->fog = %s[0];\n", value);
            break;
        case _output_point_size:
            if ((instruction->output_mask & 8) && !colors_only)
                fprintf(out, "\t\to->point_size = %s[0];\n", value);
            break;
        default:
            if (instruction->output_address >= _output_t0 &&
                instruction->output_address < _output_t0 + 4 && !colors_only) {
                snprintf(text, sizeof(text), "o->t[%d]", instruction->output_address - _output_t0);
                masked(out, text, value, instruction->output_mask);
            }
            break;
        }
    }
    fprintf(out, "\t}\n");
    (void)component_letters;
}

/* (the outputs each instruction's results reach: wl_xbox_vsh_live_needs,
which the interpreter also skips by) */

/* the outputs' components no instruction writes, as wl_xbox_vsh_run leaves
them (but the point size, which wl_xbox_vsh_run_live sets) */
static void live_defaults(FILE *out, const WlXboxVshProgram *program) {
    static const char *const names[] = {"d0", "d1", "b0", "b1", "t[0]", "t[1]", "t[2]", "t[3]"};
    unsigned written[8] = {0};
    int fog = 0, captured = 0, which, component;
    unsigned long index;

    for (index = 0; index < program->count; index++) {
        const WlXboxVshInstruction *instruction = &program->instructions[index];

        captured |= instruction->captures_clip;
        if (!(instruction->output_mask && instruction->output_is_register &&
              (instruction->output_from_ilu ? instruction->ilu : instruction->mac) != 0))
            continue;
        switch (instruction->output_address) {
        case _output_d0:
            written[0] |= instruction->output_mask;
            break;
        case _output_d1:
            written[1] |= instruction->output_mask;
            break;
        case _output_b0:
            written[2] |= instruction->output_mask;
            break;
        case _output_b1:
            written[3] |= instruction->output_mask;
            break;
        case _output_fog:
            fog |= instruction->output_mask & 8;
            break;
        default:
            if (instruction->output_address >= _output_t0 &&
                instruction->output_address < _output_t0 + 4)
                written[4 + instruction->output_address - _output_t0] |= instruction->output_mask;
            break;
        }
    }
    for (which = 0; which < 8; which++)
        for (component = 0; component < 4; component++)
            if (!(written[which] & (8u >> component)))
                fprintf(out, "\to->%s[%d] = %s;\n", names[which], component,
                        component == 3 ? "1.0f" : "0.0f");
    if (!fog)
        fprintf(out, "\to->fog = 1.0f;\n");
    if (!captured)
        fprintf(out, "\to->clip[0] = o->clip[1] = o->clip[2] = o->clip[3] = "
                     "0.0f;\n\to->clip_captured = 0;\n");
}

/* one output's dependencies: its components' input registers, "!" where it
is not affine (wl_xbox_vsh_analyze) */
static void dependency_text(FILE *out, const char *name, const unsigned long *components,
                            int count) {
    unsigned long all = 0;
    int component, input;

    for (component = 0; component < count; component++)
        all |= components[component];
    if (!all)
        return;
    fprintf(out, " %s:", name);
    for (input = 0; input < 16; input++)
        if (all & (1UL << input))
            fprintf(out, "v%d,", input);
    fprintf(out, "%s", all & WL_XBOX_VSH_NOT_AFFINE ? "!" : "");
}

static void dependencies_comment(FILE *out, const WlXboxVshProgram *program) {
    WlXboxVshDependencies dependencies;
    char name[8];
    int stage;

    wl_xbox_vsh_analyze(program, &dependencies);
    fprintf(out, "/* depends on");
    dependency_text(out, dependencies.clip_captured ? "clip" : "position",
                    dependencies.clip_captured ? dependencies.clip : dependencies.position, 4);
    dependency_text(out, "d0", dependencies.d0, 4);
    dependency_text(out, "d1", dependencies.d1, 4);
    for (stage = 0; stage < 4; stage++) {
        snprintf(name, sizeof(name), "t%d", stage);
        dependency_text(out, name, dependencies.t[stage], 2);
    }
    dependency_text(out, "fog", &dependencies.fog, 1);
    fprintf(out, " */\n");
}

/* one output's components that depend on the vertex and read no constant
(wl_xbox_vsh_analyze_constants): the same for a vertex whatever the draw's
constants, which a device can keep for a static vertex buffer */
static int constant_free_text(FILE *out, const char *name, const unsigned long *components,
                              int count) {
    char text[8];
    int component, length = 0;

    for (component = 0; component < count; component++)
        if ((components[component] & 0xFFFF) &&
            !(components[component] & WL_XBOX_VSH_READS_CONSTANT))
            text[length++] = components_name(component, count);
    if (!length)
        return 0;
    text[length] = '\0';
    fprintf(out, " %s%s%s", name, count > 1 ? "." : "", count > 1 ? text : "");
    return 1;
}

static void constant_free_comment(FILE *out, const WlXboxVshProgram *program) {
    WlXboxVshDependencies reads;
    char name[8];
    int stage, any = 0;

    wl_xbox_vsh_analyze_constants(program, &reads);
    fprintf(out, "/* the vertex's alone (no constant read):");
    any |= constant_free_text(out, "d0", reads.d0, 4);
    any |= constant_free_text(out, "d1", reads.d1, 4);
    for (stage = 0; stage < 4; stage++) {
        snprintf(name, sizeof(name), "t%d", stage);
        any |= constant_free_text(out, name, reads.t[stage], 4);
    }
    any |= constant_free_text(out, "fog", &reads.fog, 1);
    fprintf(out, "%s */\n", any ? "" : " none");
}

/* writing a program's positions function: the instructions that reach the
position (wl_xbox_vsh_live_needs), for four values of v0 at once. An instruction that
reads nothing of v0 (through its operands, the registers written from it,
or a0 made from it) is the same in every lane: written once, from lane 0's
registers, and what it writes copied to the others; one that does is
written for each lane. Each lane does what a run for the position does,
operation for operation, so its results are the same to the bit. */

/* the temporaries' and p's components (bit 3 x) an instruction writes, and
whether it writes a0 */
static void instruction_writes(const WlXboxVshInstruction *instruction, unsigned writes[13],
                               int *writes_a0) {
    memset(writes, 0, 13 * sizeof(writes[0]));
    *writes_a0 = instruction->mac == _mac_arl;
    if (instruction->mac != _mac_nop && instruction->mac != _mac_arl && instruction->mac_mask)
        writes[temporary_index(instruction->temporary)] |= instruction->mac_mask;
    if (instruction->ilu != _ilu_nop && instruction->ilu_mask)
        writes[temporary_index(instruction->mac != _mac_nop ? 1 : instruction->temporary)] |=
            instruction->ilu_mask;
    if (instruction->output_mask && instruction->output_is_register &&
        (instruction->output_from_ilu ? instruction->ilu : instruction->mac) != 0 &&
        instruction->output_address == _output_position)
        writes[12] |= instruction->output_mask;
}

/* whether an instruction reads something of v0 (tainted: each temporary's
components, bit 3 x; a0's) */
static int instruction_tainted(const WlXboxVshInstruction *instruction, const unsigned tainted[13],
                               int a0_tainted) {
    int reads[3] = {0, 0, 0}, which, component;

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
        if (operand->mux == _mux_input && instruction->input == 0)
            return 1;
        if (operand->mux == _mux_constant && instruction->relative && a0_tainted)
            return 1;
        if (operand->mux == _mux_temporary)
            for (component = 0; component < 4; component++)
                if (tainted[temporary_index(operand->index)] &
                    (8u >> (operand->swizzle[component] & 3)))
                    return 1;
    }
    return 0;
}

static void positions_code(FILE *out, const WlXboxVshProgram *program, const unsigned *needs,
                           unsigned long hash) {
    unsigned tainted[13] = {0};
    int a0_tainted = 0, which;
    unsigned long index;

    fprintf(out,
            "static void program_%08lx_positions(const float (*c)[4], const float (*v)[4], const "
            "float (*points)[4],\n"
            "\tfloat (*clips)[4], float (*positions)[4])\n{\n",
            hash);
    for (which = 0; which < 4; which++)
        fprintf(out,
                "\tfloat R%d[12][4] = { { 0.0f } };\n\tfloat P%d[4] = { 0.0f, 0.0f, 0.0f, 1.0f };\n"
                "\tint a%d = 0;\n",
                which, which, which);
    fprintf(out, "\tfloat A[4], B[4], C[4], M[4], "
                 "I[4];\n\n\t(void)c;\n\t(void)v;\n\t(void)A;\n\t(void)B;\n\t(void)C;\n"
                 "\t(void)I;\n");
    for (which = 0; which < 4; which++)
        fprintf(out,
                "\t(void)R%d;\n\t(void)a%d;\n\tclips[%d][0] = clips[%d][1] = clips[%d][2] = "
                "clips[%d][3] = 0.0f;\n",
                which, which, which, which, which, which);
    for (index = 0; index < program->count; index++) {
        const WlXboxVshInstruction *instruction = &program->instructions[index];
        unsigned writes[13];
        int writes_a0, taint, temporary, component;

        if (!(needs[index] & WL_XBOX_VSH_LIVE_POSITION))
            continue;
        instruction_writes(instruction, writes, &writes_a0);
        taint = instruction_tainted(instruction, tainted, a0_tainted);
        if (taint)
            for (lane = 0; lane < 4; lane++)
                instruction_code(out, instruction, index);
        else {
            lane = 0;
            instruction_code(out, instruction, index);
            for (which = 1; which < 4; which++) {
                /* (p as it was, then what the instruction wrote) */
                if (instruction->captures_clip)
                    fprintf(out, "\tmemcpy(clips[%d], P%d, sizeof(clips[0]));\n", which, which);
                for (temporary = 0; temporary < 13; temporary++)
                    for (component = 0; component < 4; component++)
                        if (writes[temporary] & (8u >> component)) {
                            if (temporary == 12)
                                fprintf(out, "\tP%d[%d] = P0[%d];\n", which, component, component);
                            else
                                fprintf(out, "\tR%d[%d][%d] = R0[%d][%d];\n", which, temporary,
                                        component, temporary, component);
                        }
                if (writes_a0)
                    fprintf(out, "\ta%d = a0;\n", which);
            }
        }
        lane = -1;
        for (temporary = 0; temporary < 13; temporary++)
            tainted[temporary] = taint ? tainted[temporary] | writes[temporary]
                                       : tainted[temporary] & ~writes[temporary];
        if (writes_a0)
            a0_tainted = taint;
    }
    for (which = 0; which < 4; which++)
        fprintf(out, "\tmemcpy(positions[%d], P%d, sizeof(positions[0]));\n", which, which);
    fprintf(out, "}\n\n");
}

int main(int argc, char **argv) {
    static uint32_t words[MAXIMUM_WORDS];
    static unsigned long hashes[MAXIMUM_PROGRAMS], counts[MAXIMUM_PROGRAMS];
    static int positioned[MAXIMUM_PROGRAMS];
    static WlXboxVshProgram program;
    static unsigned needs[WL_XBOX_VSH_MAX_INSTRUCTIONS];
    unsigned long word_count = 0, position, program_count = 0, index;
    char line[512];
    FILE *in, *out;
    int in_comment = 0;
    const char *table = "xbox_vsh_natives";

    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: xbox_vsh_codegen microcode.inc output.c [table]\n");
        return 2;
    }
    if (argc == 4)
        table = argv[3];
    in = fopen(argv[1], "r");
    if (!in) {
        perror(argv[1]);
        return 1;
    }
    /* the hexadecimal words, past the comment */
    while (fgets(line, sizeof(line), in)) {
        char *cursor = line;

        while (*cursor) {
            if (in_comment) {
                char *end = strstr(cursor, "*/");

                if (!end)
                    break;
                in_comment = 0;
                cursor = end + 2;
            } else if (cursor[0] == '/' && cursor[1] == '*') {
                in_comment = 1;
                cursor += 2;
            } else if (cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X')) {
                if (word_count >= MAXIMUM_WORDS) {
                    fprintf(stderr, "xbox_vsh_codegen: more than %d words\n", MAXIMUM_WORDS);
                    return 1;
                }
                words[word_count++] = (uint32_t)strtoul(cursor, &cursor, 16);
            } else {
                cursor++;
            }
        }
    }
    fclose(in);

    out = fopen(argv[2], "w");
    if (!out) {
        perror(argv[2]);
        return 1;
    }
    fprintf(out,
            "/* generated by WiiLibs' tools/xbox_vsh_codegen.c from %s: do not edit */\n\n"
            "#include \"wl/xbox/vsh_native.h\"\n\n",
            argv[1]);
    /* each program: its header (the instruction count in the high half),
    then four words an instruction */
    for (position = 0; position < word_count;) {
        unsigned long count = words[position] >> 16;
        const uint32_t *instructions = words + position + 1;
        unsigned long hash, earlier;

        position += 1 + count * 4;
        if (!count || position > word_count)
            continue;
        hash = wl_xbox_vsh_hash(instructions, count);
        for (earlier = 0; earlier < program_count; earlier++) {
            if (hashes[earlier] == hash && counts[earlier] == count)
                break;
        }
        if (earlier < program_count || program_count >= MAXIMUM_PROGRAMS)
            continue;
        hashes[program_count] = hash;
        counts[program_count] = count;
        program_count++;
        wl_xbox_vsh_compile(instructions, count, NULL, 0, &program);
        dependencies_comment(out, &program);
        constant_free_comment(out, &program);
        /* (its colours and fog alone) */
        fprintf(out,
                "static void program_%08lx_colors(const float (*c)[4], const float (*v)[4], "
                "WlXboxVshOutput *o)\n"
                "{\n\tfloat r[12][4] = { { 0.0f } };\n\tfloat p[4] = { 0.0f, 0.0f, 0.0f, 1.0f };\n"
                "\tfloat A[4], B[4], C[4], M[4], I[4];\n\tint a0 = 0;\n\n"
                "\t(void)c;\n\t(void)v;\n\t(void)a0;\n\t(void)A;\n\t(void)B;\n\t(void)C;\n\t(void)"
                "r;\n\t(void)p;\n",
                hash);
        colors_only = 1;
        for (index = 0; index < program.count; index++)
            instruction_code(out, &program.instructions[index], index);
        colors_only = 0;
        fprintf(out, "}\n\n");
        /* (the program, for the outputs a draw reads: each instruction
        that reaches none of them skipped) */
        fprintf(out,
                "static void program_%08lx(const float (*c)[4], const float (*v)[4], "
                "WlXboxVshOutput *o,\n"
                "\tunsigned live)\n"
                "{\n\tfloat r[12][4] = { { 0.0f } };\n\tfloat p[4] = { 0.0f, 0.0f, 0.0f, 1.0f };\n"
                "\tfloat A[4], B[4], C[4], M[4], I[4];\n\tint a0 = 0;\n\n"
                "\t(void)c;\n\t(void)v;\n\t(void)a0;\n\t(void)A;\n\t(void)B;\n\t(void)C;\n\t(void)"
                "r;\n\t(void)live;\n",
                hash);
        live_defaults(out, &program);
        wl_xbox_vsh_live_needs(&program, needs);
        {
            unsigned open = 0;

            for (index = 0; index < program.count; index++) {
                if (needs[index] != open) {
                    if (open)
                        fprintf(out, "\t}\n");
                    open = needs[index];
                    if (open)
                        fprintf(out, "\tif (live & 0x%xu)\n\t{\n", open);
                }
                if (open)
                    instruction_code(out, &program.instructions[index], index);
            }
            if (open)
                fprintf(out, "\t}\n");
        }
        fprintf(out, "\tmemcpy(o->position, p, sizeof(o->position));\n}\n\n");
        /* (its position for four values of v0: for the skinned programs, those
        whose position reads constants by a0 and is converted to the screen,
        whose every node a device may make a matrix of; the others' matrices
        are made once for a frame's constants, and their code would take the
        processor's memory for nothing) */
        {
            WlXboxVshDependencies dependencies;

            wl_xbox_vsh_analyze(&program, &dependencies);
            positioned[program_count - 1] = dependencies.relative && dependencies.clip_captured;
            if (positioned[program_count - 1])
                positions_code(out, &program, needs, hash);
        }
    }
    fprintf(out, "const WlXboxVshNative %s[] =\n{\n", table);
    for (index = 0; index < program_count; index++) {
        char positions[40] = "NULL";

        if (positioned[index])
            snprintf(positions, sizeof(positions), "program_%08lx_positions", hashes[index]);
        fprintf(out, "\t{ 0x%08lxUL, %lu, program_%08lx, program_%08lx_colors, %s },\n",
                hashes[index], counts[index], hashes[index], hashes[index], positions);
    }
    fprintf(out, "};\n\nconst unsigned long %s_count = %lu;\n", table, program_count);
    fclose(out);
    printf("xbox_vsh_codegen: %lu programs from %lu words\n", program_count, word_count);
    return 0;
}
