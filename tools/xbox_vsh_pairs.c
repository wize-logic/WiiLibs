// SPDX-License-Identifier: MIT
// Part of tools/xbox_vsh_codegen.c (included by it): a batch function's whole program as one asm
// statement of the Gekko's paired singles, for the Wii (WL_XBOX_VSH_PS).
//
// Each vec4 temporary lives in two floating-point registers for the whole program, its xy and its
// zw as pairs, so MUL/ADD/MAD/MOV/MIN/MAX are an instruction a half and a dot product three, with
// no packing between instructions. The vertex loop is inside the asm: the constants a program
// reads by a fixed index are loaded once a batch when registers are left for them, and a skinned
// program's prefix (reuse_prefix: the blended node matrices) stays in its registers from one
// vertex to the next while the vertex's indices and weights are the bits of the vertex before's.
// The registers are allocated here (a linear scan over the code's positions): a temporary's
// register is shared by the instructions that write one lane of it and keep the other (a web),
// the rest are values from one instruction to the next.
//
// What differs from the plain C: the multiply-adds and the dot products are fused and add in
// another order, RSQ is frsqrte's estimate made exact by Newton's steps in double (to a bit), and
// MIN/MAX choose by the sign of a difference (+0 for -0 when equal). The host checks the code the
// Wii runs with tests/xbox/vsh_ps_emulate.py's C emulation against the interpreter. A program
// with what is not written here (EXP, LOG, LIT, DST, SLT, SGE) or that needs more registers than
// the Gekko has keeps the per-instruction code.

enum { PR_VALUES = 4096, PR_OPS = 4096, PR_CACHE = 512, PR_KEYS = 256 };

typedef struct {
    int parent;      /* union-find: a web's values share one register */
    int first, last; /* positions: the first definition, the last use */
    int loop;        /* lives over the whole loop (a kept or hoisted value) */
    int phys;
    int fresh;       /* made by this instruction, not yet a temporary's or a cache's */
    int web;         /* a temporary's (in place writes allowed when not kept) */
    int splat;       /* ps1 is ps0 */
} PrValue;

typedef struct {
    char text[96];   /* @0..@3: the values' registers */
    int values[4], count;
    unsigned defines; /* bit n: values[n] is written */
} PrOp;

typedef struct {
    char key[80];
    int value;
    unsigned needs; /* the block it was loaded in */
    int hoisted;    /* loaded before the loop */
} PrCached;

static PrValue pr_values[PR_VALUES];
static int pr_value_count;
static PrOp pr_pre[PR_OPS], pr_body[PR_OPS];
static int pr_pre_count, pr_body_count;
static PrCached pr_cache[PR_CACHE];
static int pr_cache_count;
static int pr_failed;
static unsigned pr_needs;       /* the block being written's needs (~0: always runs) */
static int pr_def[13][2];       /* each temporary half's value (-1: its start value) */
static int pr_kept[13][2];      /* the prefix's halves kept from the vertex before */
static unsigned pr_live_after[13]; /* lanes read later (bit 3 x), for the instruction written */
static int pr_label;            /* labels written */
static char pr_ra_key[24];      /* what %[ra] holds (a relative row's address) */
static int pr_a0_version, pr_uses_a0, pr_uses_ra, pr_uses_c2, pr_uses_slot;
static char pr_first[24];       /* where the first vertex starts (the prefix made) */

/* the constants that may be loaded once a batch, and how often each is loaded in a vertex */
static char pr_keys[PR_KEYS][80];
static int pr_key_uses[PR_KEYS], pr_key_count, pr_hoist_limit, pr_counting;

/* the floats %[k] points at (wl_xbox_vsh_pairs_k in the generated file) */
enum {
    PRK_ZERO = 0,   /* (0, 0) */
    PRK_W1 = 8,     /* (0, 1) */
    PRK_ONE = 16,
    PRK_ARL = 20,   /* 0.001 */
    PRK_2P23 = 24,
    PRK_M2P23 = 28,
    PRK_HALF = 32,
    PRK_THREE_HALVES = 36,
    PRK_RCC_LOW = 40,
    PRK_RCC_HIGH = 44,
    PRK_RCC_NLOW = 48,
    PRK_RCC_NHIGH = 52,
};

static int pr_new(void) {
    PrValue *value;

    if (pr_value_count >= PR_VALUES) {
        pr_failed = 1;
        return 0;
    }
    value = &pr_values[pr_value_count];
    memset(value, 0, sizeof(*value));
    value->parent = pr_value_count;
    value->first = value->last = -1;
    value->phys = -1;
    value->fresh = 1;
    return pr_value_count++;
}

static int pr_root(int v) {
    while (pr_values[v].parent != v)
        v = pr_values[v].parent = pr_values[pr_values[v].parent].parent;
    return v;
}

static void pr_union(int a, int b) {
    a = pr_root(a);
    b = pr_root(b);
    if (a != b)
        pr_values[b].parent = a;
}

/* an op: defines is a mask of the values written (bit n: the nth value) */
static void pr_op(int pre, unsigned defines, int count, const int *values, const char *format, ...) {
    PrOp *op;
    va_list arguments;
    int n;

    if ((pre ? pr_pre_count : pr_body_count) >= PR_OPS) {
        pr_failed = 1;
        return;
    }
    op = pre ? &pr_pre[pr_pre_count++] : &pr_body[pr_body_count++];
    va_start(arguments, format);
    vsnprintf(op->text, sizeof(op->text), format, arguments);
    va_end(arguments);
    op->count = count;
    op->defines = defines;
    for (n = 0; n < count; n++)
        op->values[n] = values[n];
}

static void pr_remember(const char *key, int value);

/* the common shapes: d = op(a, b, c); a merge or a negation of the same
values made once (until one becomes a temporary's: pr_adopt) */
static int pr_op3(const char *name, int a, int b, int c) {
    int values[4], d, count = 1, n, pure = strncmp(name, "ps_merge", 8) == 0 || strcmp(name, "ps_neg") == 0;
    char text[64], key[80];

    if (pure) {
        snprintf(key, sizeof(key), "op %s %d %d", name, a, b);
        for (n = 0; n < pr_cache_count; n++)
            if (strcmp(pr_cache[n].key, key) == 0 && !(pr_needs & ~pr_cache[n].needs))
                return pr_cache[n].value;
    }
    d = pr_new();
    /* (both lanes the same: a merge of one lane twice, or a single-precision
    operation, which writes ps1 as ps0 in paired-single mode) */
    if ((strcmp(name, "ps_merge00") == 0 || strcmp(name, "ps_merge11") == 0) && a == b)
        pr_values[d].splat = 1;
    if (strcmp(name, "fadds") == 0 || strcmp(name, "fsubs") == 0 || strcmp(name, "fdivs") == 0 ||
        strcmp(name, "fmadds") == 0 || strcmp(name, "fnmadds") == 0)
        pr_values[d].splat = 1;
    if (strcmp(name, "ps_neg") == 0 && pr_values[a].splat)
        pr_values[d].splat = 1;
    if (pure)
        pr_remember(key, d);
    values[0] = d;
    snprintf(text, sizeof(text), "%s @0", name);
    if (a >= 0) {
        values[count++] = a;
        strcat(text, ", @1");
    }
    if (b >= 0) {
        values[count++] = b;
        strcat(text, count == 3 ? ", @2" : ", @1");
    }
    if (c >= 0) {
        values[count++] = c;
        strcat(text, ", @3");
    }
    pr_op(0, 1, count, values, "%s", text);
    return d;
}

/* d written in place: d = op(..., old, ...) where old's register is d's (a web) */
static void pr_text(const char *format, ...) {
    va_list arguments;
    char text[96];

    va_start(arguments, format);
    vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    pr_op(0, 0, 0, NULL, "%s", text);
}

static void pr_new_label(char *name, size_t size) {
    snprintf(name, size, ".Lp%%=_%d", pr_label++);
}

static void pr_place_label(const char *name) {
    pr_text("%s:", name);
    pr_ra_key[0] = 0;
}

/* the blocks' needs, each a bit of cr1 to cr7 (4 + its index) that the
code before the loop sets when live has one of them (mtcrf), so a block's
test is one bf */
enum { PR_NEED_BITS = 28 };
static unsigned pr_need_values[64];
static int pr_need_count;

