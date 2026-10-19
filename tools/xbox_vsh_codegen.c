// SPDX-License-Identifier: MIT
// Xbox vertex programs as C (a host tool): each program of a file of NV2A vertex microcode is
// decoded by wl/xbox/vsh.c's own decoder and written out as a function that does what
// wl_xbox_vsh_run does for it, its operands, swizzles and registers fixed, so the compiler keeps
// them in registers. The output defines a table of WlXboxVshNative (wl/xbox/vsh.h), which the
// program hands to wl_xbox_vsh_compile: that finds a program's function by a hash of its words
// (wl_xbox_vsh_hash); a program not among them runs on the interpreter. Functions and
// interpreter give the same results, to the bit, when both are compiled without fused
// multiply-adds; on the Wii the functions' paired singles (below) differ in the last bits. On the
// Wii (WL_XBOX_VSH_PS) a program's batch is one block of assembly from xbox_vsh_pairs.c: each
// vec4 temporary two paired-single registers (xy, zw) for the whole program, constants loaded as
// pairs and the most used kept in registers across the batch, a skinned program's bones loaded
// once for a run of vertices that share them; a program it cannot take (EXP, LOG, LIT, DST, SLT,
// SGE, or more values live than registers) keeps its per-instruction code.
//
// The microcode file is C text: hexadecimal words (0x...), comments (/* */) left out, as the
// Xbox's Direct3D takes vertex shader functions one after another: each a header word whose high
// half is its instruction count, then four words an instruction. Programs met twice are written
// once.
//
// usage: xbox_vsh_codegen [--if-exists] [--accessor name] [--colors-positions] [--no-pairs]
//            [--spill] [--no-forward] [--fused forms.txt] microcode.inc output.c [table]
//   (table: the name of the table, default xbox_vsh_natives, and of its count, <table>_count;
//   --if-exists: a microcode file that is not there has no programs, an empty table; --accessor:
//   also a function of that name, const WlXboxVshNative *name(unsigned long *count), that gives
//   the table and its count, which a runtime can declare weak: wl_add_xbox_title's
//   wl_xb_title_vsh_natives; --colors-positions: also each program's colours-only function and,
//   for a skinned one, its four-positions function (wl_xbox_vsh_run_colors and _positions, which
//   only the tests call: without it the table's entries for them are NULL, and a title's program
//   is not paying for code it never runs; --no-pairs: no whole-program assembly, the Wii's batches
//   per instruction as before; --fused: for each form the file names, "<hash> <form> ...", as
//   xbox_nv2a_replay --fused-forms prints them, the Wii's loop that reads the arrays, runs the
//   program and writes GX's words; a skinned form whose registers run out with its blend kept in
//   them keeps it in memory, or makes it at each vertex; --spill: in memory always, the tests';
//   --no-forward: a fused loop's inputs stored by its prologue and loaded back, as before))

#include "../wl/xbox/vsh.c"

#include <stdarg.h>
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

/* writing a batch function's reused prefix (reuse_prefix): an instruction's
temporaries and a0 alone (WRITE_TEMPORARIES, the prefix as it is made), or its
outputs alone (WRITE_OUTPUTS, after the prefix, whether it was made or taken
from the vertex before) */
enum { WRITE_ALL = 3, WRITE_TEMPORARIES = 1, WRITE_OUTPUTS = 2 };
static int write_mode = WRITE_ALL;

/* ---- the Wii's paired singles ----

On the Wii (WL_XBOX_VSH_PS, vsh_native.h) the dot products, and the
multiplies and adds that read a constant or an input row whole or broadcast a
component, are written as the Gekko's paired singles: two lanes an
instruction, a row loaded with one psq_l a pair, fused multiply-adds. Each is
one asm statement whose pairs never leave it: what goes in and out are floats
(ps0 of a register, as every single-precision scalar is on the Gekko), so the
compiler never moves or spills a pair with an instruction that keeps ps0
alone. An instruction is written so only when it costs fewer instructions than
the plain C (loads and operations counted, ps_merge included); the plain C
stays after #else for every other machine. The fused rounding and the dot
products' order of adding change the last bits: the console compares these
with the interpreter within a tolerance (tests/xbox/suite_vsh_ps.c), and the
host and ppc-be run the plain C, the interpreter's bits. The rows read are the
constants and the vertex's inputs, which no instruction writes. */

enum { PS_REG, PS_SPLAT, PS_MEM };

typedef struct {
    int kind;
    char lanes[4][80]; /* its lanes' values: the operand lines' (A[0] ...), or its source's */
    char row[64];      /* PS_MEM: the row's address (a const float *) */
    char base[64];     /* PS_MEM: the base register's value */
    int offset;        /* PS_MEM: the row's displacement from base, in bytes */
} PsOperand;

typedef struct {
    char code[8192];
    char outputs[1024];
    char inputs[3072];
    char seen[64][160];
    int seen_count, temporaries, results;
} PsAsm;

/* letter: the operand lines' array (A, B or C), or 0 for the source itself
(a group's shared operand, read before its instructions' operand lines) */
static void ps_classify(const WlXboxVshInstruction *instruction, const WlXboxVshOperand *operand,
                        char letter, PsOperand *x) {
    const unsigned char *s = operand->swizzle;
    char text[64];
    int k;

    memset(x, 0, sizeof(*x));
    source(text, sizeof(text), instruction, operand);
    for (k = 0; k < 4; k++) {
        if (letter)
            snprintf(x->lanes[k], sizeof(x->lanes[k]), "%c[%d]", letter, k);
        else
            snprintf(x->lanes[k], sizeof(x->lanes[k]), "%s%s[%d]", operand->negate ? "-" : "",
                     text, s[k]);
    }
    x->kind = PS_REG;
    if (s[0] == s[1] && s[1] == s[2] && s[2] == s[3]) {
        x->kind = PS_SPLAT;
        return;
    }
    if (operand->negate || s[0] != 0 || s[1] != 1 || s[2] != 2 || s[3] != 3)
        return;
    if (operand->mux == _mux_input) {
        x->kind = PS_MEM;
        snprintf(x->row, sizeof(x->row), "v[%d]", instruction->input);
        snprintf(x->base, sizeof(x->base), "(const float *)v");
        x->offset = instruction->input * 16;
    } else if (operand->mux == _mux_constant && instruction->relative) {
        x->kind = PS_MEM;
        snprintf(x->row, sizeof(x->row), "c[wl_xbox_vsh_clamp(%d + a0)]", instruction->constant);
        snprintf(x->base, sizeof(x->base), "%s", x->row);
    } else if (operand->mux == _mux_constant) {
        int constant = instruction->constant < WL_XBOX_VSH_CONSTANT_COUNT
                           ? instruction->constant
                           : WL_XBOX_VSH_CONSTANT_COUNT - 1;

        x->kind = PS_MEM;
        snprintf(x->row, sizeof(x->row), "c[%d]", constant);
        /* (psq_l's displacement is 12 bits, signed) */
        if (constant * 16 + 8 < 2048) {
            snprintf(x->base, sizeof(x->base), "(const float *)c");
            x->offset = constant * 16;
        } else {
            snprintf(x->base, sizeof(x->base), "%s", x->row);
        }
    }
}

static void ps_line(PsAsm *a, const char *format, ...) {
    va_list arguments;
    size_t used = strlen(a->code);

    used += (size_t)snprintf(a->code + used, sizeof(a->code) - used, "\t\t\t\"");
    va_start(arguments, format);
    used += (size_t)vsnprintf(a->code + used, sizeof(a->code) - used, format, arguments);
    va_end(arguments);
    snprintf(a->code + used, sizeof(a->code) - used, "\\n\\t\"\n");
}

static void ps_append(char *list, size_t size, const char *format, ...) {
    va_list arguments;
    size_t used = strlen(list);

    if (used)
        used += (size_t)snprintf(list + used, size - used, ", ");
    va_start(arguments, format);
    vsnprintf(list + used, size - used, format, arguments);
    va_end(arguments);
}