static int pr_need_bit(unsigned needs) {
    int n;

    for (n = 0; n < pr_need_count; n++)
        if (pr_need_values[n] == needs)
            return n < PR_NEED_BITS ? 4 + n : -1;
    if (pr_need_count < 64)
        pr_need_values[pr_need_count++] = needs;
    return n < PR_NEED_BITS ? 4 + n : -1;
}

/* to label when live has none of needs */
static void pr_skip(unsigned needs, const char *label) {
    int bit = pr_need_bit(needs);

    if (bit >= 0) {
        pr_text("bf %d, %s", bit, label);
    } else {
        pr_text("andi. 0, %%[live], %u", needs);
        pr_text("beq %s", label);
    }
}

/* ---- loads ---- */

static int pr_find_key(const char *key) {
    int n;

    for (n = 0; n < pr_key_count; n++)
        if (strcmp(pr_keys[n], key) == 0)
            return n;
    return -1;
}

/* a float or pair from memory: kind 0 a pair (psq_l), 1 a float in both
lanes (lfs), 2 a pair whose second lane is 1.0 (psq_l W=1); base a GPR's
operand (%[c], %[c2], %[k], %[v], %[ra]); hoistable: the same for the whole
batch (a constant by a fixed index, or %[k]) */
static int pr_load(int kind, const char *base, int offset, int hoistable) {
    char key[80];
    int n, value, pre = 0;

    if (strcmp(base, "%[ra]") == 0)
        snprintf(key, sizeof(key), "%s %d %d %s", base, kind, offset, pr_ra_key);
    else
        snprintf(key, sizeof(key), "%s %d %d", base, kind, offset);
    for (n = 0; n < pr_cache_count; n++) {
        if (strcmp(pr_cache[n].key, key) == 0 && !(pr_needs & ~pr_cache[n].needs))
            return pr_cache[n].value;
    }
    if (hoistable) {
        int k = pr_find_key(key);

        if (k < 0 && pr_key_count < PR_KEYS) {
            k = pr_key_count++;
            snprintf(pr_keys[k], sizeof(pr_keys[k]), "%s", key);
            pr_key_uses[k] = 0;
        }
        if (k >= 0 && pr_counting)
            pr_key_uses[k]++;
        if (k >= 0 && !pr_counting && k < pr_hoist_limit)
            pre = 1;
    }
    value = pr_new();
    pr_values[value].fresh = 0;
    if (pre)
        pr_values[value].loop = 1;
    if (kind == 1) {
        pr_op(pre, 1, 1, &value, "lfs @0, %d(%s)", offset, base);
        pr_values[value].splat = 1;
    } else {
        pr_op(pre, 1, 1, &value, "psq_l @0, %d(%s), %d, 0", offset, base, kind == 2);
    }
    pr_remember(key, value);
    if (pre) {
        pr_cache[pr_cache_count - 1].needs = ~0u;
        pr_cache[pr_cache_count - 1].hoisted = 1;
    }
    return value;
}

/* a value kept for later instructions (valid where the block it was made in runs) */
static void pr_remember(const char *key, int value) {
    if (pr_cache_count < PR_CACHE) {
        PrCached *cached = &pr_cache[pr_cache_count++];

        snprintf(cached->key, sizeof(cached->key), "%s", key);
        cached->value = value;
        cached->needs = pr_needs;
        cached->hoisted = 0;
    }
}

/* a value made a temporary's register (written in place later): 1 when it
can be (fresh, or a load or merge not loaded before the loop: no longer
taken from the cache), 0 when a copy is needed */
static int pr_adopt(int value) {
    int n, kept = 0;

    if (value <= 0 || pr_values[value].web || pr_values[value].loop)
        return 0;
    for (n = 0; n < pr_cache_count; n++)
        if (pr_cache[n].value == value && pr_cache[n].hoisted)
            return 0;
    for (n = 0; n < pr_cache_count; n++)
        if (pr_cache[n].value != value)
            pr_cache[kept++] = pr_cache[n];
    pr_cache_count = kept;
    pr_values[value].fresh = 0;
    pr_values[value].web = 1;
    return 1;
}

static int pr_k(int kind, int offset) {
    return pr_load(kind, "%[k]", offset, 1);
}

/* the cache past the prefix (what it loaded is not loaded when it is reused) */
static void pr_forget(void) {
    int n, kept = 0;

    for (n = 0; n < pr_cache_count; n++)
        if (pr_cache[n].hoisted)
            pr_cache[kept++] = pr_cache[n];
    pr_cache_count = kept;
    pr_ra_key[0] = 0;
}

/* ---- operands ---- */

static int pr_temporary_index(unsigned index) {
    return index == 12 ? 12 : index < 12 ? (int)index : 0;
}

/* a temporary half's value: its definition, or its start value (0, or the
position's (0, 0, 0, 1)) */
static int pr_half(int temporary, int half) {
    if (pr_def[temporary][half] >= 0)
        return pr_def[temporary][half];
    return pr_k(0, temporary == 12 && half ? PRK_W1 : PRK_ZERO);
}

/* a constant operand's row: base and offset (%[ra] made for a relative one) */
static void pr_constant_row(const WlXboxVshInstruction *instruction, const char **base, int *offset) {
    if (instruction->relative) {
        char key[24];

        snprintf(key, sizeof(key), "%d.%d", instruction->constant, pr_a0_version);
        if (strcmp(key, pr_ra_key) != 0) {
            char done[24];

            pr_new_label(done, sizeof(done));
            pr_text("addi %%[ra], %%[a0], %d", instruction->constant);
            pr_text("cmplwi %%[ra], %d", WL_XBOX_VSH_CONSTANT_COUNT - 1);
            pr_text("ble %s", done);
            pr_text("srawi %%[ra], %%[ra], 31");
            pr_text("li 0, %d", WL_XBOX_VSH_CONSTANT_COUNT - 1);
            pr_text("andc %%[ra], 0, %%[ra]");
            pr_place_label(done);
            pr_text("slwi %%[ra], %%[ra], 4");
            pr_text("add %%[ra], %%[ra], %%[c]");
            snprintf(pr_ra_key, sizeof(pr_ra_key), "%s", key);
        }
        pr_uses_ra = pr_uses_a0 = 1;
        *base = "%[ra]";
        *offset = 0;
        return;
    }
    {
        int row = instruction->constant < WL_XBOX_VSH_CONSTANT_COUNT ? instruction->constant
                                                                   : WL_XBOX_VSH_CONSTANT_COUNT - 1;

        if (row < 127) {
            *base = "%[c]";
            *offset = 16 * row;
        } else {
            *base = "%[c2]";
            *offset = 16 * (row - 128);
            pr_uses_c2 = 1;
        }
    }
}

/* a memory operand's lanes l0 l1 of its source (before the swizzle): one
load, or two and a merge */
static int pr_memory_pair(const char *base, int offset, int s0, int s1, int hoistable) {
    int a, b;

    if (s0 == s1)
        return pr_load(1, base, offset + 4 * s0, hoistable);
    if ((s0 >> 1) == (s1 >> 1) && s0 < s1)
        return pr_load(0, base, offset + 8 * (s0 >> 1), hoistable);
    a = pr_load(0, base, offset + 8 * (s0 >> 1), hoistable);
    b = pr_load(0, base, offset + 8 * (s1 >> 1), hoistable);
    {
        char name[24];

        snprintf(name, sizeof(name), "ps_merge%d%d", s0 & 1, s1 & 1);
        return pr_op3(name, a, b, -1);
    }
}

/* the operand's lanes l0, l1 (after the swizzle) as a pair, its negation
not applied (*negate) */
static int pr_pair(const WlXboxVshInstruction *instruction, const WlXboxVshOperand *operand, int l0,
                   int l1, int *negate) {
    int s0 = operand->swizzle[l0] & 3, s1 = operand->swizzle[l1] & 3;

    *negate = operand->negate;
    switch (operand->mux) {
    case _mux_temporary: {
        int t = pr_temporary_index(operand->index);
        int a = pr_half(t, s0 >> 1), b = pr_half(t, s1 >> 1);
        char name[24];

        if (a == b && (s0 & 1) == 0 && (s1 & 1) == 1)
            return a;
        snprintf(name, sizeof(name), "ps_merge%d%d", s0 & 1, s1 & 1);
        return pr_op3(name, a, b, -1);
    }
    case _mux_input:
        return pr_memory_pair("%[v]", 16 * instruction->input, s0, s1, 0);
    case _mux_constant: {
        const char *base;
        int offset;

        pr_constant_row(instruction, &base, &offset);
        return pr_memory_pair(base, offset, s0, s1, !instruction->relative);
    }
    default:
        *negate = 0;
        return pr_k(0, PRK_ZERO);
    }
}

/* the operand's lane l as a register's ps0 (the other lane anything) */
static int pr_scalar(const WlXboxVshInstruction *instruction, const WlXboxVshOperand *operand, int l,
                     int *negate) {
    int s = operand->swizzle[l] & 3;

    if (operand->mux == _mux_temporary && !(s & 1)) {
        *negate = operand->negate;
        return pr_half(pr_temporary_index(operand->index), s >> 1);
    }
    if ((operand->mux == _mux_input || operand->mux == _mux_constant) && !(s & 1)) {
        /* (the pair that has it in ps0: often loaded for another lane) */
        const char *base = "%[v]";
        int offset = 16 * instruction->input;

        *negate = operand->negate;
        if (operand->mux == _mux_constant)
            pr_constant_row(instruction, &base, &offset);
        return pr_load(0, base, offset + 4 * s, operand->mux == _mux_constant && !instruction->relative);
    }
    if (operand->mux == _mux_temporary || operand->mux == _mux_input || operand->mux == _mux_constant)
        return pr_pair(instruction, operand, l, l, negate);
    *negate = 0;
    return pr_k(0, PRK_ZERO);
}

/* a pair negated when asked (a new value) */
static int pr_negated(int value, int negate) {
    return negate ? pr_op3("ps_neg", value, -1, -1) : value;
}

/* ---- the program ---- */

/* a result lane: its value's register and lane */
typedef struct {
    int value, lane;
} PrLane;

/* the components of each operand an instruction's results read (bit 3 x),
for its writes in mode */
static void pr_reads(const WlXboxVshInstruction *in, int mode, unsigned reads[3]) {
    int mac_t = in->mac != _mac_nop && (in->mac == _mac_arl || in->mac_mask) && (mode & WRITE_TEMPORARIES);
    int ilu_t = in->ilu != _ilu_nop && in->ilu_mask && (mode & WRITE_TEMPORARIES);
    int out = in->output_mask && in->output_is_register &&
              (in->output_from_ilu ? in->ilu : in->mac) != 0 && (mode & WRITE_OUTPUTS);
    unsigned mac_lanes = (mac_t ? in->mac_mask : 0) | (out && !in->output_from_ilu ? in->output_mask : 0);
    unsigned ilu_lanes = (ilu_t ? in->ilu_mask : 0) | (out && in->output_from_ilu ? in->output_mask : 0);
    int component, which;

    reads[0] = reads[1] = reads[2] = 0;
    if (in->mac == _mac_arl && mac_t)
        mac_lanes = 8;
    if (mac_lanes) {
        unsigned used = 0;

        switch (in->mac) {
        case _mac_dp3:
            used = 0xe;
            break;
        case _mac_dp4:
        case _mac_dph:
            used = 0xf;
            break;
        case _mac_arl:
            used = 8;
            break;
        default:
            used = mac_lanes;
            break;
        }
        for (which = 0; which < 2; which++) {
            unsigned u = used;

            if (which == 0 && in->mac == _mac_dph)
                u = 0xe;
            if (which == 1 && (in->mac == _mac_mov || in->mac == _mac_arl || in->mac == _mac_add))
                continue;
            for (component = 0; component < 4; component++)
                if (u & (8u >> component))
                    reads[which] |= 8u >> (in->operands[which].swizzle[component] & 3);
        }
        if (in->mac == _mac_add || in->mac == _mac_mad)
            for (component = 0; component < 4; component++)
                if (used & (8u >> component))
                    reads[2] |= 8u >> (in->operands[2].swizzle[component] & 3);
        if (in->mac == _mac_add) {
            /* (ADD's A and C) */
            reads[1] = 0;
        }
    }
    if (ilu_lanes) {
        unsigned used = in->ilu == _ilu_mov ? ilu_lanes : 8;

        for (component = 0; component < 4; component++)
            if (used & (8u >> component))
                reads[2] |= 8u >> (in->operands[2].swizzle[component] & 3);
    }
}

/* whether the instruction can be written as pairs */
static int pr_supported(const WlXboxVshInstruction *in) {
    switch (in->mac) {
    case _mac_nop:
    case _mac_mov:
    case _mac_mul:
    case _mac_add:
    case _mac_mad:
    case _mac_dp3:
    case _mac_dph:
    case _mac_dp4:
    case _mac_min:
    case _mac_max:
    case _mac_arl:
        break;
    default:
        return 0;
    }
    switch (in->ilu) {
    case _ilu_nop:
    case _ilu_mov:
    case _ilu_rcp:
    case _ilu_rcc:
    case _ilu_rsq:
        return 1;
    default:
        return 0;
    }
}