/* an input of the asm (constraint and value), each value once: its name */
static const char *ps_input(PsAsm *a, const char *constraint, const char *value, char *name,
                            size_t size) {
    char key[160];
    int n;

    snprintf(key, sizeof(key), "%s %s", constraint, value);
    for (n = 0; n < a->seen_count; n++) {
        if (strcmp(a->seen[n], key) == 0)
            break;
    }
    if (n == a->seen_count) {
        snprintf(a->seen[a->seen_count++], sizeof(a->seen[0]), "%s", key);
        ps_append(a->inputs, sizeof(a->inputs), "[i%d] \"%s\"(%s)", n, constraint, value);
    }
    snprintf(name, size, "%%[i%d]", n);
    return name;
}

/* a scalar input: the operand's lane k */
static const char *ps_scalar(PsAsm *a, const PsOperand *x, int k, char *name, size_t size) {
    return ps_input(a, "f", x->lanes[k], name, size);
}

static const char *ps_temporary(PsAsm *a, char *name, size_t size) {
    snprintf(name, size, "%%[t%d]", a->temporaries);
    ps_append(a->outputs, sizeof(a->outputs), "[t%d] \"=&f\"(t%d)", a->temporaries,
              a->temporaries);
    a->temporaries++;
    return name;
}

/* an output: a float (M[k], or a group's g<index>) */
static const char *ps_output(PsAsm *a, const char *value, char *name, size_t size) {
    int n = a->results++;

    ps_append(a->outputs, sizeof(a->outputs), "[o%d] \"=&f\"(%s)", n, value);
    snprintf(name, size, "%%[o%d]", n);
    return name;
}

static const char *ps_result(PsAsm *a, int k, char *name, size_t size) {
    char value[16];

    snprintf(value, sizeof(value), "M[%d]", k);
    return ps_output(a, value, name, size);
}

/* a pair of the operand, into the register named (a new temporary when into
is NULL): its lanes 2 half and 2 half + 1, or with one its lane 2 half and
1.0 */
static const char *ps_pair(PsAsm *a, const PsOperand *x, int half, int one, const char *into,
                           char *name, size_t size) {
    char first[16], second[16];

    if (into)
        snprintf(name, size, "%s", into);
    else
        ps_temporary(a, name, size);
    if (x->kind == PS_MEM) {
        char base[16], row[16], memory[96];

        ps_input(a, "b", x->base, base, sizeof(base));
        snprintf(memory, sizeof(memory), "*(const float (*)[4])(%s)", x->row);
        ps_input(a, "m", memory, row, sizeof(row));
        /* (W=1: the lane alone, and 1.0) */
        ps_line(a, "psq_l %s, %d(%s), %d, 0", name, x->offset + 8 * half, base, one ? 1 : 0);
        return name;
    }
    ps_scalar(a, x, x->kind == PS_SPLAT ? 0 : 2 * half, first, sizeof(first));
    if (one)
        ps_input(a, "f", "1.0f", second, sizeof(second));
    else
        ps_scalar(a, x, x->kind == PS_SPLAT ? 0 : 2 * half + 1, second, sizeof(second));
    ps_line(a, "ps_merge00 %s, %s, %s", name, first, second);
    return name;
}

static void ps_write(FILE *out, const PsAsm *a, const char *after) {
    int n;

    fprintf(out, "\t\t{\n");
    if (a->temporaries) {
        fprintf(out, "\t\tdouble ");
        for (n = 0; n < a->temporaries; n++)
            fprintf(out, "%st%d", n ? ", " : "", n);
        fprintf(out, ";\n\n");
    }
    fprintf(out, "\t\t__asm__(\n%s\t\t\t: %s\n\t\t\t: %s);\n", a->code, a->outputs, a->inputs);
    fprintf(out, "%s\t\t}\n", after);
}

/* the lanes of M (bit k: lane k) the instruction's writes read */
static unsigned mac_lanes_used(const WlXboxVshInstruction *instruction) {
    unsigned mask = 0, lanes = 0;
    int k;

    if ((write_mode & WRITE_TEMPORARIES) && instruction->mac != _mac_arl)
        mask |= instruction->mac_mask;
    if ((write_mode & WRITE_OUTPUTS) && instruction->output_is_register &&
        !instruction->output_from_ilu)
        mask |= instruction->output_mask;
    for (k = 0; k < 4; k++) {
        if (mask & (8u >> k))
            lanes |= 1u << k;
    }
    return lanes;
}

/* MUL, ADD and MAD: each half (lanes 0 1, lanes 2 3) as a pair when that is
cheaper, the other as plain C; 0 (nothing written) when neither is */
static int ps_mac_code(FILE *out, const WlXboxVshInstruction *instruction, unsigned lanes) {
    static const char *const plain[] = {"", "M[%d] = A[%d] * B[%d];", "M[%d] = A[%d] + C[%d];",
                                        "M[%d] = A[%d] * B[%d] + C[%d];"};
    int mac = instruction->mac, which = mac == _mac_mul ? 1 : mac == _mac_add ? 2 : 3;
    int half, any = 0, ps[2] = {0, 0};
    PsOperand x[3];

    ps_classify(instruction, &instruction->operands[0], 'A', &x[0]);
    ps_classify(instruction, &instruction->operands[1], 'B', &x[1]);
    ps_classify(instruction, &instruction->operands[2], 'C', &x[2]);
    for (half = 0; half < 2; half++) {
        unsigned used = (lanes >> (2 * half)) & 3u;
        int count = (int)(used & 1) + (int)(used >> 1), memories, scalar, paired;

        if (!used)
            continue;
        if (mac == _mac_add) {
            memories = (x[0].kind == PS_MEM) + (x[2].kind == PS_MEM);
            scalar = count * (1 + memories);
            paired = 3 + (int)(used >> 1);
        } else {
            int slot = x[0].kind == PS_SPLAT || x[1].kind == PS_SPLAT;

            if (x[0].kind == PS_SPLAT && x[1].kind == PS_SPLAT)
                continue;
            memories = (x[0].kind == PS_MEM) + (x[1].kind == PS_MEM);
            if (mac == _mac_mad)
                memories += x[2].kind == PS_MEM;
            scalar = count * ((mac == _mac_mad ? 2 : 1) + memories);
            paired = (slot ? 1 : 2) + (mac == _mac_mad) + 1 + (int)(used >> 1);
        }
        ps[half] = paired < scalar;
        any |= ps[half];
    }
    if (!any)
        return 0;
    fprintf(out, "#if WL_XBOX_VSH_PS\n");
    for (half = 0; half < 2; half++) {
        unsigned used = (lanes >> (2 * half)) & 3u;
        char result[16], left[16], right[16], addend[16], odd[16];
        PsAsm a;
        int k;

        if (!used)
            continue;
        if (!ps[half]) {
            for (k = 2 * half; k < 2 * half + 2; k++) {
                fprintf(out, "\t\t");
                fprintf(out, plain[which], k, k, k, k);
                fprintf(out, "\n");
            }
            continue;
        }
        memset(&a, 0, sizeof(a));
        if (used & 1)
            ps_result(&a, 2 * half, result, sizeof(result));
        else
            ps_temporary(&a, result, sizeof(result));
        if (mac == _mac_add) {
            ps_pair(&a, &x[0], half, 0, NULL, left, sizeof(left));
            ps_pair(&a, &x[2], half, 0, NULL, addend, sizeof(addend));
            ps_line(&a, "ps_add %s, %s, %s", result, left, addend);
        } else {
            /* (a broadcast multiplier is ps_muls0's and ps_madds0's scalar) */
            const PsOperand *pair = x[0].kind == PS_SPLAT ? &x[1] : &x[0];
            const PsOperand *other = x[0].kind == PS_SPLAT ? &x[0] : &x[1];
            int slot = other->kind == PS_SPLAT;

            ps_pair(&a, pair, half, 0, NULL, left, sizeof(left));
            if (slot)
                ps_scalar(&a, other, 0, right, sizeof(right));
            else
                ps_pair(&a, other, half, 0, NULL, right, sizeof(right));
            if (mac == _mac_mad) {
                ps_pair(&a, &x[2], half, 0, NULL, addend, sizeof(addend));
                ps_line(&a, "%s %s, %s, %s, %s", slot ? "ps_madds0" : "ps_madd", result, left,
                        right, addend);
            } else {
                ps_line(&a, "%s %s, %s, %s", slot ? "ps_muls0" : "ps_mul", result, left, right);
            }
        }
        if (used & 2) {
            ps_result(&a, 2 * half + 1, odd, sizeof(odd));
            ps_line(&a, "ps_merge11 %s, %s, %s", odd, result, result);
        }
        ps_write(out, &a, "");
    }
    fprintf(out, "#else\n");
    return 1;
}