/* the MAC's result lanes (needed: bit 3 x); dot products write one lane in
place when that is all they write (*placed) */
static void pr_mac(const WlXboxVshInstruction *in, unsigned needed, PrLane result[4], int *dot) {
    int half, negate[3];

    *dot = -1;
    if (in->mac == _mac_dp3 || in->mac == _mac_dp4 || in->mac == _mac_dph) {
        int axy, bxy, t, n;

        axy = pr_pair(in, &in->operands[0], 0, 1, &negate[0]);
        bxy = pr_pair(in, &in->operands[1], 0, 1, &negate[1]);
        n = negate[0] ^ negate[1];
        /* (a negated DP3 or DP4 negates its last multiply-add; DPH's w term
        is not A's) */
        if (n && in->mac == _mac_dph)
            bxy = pr_negated(bxy, 1);
        t = pr_op3("ps_mul", axy, bxy, -1);
        if (in->mac == _mac_dp3) {
            int az, bz, s;

            az = pr_scalar(in, &in->operands[0], 2, &negate[0]);
            bz = pr_scalar(in, &in->operands[1], 2, &negate[1]);
            if (negate[0] ^ negate[1]) {
                /* -(az bz + (ax bx + ay by)), in both lanes */
                int u = pr_op3("ps_sum0", t, t, t);

                s = pr_op3("fnmadds", az, bz, u);
                for (half = 0; half < 4; half++) {
                    result[half].value = s;
                    result[half].lane = 0;
                }
                return;
            }
            /* (az bz + ax bx in both lanes; the sum's ps0 adds t's ps1) */
            s = pr_op3("fmadds", az, bz, t);
            *dot = s;
            result[0].value = t;
            result[0].lane = -3;
        } else {
            int azw, bzw;

            if (in->mac == _mac_dph) {
                const WlXboxVshOperand *a = &in->operands[0];

                if (a->mux == _mux_input && (a->swizzle[2] & 3) == 2) {
                    azw = pr_load(2, "%[v]", 16 * in->input + 8, 0);
                    negate[0] = a->negate;
                    if (negate[0]) {
                        /* (-z, 1): the negation is B's */
                        azw = pr_op3("ps_merge01", pr_negated(azw, 1), azw, -1);
                        negate[0] = 0;
                    }
                } else {
                    int z = pr_scalar(in, a, 2, &negate[0]);

                    z = pr_negated(z, negate[0]);
                    negate[0] = 0;
                    azw = pr_op3("ps_merge01", z, pr_k(0, PRK_W1), -1);
                }
                bzw = pr_pair(in, &in->operands[1], 2, 3, &negate[1]);
                if (negate[1])
                    bzw = pr_negated(bzw, 1);
            } else {
                azw = pr_pair(in, &in->operands[0], 2, 3, &negate[0]);
                bzw = pr_pair(in, &in->operands[1], 2, 3, &negate[1]);
                if (n) {
                    t = pr_op3("ps_nmadd", azw, bzw, t);
                    *dot = t;
                    result[0].value = t;
                    result[0].lane = -4;
                    return;
                }
            }
            t = pr_op3("ps_madd", azw, bzw, t);
            *dot = t;
            result[0].value = t;
            result[0].lane = -4;
        }
        return;
    }
    if (in->mac == _mac_arl) {
        result[0].value = pr_scalar(in, &in->operands[0], 0, &negate[0]);
        result[0].value = pr_negated(result[0].value, negate[0]);
        result[0].lane = 0;
        return;
    }
    for (half = 0; half < 2; half++) {
        int l0 = 2 * half, l1 = l0 + 1, r = -1;

        if (!(needed & (0xcu >> l0)))
            continue;
        switch (in->mac) {
        case _mac_mov:
            r = pr_negated(pr_pair(in, &in->operands[0], l0, l1, &negate[0]), 0);
            if (in->operands[0].negate)
                r = pr_negated(r, 1);
            break;
        case _mac_mul:
        case _mac_mad: {
            const WlXboxVshOperand *a = &in->operands[0], *b = &in->operands[1];
            int pa, pb, c = -1, splat = -1, product_negate;
            char name[24];

            /* (a broadcast multiplicand from a register: ps_muls/ps_madds) */
            if (b->mux == _mux_temporary && b->swizzle[l0] == b->swizzle[l1]) {
                splat = b->swizzle[l0] & 3;
            } else if (a->mux == _mux_temporary && a->swizzle[l0] == a->swizzle[l1]) {
                const WlXboxVshOperand *swap = a;

                a = b;
                b = swap;
                splat = b->swizzle[l0] & 3;
            }
            pa = pr_pair(in, a, l0, l1, &negate[0]);
            if (splat >= 0) {
                pb = pr_half(pr_temporary_index(b->index), splat >> 1);
                negate[1] = b->negate;
            } else {
                pb = pr_pair(in, b, l0, l1, &negate[1]);
            }
            product_negate = negate[0] ^ negate[1];
            if (in->mac == _mac_mad)
                c = pr_pair(in, &in->operands[2], l0, l1, &negate[2]);
            if (in->mac == _mac_mul) {
                if (splat >= 0) {
                    snprintf(name, sizeof(name), "ps_muls%d", splat & 1);
                    r = pr_op3(name, pa, pb, -1);
                } else {
                    r = pr_op3("ps_mul", pa, pb, -1);
                }
                r = pr_negated(r, product_negate);
            } else if (splat >= 0) {
                pa = pr_negated(pa, product_negate);
                c = pr_negated(c, negate[2]);
                snprintf(name, sizeof(name), "ps_madds%d", splat & 1);
                r = pr_op3(name, pa, pb, c);
            } else {
                static const char *const names[2][2] = {{"ps_madd", "ps_msub"}, {"ps_nmsub", "ps_nmadd"}};

                r = pr_op3(names[product_negate][negate[2]], pa, pb, c);
            }
            break;
        }
        case _mac_add: {
            int pa = pr_pair(in, &in->operands[0], l0, l1, &negate[0]);
            int pc = pr_pair(in, &in->operands[2], l0, l1, &negate[2]);

            if (negate[0] && negate[2])
                r = pr_negated(pr_op3("ps_add", pa, pc, -1), 1);
            else if (negate[0])
                r = pr_op3("ps_sub", pc, pa, -1);
            else if (negate[2])
                r = pr_op3("ps_sub", pa, pc, -1);
            else
                r = pr_op3("ps_add", pa, pc, -1);
            break;
        }
        case _mac_min:
        case _mac_max: {
            int pa = pr_negated(pr_pair(in, &in->operands[0], l0, l1, &negate[0]), 0);
            int pb, d;

            pa = pr_negated(pa, negate[0]);
            pb = pr_pair(in, &in->operands[1], l0, l1, &negate[1]);
            pb = pr_negated(pb, negate[1]);
            /* (MAX: A - B >= 0 ? A : B; MIN: B - A >= 0 ? A : B: B when
            either is a NaN, as the C's compares) */
            if (in->mac == _mac_max)
                d = pr_op3("ps_sub", pa, pb, -1);
            else
                d = pr_op3("ps_sub", pb, pa, -1);
            r = pr_op3("ps_sel", d, pa, pb);
            break;
        }
        default:
            pr_failed = 1;
            return;
        }
        result[l0].value = result[l1].value = r;
        result[l0].lane = 0;
        result[l1].lane = 1;
    }
}

/* the ILU's result lanes */
static void pr_ilu(const WlXboxVshInstruction *in, unsigned needed, PrLane result[4]) {
    const WlXboxVshOperand *c = &in->operands[2];
    int half, negate, x, s;

    if (in->ilu == _ilu_mov) {
        for (half = 0; half < 2; half++) {
            int r;

            if (!(needed & (0xcu >> (2 * half))))
                continue;
            r = pr_pair(in, c, 2 * half, 2 * half + 1, &negate);
            r = pr_negated(r, negate);
            result[2 * half].value = result[2 * half + 1].value = r;
            result[2 * half].lane = 0;
            result[2 * half + 1].lane = 1;
        }
        return;
    }
    x = pr_scalar(in, c, 0, &negate);
    x = pr_negated(x, negate);
    if (in->ilu == _ilu_rcp) {
        s = pr_op3("fdivs", pr_k(1, PRK_ONE), x, -1);
    } else if (in->ilu == _ilu_rcc) {
        char positive[24], second[24], low[24], done[24];
        int zero = pr_k(0, PRK_ZERO), values[2], bounds[4], b;

        /* (the bounds loaded before the branches: a load on one path is not
        there on the other) */
        for (b = 0; b < 4; b++)
            bounds[b] = pr_k(1, PRK_RCC_LOW + 4 * b);
        s = pr_op3("fdivs", pr_k(1, PRK_ONE), x, -1);
        pr_new_label(positive, sizeof(positive));
        pr_new_label(second, sizeof(second));
        pr_new_label(low, sizeof(low));
        pr_new_label(done, sizeof(done));
        values[0] = s;
        values[1] = zero;
        pr_op(0, 0, 2, values, "fcmpu 0, @0, @1");
        pr_text("bgt %s", positive);
        values[0] = x;
        pr_op(0, 0, 2, values, "fcmpu 0, @0, @1");
        pr_text("bgt %s", positive);
        /* (negative: r > -low ? -low : r < -high ? -high : r) */
        values[0] = s;
        values[1] = bounds[2];
        pr_op(0, 0, 2, values, "fcmpu 0, @0, @1");
        pr_text("bng %s", second);
        pr_op(0, 1, 2, values, "fmr @0, @1");
        pr_text("b %s", done);
        pr_place_label(second);
        values[1] = bounds[3];
        pr_op(0, 0, 2, values, "fcmpu 0, @0, @1");
        pr_text("bnl %s", done);
        pr_op(0, 1, 2, values, "fmr @0, @1");
        pr_text("b %s", done);
        /* (positive: r < low ? low : r > high ? high : r) */
        pr_place_label(positive);
        values[1] = bounds[0];
        pr_op(0, 0, 2, values, "fcmpu 0, @0, @1");
        pr_text("bnl %s", low);
        pr_op(0, 1, 2, values, "fmr @0, @1");
        pr_text("b %s", done);
        pr_place_label(low);
        values[1] = bounds[1];
        pr_op(0, 0, 2, values, "fcmpu 0, @0, @1");
        pr_text("bng %s", done);
        pr_op(0, 1, 2, values, "fmr @0, @1");
        pr_place_label(done);
    } else {
        /* RSQ: 1.0f / sqrtf(|x|), as the C: frsqrte's estimate made exact
        by four Newton's steps in double, |x| times it rounded to a float
        (sqrtf's bits), its reciprocal; 0, infinity and NaN, where the steps
        make a NaN, take the estimate (exact for them) */
        int a, e, h, t, step, one = pr_k(1, PRK_ONE), halves = pr_k(1, PRK_HALF),
                              three_halves = pr_k(1, PRK_THREE_HALVES), values[3];
        char special[24], done[24];

        a = pr_op3("fabs", x, -1, -1);
        e = pr_op3("frsqrte", a, -1, -1);
        h = pr_op3("fmul", a, halves, -1);
        for (step = 0; step < 4; step++) {
            t = pr_op3("fmul", e, e, -1);
            t = pr_op3("fnmsub", h, t, three_halves);
            values[0] = e;
            values[1] = t;
            pr_op(0, 1, 2, values, "fmul @0, @0, @1");
        }
        pr_new_label(special, sizeof(special));
        pr_new_label(done, sizeof(done));
        values[0] = e;
        pr_op(0, 0, 1, values, "fcmpu 0, @0, @0");
        pr_text("bun %s", special);
        values[1] = a;
        pr_op(0, 1, 2, values, "fmul @0, @1, @0");
        pr_op(0, 1, 1, values, "frsp @0, @0");
        s = pr_op3("fdivs", one, e, -1);
        pr_text("b %s", done);
        pr_place_label(special);
        values[0] = s;
        values[1] = a;
        pr_op(0, 1, 2, values, "frsqrte @0, @1");
        pr_op(0, 1, 1, values, "frsp @0, @0");
        pr_place_label(done);
    }
    for (half = 0; half < 4; half++) {
        result[half].value = s;
        result[half].lane = 0;
    }
}