/* ---- groups: dot products that share their first operand ----

A vertex transformed by a matrix is consecutive DP4s (DPH, DP3) of one
operand with the matrix's rows, constants or inputs. Such a run is made in one
asm statement, the shared operand's pairs made once, into g<index> for each
instruction, at the first one's place and for the outputs any of them reach;
the instructions then take their M from it. An instruction made where its
own outputs are not read changes nothing that is (its writes reach none of
them), and none of the run before the last writes the shared operand's
register, so each reads it as it was. */

enum { PS_GROUP_MAXIMUM = 6 };

/* the instruction being written takes M from g<it> (-1: none) */
static long ps_group_member = -1;

/* 1 when the instruction writes temporary register t (12: the position) */
static int writes_temporary(const WlXboxVshInstruction *instruction, unsigned t) {
    if (instruction->mac != _mac_nop && instruction->mac != _mac_arl && instruction->mac_mask &&
        instruction->temporary == t)
        return 1;
    if (instruction->ilu != _ilu_nop && instruction->ilu_mask &&
        (instruction->mac != _mac_nop ? 1u : instruction->temporary) == t)
        return 1;
    return t == 12 && instruction->output_mask && instruction->output_is_register &&
           instruction->output_address == _output_position;
}

static int same_operand(const WlXboxVshInstruction *a, const WlXboxVshInstruction *b) {
    const WlXboxVshOperand *x = &a->operands[0], *y = &b->operands[0];

    if (x->mux != y->mux || x->negate != y->negate || memcmp(x->swizzle, y->swizzle, 4))
        return 0;
    if (x->mux == _mux_temporary)
        return x->index == y->index;
    if (x->mux == _mux_input)
        return a->input == b->input;
    if (x->mux == _mux_constant)
        return a->constant == b->constant && a->relative == b->relative;
    return 1;
}

/* the end of the group that starts at first (first itself when none) */
static unsigned long ps_group_end(const WlXboxVshProgram *program, unsigned long first,
                                  const unsigned *needs) {
    const WlXboxVshInstruction *head = &program->instructions[first];
    PsOperand shared;
    unsigned long end, earlier;
    /* (an asm statement has at most 30 operands: 5 temporaries, the
    shared operand's at most 5 inputs, and each instruction's result and
    row, its base and memory or its 4 scalars) */
    int operands;

    if (head->mac != _mac_dp3 && head->mac != _mac_dph && head->mac != _mac_dp4)
        return first;
    ps_classify(head, &head->operands[0], 'A', &shared);
    operands = 5 + (shared.kind == PS_MEM ? 2 : 5);
    for (end = first; end < program->count && end - first < PS_GROUP_MAXIMUM; end++) {
        const WlXboxVshInstruction *instruction = &program->instructions[end];
        const WlXboxVshOperand *row = &instruction->operands[1];
        PsOperand x;

        if (instruction->mac != head->mac || !needs[end] || !mac_lanes_used(instruction) ||
            !same_operand(head, instruction))
            break;
        /* (a row of registers is merged; one of constants or inputs loaded:
        a group of registers alone saves nothing) */
        ps_classify(instruction, row, 'B', &x);
        if (x.kind != PS_MEM && shared.kind != PS_MEM)
            break;
        operands += 1 + (x.kind == PS_MEM ? 2 : 4);
        if (operands > 30)
            break;
        /* (what the group reads, none of it written before its instruction) */
        for (earlier = first; earlier < end; earlier++) {
            const WlXboxVshInstruction *before = &program->instructions[earlier];

            if (head->operands[0].mux == _mux_temporary &&
                writes_temporary(before, head->operands[0].index < 12 ? head->operands[0].index : 12))
                break;
            if (row->mux == _mux_temporary &&
                writes_temporary(before, row->index < 12 ? row->index : 12))
                break;
        }
        if (earlier < end)
            break;
    }
    return end - first >= 2 ? end : first;
}

static void ps_group_code(FILE *out, const WlXboxVshProgram *program, unsigned long first,
                          unsigned long end, const unsigned *needs) {
    const WlXboxVshInstruction *head = &program->instructions[first];
    int mac = head->mac, slot;
    unsigned mask = 0;
    unsigned long index;
    PsOperand shared;
    char low[16], high[16], scalar[16], b0[16], b1[16], sum[16], result[16], value[32];
    PsAsm a;

    ps_classify(head, &head->operands[0], 0, &shared);
    if (mac == _mac_dph && shared.kind == PS_SPLAT)
        shared.kind = PS_REG;
    slot = shared.kind == PS_SPLAT;
    memset(&a, 0, sizeof(a));
    ps_temporary(&a, b0, sizeof(b0));
    ps_temporary(&a, b1, sizeof(b1));
    ps_temporary(&a, sum, sizeof(sum));
    if (slot) {
        ps_scalar(&a, &shared, 0, scalar, sizeof(scalar));
    } else {
        ps_pair(&a, &shared, 0, 0, NULL, low, sizeof(low));
        if (mac == _mac_dp3 && shared.kind != PS_MEM)
            ps_scalar(&a, &shared, 2, high, sizeof(high));
        else
            ps_pair(&a, &shared, 1, mac != _mac_dp4, NULL, high, sizeof(high));
    }
    fprintf(out, "#if WL_XBOX_VSH_PS\n\tfloat ");
    for (index = first; index < end; index++) {
        const WlXboxVshInstruction *instruction = &program->instructions[index];
        PsOperand row;

        mask |= needs[index];
        fprintf(out, "%sg%lu = 0.0f", index > first ? ", " : "", index);
        ps_classify(instruction, &instruction->operands[1], 0, &row);
        ps_pair(&a, &row, 0, 0, b0, value, sizeof(value));
        ps_pair(&a, &row, 1, mac == _mac_dp3, b1, value, sizeof(value));
        snprintf(value, sizeof(value), "g%lu", index);
        ps_output(&a, value, result, sizeof(result));
        if (mac == _mac_dp3) {
            /* (x y as a pair; z, ps0 of the rows' second pair, as a scalar) */
            if (slot)
                ps_line(&a, "ps_muls0 %s, %s, %s", sum, b0, scalar);
            else
                ps_line(&a, "ps_mul %s, %s, %s", sum, low, b0);
            ps_line(&a, "ps_sum0 %s, %s, %s, %s", sum, sum, sum, sum);
            ps_line(&a, "fmadds %s, %s, %s, %s", result, slot ? scalar : high, b1, sum);
        } else {
            if (slot) {
                ps_line(&a, "ps_muls0 %s, %s, %s", sum, b0, scalar);
                ps_line(&a, "ps_madds0 %s, %s, %s, %s", sum, b1, scalar, sum);
            } else {
                ps_line(&a, "ps_mul %s, %s, %s", sum, low, b0);
                ps_line(&a, "ps_madd %s, %s, %s, %s", sum, high, b1, sum);
            }
            ps_line(&a, "ps_sum0 %s, %s, %s, %s", result, sum, sum, sum);
        }
    }
    fprintf(out, ";\n\n\tif (live & 0x%xu)\n\t{\n", mask);
    ps_write(out, &a, "");
    fprintf(out, "\t}\n#endif\n");
}