/* lane l of a result moved to a new value's lane 0 (for a store) */
static int pr_lane0(PrLane r) {
    if (r.lane == 0 || pr_values[r.value].splat)
        return r.value;
    return pr_op3("ps_merge11", r.value, r.value, -1);
}

/* the value whose lanes are a and b, or -1 when a merge must make it */
static int pr_pair_of(PrLane a, PrLane b) {
    if (a.value != b.value)
        return -1;
    if (pr_values[a.value].splat || (a.lane == 0 && b.lane == 1))
        return a.value;
    return -1;
}

/* a and b as a pair */
static int pr_merged(PrLane a, PrLane b) {
    int v = pr_pair_of(a, b);
    char name[24];

    if (v >= 0)
        return v;
    snprintf(name, sizeof(name), "ps_merge%d%d", a.lane, b.lane);
    return pr_op3(name, a.value, b.value, -1);
}

/* a value as a temporary's new register: itself when it can be, else a copy */
static int pr_owned(int value) {
    int d;

    if (pr_adopt(value))
        return value;
    d = pr_op3("ps_mr", value, -1, -1);
    pr_values[d].splat = pr_values[value].splat;
    pr_adopt(d);
    return d;
}

/* the result's lanes written to the output at offset (mask bit 3 x) */
static void pr_store(const PrLane result[4], unsigned mask, const char *field) {
    int half;

    for (half = 0; half < 2; half++) {
        int l0 = 2 * half, l1 = l0 + 1, values[1];

        if ((mask & (8u >> l0)) && (mask & (8u >> l1))) {
            values[0] = pr_merged(result[l0], result[l1]);
            pr_op(0, 0, 1, values, "psq_st @0, %s+%d(%%[o]), 0, 0", field, 8 * half);
            continue;
        }
        if (mask & (8u >> l0)) {
            values[0] = pr_lane0(result[l0]);
            pr_op(0, 0, 1, values, "stfs @0, %s+%d(%%[o])", field, 8 * half);
        }
        if (mask & (8u >> l1)) {
            values[0] = pr_lane0(result[l1]);
            pr_op(0, 0, 1, values, "stfs @0, %s+%d(%%[o])", field, 8 * half + 4);
        }
    }
}

/* the result's lanes into a temporary (mask bit 3 x) */
static void pr_commit(int t, const PrLane result[4], unsigned mask) {
    int half;

    for (half = 0; half < 2; half++) {
        int l0 = 2 * half, l1 = l0 + 1, d, old = pr_def[t][half], in_place;

        if (!(mask & (0xcu >> l0)))
            continue;
        if ((mask & (8u >> l0)) && (mask & (8u >> l1))) {
            pr_def[t][half] = pr_owned(pr_merged(result[l0], result[l1]));
            continue;
        }
        {
            int which_lane = (mask & (8u >> l0)) ? 0 : 1;
            PrLane r = result[l0 + which_lane];
            int other_live = (pr_live_after[t] & (8u >> (l0 + !which_lane))) != 0;

            in_place = other_live && old >= 0 && !pr_kept[t][half];
            if (!other_live) {
                /* (the other lane is not read: any value whose lane is right) */
                if (r.lane == which_lane || pr_values[r.value].splat)
                    d = pr_owned(r.value);
                else
                    d = pr_owned(pr_merged(r, r));
            } else {
                int base = pr_half(t, half), values[3];

                d = pr_new();
                values[0] = d;
                if (which_lane == 0) {
                    values[1] = r.value;
                    values[2] = base;
                    pr_op(0, 1, 3, values, "ps_merge%d1 @0, @1, @2", r.lane);
                } else {
                    values[1] = base;
                    values[2] = r.value;
                    pr_op(0, 1, 3, values, "ps_merge0%d @0, @1, @2", r.lane);
                }
                if (in_place)
                    pr_union(old, d);
                pr_values[d].fresh = 0;
                pr_values[d].web = 1;
            }
            pr_def[t][half] = d;
        }
    }
}

/* a dot product (t: its pair of partial sums; DP3: s its ln 0 and z) into
one ln of a temporary, in place */
static int pr_dot_lane(int t, int s, int dp3, int temporary, int ln) {
    int half = ln >> 1, old = pr_def[temporary][half], other_live, d, values[4];

    other_live = (pr_live_after[temporary] & (8u >> (ln ^ 1))) != 0;
    d = pr_new();
    values[0] = d;
    values[1] = dp3 ? s : t;
    values[2] = other_live ? pr_half(temporary, half) : t;
    values[3] = t;
    /* (ps_sum0 d, a, c, b: d = (a0 + b1, c1); ps_sum1: (c0, a0 + b1)) */
    pr_op(0, 1, 4, values, "ps_sum%d @0, @1, @2, @3", ln & 1);
    if (other_live && old >= 0 && !pr_kept[temporary][half])
        pr_union(old, d);
    pr_values[d].fresh = 0;
    pr_values[d].web = 1;
    pr_def[temporary][half] = d;
    return d;
}

static const char *pr_output_field(unsigned address, char *text, size_t size) {
    switch (address) {
    case _output_d0:
        return "%[od0]";
    case _output_d1:
        return "%[od1]";
    case _output_b0:
        return "%[ob0]";
    case _output_b1:
        return "%[ob1]";
    default:
        snprintf(text, size, "%%[ot]+%u", 16 * (address - _output_t0));
        return text;
    }
}