/* DP3, DPH and DP4 as pairs when that is cheaper; 0 (nothing written) when
not */
static int ps_dot_code(FILE *out, const WlXboxVshInstruction *instruction) {
    int mac = instruction->mac, slot, scalar, paired, memories, k;
    PsOperand x[2];
    const PsOperand *pair, *other;
    char sum[16], result[16], left[16], right[16], third[16], fourth[16];
    PsAsm a;

    ps_classify(instruction, &instruction->operands[0], 'A', &x[0]);
    ps_classify(instruction, &instruction->operands[1], 'B', &x[1]);
    /* (DPH's A is (x y z 1): a broadcast is merged like any other) */
    for (k = 0; k < 2; k++) {
        if (mac == _mac_dph && x[k].kind == PS_SPLAT)
            x[k].kind = PS_REG;
    }
    if (x[0].kind == PS_SPLAT && x[1].kind == PS_SPLAT)
        return 0;
    slot = x[0].kind == PS_SPLAT || x[1].kind == PS_SPLAT;
    memories = (x[0].kind == PS_MEM) + (x[1].kind == PS_MEM);
    if (mac == _mac_dp4) {
        scalar = 7 + 4 * memories;
        paired = (slot ? 2 : 4) + 3;
    } else if (mac == _mac_dph) {
        scalar = 6 + 3 * (x[0].kind == PS_MEM) + 4 * (x[1].kind == PS_MEM);
        paired = 4 + 3;
    } else {
        scalar = 5 + 3 * memories;
        paired = (slot ? 1 : 2) + 3 + memories;
    }
    if (paired >= scalar)
        return 0;
    memset(&a, 0, sizeof(a));
    pair = x[0].kind == PS_SPLAT ? &x[1] : &x[0];
    other = x[0].kind == PS_SPLAT ? &x[0] : &x[1];
    ps_temporary(&a, sum, sizeof(sum));
    ps_pair(&a, pair, 0, 0, NULL, left, sizeof(left));
    if (slot) {
        ps_scalar(&a, other, 0, right, sizeof(right));
        ps_line(&a, "ps_muls0 %s, %s, %s", sum, left, right);
    } else {
        ps_pair(&a, other, 0, 0, NULL, right, sizeof(right));
        ps_line(&a, "ps_mul %s, %s, %s", sum, left, right);
    }
    if (mac == _mac_dp3) {
        char partial[16];

        /* (x y as a pair, z as a scalar) */
        ps_temporary(&a, partial, sizeof(partial));
        ps_line(&a, "ps_sum0 %s, %s, %s, %s", partial, sum, sum, sum);
        ps_result(&a, 0, result, sizeof(result));
        ps_scalar(&a, &x[0], x[0].kind == PS_SPLAT ? 0 : 2, third, sizeof(third));
        ps_scalar(&a, &x[1], x[1].kind == PS_SPLAT ? 0 : 2, fourth, sizeof(fourth));
        ps_line(&a, "fmadds %s, %s, %s, %s", result, third, fourth, partial);
    } else {
        /* (DPH: A's second pair is (z 1), so its w term is B's w) */
        ps_pair(&a, pair, 1, mac == _mac_dph && pair == &x[0], NULL, left, sizeof(left));
        if (slot) {
            ps_line(&a, "ps_madds0 %s, %s, %s, %s", sum, left, right, sum);
        } else {
            ps_pair(&a, other, 1, mac == _mac_dph && other == &x[0], NULL, third, sizeof(third));
            ps_line(&a, "ps_madd %s, %s, %s, %s", sum, left, third, sum);
        }
        ps_result(&a, 0, result, sizeof(result));
        ps_line(&a, "ps_sum0 %s, %s, %s, %s", result, sum, sum, sum);
    }
    fprintf(out, "#if WL_XBOX_VSH_PS\n");
    ps_write(out, &a, "\t\tM[1] = M[2] = M[3] = M[0];\n");
    fprintf(out, "#else\n");
    return 1;
}