/* one instruction (mode: WRITE_TEMPORARIES, WRITE_OUTPUTS or both) */
static void pr_instruction(const WlXboxVshInstruction *in, int mode) {
    int mac_t = in->mac != _mac_nop && (in->mac == _mac_arl || in->mac_mask) && (mode & WRITE_TEMPORARIES);
    int ilu_t = in->ilu != _ilu_nop && in->ilu_mask && (mode & WRITE_TEMPORARIES);
    int out = in->output_mask && in->output_is_register &&
              (in->output_from_ilu ? in->ilu : in->mac) != 0 && (mode & WRITE_OUTPUTS);
    unsigned mac_lanes = (mac_t ? in->mac_mask : 0) | (out && !in->output_from_ilu ? in->output_mask : 0);
    unsigned ilu_lanes = (ilu_t ? in->ilu_mask : 0) | (out && in->output_from_ilu ? in->output_mask : 0);
    PrLane mac[4], ilu[4];
    int dot = -1, k, mac_temporary = pr_temporary_index(in->temporary);
    int ilu_temporary = pr_temporary_index(in->mac != _mac_nop ? 1 : in->temporary);
    char text[24];

    memset(mac, 0, sizeof(mac));
    memset(ilu, 0, sizeof(ilu));
    if (in->mac == _mac_arl && mac_t)
        mac_lanes = 8;
    if (in->mac != _mac_nop && mac_lanes)
        pr_mac(in, mac_lanes, mac, &dot);
    if (in->ilu != _ilu_nop && ilu_lanes)
        pr_ilu(in, ilu_lanes, ilu);
    if (pr_failed)
        return;
    /* (the clip-space position: p before this instruction's writes) */
    if (in->captures_clip && (mode & WRITE_OUTPUTS)) {
        PrLane p[4];

        for (k = 0; k < 4; k++) {
            p[k].value = pr_half(12, k >> 1);
            p[k].lane = k & 1;
        }
        pr_store(p, 0xf, "%[oclip]");
        pr_text("li 0, 1");
        pr_text("stw 0, %%[ocaptured](%%[o])");
    }
    /* (a dot product's lanes: written in place when one ln of a
    temporary is all it writes there) */
    if (dot >= 0) {
        unsigned temporary_lanes = mac_t ? in->mac_mask : 0;
        int position_lanes = out && !in->output_from_ilu && in->output_address == _output_position
                                 ? in->output_mask : 0;
        int dp3 = in->mac == _mac_dp3;
        int t = dp3 ? mac[0].value : dot, s = dp3 ? dot : -1;
        PrLane r;

        if ((temporary_lanes == 8 || temporary_lanes == 4 || temporary_lanes == 2 || temporary_lanes == 1) &&
            !position_lanes) {
            int ln = temporary_lanes == 8 ? 0 : temporary_lanes == 4 ? 1 : temporary_lanes == 2 ? 2 : 3;

            r.value = pr_dot_lane(t, s, dp3, mac_temporary, ln);
            r.lane = ln & 1;
            mac_t = 0;
        } else if (!temporary_lanes && (position_lanes == 8 || position_lanes == 4 || position_lanes == 2 ||
                                        position_lanes == 1)) {
            int ln = position_lanes == 8 ? 0 : position_lanes == 4 ? 1 : position_lanes == 2 ? 2 : 3;

            r.value = pr_dot_lane(t, s, dp3, 12, ln);
            r.lane = ln & 1;
            out = 0;
        } else {
            int values[4];

            r.value = pr_new();
            r.lane = 0;
            values[0] = r.value;
            values[1] = dp3 ? s : t;
            values[2] = t;
            values[3] = t;
            pr_op(0, 1, 4, values, "ps_sum0 @0, @1, @2, @3");
        }
        for (k = 0; k < 4; k++)
            mac[k] = r;
    }
    if (in->mac == _mac_arl && mac_t) {
        int y, z, w, values[3], big = pr_k(1, PRK_2P23), minus = pr_k(1, PRK_M2P23), one = pr_k(1, PRK_ONE);
        char slow[24], store[24], whole[24];

        pr_new_label(slow, sizeof(slow));
        pr_new_label(store, sizeof(store));
        pr_new_label(whole, sizeof(whole));
        y = pr_op3("fadds", mac[0].value, pr_k(1, PRK_ARL), -1);
        z = pr_op3("fabs", y, -1, -1);
        values[0] = z;
        values[1] = big;
        pr_op(0, 0, 2, values, "fcmpu 0, @0, @1");
        pr_text("bnl %s", slow);
        w = pr_op3("fsel", y, big, minus);
        values[0] = z;
        values[1] = y;
        values[2] = w;
        pr_op(0, 1, 3, values, "fadds @0, @1, @2");
        values[1] = z;
        pr_op(0, 1, 3, values, "fsubs @0, @1, @2");
        values[1] = y;
        pr_op(0, 0, 2, values, "fcmpu 0, @0, @1");
        pr_text("bng %s", whole);
        values[1] = z;
        values[2] = one;
        pr_op(0, 1, 3, values, "fsubs @0, @1, @2");
        pr_place_label(whole);
        pr_op(0, 1, 2, values, "fctiwz @0, @1");
        pr_text("b %s", store);
        pr_place_label(slow);
        values[1] = y;
        pr_op(0, 1, 2, values, "fctiwz @0, @1");
        pr_place_label(store);
        pr_op(0, 0, 1, values, "stfiwx @0, 0, %%[slot]");
        pr_text("lwz %%[a0], 0(%%[slot])");
        pr_uses_a0 = pr_uses_slot = 1;
        pr_a0_version++;
        pr_ra_key[0] = 0;
        mac_t = 0;
    }
    /* (a MOV's result that is a temporary's register, when this
    instruction writes that temporary: copied first) */
    for (k = 0; k < 4; k++) {
        PrLane *lists[2] = {&mac[k], &ilu[k]};
        int which;

        for (which = 0; which < 2; which++) {
            PrLane *r = lists[which];
            int t, h;

            if (!pr_values[r->value].web || r->value == 0)
                continue;
            for (t = 0; t < 13; t++)
                for (h = 0; h < 2; h++)
                    if (pr_def[t][h] >= 0 && pr_root(pr_def[t][h]) == pr_root(r->value) &&
                        ((mac_t && t == mac_temporary) || (ilu_t && t == ilu_temporary) ||
                         (out && in->output_address == _output_position && t == 12))) {
                        int copy = pr_op3("ps_mr", r->value, -1, -1);
                        int j;

                        for (j = 0; j < 4; j++) {
                            if (mac[j].value == r->value)
                                mac[j].value = copy;
                            if (ilu[j].value == r->value)
                                ilu[j].value = copy;
                        }
                        pr_values[copy].fresh = 0;
                        t = 13;
                        break;
                    }
        }
    }
    /* outputs to memory */
    if (out && in->output_address != _output_position) {
        const PrLane *r = in->output_from_ilu ? ilu : mac;

        switch (in->output_address) {
        case _output_fog:
            if (in->output_mask & 8)
                pr_store(r, 8, "%[ofog]");
            break;
        case _output_point_size:
            if (in->output_mask & 8)
                pr_store(r, 8, "%[opoint]");
            break;
        default:
            if (in->output_address == _output_d0 || in->output_address == _output_d1 ||
                in->output_address == _output_b0 || in->output_address == _output_b1 ||
                (in->output_address >= _output_t0 && in->output_address < _output_t0 + 4))
                pr_store(r, in->output_mask, pr_output_field(in->output_address, text, sizeof(text)));
            break;
        }
    }
    if (mac_t)
        pr_commit(mac_temporary, mac, in->mac_mask);
    if (ilu_t)
        pr_commit(ilu_temporary, ilu, in->ilu_mask);
    if (out && in->output_address == _output_position)
        pr_commit(12, in->output_from_ilu ? ilu : mac, in->output_mask);
}

/* the temporaries' lanes read after each instruction (bit 3 x), the
position's all read at the end; instructions no output needs left out */
static void pr_liveness(const WlXboxVshProgram *program, const unsigned *needs, unsigned long from,
                        unsigned (*after)[13]) {
    unsigned live[13];
    long index;
    int t, which;

    memset(live, 0, sizeof(live));
    live[12] = 0xf;
    for (index = (long)program->count - 1; index >= (long)from; index--) {
        const WlXboxVshInstruction *in = &program->instructions[index];
        unsigned reads[3];

        memcpy(after[index], live, sizeof(live));
        if (!needs[index])
            continue;
        if (in->mac != _mac_nop && in->mac != _mac_arl && in->mac_mask)
            live[pr_temporary_index(in->temporary)] &= ~in->mac_mask;
        if (in->ilu != _ilu_nop && in->ilu_mask)
            live[pr_temporary_index(in->mac != _mac_nop ? 1 : in->temporary)] &= ~in->ilu_mask;
        if (in->output_mask && in->output_is_register && (in->output_from_ilu ? in->ilu : in->mac) != 0 &&
            in->output_address == _output_position)
            live[12] &= ~in->output_mask;
        if (in->captures_clip)
            live[12] = 0xf;
        pr_reads(in, WRITE_ALL, reads);
        for (which = 0; which < 3; which++)
            if (in->operands[which].mux == _mux_temporary)
                live[pr_temporary_index(in->operands[which].index)] |= reads[which];
    }
    (void)t;
}