static void instruction_code(FILE *out, const WlXboxVshInstruction *instruction,
                             unsigned long index) {
    char text[32];
    int component, paired;

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
    /* (on the Wii, as paired singles when cheaper: the plain C after #else) */
    paired = 0;
    if (lane < 0 && mac_lanes_used(instruction)) {
        if (instruction->mac == _mac_mul || instruction->mac == _mac_add ||
            instruction->mac == _mac_mad)
            paired = ps_mac_code(out, instruction, mac_lanes_used(instruction));
        else if (instruction->mac == _mac_dp3 || instruction->mac == _mac_dph ||
                 instruction->mac == _mac_dp4)
        {
            if (ps_group_member >= 0) {
                fprintf(out, "#if WL_XBOX_VSH_PS\n\t\tM[0] = M[1] = M[2] = M[3] = g%ld;\n#else\n",
                        ps_group_member);
                paired = 1;
            } else {
                paired = ps_dot_code(out, instruction);
            }
        }
    }
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
    if (paired)
        fprintf(out, "#endif\n");
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
    if (!(write_mode & WRITE_TEMPORARIES))
        ; /* (the outputs alone) */
    else if (instruction->mac == _mac_arl && lane >= 0)
        fprintf(out, "\t\ta%d = wl_xbox_vsh_floor_index(M[0] + 0.001f);\n", lane);
    else if (instruction->mac == _mac_arl)
        fprintf(out, "\t\ta0 = wl_xbox_vsh_floor_index(M[0] + 0.001f);\n");
    else if (instruction->mac != _mac_nop && instruction->mac_mask)
        masked(out, temporary_name(instruction->temporary, text, sizeof(text)), "M",
               instruction->mac_mask);
    if (instruction->ilu != _ilu_nop && instruction->ilu_mask && (write_mode & WRITE_TEMPORARIES)) {
        unsigned temporary = instruction->mac != _mac_nop ? 1 : instruction->temporary;

        masked(out, temporary_name(temporary, text, sizeof(text)), "I", instruction->ilu_mask);
    }
    if ((write_mode & WRITE_OUTPUTS) && instruction->output_mask && instruction->output_is_register &&
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

/* A skinned program's prefix reused from the vertex before: the instructions
from the first that build a0 and the node matrices from the vertex's indices and
weights alone (no v0, no output but to a register the vertex's own inputs give,
no clip capture), as long as they read only those inputs, constants and what the
prefix itself wrote. A vertex whose inputs there are the bits of the vertex
before's (the same nodes and weights: a mesh's neighbours mostly are) takes the
temporaries the prefix leaves that later instructions read, as the vertex
before made them, and a0, instead of running it: the same operations on the
same bits, so the same results. The prefix's outputs (an output an input gives
directly, such as a colour's alpha) are written after it either way. */
typedef struct ReusePrefix {
    unsigned long end;  /* instructions [0, end) */
    unsigned long inputs; /* the input registers they read (bit n: v[n]) */
    unsigned live_out[12]; /* the temporaries' components later instructions read (bit 3 x) */
    int a0_out;
} ReusePrefix;

/* the temporaries' components an operand reads (bit 3 x) */
static unsigned operand_reads(const WlXboxVshOperand *operand) {
    unsigned bits = 0;
    int component;

    for (component = 0; component < 4; component++)
        bits |= 8u >> (operand->swizzle[component] & 3);
    return bits;
}

/* an instruction's operands that are read (A, B, C), as instruction_code reads them */
static void operands_read(const WlXboxVshInstruction *instruction, int reads[3]) {
    reads[0] = reads[1] = reads[2] = 0;
    if (instruction->mac != _mac_nop) {
        reads[0] = 1;
        if (instruction->mac != _mac_mov && instruction->mac != _mac_arl && instruction->mac != _mac_add)
            reads[1] = 1;
    }
    if (instruction->ilu != _ilu_nop || instruction->mac == _mac_add || instruction->mac == _mac_mad)
        reads[2] = 1;
}

static int reuse_prefix(const WlXboxVshProgram *program, ReusePrefix *prefix) {
    unsigned written[13] = {0};
    int relative = 0, which, temporary;
    unsigned long index;

    memset(prefix, 0, sizeof(*prefix));
    for (index = 0; index < program->count; index++) {
        const WlXboxVshInstruction *instruction = &program->instructions[index];
        int reads[3], output = instruction->output_mask && instruction->output_is_register &&
                                   (instruction->output_from_ilu ? instruction->ilu : instruction->mac) != 0;
        unsigned writes_now[13];
        int writes_a0;

        operands_read(instruction, reads);
        instruction_writes(instruction, writes_now, &writes_a0);
        /* (r12 is the position: what writes it is not kept from the vertex before) */
        if (instruction->captures_clip || (output && instruction->output_address == _output_position) ||
            writes_now[12])
            break;
        /* (an output: from an input or a constant alone, read by the ILU's C or a
        MOV's A, so it can be written after the prefix) */
        if (output) {
            const WlXboxVshOperand *from =
                &instruction->operands[instruction->output_from_ilu ? 2 : 0];

            if (instruction->output_from_ilu ? instruction->ilu != _ilu_mov : instruction->mac != _mac_mov)
                break;
            if (from->mux == _mux_temporary || (from->mux == _mux_constant && instruction->relative))
                break;
        }
        for (which = 0; which < 3; which++) {
            const WlXboxVshOperand *operand = &instruction->operands[which];

            if (!reads[which])
                continue;
            if (operand->mux == _mux_input && instruction->input == 0)
                break;
            if (operand->mux == _mux_temporary && operand->index == 12)
                break;
        }
        if (which < 3)
            break;
        /* (the operands that make temporaries or a0: their inputs are what a vertex
        must share with the vertex before; an output's are read after the prefix) */
        {
            int mac_t = instruction->mac != _mac_nop && (instruction->mac == _mac_arl || instruction->mac_mask);
            int ilu_t = instruction->ilu != _ilu_nop && instruction->ilu_mask;
            int feeds[3];

            feeds[0] = mac_t && reads[0];
            feeds[1] = mac_t && reads[1];
            feeds[2] = reads[2] && (ilu_t || (mac_t && (instruction->mac == _mac_add || instruction->mac == _mac_mad)));
            for (which = 0; which < 3; which++) {
                const WlXboxVshOperand *operand = &instruction->operands[which];

                if (feeds[which] && operand->mux == _mux_input)
                    prefix->inputs |= 1UL << (instruction->input & 15);
                if (feeds[which] && operand->mux == _mux_constant && instruction->relative)
                    relative = 1;
            }
        }
        for (temporary = 0; temporary < 12; temporary++)
            written[temporary] |= writes_now[temporary];
        prefix->end = index + 1;
    }
    /* (worth it: a skinning blend of some length, from few inputs) */
    if (!relative || prefix->end < 6)
        return 0;
    /* what the rest reads of the prefix's temporaries before writing them, and a0 */
    {
        unsigned pending[12];
        int a0_pending = 1;

        memcpy(pending, written, sizeof(pending));
        for (index = prefix->end; index < program->count; index++) {
            const WlXboxVshInstruction *instruction = &program->instructions[index];
            int reads[3], writes_a0;
            unsigned writes_now[13];

            operands_read(instruction, reads);
            for (which = 0; which < 3; which++) {
                const WlXboxVshOperand *operand = &instruction->operands[which];

                if (!reads[which])
                    continue;
                if (operand->mux == _mux_temporary && operand->index < 12)
                    prefix->live_out[operand->index] |= pending[operand->index] & operand_reads(operand);
                if (operand->mux == _mux_constant && instruction->relative && a0_pending)
                    prefix->a0_out = 1;
            }
            instruction_writes(instruction, writes_now, &writes_a0);
            for (temporary = 0; temporary < 12; temporary++)
                pending[temporary] &= ~writes_now[temporary];
            if (writes_a0)
                a0_pending = 0;
        }
    }
    return 1;
}

/* An instruction the prefix may take (reuse_prefix's tests), for prefix_first */
static int prefix_takes(const WlXboxVshInstruction *instruction) {
    int reads[3], which, writes_a0;
    unsigned writes_now[13];
    int output = instruction->output_mask && instruction->output_is_register &&
                 (instruction->output_from_ilu ? instruction->ilu : instruction->mac) != 0;

    operands_read(instruction, reads);
    instruction_writes(instruction, writes_now, &writes_a0);
    if (instruction->captures_clip || (output && instruction->output_address == _output_position) ||
        writes_now[12] || (instruction->output_mask && !instruction->output_is_register))
        return 0;
    if (output) {
        const WlXboxVshOperand *from = &instruction->operands[instruction->output_from_ilu ? 2 : 0];

        if (instruction->output_from_ilu ? instruction->ilu != _ilu_mov : instruction->mac != _mac_mov)
            return 0;
        if (from->mux == _mux_temporary || (from->mux == _mux_constant && instruction->relative))
            return 0;
    }
    for (which = 0; which < 3; which++) {
        const WlXboxVshOperand *operand = &instruction->operands[which];

        if (!reads[which])
            continue;
        if ((operand->mux == _mux_input && instruction->input == 0) ||
            (operand->mux == _mux_temporary && operand->index == 12))
            return 0;
    }
    return 1;
}

/* what an instruction reads and writes, for prefix_first: the temporaries
(bit n: r[n], 12 the position), a0, an output register (bit n: its address),
the constants it writes and the inputs it makes temporaries from */
typedef struct PrefixAccess {
    unsigned long feeds; /* the input registers that make its temporaries or a0 (reuse_prefix's inputs) */
    unsigned reads, writes, output_writes;
    int reads_a0, writes_a0, writes_constant;
} PrefixAccess;

static void prefix_access(const WlXboxVshInstruction *instruction, PrefixAccess *access) {
    int reads[3], which, temporary;
    unsigned writes_now[13];

    memset(access, 0, sizeof(*access));
    operands_read(instruction, reads);
    instruction_writes(instruction, writes_now, &access->writes_a0);
    for (temporary = 0; temporary < 13; temporary++)
        if (writes_now[temporary])
            access->writes |= 1u << temporary;
    for (which = 0; which < 3; which++) {
        const WlXboxVshOperand *operand = &instruction->operands[which];

        if (!reads[which])
            continue;
        if (operand->mux == _mux_temporary)
            access->reads |= 1u << (operand->index < 12 ? operand->index : 12);
        if (operand->mux == _mux_constant && instruction->relative)
            access->reads_a0 = 1;
    }
    /* (the clip-space capture reads the position) */
    if (instruction->captures_clip)
        access->reads |= 1u << 12;
    if (instruction->output_mask && instruction->output_is_register &&
        (instruction->output_from_ilu ? instruction->ilu : instruction->mac) != 0)
        access->output_writes = 1u << (instruction->output_address & 31);
    access->writes_constant = instruction->output_mask && !instruction->output_is_register;
    {
        int mac_t = instruction->mac != _mac_nop && (instruction->mac == _mac_arl || instruction->mac_mask);
        int ilu_t = instruction->ilu != _ilu_nop && instruction->ilu_mask;
        int feeds[3];

        feeds[0] = mac_t && reads[0];
        feeds[1] = mac_t && reads[1];
        feeds[2] = reads[2] && (ilu_t || (mac_t && (instruction->mac == _mac_add || instruction->mac == _mac_mad)));
        for (which = 0; which < 3; which++)
            if (feeds[which] && instruction->operands[which].mux == _mux_input)
                access->feeds |= 1UL << (instruction->input & 15);
    }
}

/* The instructions the prefix may take moved ahead of those it may not, where
nothing between depends on them nor they on it (a skinned program that works
out a texture coordinate between its node matrices' blends, say): each
instruction still reads what it read, so the results are the same to the bit,
and reuse_prefix then finds the whole blend. Only a program the prefix is worth
it for is changed (reuse_prefix says so before and after) */
static void prefix_first(WlXboxVshProgram *program) {
    static WlXboxVshInstruction moved[WL_XBOX_VSH_MAX_INSTRUCTIONS];
    static PrefixAccess deferred_access[WL_XBOX_VSH_MAX_INSTRUCTIONS];
    static unsigned long deferred[WL_XBOX_VSH_MAX_INSTRUCTIONS];
    unsigned long index, taken = 0, deferred_count = 0, d;
    ReusePrefix before, after;

    if (!reuse_prefix(program, &before))
        return;
    for (index = 0; index < program->count; index++) {
        const WlXboxVshInstruction *instruction = &program->instructions[index];
        PrefixAccess access;
        int free = prefix_takes(instruction);

        prefix_access(instruction, &access);
        /* (no input the prefix did not compare already: v4's texture coordinate, say) */
        if (access.feeds & ~before.inputs)
            free = 0;
        for (d = 0; free && d < deferred_count; d++) {
            const PrefixAccess *earlier = &deferred_access[d];

            if ((access.reads & earlier->writes) || (access.writes & (earlier->reads | earlier->writes)) ||
                (access.reads_a0 && earlier->writes_a0) ||
                (access.writes_a0 && (earlier->reads_a0 || earlier->writes_a0)) ||
                (access.output_writes & earlier->output_writes) || access.writes_constant ||
                earlier->writes_constant)
                free = 0;
        }
        if (free) {
            moved[taken++] = *instruction;
        } else {
            deferred_access[deferred_count] = access;
            deferred[deferred_count++] = index;
        }
    }
    for (d = 0; d < deferred_count; d++)
        moved[taken + d] = program->instructions[deferred[d]];
    {
        WlXboxVshInstruction original[WL_XBOX_VSH_MAX_INSTRUCTIONS];

        memcpy(original, program->instructions, program->count * sizeof(original[0]));
        memcpy(program->instructions, moved, program->count * sizeof(moved[0]));
        if (!reuse_prefix(program, &after) || after.end <= before.end)
            memcpy(program->instructions, original, program->count * sizeof(original[0]));
    }
}

/* the prefix's code in a batch function: made, or taken from the vertex before */
static void reuse_code(FILE *out, const WlXboxVshProgram *program, const ReusePrefix *prefix,
                       const unsigned *needs) {
    unsigned long index;
    int input, temporary, component;

    fprintf(out, "\tif (n > 0");
    for (input = 0; input < 16; input++)
        if (prefix->inputs & (1UL << input))
            fprintf(out, " && memcmp(v[%d], vs[n - 1][%d], sizeof(v[%d])) == 0", input, input, input);
    fprintf(out, ")\n\t{\n");
    for (temporary = 0; temporary < 12; temporary++)
        for (component = 0; component < 4; component++)
            if (prefix->live_out[temporary] & (8u >> component))
                fprintf(out, "\t\tr[%d][%d] = kept_r[%d][%d];\n", temporary, component, temporary, component);
    if (prefix->a0_out)
        fprintf(out, "\t\ta0 = kept_a0;\n");
    fprintf(out, "\t}\n\telse\n\t{\n");
    write_mode = WRITE_TEMPORARIES;
    for (index = 0; index < prefix->end; index++) {
        if (!needs[index])
            continue;
        fprintf(out, "\tif (live & 0x%xu)\n", needs[index]);
        instruction_code(out, &program->instructions[index], index);
    }
    for (temporary = 0; temporary < 12; temporary++)
        for (component = 0; component < 4; component++)
            if (prefix->live_out[temporary] & (8u >> component))
                fprintf(out, "\t\tkept_r[%d][%d] = r[%d][%d];\n", temporary, component, temporary, component);
    if (prefix->a0_out)
        fprintf(out, "\t\tkept_a0 = a0;\n");
    fprintf(out, "\t}\n");
    /* (the prefix's outputs, from the vertex's inputs) */
    write_mode = WRITE_OUTPUTS;
    for (index = 0; index < prefix->end; index++) {
        const WlXboxVshInstruction *instruction = &program->instructions[index];

        if (!(instruction->output_mask && instruction->output_is_register &&
              (instruction->output_from_ilu ? instruction->ilu : instruction->mac) != 0))
            continue;
        if (!needs[index])
            continue;
        fprintf(out, "\tif (live & 0x%xu)\n", needs[index]);
        instruction_code(out, instruction, index);
    }
    write_mode = WRITE_ALL;
}

#include "xbox_vsh_pairs.c"

/* a fused form's asm (pr_program with pr_fuse set): the prefix kept in registers from one vertex to
the next, else (too few registers) kept in the slot, else made at each vertex (*reused cleared) */
static int g_spill_first;

static int fused_program_once(const WlXboxVshProgram *program, const unsigned *needs, int *reused,
                              const ReusePrefix *prefix) {
    pr_spill = 0;
    if (!(g_spill_first && *reused) && pr_program(program, needs, *reused, prefix))
        return 1;
    if (!*reused)
        return 0;
    pr_spill = 1;
    if (pr_program(program, needs, *reused, prefix))
        return 1;
    pr_spill = 0;
    *reused = 0;
    return pr_program(program, needs, 0, prefix);
}

/* ... and for a "w" form, the world position kept in registers to the end, else stored where the
program captures the clip-space position (pr_world_spill) */
static int fused_program(const WlXboxVshProgram *program, const unsigned *needs, int *reused,
                         const ReusePrefix *prefix) {
    int first = *reused, made;

    pr_world_spill = 0;
    made = fused_program_once(program, needs, reused, prefix);
    if (made || !pr_fuse || pr_fuse->perspective != WL_XBOX_VSH_FUSED_WORLD)
        return made;
    pr_world_spill = 1;
    *reused = first;
    made = fused_program_once(program, needs, reused, prefix);
    pr_world_spill = 0;
    return made;
}

/* the fused forms asked for (--fused): a program's hash and its form */
enum { MAXIMUM_FORMS = 256, MAXIMUM_FUSED = 16 };
static struct {
    unsigned long hash;
    WlXboxVshFusedKey key;
} g_forms[MAXIMUM_FORMS];
static int g_form_count;

/* a --fused file: a form a line, "<hash> <form> ..." (the replay's --fused-forms), "#" a comment */
static int read_forms(const char *path) {
    char line[512];
    FILE *in = fopen(path, "r");

    if (!in) {
        perror(path);
        return 0;
    }
    while (fgets(line, sizeof(line), in)) {
        char *rest;
        unsigned long hash = strtoul(line, &rest, 16);

        if (line[0] == '#' || rest == line || *rest != ' ')
            continue;
        if (g_form_count < MAXIMUM_FORMS && wl_xbox_vsh_fused_parse(rest + 1, &g_forms[g_form_count].key)) {
            g_forms[g_form_count].hash = hash;
            g_form_count++;
        }
    }
    fclose(in);
    /* (a form and its interleaved twin: the twin alone, which the draws that have the form run. A
    file may keep both, so that a generator that does not read "I" still writes the form) */
    {
        int n, k, kept = 0;

        for (n = 0; n < g_form_count; n++) {
            WlXboxVshFusedKey twin = g_forms[n].key;

            twin.interleaved = 1;
            for (k = 0; !g_forms[n].key.interleaved && k < g_form_count; k++)
                if (g_forms[k].hash == g_forms[n].hash && memcmp(&g_forms[k].key, &twin, sizeof(twin)) == 0)
                    break;
            if (g_forms[n].key.interleaved || k == g_form_count)
                g_forms[kept++] = g_forms[n];
        }
        g_form_count = kept;
    }
    return 1;
}

int main(int argc, char **argv) {
    static uint32_t words[MAXIMUM_WORDS];
    static unsigned long hashes[MAXIMUM_PROGRAMS], counts[MAXIMUM_PROGRAMS], checks[MAXIMUM_PROGRAMS];
    static int positioned[MAXIMUM_PROGRAMS], fused[MAXIMUM_PROGRAMS], fused_index[MAXIMUM_PROGRAMS][MAXIMUM_FUSED];
    static WlXboxVshProgram program;
    static unsigned needs[WL_XBOX_VSH_MAX_INSTRUCTIONS];
    ReusePrefix prefix;
    int reused, paired, fused_reused;
    unsigned long word_count = 0, position, program_count = 0, index;
    char line[512];
    FILE *in, *out;
    int in_comment = 0;
    const char *table = "xbox_vsh_natives", *accessor = NULL;
    int if_exists = 0, colors_positions = 0, no_pairs = 0;

    while (argc > 1 && argv[1][0] == '-' && argv[1][1] == '-') {
        if (strcmp(argv[1], "--if-exists") == 0) {
            if_exists = 1;
        } else if (strcmp(argv[1], "--no-pairs") == 0) {
            no_pairs = 1;
        } else if (strcmp(argv[1], "--spill") == 0) {
            g_spill_first = 1;
        } else if (strcmp(argv[1], "--no-forward") == 0) {
            pr_fetched_off = 1;
        } else if (strcmp(argv[1], "--colors-positions") == 0) {
            colors_positions = 1;
        } else if (strcmp(argv[1], "--accessor") == 0 && argc > 2) {
            accessor = argv[2];
            argc--;
            argv++;
        } else if (strcmp(argv[1], "--fused") == 0 && argc > 2) {
            if (!read_forms(argv[2]))
                return 1;
            argc--;
            argv++;
        } else {
            break;
        }
        argc--;
        argv++;
    }
    if (argc != 3 && argc != 4) {
        fprintf(stderr,
                "usage: xbox_vsh_codegen [--if-exists] [--accessor name] [--colors-positions] "
                "[--no-pairs] [--spill] [--no-forward] [--fused forms.txt] microcode.inc output.c [table]\n");
        return 2;
    }
    if (argc == 4)
        table = argv[3];
    in = fopen(argv[1], "r");
    if (!in && !if_exists) {
        perror(argv[1]);
        return 1;
    }
    /* the hexadecimal words, past the comment */
    while (in && fgets(line, sizeof(line), in)) {
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
    if (in)
        fclose(in);

    out = fopen(argv[2], "w");
    if (!out) {
        perror(argv[2]);
        return 1;
    }
    fprintf(out,
            "/* generated by WiiLibs' tools/xbox_vsh_codegen.c from %s: do not edit */\n\n"
            "#include \"wl/xbox/vsh_native.h\"\n\n"
            "#if WL_XBOX_VSH_PS\n#include <stddef.h>\n\n"
            "/* the floats the paired-single programs load (tools/xbox_vsh_pairs.c) */\n"
            "static const float wl_xbox_vsh_pairs_k[38] __attribute__((aligned(8), unused)) = {\n"
            "\t0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.001f, 8388608.0f, -8388608.0f, 0.5f, 1.5f,\n"
            "\t5.42101e-20f, 1.884467e+19f, -5.42101e-20f, -1.884467e+19f,\n"
            "\t/* (EXP and LOG: wl/xbox/vsh.c's ilu_power_of_two, ilu_exp2 and ilu_log2) */\n"
            "\t128.0f, -150.0f, -126.0f, 127.0f, 191.0f, 5.42101086e-20f, __builtin_inff(),\n"
            "\t12582912.0f, 1.53375775e-4f, 1.33998599e-3f, 9.61851981e-3f, 5.55032901e-2f,\n"
            "\t2.40226462e-1f, 6.93147182e-1f,\n"
            "\t-1.0e30f, 1.17549435e-38f, 8388735.0f, 8388758.0f, 1.41421356f,\n"
            "\t3.39634150e-1f, 4.11730886e-1f, 5.77082634e-1f, 9.61796701e-1f, 2.88539004e+0f};\n\n"
            "/* (the fused batches' floats where the tool put them) */\n"
            "_Static_assert(offsetof(WlXboxVshFused, sphere) == %d && offsetof(WlXboxVshFused, pack) == %d &&\n"
            "\toffsetof(WlXboxVshFused, stride) == %d && offsetof(WlXboxVshFused, saved) == %d &&\n"
            "\toffsetof(WlXboxVshOutput, fog) == %d && offsetof(WlXboxVshOutput, clip_captured) == %d &&\n"
            "\tsizeof(WlXboxVshOutput) == %d,\n"
            "\t\"WlXboxVshFused's or WlXboxVshOutput's floats moved: tools/xbox_vsh_codegen.c\");\n#endif\n\n",
            argv[1], (int)offsetof(WlXboxVshFused, sphere), (int)offsetof(WlXboxVshFused, pack),
            (int)offsetof(WlXboxVshFused, stride), (int)offsetof(WlXboxVshFused, saved),
            (int)offsetof(WlXboxVshOutput, fog), (int)offsetof(WlXboxVshOutput, clip_captured),
            (int)sizeof(WlXboxVshOutput));
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
        /* (a second program of one hash and count is left to the interpreter:
        the functions are named by the hash) */
        for (earlier = 0; earlier < program_count; earlier++) {
            if (hashes[earlier] == hash && counts[earlier] == count)
                break;
        }
        if (earlier < program_count || program_count >= MAXIMUM_PROGRAMS)
            continue;
        hashes[program_count] = hash;
        counts[program_count] = count;
        checks[program_count] = wl_xbox_vsh_check(instructions, count);
        program_count++;
        wl_xbox_vsh_compile(instructions, count, NULL, 0, &program);
        prefix_first(&program);
        dependencies_comment(out, &program);
        constant_free_comment(out, &program);
        /* (its colours and fog alone, when asked for) */
        if (colors_positions) {
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
        }
        /* (the program, for the outputs a draw reads: each instruction
        that reaches none of them skipped) */
        /* (for count vertices at once, so the function's entry and exit, its
        registers saved and restored, are paid once for them) */
        wl_xbox_vsh_live_needs(&program, needs);
        reused = reuse_prefix(&program, &prefix);
        fprintf(out,
                "static void program_%08lx_batch(const float (*c)[4], const float "
                "(*vs)[WL_XBOX_VSH_INPUT_COUNT][4],\n"
                "\tWlXboxVshOutput *os, unsigned count, unsigned live)\n"
                "{\n",
                hash);
        paired = !no_pairs && pr_program(&program, needs, reused, &prefix);
        if (paired) {
            fprintf(out, "#if WL_XBOX_VSH_PS\n");
            pr_print(out, reused);
            fprintf(out, "#else\n");
        }
        fprintf(out, "\tunsigned n;\n");
        if (reused)
            fprintf(out, "\tfloat kept_r[12][4] = { { 0.0f } };\n\tint kept_a0 = 0;\n\n\t(void)kept_r;\n\t(void)kept_a0;\n");
        fprintf(out,
                "\n\tfor (n = 0; n < count; n++)\n\t{\n"
                "\tconst float (*v)[4] = vs[n];\n\tWlXboxVshOutput *o = &os[n];\n"
                "\tfloat r[12][4] = { { 0.0f } };\n\tfloat p[4] = { 0.0f, 0.0f, 0.0f, 1.0f };\n"
                "\tfloat A[4], B[4], C[4], M[4], I[4];\n\tint a0 = 0;\n\n"
                "\t(void)c;\n\t(void)v;\n\t(void)a0;\n\t(void)A;\n\t(void)B;\n\t(void)C;\n\t(void)"
                "r;\n\t(void)live;\n");
        live_defaults(out, &program);
        if (reused)
            reuse_code(out, &program, &prefix, needs);
        {
            unsigned open = 0;
            unsigned long group_end = 0;

            for (index = reused ? prefix.end : 0; index < program.count; index++) {
                /* (a group of dot products made ahead, outside the blocks) */
                if (index >= group_end) {
                    group_end = ps_group_end(&program, index, needs);
                    if (group_end > index) {
                        if (open)
                            fprintf(out, "\t}\n");
                        open = 0;
                        ps_group_code(out, &program, index, group_end, needs);
                    }
                }
                if (needs[index] != open) {
                    if (open)
                        fprintf(out, "\t}\n");
                    open = needs[index];
                    if (open)
                        fprintf(out, "\tif (live & 0x%xu)\n\t{\n", open);
                }
                ps_group_member = index < group_end ? (long)index : -1;
                if (open)
                    instruction_code(out, &program.instructions[index], index);
                ps_group_member = -1;
            }
            if (open)
                fprintf(out, "\t}\n");
        }
        fprintf(out, "\tmemcpy(o->position, p, sizeof(o->position));\n\t}\n");
        if (paired)
            fprintf(out, "#endif\n");
        fprintf(out, "}\n\n");
        fprintf(out,
                "static void program_%08lx(const float (*c)[4], const float (*v)[4], "
                "WlXboxVshOutput *o,\n\tunsigned live)\n{\n"
                "\tprogram_%08lx_batch(c, (const float (*)[WL_XBOX_VSH_INPUT_COUNT][4])v, o, 1, live);\n}\n\n",
                hash, hash);
        /* (its fused forms asked for, the Wii's) */
        fused[program_count - 1] = 0;
        for (index = 0; paired && index < (unsigned long)g_form_count; index++) {
            int made = fused[program_count - 1];

            if (g_forms[index].hash != hash)
                continue;
            pr_fuse = &g_forms[index].key;
            if (pr_fuse->lit) {
                /* (a lit form: the program without d0's r, g and b, which GX's lighting makes) */
                static WlXboxVshProgram unlit;
                static unsigned unlit_needs[WL_XBOX_VSH_MAX_INSTRUCTIONS];
                ReusePrefix unlit_prefix;
                int unlit_reused, made_lit;

                if (!program.light.count) {
                    fprintf(stderr, "xbox_vsh_codegen: %08lx's fused form %d is lit, but its d0 has no light shape\n",
                            hash, (int)index);
                    pr_fuse = NULL;
                    continue;
                }
                wl_xbox_vsh_light_unlit(&program, &unlit);
                wl_xbox_vsh_live_needs(&unlit, unlit_needs);
                unlit_reused = reuse_prefix(&unlit, &unlit_prefix);
                made_lit = fused_program(&unlit, unlit_needs, &unlit_reused, &unlit_prefix);
                if (made_lit) {
                    fprintf(out, "#if WL_XBOX_VSH_PS\n");
                    fprintf(out,
                            "static void program_%08lx_fused_%d(const float (*c)[4], WlXboxVshFused *fused, "
                            "unsigned count, unsigned live)\n{\n",
                            hash, made);
                    pr_print(out, unlit_reused);
                    pr_spill = 0;
                    fprintf(out, "}\n#endif\n\n");
                    fused_index[program_count - 1][made] = (int)index;
                    fused[program_count - 1] = made + 1;
                } else {
                    fprintf(stderr, "xbox_vsh_codegen: %08lx's fused form %d not written (registers)\n", hash,
                            (int)index);
                }
                pr_fuse = NULL;
                if (fused[program_count - 1] >= MAXIMUM_FUSED)
                    break;
                continue;
            }
            fused_reused = reused;
            if (fused_program(&program, needs, &fused_reused, &prefix)) {
                fprintf(out, "#if WL_XBOX_VSH_PS\n");
                fprintf(out,
                        "static void program_%08lx_fused_%d(const float (*c)[4], WlXboxVshFused *fused, unsigned count, "
                        "unsigned live)\n{\n",
                        hash, made);
                pr_print(out, fused_reused);
                pr_spill = 0;
                fprintf(out, "}\n#endif\n\n");
                fused_index[program_count - 1][made] = (int)index;
                fused[program_count - 1] = made + 1;
            } else {
                fprintf(stderr, "xbox_vsh_codegen: %08lx's fused form %d not written (registers)\n", hash, (int)index);
            }
            pr_fuse = NULL;
            if (fused[program_count - 1] >= MAXIMUM_FUSED)
                break;
        }
        if (fused[program_count - 1]) {
            int f;

            fprintf(out, "#if WL_XBOX_VSH_PS\nstatic const WlXboxVshFusedForm program_%08lx_fused[] =\n{\n", hash);
            for (f = 0; f < fused[program_count - 1]; f++) {
                const WlXboxVshFusedKey *key = &g_forms[fused_index[program_count - 1][f]].key;
                int c;

                fprintf(out, "\t{ { 0x%x, {", key->live);
                for (c = 0; c < 16; c++)
                    fprintf(out, " %u%s", key->format[c], c < 15 ? "," : "");
                fprintf(out, " }, %u, %u, %u, %u, { %u, %u, %u, %u }, %u, %u }, program_%08lx_fused_%d },\n",
                        key->perspective, key->colors, key->channels, key->stage_count, key->stages[0], key->stages[1],
                        key->stages[2], key->stages[3], key->lit, key->interleaved, hash, f);
            }
            fprintf(out, "};\n#endif\n\n");
        }
        /* (its position for four values of v0: for the skinned programs, those
        whose position reads constants by a0 and is converted to the screen,
        whose every node a device may make a matrix of; the others' matrices
        are made once for a frame's constants, and their code would take the
        processor's memory for nothing) */
        if (colors_positions) {
            WlXboxVshDependencies dependencies;

            wl_xbox_vsh_analyze(&program, &dependencies);
            positioned[program_count - 1] = dependencies.relative && dependencies.clip_captured;
            if (positioned[program_count - 1])
                positions_code(out, &program, needs, hash);
        }
    }
    fprintf(out, "const WlXboxVshNative %s[] =\n{\n", table);
    for (index = 0; index < program_count; index++) {
        char colors[40] = "NULL", positions[40] = "NULL";

        if (colors_positions)
            snprintf(colors, sizeof(colors), "program_%08lx_colors", hashes[index]);
        if (positioned[index])
            snprintf(positions, sizeof(positions), "program_%08lx_positions", hashes[index]);
        fprintf(out, "\t{ 0x%08lxUL, %lu, 0x%08lxUL, program_%08lx, %s, %s, program_%08lx_batch, ",
                hashes[index], counts[index], checks[index], hashes[index], colors, positions, hashes[index]);
        if (fused[index])
            fprintf(out, "WL_XBOX_VSH_FUSED_FORMS(program_%08lx_fused, %d) },\n", hashes[index], fused[index]);
        else
            fprintf(out, "NULL, 0 },\n");
    }
    /* (C has no empty array: a table of none holds one entry no program matches) */
    if (!program_count)
        fprintf(out, "\t{ 0UL, 0, 0UL, NULL, NULL, NULL, NULL, NULL, 0 },\n");
    fprintf(out, "};\n\nconst unsigned long %s_count = %lu;\n", table, program_count);
    if (accessor)
        fprintf(out,
                "\nconst WlXboxVshNative *%s(unsigned long *count);\n\n"
                "const WlXboxVshNative *%s(unsigned long *count)\n{\n\t*count = %s_count;\n"
                "\treturn %s;\n}\n",
                accessor, accessor, table, table);
    fclose(out);
    printf("xbox_vsh_codegen: %lu programs from %lu words\n", program_count, word_count);
    return 0;
}