/* the outputs' components no instruction writes, as live_defaults */
static void pr_defaults(const WlXboxVshProgram *program) {
    static const char *const names[] = {"%[od0]", "%[od1]", "%[ob0]", "%[ob1]",
                                        "%[ot]", "%[ot]+16", "%[ot]+32", "%[ot]+48"};
    unsigned written[8] = {0};
    int fog = 0, captured = 0, which, component, one;
    unsigned long index;

    for (index = 0; index < program->count; index++) {
        const WlXboxVshInstruction *in = &program->instructions[index];

        captured |= in->captures_clip;
        if (!(in->output_mask && in->output_is_register && (in->output_from_ilu ? in->ilu : in->mac) != 0))
            continue;
        if (in->output_address == _output_d0)
            written[0] |= in->output_mask;
        else if (in->output_address == _output_d1)
            written[1] |= in->output_mask;
        else if (in->output_address == _output_b0)
            written[2] |= in->output_mask;
        else if (in->output_address == _output_b1)
            written[3] |= in->output_mask;
        else if (in->output_address == _output_fog)
            fog |= in->output_mask & 8;
        else if (in->output_address >= _output_t0 && in->output_address < _output_t0 + 4)
            written[4 + in->output_address - _output_t0] |= in->output_mask;
    }
    /* (two lanes at once: (0, 0) and (0, 1) as pairs; a lane alone from r0) */
    {
        int pairs[2], values[1];

        pairs[0] = pr_k(0, PRK_ZERO);
        pairs[1] = pr_k(0, PRK_W1);
        for (which = 0; which < 8; which++)
            for (component = 0; component < 4; component += 2)
                if (!(written[which] & (0xcu >> component))) {
                    values[0] = pairs[component == 2];
                    pr_op(0, 0, 1, values, "psq_st @0, %s+%d(%%[o]), 0, 0", names[which], 4 * component);
                    written[which] |= 0xcu >> component;
                }
        if (!captured) {
            values[0] = pairs[0];
            pr_op(0, 0, 1, values, "psq_st @0, %%[oclip](%%[o]), 0, 0");
            pr_op(0, 0, 1, values, "psq_st @0, %%[oclip]+8(%%[o]), 0, 0");
        }
    }
    for (one = 0; one < 2; one++) {
        int first = 1;

        for (which = 0; which < 8 + 2; which++)
            for (component = 0; component < 4; component++) {
                int wanted = which < 8 ? !(written[which] & (8u >> component)) && (component == 3) == one
                                       : which == 8 ? one && !fog && component == 0
                                                    : !one && !captured && component == 0;

                if (!wanted)
                    continue;
                if (first)
                    pr_text(one ? "lis 0, 0x3f80" : "li 0, 0");
                first = 0;
                if (which < 8)
                    pr_text("stw 0, %s+%d(%%[o])", names[which], 4 * component);
                else
                    pr_text("stw 0, %s(%%[o])", which == 8 ? "%[ofog]" : "%[ocaptured]");
            }
    }
}

/* the instructions as ops (pr_pre, pr_body); 0 when one can't be */
static int pr_generate(const WlXboxVshProgram *program, const unsigned *needs, int reused,
                       const ReusePrefix *prefix, int *compare_count) {
    static unsigned after[WL_XBOX_VSH_MAX_INSTRUCTIONS][13];
    unsigned long index, start = reused ? prefix->end : 0;
    unsigned open = ~0u;
    char skip[24] = "", top[24], made[24];
    int t, h;

    pr_value_count = pr_pre_count = pr_body_count = pr_cache_count = 0;
    pr_failed = 0;
    pr_label = 0;
    pr_ra_key[0] = 0;
    pr_a0_version = pr_uses_a0 = pr_uses_ra = pr_uses_c2 = pr_uses_slot = 0;
    memset(pr_def, -1, sizeof(pr_def));
    memset(pr_kept, 0, sizeof(pr_kept));
    /* (value 0: a placeholder no op names) */
    pr_new();
    pr_needs = ~0u;
    for (index = 0; index < program->count; index++)
        if (needs[index] && !pr_supported(&program->instructions[index]))
            return 0;
    pr_liveness(program, needs, 0, after);
    /* (each block's test: a condition register bit set once a batch) */
    pr_need_count = 0;
    for (index = 0; index < program->count; index++)
        if (needs[index])
            pr_need_bit(needs[index]);
    pr_need_bit(WL_XBOX_VSH_LIVE_POSITION);
    *compare_count = 0;
    pr_new_label(top, sizeof(top));
    pr_new_label(made, sizeof(made));
    pr_place_label(top);
    /* the prefix: made, or kept while the inputs it reads are the vertex before's */
    if (reused) {
        char again[24];
        int which;
        unsigned words[16] = {0};

        for (index = 0; index < prefix->end; index++) {
            const WlXboxVshInstruction *in = &program->instructions[index];
            unsigned reads[3];

            if (!needs[index])
                continue;
            pr_reads(in, WRITE_TEMPORARIES, reads);
            for (which = 0; which < 3; which++)
                if (in->operands[which].mux == _mux_input)
                    words[in->input & 15] |= reads[which];
        }
        pr_new_label(again, sizeof(again));
        snprintf(pr_first, sizeof(pr_first), "%s", again);
        for (index = 0; index < 16; index++)
            for (which = 0; which < 4; which++)
                if (words[index] & (8u >> which)) {
                    /* (the vertex before's: %[vs] bytes before) */
                    pr_text("lwz 0, %lu(%%[v])", 16 * index + 4 * (unsigned long)which);
                    pr_text("lwz %%[g], %lu-%%[vs](%%[v])", 16 * index + 4 * (unsigned long)which);
                    pr_text("cmpw 0, %%[g]");
                    pr_text("bne %s", again);
                    (*compare_count)++;
                }
        pr_text("b %s", made);
        pr_place_label(again);
        for (index = 0; index < prefix->end; index++) {
            const WlXboxVshInstruction *in = &program->instructions[index];

            if (!needs[index])
                continue;
            if (needs[index] != open) {
                if (open != ~0u)
                    pr_place_label(skip);
                pr_new_label(skip, sizeof(skip));
                pr_skip(needs[index], skip);
                open = pr_needs = needs[index];
            }
            memcpy(pr_live_after, after[index], sizeof(pr_live_after));
            pr_instruction(in, WRITE_TEMPORARIES);
        }
        if (open != ~0u)
            pr_place_label(skip);
        open = ~0u;
        pr_needs = ~0u;
        pr_place_label(made);
        /* (the halves the rest reads of the prefix: kept for the vertex after) */
        for (t = 0; t < 12; t++)
            for (h = 0; h < 2; h++)
                if ((prefix->live_out[t] & (0xcu >> (2 * h))) && pr_def[t][h] >= 0) {
                    pr_kept[t][h] = 1;
                    pr_values[pr_def[t][h]].loop = 1;
                }
        pr_forget();
        for (index = 0; index < prefix->end; index++) {
            const WlXboxVshInstruction *in = &program->instructions[index];

            if (!needs[index] ||
                !(in->output_mask && in->output_is_register && (in->output_from_ilu ? in->ilu : in->mac) != 0))
                continue;
            if (needs[index] != open) {
                if (open != ~0u)
                    pr_place_label(skip);
                pr_new_label(skip, sizeof(skip));
                pr_skip(needs[index], skip);
                open = pr_needs = needs[index];
            }
            memcpy(pr_live_after, after[index], sizeof(pr_live_after));
            pr_instruction(in, WRITE_OUTPUTS);
        }
    } else {
        pr_first[0] = 0;
        pr_place_label(made);
    }
    for (index = start; index < program->count && !pr_failed; index++) {
        if (!needs[index])
            continue;
        if (needs[index] != open) {
            if (open != ~0u)
                pr_place_label(skip);
            pr_new_label(skip, sizeof(skip));
            pr_skip(needs[index], skip);
            open = pr_needs = needs[index];
        }
        memcpy(pr_live_after, after[index], sizeof(pr_live_after));
        pr_instruction(&program->instructions[index], WRITE_ALL);
    }
    if (open != ~0u)
        pr_place_label(skip);
    pr_needs = ~0u;
    /* the position (as the program left p when it is live; else (0, 0, 0, 1)) */
    {
        char other[24], done[24];
        int values[1], p0 = pr_half(12, 0), p1 = pr_half(12, 1), zero = pr_k(0, PRK_ZERO),
                       w1 = pr_k(0, PRK_W1);

        pr_new_label(other, sizeof(other));
        pr_new_label(done, sizeof(done));
        pr_skip(WL_XBOX_VSH_LIVE_POSITION, other);
        values[0] = p0;
        pr_op(0, 0, 1, values, "psq_st @0, 0(%%[o]), 0, 0");
        values[0] = p1;
        pr_op(0, 0, 1, values, "psq_st @0, 8(%%[o]), 0, 0");
        pr_text("b %s", done);
        pr_place_label(other);
        values[0] = zero;
        pr_op(0, 0, 1, values, "psq_st @0, 0(%%[o]), 0, 0");
        values[0] = w1;
        pr_op(0, 0, 1, values, "psq_st @0, 8(%%[o]), 0, 0");
        pr_place_label(done);
    }
    pr_defaults(program);
    pr_text("addi %%[v], %%[v], %%[vs]");
    pr_text("addi %%[o], %%[o], %%[os]");
    pr_text("bdnz %s", top);
    return !pr_failed;
}

/* registers for the values (a linear scan); 0 when there are too few */
static int pr_allocate(unsigned *used) {
    static int order[PR_VALUES];
    int position = 0, n, k, count = 0, loop_start = pr_pre_count, end = pr_pre_count + pr_body_count;
    int busy[32];

    for (n = 0; n < pr_value_count; n++) {
        pr_values[n].first = pr_values[n].last = -1;
        pr_values[n].phys = -1;
    }
    for (n = 0; n < pr_pre_count + pr_body_count; n++, position++) {
        const PrOp *op = n < pr_pre_count ? &pr_pre[n] : &pr_body[n - pr_pre_count];

        for (k = 0; k < op->count; k++) {
            PrValue *v = &pr_values[pr_root(op->values[k])];

            if (v->first < 0 || position < v->first)
                v->first = position;
            if (position > v->last)
                v->last = position;
        }
    }
    for (n = 0; n < pr_value_count; n++) {
        if (pr_values[n].loop) {
            PrValue *v = &pr_values[pr_root(n)];

            if (v->first < 0)
                continue;
            if (v->first > loop_start)
                v->first = loop_start;
            v->last = end;
        }
    }
    for (n = 0; n < pr_value_count; n++)
        if (pr_root(n) == n && pr_values[n].first >= 0)
            order[count++] = n;
    /* (by start: an insertion sort, the values are mostly in order) */
    for (n = 1; n < count; n++) {
        int v = order[n];

        for (k = n; k > 0 && pr_values[order[k - 1]].first > pr_values[v].first; k--)
            order[k] = order[k - 1];
        order[k] = v;
    }
    for (k = 0; k < 32; k++)
        busy[k] = -1;
    *used = 0;
    for (n = 0; n < count; n++) {
        PrValue *v = &pr_values[order[n]];
        int phys = -1;

        for (k = 0; k < 32; k++) {
            /* (a register is free past its value's last use; an op may write
            the register it reads) */
            if (busy[k] < 0 || pr_values[busy[k]].last <= v->first) {
                if (busy[k] >= 0 && pr_values[busy[k]].last == v->first && pr_values[busy[k]].loop)
                    continue;
                phys = k;
                break;
            }
        }
        if (phys < 0)
            return 0;
        busy[phys] = order[n];
        v->phys = phys;
        *used |= 1u << phys;
    }
    return 1;
}

/* the batch function's body for the Wii as one asm statement (pr_print
writes it), or 0 when the program can't be written so */
static unsigned pr_used;

static int pr_program(const WlXboxVshProgram *program, const unsigned *needs, int reused,
                      const ReusePrefix *prefix) {
    int limit, n, k, compares;
    unsigned used = 0;

    /* (which constants are loaded most: those first loaded once a batch) */
    pr_key_count = 0;
    pr_counting = 1;
    pr_hoist_limit = 0;
    if (!pr_generate(program, needs, reused, prefix, &compares)) {
        pr_counting = 0;
        return 0;
    }
    pr_counting = 0;
    for (n = 1; n < pr_key_count; n++) {
        char key[80];
        int uses = pr_key_uses[n];

        snprintf(key, sizeof(key), "%s", pr_keys[n]);
        for (k = n; k > 0 && pr_key_uses[k - 1] < uses; k--) {
            memcpy(pr_keys[k], pr_keys[k - 1], sizeof(pr_keys[k]));
            pr_key_uses[k] = pr_key_uses[k - 1];
        }
        memcpy(pr_keys[k], key, sizeof(key));
        pr_key_uses[k] = uses;
    }
    for (limit = pr_key_count; limit >= 0; limit--) {
        pr_hoist_limit = limit;
        if (pr_generate(program, needs, reused, prefix, &compares) && pr_allocate(&used))
            break;
    }
    if (limit < 0)
        return 0;
    pr_used = used;
    return 1;
}

/* the asm pr_program made */
static void pr_print(FILE *out) {
    unsigned used = pr_used;
    int n, k;

    fprintf(out, "\tif (count) {\n\t\tconst char *vp = (const char *)vs;\n\t\tchar *op = (char *)os;\n"
                 "\t\tint slot[2];\n\t\tlong g, ra, a0;\n\n");
    fprintf(out, "\t\t__asm__ volatile(\n");
    for (n = 0; n < pr_pre_count + pr_body_count + 2; n++) {
        const PrOp *op;
        const char *t;

        if (n == pr_pre_count) {
            /* (the blocks' bits: live & needs != 0 into bit 4 + k) */
            fprintf(out, "\t\t\t\"li %%[g], 0\\n\\t\"\n");
            for (k = 0; k < pr_need_count && k < PR_NEED_BITS; k++)
                fprintf(out,
                        "\t\t\t\"andi. 0, %%[live], %u\\n\\t\"\n\t\t\t\"neg 0, 0\\n\\t\"\n"
                        "\t\t\t\"srwi 0, 0, 31\\n\\t\"\n\t\t\t\"rlwimi %%[g], 0, %d, %d, %d\\n\\t\"\n",
                        pr_need_values[k], 27 - k, 4 + k, 4 + k);
            fprintf(out, "\t\t\t\"mtcrf 127, %%[g]\\n\\t\"\n");
            fprintf(out, "\t\t\t\"mtctr %%[n]\\n\\t\"\n");
            if (pr_first[0])
                fprintf(out, "\t\t\t\"b %s\\n\\t\"\n", pr_first);
            continue;
        }
        if (n == pr_pre_count + 1)
            continue;
        op = n < pr_pre_count ? &pr_pre[n] : &pr_body[n - pr_pre_count - 2];
        fprintf(out, "\t\t\t\"");
        for (t = op->text; *t; t++) {
            if (*t == '@' && t[1] >= '0' && t[1] <= '3') {
                fprintf(out, "%d", pr_values[pr_root(op->values[t[1] - '0'])].phys);
                t++;
            } else {
                fputc(*t, out);
            }
        }
        fprintf(out, "\\n\\t\"\n");
    }
    fprintf(out, "\t\t\t: [v] \"+b\"(vp), [o] \"+b\"(op), [g] \"=&r\"(g), [ra] \"=&b\"(ra), [a0] \"=&r\"(a0)\n"
                 "\t\t\t: [c] \"b\"(c), [c2] \"b\"(c + 128), [k] \"b\"(wl_xbox_vsh_pairs_k), "
                 "[live] \"r\"(live), [n] \"r\"(count), [slot] \"b\"(slot),\n"
                 "\t\t\t  [vs] \"i\"(sizeof(vs[0])), [os] \"i\"(sizeof(*os)), "
                 "[od0] \"i\"(offsetof(WlXboxVshOutput, d0)), [od1] \"i\"(offsetof(WlXboxVshOutput, d1)),\n"
                 "\t\t\t  [ob0] \"i\"(offsetof(WlXboxVshOutput, b0)), [ob1] \"i\"(offsetof(WlXboxVshOutput, b1)), "
                 "[ot] \"i\"(offsetof(WlXboxVshOutput, t)), [ofog] \"i\"(offsetof(WlXboxVshOutput, fog)),\n"
                 "\t\t\t  [opoint] \"i\"(offsetof(WlXboxVshOutput, point_size)), "
                 "[oclip] \"i\"(offsetof(WlXboxVshOutput, clip)), "
                 "[ocaptured] \"i\"(offsetof(WlXboxVshOutput, clip_captured))\n"
                 "\t\t\t: \"r0\", \"cr0\", \"cr1\", \"cr2\", \"cr3\", \"cr4\", \"cr5\", \"cr6\", \"cr7\", "
                 "\"ctr\", \"memory\"");
    for (k = 0; k < 32; k++)
        if (used & (1u << k))
            fprintf(out, ", \"fr%d\"", k);
    fprintf(out, ");\n\t\t(void)g;\n\t\t(void)ra;\n\t\t(void)a0;\n\t}\n");
}
