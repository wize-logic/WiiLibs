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
// another order, RSQ is frsqrte's estimate and two Newton's steps in single precision (two ulps), and
// MIN/MAX choose by the sign of a difference (+0 for -0 when equal). EXP and LOG are vsh.c's
// (ilu_floor, ilu_power_of_two, ilu_exp2, ilu_log2) with the float's bits made and taken apart
// through the slot, their polynomials' multiply-adds fused, EXP's lanes made only where read.
// The host checks the code the Wii runs with tests/xbox/vsh_ps_emulate.py's C emulation against
// the interpreter. A program with what is not written here (LIT, DST, SLT, SGE) or that needs
// more registers than the Gekko has keeps the per-instruction code.

enum { PR_VALUES = 4096, PR_OPS = 4096, PR_CACHE = 512, PR_KEYS = 256 };

typedef struct {
    int parent;      /* union-find: a web's values share one register */
    int first, last; /* positions: the first definition, the last use */
    int loop;        /* lives over the whole loop (a kept or hoisted value) */
    int phys;
    int fresh;       /* made by this instruction, not yet a temporary's or a cache's */
    int web;         /* a temporary's (in place writes allowed when not kept) */
    int splat;       /* ps1 is ps0 */
    int fetched;     /* (fused) an input made by the prologue, read from its register (pr_fetched_*) */
    int lane_of;     /* both lanes lane lane_of & 1 of value lane_of >> 1 (a ps_merge00 or 11 of it), + 1;
                        0: none */
    int resident;    /* (fused, reused) a constant loaded at the prefix's end: its register the prefix's
                        alone from the again label to there (pr_resident) */
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
/* (fused, reused: the prefix's halves the rest reads stored in the slot from PR_SPILL_AT, not
kept in registers from one vertex to the next, when those run out) */
enum { PR_SPILLS = 12, PR_SPILL_AT = 16 };
static int pr_spill;
static char pr_ra_key[24];      /* what %[ra] holds (a relative row's address) */
static int pr_a0_version, pr_uses_a0, pr_uses_ra, pr_uses_c2, pr_uses_slot;
static char pr_first[24];       /* where the first vertex starts (the prefix made) */
/* a fused function's form (NULL: a batch function's); its live is the form's,
so each block runs or is left out, with no test */
static const WlXboxVshFusedKey *pr_fuse;
static int pr_fused_captured;   /* (fused) the clip-space position captured, always */
static int pr_lit_values[2];    /* (fused, lit) the normal's halves as the program ends */
static int pr_world_values[2];  /* (fused, "w") the world position's halves as the program ends */
/* (fused, "w", too few registers to keep it to the end: the world position stored in the output's
position where the program captures the clip-space one, and its words copied from there) */
static int pr_world_spill;
static int pr_program_world;    /* (fused, "w") the world position's temporary */
static int pr_texgen_from[4];   /* (fused) each stage's input register, for GX's texgen ("M"; -1: none) */
static int pr_world_w;          /* (fused, "w") the clip-space w (both halves) as the program captures it */
/* (fused: GX's vertex's address (%[ad]) taken where the program's own part starts, not in the
epilogue, so a "w" form's world position goes there where the program captures the clip-space
position, its registers free from there on, and is not copied through the output's scratch.
pr_early_off: in the epilogue, as before: xbox_vsh_codegen --no-early) */
static int pr_early_off, pr_early;
static void pr_sweep(void);
static int pr_fused_op(const char *name, int a, int b, int c);
static void pr_fused_store(int value, const char *format, ...);
/* (fused, GX taking the clip-space position: the instructions whose writes nothing reads, the
screen-space position's divide and viewport among them; pr_dead_now: the one being written's) */
static unsigned char pr_dead[WL_XBOX_VSH_MAX_INSTRUCTIONS];
static int pr_dead_now;

/* the constants that may be loaded once a batch, and how often each is loaded in a vertex */
static char pr_keys[PR_KEYS][80];
static int pr_key_uses[PR_KEYS], pr_key_count, pr_hoist_limit, pr_counting;
/* (a load's weight in pr_key_uses: with pr_weighted, 1 in a reused prefix, which most vertices skip,
and 4 elsewhere, so that the loads every vertex makes are hoisted first; pr_program keeps the loop
that costs less, weighted or not. pr_weights_off: never weighted, as before: xbox_vsh_codegen
--no-weights. pr_prefix_first, pr_prefix_end: the reused prefix's ops in pr_body) */
static int pr_use_weight = 4, pr_weighted, pr_weights_off, pr_prefix_first, pr_prefix_end;
/* (fused: loads past the limit hoisted too, each where the registers still allow it: pr_program's) */
static unsigned char pr_hoist_extra[PR_KEYS];
/* (fused: then each load left in the loop hoisted in place of one that is, where the loop costs less
(pr_loop_cost): the registers go to the loads that save the most, not to the most counted. pr_swap_off:
not, as before: xbox_vsh_codegen --no-swap) */
static int pr_swap_off;
static long pr_loop_cost(void);
static long pr_cost(int prefix);
/* (fused, reused: a constant only the part after the prefix reads, loaded at the prefix's end, not
before the loop, where the prefix runs (pr_resident[key]): its register is free from the again
label to there, so the prefix's temporaries borrow it, and kept from there round the loop to the
next prefix, which most vertices skip. pr_region: where the code being written runs (0 the
prologue and the compare, 1 the prefix, 2 the rest); pr_key_regions: each key's (bit n: region n),
from the counting pass. pr_resident_off: each such constant hoisted or loaded at each vertex, as
before: xbox_vsh_codegen --no-resident) */
static unsigned char pr_resident[PR_KEYS], pr_key_regions[PR_KEYS];
static int pr_region, pr_resident_off;
/* (RSQ's estimate for 0, infinity and NaN taken again from |x| / 2, not |x|: a register fewer over its
steps. pr_rsq_off: from |x|, as before: xbox_vsh_codegen --no-rsq-half) */
static int pr_rsq_off;
/* (pr_spill: a half the prefix stores to the slot that no load reads back, once pr_sweep has taken out
the loads nothing reads, left out. pr_dead_spills_off: stored, as before: xbox_vsh_codegen
--no-dead-spills) */
static int pr_dead_spills_off;

static void pr_dead_spills(void) {
    int n, k, kept = 0, at;
    char load[32];

    if (pr_dead_spills_off)
        return;
    for (n = 0; n < pr_body_count; n++) {
        if (strncmp(pr_body[n].text, "psq_st @0, ", 11) != 0 || !strstr(pr_body[n].text, "(%[slot]), 0, 0") ||
            (at = atoi(pr_body[n].text + 11)) < PR_SPILL_AT)
            continue;
        snprintf(load, sizeof(load), ", %d(%%[slot]), 0, 0", at);
        for (k = 0; k < pr_body_count; k++)
            if (strncmp(pr_body[k].text, "psq_l ", 6) == 0 && strstr(pr_body[k].text, load))
                break;
        if (k == pr_body_count)
            pr_body[n].text[0] = 0;
    }
    for (n = 0; n < pr_body_count; n++)
        if (pr_body[n].text[0])
            pr_body[kept++] = pr_body[n];
    pr_body_count = kept;
}
typedef struct {
    int kind, offset;
    char base[16];
} PrKeyLoad;
static PrKeyLoad pr_key_loads[PR_KEYS];

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
    PRK_128 = 56,
    PRK_M150 = 60,
    PRK_M126 = 64,
    PRK_127 = 68,
    PRK_191 = 72,
    PRK_2M64 = 76,    /* 2^-64 */
    PRK_INFINITY = 80,
    PRK_ROUND = 84,   /* 1.5 2^23 */
    PRK_EXP2 = 88,    /* 2^f's polynomial, 6 floats (and 1) */
    PRK_LOG_ZERO = 112, /* -1e30 */
    PRK_NORMAL = 116, /* the least normal float */
    PRK_BIAS = 120,   /* 2^23 + 127 */
    PRK_BIAS_SUB = 124, /* 2^23 + 127 + 23 */
    PRK_SQRT2 = 128,
    PRK_LOG2 = 132,   /* log2's polynomial in t^2, 5 floats */
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
    if ((strcmp(name, "ps_merge00") == 0 || strcmp(name, "ps_merge11") == 0) && a == b) {
        pr_values[d].splat = 1;
        pr_values[d].lane_of = 2 * a + (name[9] == '1') + 1;
    }
    if (strcmp(name, "fadds") == 0 || strcmp(name, "fsubs") == 0 || strcmp(name, "fdivs") == 0 ||
        strcmp(name, "fmadds") == 0 || strcmp(name, "fnmadds") == 0 || strcmp(name, "fmuls") == 0 ||
        strcmp(name, "fnmsubs") == 0)
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
lanes (lfs), 2 a pair whose second lane is 1.0 (psq_l W=1), 3 a double
(lfd); base a GPR's
operand (%[c], %[c2], %[k], %[v], %[ra]); hoistable: the same for the whole
batch (a constant by a fixed index, or %[k]) */
static int pr_fetched_load(int kind, int offset);

/* (a constant that %[k] and WlXboxVshFused both hold, or that is loaded more than one way, as one
key: the float in both lanes from %[k], so a loop holds one register for it. pr_alias_off: each
place its own, as before: xbox_vsh_codegen --no-alias) */
static int pr_alias_off;

static void pr_alias(int *kind, const char **base, int *offset) {
    static const struct {
        int f, kinds, offset, k;
    } aliases[] = {
        /* (%[k]: (0, 0) and 0 in both lanes; 1 at 12 and at 16) */
        {0, 1 << 0, PRK_ZERO, PRK_ZERO}, {0, 1 << 1, PRK_ZERO + 4, PRK_ZERO}, {0, 1 << 1, PRK_W1 + 4, PRK_ONE},
        /* (WlXboxVshFused's pairs and doubles nv2a_gx.c sets to the same values always) */
        {1, (1 << 0) | (1 << 1), (int)offsetof(WlXboxVshFused, pack) + 4 * WL_XBOX_VSH_FUSED_HALF, PRK_HALF},
        {1, (1 << 0) | (1 << 1), (int)offsetof(WlXboxVshFused, pack) + 4 * WL_XBOX_VSH_FUSED_ONES, PRK_ONE},
        {1, 1 << 3, (int)offsetof(WlXboxVshFused, sphere) + 8 * 2, PRK_THREE_HALVES},
        {1, 1 << 3, (int)offsetof(WlXboxVshFused, sphere) + 8 * 3, PRK_HALF},
    };
    size_t n;
    int f = strcmp(*base, "%[f]") == 0;

    if (pr_alias_off || (!f && strcmp(*base, "%[k]") != 0))
        return;
    for (n = 0; n < sizeof(aliases) / sizeof(aliases[0]); n++)
        if (aliases[n].f == f && (aliases[n].kinds & (1 << *kind)) && aliases[n].offset == *offset) {
            *kind = 1;
            *base = "%[k]";
            *offset = aliases[n].k;
            return;
        }
}

static int pr_load(int kind, const char *base, int offset, int hoistable) {
    char key[80];
    int n, value, pre = 0;

    /* (fused: an input the prologue made, from its registers) */
    if ((n = pr_fetched_load(kind, strcmp(base, "%[v]") == 0 ? offset : -1)) >= 0)
        return n;
    pr_alias(&kind, &base, &offset);

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
            pr_key_regions[k] = 0;
            pr_key_loads[k].kind = kind;
            pr_key_loads[k].offset = offset;
            snprintf(pr_key_loads[k].base, sizeof(pr_key_loads[k].base), "%s", base);
        }
        if (k >= 0 && pr_counting) {
            pr_key_uses[k] += pr_use_weight;
            pr_key_regions[k] |= (unsigned char)(1u << pr_region);
        }
        if (k >= 0 && !pr_counting && (k < pr_hoist_limit || pr_hoist_extra[k]))
            pre = 1;
    }
    value = pr_new();
    pr_values[value].fresh = 0;
    if (pre)
        pr_values[value].loop = 1;
    if (kind == 1) {
        pr_op(pre, 1, 1, &value, "lfs @0, %d(%s)", offset, base);
        pr_values[value].splat = 1;
    } else if (kind == 3) {
        pr_op(pre, 1, 1, &value, "lfd @0, %d(%s)", offset, base);
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

/* (pr_resident: the constants loaded at the prefix's end, each kept as a hoisted one is) */
static void pr_resident_loads(void) {
    char key[sizeof(pr_cache[0].key)];
    int k, value;

    if (pr_counting || pr_resident_off)
        return;
    for (k = 0; k < pr_key_count; k++) {
        const char *base = pr_key_loads[k].base;
        int kind = pr_key_loads[k].kind, offset = pr_key_loads[k].offset;

        if (!pr_resident[k])
            continue;
        value = pr_new();
        pr_values[value].fresh = 0;
        pr_values[value].resident = 1;
        if (kind == 1) {
            pr_op(0, 1, 1, &value, "lfs @0, %d(%s)", offset, base);
            pr_values[value].splat = 1;
        } else if (kind == 3) {
            pr_op(0, 1, 1, &value, "lfd @0, %d(%s)", offset, base);
        } else {
            pr_op(0, 1, 1, &value, "psq_l @0, %d(%s), %d, 0", offset, base, kind == 2);
        }
        /* (a copy: a key of the table's, bounded as a cache's) */
        memcpy(key, pr_keys[k], sizeof(key));
        key[sizeof(key) - 1] = 0;
        pr_remember(key, value);
        if (pr_cache_count > 0 && pr_cache[pr_cache_count - 1].value == value) {
            pr_cache[pr_cache_count - 1].needs = ~0u;
            pr_cache[pr_cache_count - 1].hoisted = 1;
        }
    }
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

    if (value <= 0 || pr_values[value].web || pr_values[value].loop || pr_values[value].fetched)
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

/* the cache as it was at mark (a branch's first): what was loaded on one way
alone is not there on the other */
static void pr_cache_trim(int mark) {
    int n, kept = mark;

    for (n = mark; n < pr_cache_count; n++)
        if (pr_cache[n].hoisted)
            pr_cache[kept++] = pr_cache[n];
    pr_cache_count = kept;
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
static int pr_fetched_lanes(int offset, int s0, int s1);
static int pr_fetched_source(int n, int c, int *at);
static unsigned pr_fetched_on;
static int pr_splat_off;

static int pr_memory_pair(const char *base, int offset, int s0, int s1, int hoistable) {
    int a, b;

    if (strcmp(base, "%[v]") == 0 && (a = pr_fetched_lanes(offset, s0, s1)) >= 0)
        return a;
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
    if (operand->mux == _mux_input && pr_fuse && !pr_splat_off && (pr_fetched_on & (1u << instruction->input))) {
        /* (an input the prologue made: the value that has it in ps0, not merged with the next lane) */
        int at, v = pr_fetched_source((int)instruction->input, s, &at);

        if (at == 0) {
            *negate = operand->negate;
            return v;
        }
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
    case _ilu_exp:
    case _ilu_log:
        return 1;
    default:
        return 0;
    }
}

/* (the value whose lane both of v's lanes are, or -1: one no in-place write has written since, its
register its own. pr_splat_off: never, as before: xbox_vsh_codegen --no-splat; and pr_scalar's lane of
an input the prologue made) */

static int pr_lane_source(int v) {
    int a;

    if (v <= 0 || !pr_values[v].lane_of)
        return -1;
    a = (pr_values[v].lane_of - 1) >> 1;
    if (pr_values[a].web || pr_root(a) != a || pr_values[a].loop || pr_values[a].fetched)
        return -1;
    return a;
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
                /* (a pair that is one lane of a value twice, that value not written since: ps_muls/ps_madds
                of the value, the merge then unread: pr_sweep) */
                if (!pr_splat_off && pr_lane_source(pa) >= 0 && pr_lane_source(pb) < 0) {
                    int swap = pa;

                    pa = pb;
                    pb = swap;
                    swap = negate[0];
                    negate[0] = negate[1];
                    negate[1] = swap;
                }
                if (!pr_splat_off && pr_lane_source(pb) >= 0) {
                    splat = (pr_values[pb].lane_of - 1) & 1;
                    pb = pr_lane_source(pb);
                }
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

/* d (a value made before) written again: d = name(a, b, c) */
static void pr_set(int d, const char *name, int a, int b, int c) {
    int values[4] = {d, a, b, c}, count = c >= 0 ? 4 : b >= 0 ? 3 : 2;
    static const char *const formats[3] = {"%s @0, @1", "%s @0, @1, @2", "%s @0, @1, @2, @3"};

    pr_op(0, 1, count, values, formats[count - 2], name);
}

static void pr_compare(int a, int b) {
    int values[2] = {a, b};

    pr_op(0, 0, 2, values, "fcmpu 0, @0, @1");
}

/* a value each way of a branch writes (not a splat: an fmr leaves ps1) */
static int pr_ways_value(void) {
    int d = pr_new();

    pr_values[d].fresh = 0;
    return d;
}

/* d made from the word at 0(%[slot]) in r0: rotated, masked (begin to end), ored with
high << 16, stored at offset and loaded as a float */
static void pr_bits(int d, int rotate, int begin, int end, unsigned high, int offset) {
    pr_text("lwz 0, 0(%%[slot])");
    pr_text("rlwinm 0, 0, %d, %d, %d", rotate, begin, end);
    if (high)
        pr_text("oris 0, 0, %u", high);
    pr_text("stw 0, %d(%%[slot])", offset);
    pr_op(0, 1, 1, &d, "lfs @0, %d(%%[slot])", offset);
}

/* the ILU's floor (vsh.c's ilu_floor): 2^23 added and taken away (by x's sign), 1 less where
that rounded up; from 2^23 on, and a NaN, x */
static int pr_floor(int x) {
    int big = pr_k(1, PRK_2P23), minus = pr_k(1, PRK_M2P23), one = pr_k(1, PRK_ONE),
        zero = pr_k(0, PRK_ZERO), mark = pr_cache_count, a, w = pr_ways_value();
    char same[24], negative[24], check[24], done[24];

    pr_new_label(same, sizeof(same));
    pr_new_label(negative, sizeof(negative));
    pr_new_label(check, sizeof(check));
    pr_new_label(done, sizeof(done));
    a = pr_op3("fabs", x, -1, -1);
    pr_compare(a, big);
    pr_text("bnl %s", same);
    pr_compare(x, zero);
    pr_text("blt %s", negative);
    pr_set(w, "fadds", x, big, -1);
    pr_set(w, "fsubs", w, big, -1);
    pr_text("b %s", check);
    pr_place_label(negative);
    pr_set(w, "fadds", x, minus, -1);
    pr_set(w, "fsubs", w, minus, -1);
    pr_place_label(check);
    pr_compare(w, x);
    pr_text("bng %s", done);
    pr_set(w, "fsubs", w, one, -1);
    pr_text("b %s", done);
    pr_place_label(same);
    pr_set(w, "fmr", x, -1, -1);
    pr_place_label(done);
    pr_cache_trim(mark);
    return w;
}

/* 2^n for a whole n (vsh.c's ilu_power_of_two from 2^-149 to 2^127; below, 0; above, infinity;
a NaN, itself): n + 127 the float's exponent (below 2^-126, n + 191's times 2^-64) */
static int pr_power_of_two(int n) {
    int high = pr_k(1, PRK_128), low = pr_k(1, PRK_M150), least = pr_k(1, PRK_M126),
        bias = pr_k(1, PRK_127), bias_sub = pr_k(1, PRK_191), scale = pr_k(1, PRK_2M64),
        infinity = pr_k(1, PRK_INFINITY), zero = pr_k(0, PRK_ZERO), mark = pr_cache_count,
        r = pr_ways_value(), t = pr_ways_value();
    char above[24], below[24], subnormal[24], made[24], done[24];

    pr_new_label(above, sizeof(above));
    pr_new_label(below, sizeof(below));
    pr_new_label(subnormal, sizeof(subnormal));
    pr_new_label(made, sizeof(made));
    pr_new_label(done, sizeof(done));
    pr_compare(n, high);
    pr_text("bnl %s", above);
    pr_compare(n, low);
    pr_text("bng %s", below);
    pr_compare(n, least);
    pr_text("blt %s", subnormal);
    pr_set(t, "fadds", n, bias, -1);
    pr_text("b %s", made);
    pr_place_label(subnormal);
    pr_set(t, "fadds", n, bias_sub, -1);
    pr_place_label(made);
    pr_set(t, "fctiwz", t, -1, -1);
    pr_op(0, 0, 1, &t, "stfiwx @0, 0, %%[slot]");
    pr_bits(r, 23, 1, 8, 0, 0);
    pr_compare(n, least);
    pr_text("bnl %s", done);
    pr_set(r, "fmuls", r, scale, -1);
    pr_text("b %s", done);
    pr_place_label(above);
    pr_set(r, "fadds", n, infinity, -1);
    pr_text("b %s", done);
    pr_place_label(below);
    pr_set(r, "fmr", zero, -1, -1);
    pr_place_label(done);
    pr_cache_trim(mark);
    return r;
}

/* 2^x (vsh.c's ilu_exp2): x rounded to a whole n, 2^(x - n) by the polynomial, times 2^n; from
128 on infinity (a NaN itself), to -150 0 */
static int pr_exp2(int x) {
    int high = pr_k(1, PRK_128), low = pr_k(1, PRK_M150), round = pr_k(1, PRK_ROUND),
        infinity = pr_k(1, PRK_INFINITY), zero = pr_k(0, PRK_ZERO), one = pr_k(1, PRK_ONE),
        mark = pr_cache_count, r = pr_ways_value(), n, f, p, c;
    char above[24], below[24], done[24];

    pr_new_label(above, sizeof(above));
    pr_new_label(below, sizeof(below));
    pr_new_label(done, sizeof(done));
    pr_compare(x, high);
    pr_text("bnl %s", above);
    pr_compare(x, low);
    pr_text("bng %s", below);
    n = pr_op3("fadds", x, round, -1);
    pr_set(n, "fsubs", n, round, -1);
    f = pr_op3("fsubs", x, n, -1);
    p = pr_k(1, PRK_EXP2);
    for (c = 1; c < 7; c++)
        p = pr_op3("fmadds", p, f, c < 6 ? pr_k(1, PRK_EXP2 + 4 * c) : one);
    pr_set(r, "fmuls", p, pr_power_of_two(n), -1);
    pr_text("b %s", done);
    pr_place_label(above);
    pr_set(r, "fadds", x, infinity, -1);
    pr_text("b %s", done);
    pr_place_label(below);
    pr_set(r, "fmr", zero, -1, -1);
    pr_place_label(done);
    pr_cache_trim(mark);
    return r;
}

/* EXP: (2^floor(x), x - floor(x), 2^x, 1), the lanes needed (bit 3 x) */
static void pr_exp(int x, unsigned needed, PrLane result[4]) {
    int whole = -1, k;

    for (k = 0; k < 4; k++)
        result[k].lane = 0;
    if (needed & 0xc)
        whole = pr_floor(x);
    if (needed & 8)
        result[0].value = pr_power_of_two(whole);
    if (needed & 4)
        result[1].value = pr_op3("fsubs", x, whole, -1);
    if (needed & 2)
        result[2].value = pr_exp2(x);
    result[3].value = pr_k(1, PRK_ONE);
}

/* LOG of |x| (vsh.c's ilu_log2): (its exponent e, its mantissa m = |x| / 2^e in [1, 2), log2 |x|,
1) from its bits, a subnormal's times 2^23; 0: (-1e30, 1, -1e30, 1); infinity and a NaN: (|x|,
NaN, |x|, 1) */
static void pr_log(int x, PrLane result[4]) {
    int zero = pr_k(0, PRK_ZERO), one = pr_k(1, PRK_ONE), infinity = pr_k(1, PRK_INFINITY),
        normal = pr_k(1, PRK_NORMAL), big = pr_k(1, PRK_2P23), bias = pr_k(1, PRK_BIAS),
        bias_sub = pr_k(1, PRK_BIAS_SUB), root = pr_k(1, PRK_SQRT2), half = pr_k(1, PRK_HALF),
        nothing = pr_k(1, PRK_LOG_ZERO), mark = pr_cache_count, a, s = pr_ways_value(),
        offset = pr_ways_value(), e = pr_ways_value(), m = pr_ways_value(), l = pr_ways_value(),
        mm = pr_ways_value(), ee = pr_ways_value(), t, t2, p, c, k;
    char special[24], scaled[24], split[24], kept[24], finite[24], done[24];

    pr_new_label(special, sizeof(special));
    pr_new_label(scaled, sizeof(scaled));
    pr_new_label(split, sizeof(split));
    pr_new_label(kept, sizeof(kept));
    pr_new_label(finite, sizeof(finite));
    pr_new_label(done, sizeof(done));
    a = pr_op3("fabs", x, -1, -1);
    pr_compare(a, zero);
    pr_text("beq %s", special);
    pr_compare(a, infinity);
    pr_text("bnl %s", special);
    pr_compare(a, normal);
    pr_text("blt %s", scaled);
    pr_set(s, "fmr", a, -1, -1);
    pr_set(offset, "fmr", bias, -1, -1);
    pr_text("b %s", split);
    pr_place_label(scaled);
    pr_set(s, "fmuls", a, big, -1);
    pr_set(offset, "fmr", bias_sub, -1, -1);
    pr_place_label(split);
    /* (the mantissa's bits with 127's exponent at 4(%[slot]), then the exponent's as 2^23 + it) */
    pr_op(0, 0, 1, &s, "stfs @0, 0(%%[slot])");
    pr_bits(m, 0, 9, 31, 0x3f80, 4);
    pr_bits(e, 9, 24, 31, 0x4b00, 0);
    pr_set(e, "fsubs", e, offset, -1);
    pr_compare(m, root);
    pr_text("blt %s", kept);
    pr_set(mm, "fmuls", m, half, -1);
    pr_set(ee, "fadds", e, one, -1);
    pr_text("b %s", finite);
    pr_place_label(kept);
    pr_set(mm, "fmr", m, -1, -1);
    pr_set(ee, "fmr", e, -1, -1);
    pr_place_label(finite);
    t = pr_op3("fsubs", mm, one, -1);
    t = pr_op3("fdivs", t, pr_op3("fadds", mm, one, -1), -1);
    t2 = pr_op3("fmuls", t, t, -1);
    p = pr_k(1, PRK_LOG2);
    for (c = 1; c < 5; c++)
        p = pr_op3("fmadds", p, t2, pr_k(1, PRK_LOG2 + 4 * c));
    pr_set(l, "fmadds", t, p, ee);
    pr_text("b %s", done);
    /* (0: -1e30, 1, -1e30; infinity or a NaN: a, a - a, a) */
    pr_place_label(special);
    pr_compare(a, zero);
    pr_set(e, "fmr", a, -1, -1);
    pr_set(m, "fsubs", a, a, -1);
    pr_set(l, "fmr", a, -1, -1);
    pr_text("bne %s", done);
    pr_set(e, "fmr", nothing, -1, -1);
    pr_set(m, "fmr", one, -1, -1);
    pr_set(l, "fmr", nothing, -1, -1);
    pr_place_label(done);
    pr_cache_trim(mark);
    for (k = 0; k < 4; k++)
        result[k].lane = 0;
    result[0].value = e;
    result[1].value = m;
    result[2].value = l;
    result[3].value = one;
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
    if (in->ilu == _ilu_exp) {
        pr_exp(x, needed, result);
        return;
    }
    if (in->ilu == _ilu_log) {
        pr_log(x, result);
        return;
    }
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
        /* RSQ: 1 / sqrt(|x|) in single precision: frsqrte's estimate (Broadway's
        table, within 1/2^12.4) and two Newton's steps, within two ulps of the
        C's 1.0f / sqrtf (no division: frames the same on the captures); 0,
        infinity and NaN, where the steps make a NaN, take the estimate (exact
        for them) */
        int a, e, h, t, step, halves = pr_k(1, PRK_HALF),
                              three_halves = pr_k(1, PRK_THREE_HALVES), values[3];
        char done[24];

        a = pr_op3("fabs", x, -1, -1);
        e = pr_op3("frsqrte", a, -1, -1);
        h = pr_op3("fmuls", a, halves, -1);
        for (step = 0; step < 2; step++) {
            t = pr_op3("fmuls", e, e, -1);
            t = pr_op3("fnmsubs", h, t, three_halves);
            e = pr_op3("fmuls", e, t, -1);
        }
        s = e;
        pr_new_label(done, sizeof(done));
        values[0] = s;
        pr_op(0, 0, 1, values, "fcmpu 0, @0, @0");
        pr_text("bnu %s", done);
        /* (from |x| / 2, which is 0, infinity or NaN where |x| is, so that |x| is not held over the
        steps; but the least denormal, which halves to 0) */
        values[1] = pr_rsq_off ? a : h;
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
    pr_values[d].lane_of = pr_values[value].lane_of;
    pr_adopt(d);
    return d;
}

/* ---- (fused) the output's scratch forwarded ----
A fused batch's output scratch (%[o]) is read by its epilogue alone: a value
the loop stores there is taken from its register while that still holds it
(pr_fused_output), and a store nothing loads back is left out
(pr_forward_finish). */
enum { PR_FORWARDS = 64 };

typedef struct {
    int offset, size; /* bytes in WlXboxVshOutput */
    int value;        /* lane 0 (and 1 for 8 bytes) of it; -1: partly written over since */
    int op;           /* the store, in pr_body */
    int covered;      /* written over whole: the store is dead */
} PrForward;

static PrForward pr_forwards[PR_FORWARDS];
static int pr_forward_count, pr_forward_on;
static unsigned char pr_forward_read[sizeof(WlXboxVshOutput)];

static void pr_forward_reset(void) {
    pr_forward_count = pr_forward_on = 0;
    memset(pr_forward_read, 0, sizeof(pr_forward_read));
}

/* an output field's operand ("%[od0]", "%[ot]+16", ...; NULL: the position) as its offset */
static int pr_field_offset(const char *field) {
    static const struct {
        const char *name;
        int offset;
    } fields[] = {
        {"%[od0]", (int)offsetof(WlXboxVshOutput, d0)}, {"%[od1]", (int)offsetof(WlXboxVshOutput, d1)},
        {"%[ob0]", (int)offsetof(WlXboxVshOutput, b0)}, {"%[ob1]", (int)offsetof(WlXboxVshOutput, b1)},
        {"%[ot]", (int)offsetof(WlXboxVshOutput, t)},   {"%[oclip]", (int)offsetof(WlXboxVshOutput, clip)},
    };
    size_t n;

    if (field == NULL)
        return (int)offsetof(WlXboxVshOutput, position);
    for (n = 0; n < sizeof(fields) / sizeof(fields[0]); n++) {
        size_t length = strlen(fields[n].name);

        if (strncmp(field, fields[n].name, length) == 0)
            return fields[n].offset + (field[length] == '+' ? atoi(field + length + 1) : 0);
    }
    pr_failed = 1;
    return 0;
}

/* the store just written (the last op): size bytes at offset from value's lanes */
static void pr_forward_store(int offset, int size, int value) {
    int n;

    if (!pr_fuse || !pr_forward_on)
        return;
    for (n = 0; n < pr_forward_count; n++) {
        PrForward *f = &pr_forwards[n];

        if (f->offset >= offset && f->offset + f->size <= offset + size)
            f->covered = 1;
        else if (f->offset < offset + size && offset < f->offset + f->size)
            f->value = -1;
    }
    if (pr_forward_count >= PR_FORWARDS) {
        pr_failed = 1;
        return;
    }
    pr_forwards[pr_forward_count].offset = offset;
    pr_forwards[pr_forward_count].size = size;
    pr_forwards[pr_forward_count].value = value;
    pr_forwards[pr_forward_count].op = pr_body_count - 1;
    pr_forwards[pr_forward_count].covered = 0;
    pr_forward_count++;
}

/* the value the loop stored as size bytes at offset, still in its register; -1 when none */
static int pr_forward_find(int offset, int size) {
    int n, k, at;

    for (n = pr_forward_count - 1; n >= 0; n--) {
        const PrForward *f = &pr_forwards[n];

        if (f->offset != offset || f->size != size || f->value < 0 || f->covered)
            continue;
        /* (not when its register is written again later: a temporary's web) */
        for (at = f->op + 1; at < pr_body_count; at++)
            for (k = 0; k < pr_body[at].count; k++)
                if ((pr_body[at].defines & (1u << k)) && pr_root(pr_body[at].values[k]) == pr_root(f->value))
                    return -1;
        return f->value;
    }
    return -1;
}

/* the stores no load reads back left out (once the epilogue is written) */
static void pr_forward_finish(void) {
    int n, kept = 0, at;

    for (n = 0; n < pr_forward_count; n++) {
        const PrForward *f = &pr_forwards[n];
        int read = 0, b;

        for (b = f->offset; b < f->offset + f->size; b++)
            read |= pr_forward_read[b];
        if (f->covered || !read)
            pr_body[f->op].text[0] = 0;
    }
    for (at = 0; at < pr_body_count; at++)
        if (pr_body[at].text[0])
            pr_body[kept++] = pr_body[at];
    pr_body_count = kept;
    pr_forward_count = 0;
}

/* the result's lanes written to the output at offset (mask bit 3 x) */
static void pr_store(const PrLane result[4], unsigned mask, const char *field) {
    int half, at = pr_fuse && pr_forward_on ? pr_field_offset(field) : 0;

    for (half = 0; half < 2; half++) {
        int l0 = 2 * half, l1 = l0 + 1, values[1];

        if ((mask & (8u >> l0)) && (mask & (8u >> l1))) {
            values[0] = pr_merged(result[l0], result[l1]);
            pr_op(0, 0, 1, values, "psq_st @0, %s+%d(%%[o]), 0, 0", field, 8 * half);
            pr_forward_store(at + 8 * half, 8, values[0]);
            continue;
        }
        if (mask & (8u >> l0)) {
            values[0] = pr_lane0(result[l0]);
            pr_op(0, 0, 1, values, "stfs @0, %s+%d(%%[o])", field, 8 * half);
            pr_forward_store(at + 8 * half, 4, values[0]);
        }
        if (mask & (8u >> l1)) {
            values[0] = pr_lane0(result[l1]);
            pr_op(0, 0, 1, values, "stfs @0, %s+%d(%%[o])", field, 8 * half + 4);
            pr_forward_store(at + 8 * half + 4, 4, values[0]);
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

/* the output an address names, as a draw reads it (WL_XBOX_VSH_LIVE_...) */
static unsigned pr_output_live(unsigned address) {
    switch (address) {
    case _output_position:
        return WL_XBOX_VSH_LIVE_POSITION;
    case _output_d0:
        return WL_XBOX_VSH_LIVE_D0;
    case _output_d1:
        return WL_XBOX_VSH_LIVE_D1;
    case _output_b0:
    case _output_b1:
        return WL_XBOX_VSH_LIVE_BACK;
    case _output_fog:
        return WL_XBOX_VSH_LIVE_FOG;
    case _output_point_size:
        return WL_XBOX_VSH_LIVE_POINT_SIZE;
    default:
        return address >= _output_t0 && address < _output_t0 + 4 ? WL_XBOX_VSH_LIVE_T0 << (address - _output_t0)
                                                                 : WL_XBOX_VSH_LIVE_ALL;
    }
}

/* one instruction (mode: WRITE_TEMPORARIES, WRITE_OUTPUTS or both) */
static void pr_instruction(const WlXboxVshInstruction *in, int mode) {
    /* (an instruction pr_liveness found dead writes nothing: its clip-space capture alone) */
    int mac_t = !pr_dead_now && in->mac != _mac_nop && (in->mac == _mac_arl || in->mac_mask) &&
                (mode & WRITE_TEMPORARIES);
    int ilu_t = !pr_dead_now && in->ilu != _ilu_nop && in->ilu_mask && (mode & WRITE_TEMPORARIES);
    int out = !pr_dead_now && in->output_mask && in->output_is_register &&
              (in->output_from_ilu ? in->ilu : in->mac) != 0 && (mode & WRITE_OUTPUTS);
    int dot = -1, k, mac_temporary = pr_temporary_index(in->temporary);
    int ilu_temporary = pr_temporary_index(in->mac != _mac_nop ? 1 : in->temporary);
    /* (a unit's lanes of its temporary that nothing reads after: not made, as when the other unit's
    output alone keeps the instruction) */
    unsigned mac_mask = in->mac == _mac_arl ? in->mac_mask : in->mac_mask & pr_live_after[mac_temporary];
    unsigned ilu_mask = in->ilu_mask & pr_live_after[ilu_temporary];
    unsigned mac_lanes, ilu_lanes;
    PrLane mac[4], ilu[4];
    char text[24];

    /* (fused: an output the form does not read not made, as the other unit's work keeps the
    instruction: the position a draw whose position is the transform unit's co-issues with an RSQ; a "w"
    form's clip-space w is made, the rest of it dead: pr_liveness) */
    if (pr_fuse && out && !(pr_fuse->live & pr_output_live(in->output_address)) &&
        !(pr_fuse->perspective == WL_XBOX_VSH_FUSED_WORLD && in->output_address == _output_position))
        out = 0;

    if (in->mac != _mac_arl && !mac_mask)
        mac_t = 0;
    if (!ilu_mask)
        ilu_t = 0;
    mac_lanes = (mac_t ? mac_mask : 0) | (out && !in->output_from_ilu ? in->output_mask : 0);
    ilu_lanes = (ilu_t ? ilu_mask : 0) | (out && in->output_from_ilu ? in->output_mask : 0);

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
    if (in->captures_clip && (mode & WRITE_OUTPUTS) && pr_fuse && pr_fuse->perspective == WL_XBOX_VSH_FUSED_WORLD) {
        /* (the clip-space w, the processor's as for every other form, for GX's depth to be the same
        to the bit: it is sent in place of the world position's component the matrix recovers) */
        int values[1];

        pr_world_w = pr_fused_op("ps_merge11", pr_half(12, 1), pr_half(12, 1), -1);
        if (pr_world_spill) {
            const char *to = pr_early ? "%[ad]" : "%[o]";

            values[0] = pr_half(pr_program_world, 0);
            pr_op(0, 0, 1, values, "psq_st @0, 0(%s), 0, 0", to);
            values[0] = pr_half(pr_program_world, 1);
            pr_op(0, 0, 1, values, "stfs @0, 8(%s)", to);
            pr_fused_store(pr_world_w, "stfsx @0, %s, %%[ws]", to);
        }
    }
    if (in->captures_clip && (mode & WRITE_OUTPUTS) && !(pr_fuse && pr_fuse->perspective == WL_XBOX_VSH_FUSED_WORLD)) {
        PrLane p[4];

        for (k = 0; k < 4; k++) {
            p[k].value = pr_half(12, k >> 1);
            p[k].lane = k & 1;
        }
        pr_store(p, 0xf, "%[oclip]");
        if (pr_fuse) {
            pr_fused_captured = 1;
        } else {
            pr_text("li 0, 1");
            pr_text("stw 0, %%[ocaptured](%%[o])");
        }
    }
    /* (a dot product's lanes: written in place when one ln of a
    temporary is all it writes there) */
    if (dot >= 0) {
        unsigned temporary_lanes = mac_t ? mac_mask : 0;
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
        /* (floor: fctiwz's own for y at least 0, a NaN, or of 2^23 and more; else y rounded to
        a whole number by - 2^23 + 2^23, less 1 when that is above y) */
        y = pr_op3("fadds", mac[0].value, pr_k(1, PRK_ARL), -1);
        values[0] = y;
        values[1] = pr_k(0, PRK_ZERO);
        pr_op(0, 0, 2, values, "fcmpu 0, @0, @1");
        pr_text("bnl %s", slow);
        z = pr_op3("fabs", y, -1, -1);
        values[0] = z;
        values[1] = big;
        pr_op(0, 0, 2, values, "fcmpu 0, @0, @1");
        pr_text("bnl %s", slow);
        w = minus;
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
        pr_commit(mac_temporary, mac, mac_mask);
    if (ilu_t)
        pr_commit(ilu_temporary, ilu, ilu_mask);
    if (out && in->output_address == _output_position)
        pr_commit(12, in->output_from_ilu ? ilu : mac, in->output_mask);
}

/* the temporaries' lanes read after each instruction (bit 3 x), the
position's all read at the end; instructions no output needs left out.
drop_position (fused, GX taking the clip-space position the program captures):
the position read by none at the end, and an instruction whose writes are then
all unread is dead (pr_dead), reading nothing but for its capture; of the
capture, x, y and w alone (GX's vertex is x w, y w, -w: the clip-space z is
never sent); 2 (a "w" form: GX's position matrix makes it from the world
position sent), of the capture w alone */
static void pr_liveness(const WlXboxVshProgram *program, const unsigned *needs, unsigned long from,
                        unsigned (*after)[13], int drop_position) {
    unsigned live[13];
    long index;
    int t, which;

    memset(live, 0, sizeof(live));
    live[12] = drop_position ? 0 : 0xf;
    /* (a lit form sends the normal: its x, y and z read as the program ends) */
    if (pr_fuse && pr_fuse->lit && program->light.count)
        live[pr_temporary_index(program->light.normal)] |= 0xe;
    /* (a "w" form sends the world position: its x, y and z read as the program ends, or where the
    clip-space position is captured: pr_world_spill) */
    if (pr_fuse && pr_fuse->perspective == WL_XBOX_VSH_FUSED_WORLD && program->world.found && !pr_world_spill)
        live[pr_temporary_index(program->world.temporary)] |= 0xe;
    for (index = (long)program->count - 1; index >= (long)from; index--) {
        const WlXboxVshInstruction *in = &program->instructions[index];
        WlXboxVshInstruction made;
        unsigned reads[3];

        memcpy(after[index], live, sizeof(live));
        pr_dead[index] = 0;
        if (!needs[index])
            continue;
        if (drop_position && in->mac != _mac_arl && !(in->output_mask && !in->output_is_register)) {
            int out = in->output_mask && in->output_is_register && (in->output_from_ilu ? in->ilu : in->mac) != 0;
            unsigned mac_live = in->mac != _mac_nop && in->mac_mask
                                    ? live[pr_temporary_index(in->temporary)] & in->mac_mask : 0;
            unsigned ilu_live = in->ilu != _ilu_nop && in->ilu_mask
                                    ? live[pr_temporary_index(in->mac != _mac_nop ? 1 : in->temporary)] & in->ilu_mask
                                    : 0;
            unsigned out_live = out ? (in->output_address == _output_position ? live[12] & in->output_mask : 1u) : 0;

            if (!mac_live && !ilu_live && !out_live) {
                pr_dead[index] = 1;
                if (in->captures_clip)
                    live[12] = drop_position == 2 ? 0x1 : 0xd;
                if (in->captures_clip && drop_position == 2 && pr_world_spill)
                    live[pr_temporary_index(program->world.temporary)] |= 0xe;
                continue;
            }
        }
        /* (the operands' lanes the live lanes read: a unit's lanes nothing reads after are not
        made, pr_instruction's) */
        made = *in;
        /* (fused: an output the form does not read reads nothing, pr_instruction leaving it out: the
        temporaries it alone reads not made) */
        if (pr_fuse && made.output_mask && made.output_is_register && made.output_address != _output_position &&
            !(pr_fuse->live & pr_output_live(made.output_address)))
            made.output_mask = 0;
        if (made.mac != _mac_nop && made.mac != _mac_arl)
            made.mac_mask = (unsigned char)(made.mac_mask & live[pr_temporary_index(in->temporary)]);
        if (made.ilu != _ilu_nop)
            made.ilu_mask =
                (unsigned char)(made.ilu_mask & live[pr_temporary_index(in->mac != _mac_nop ? 1 : in->temporary)]);
        if (in->mac != _mac_nop && in->mac != _mac_arl && in->mac_mask)
            live[pr_temporary_index(in->temporary)] &= ~in->mac_mask;
        if (in->ilu != _ilu_nop && in->ilu_mask)
            live[pr_temporary_index(in->mac != _mac_nop ? 1 : in->temporary)] &= ~in->ilu_mask;
        if (in->output_mask && in->output_is_register && (in->output_from_ilu ? in->ilu : in->mac) != 0 &&
            in->output_address == _output_position)
            live[12] &= ~in->output_mask;
        if (in->captures_clip)
            live[12] = drop_position == 2 ? 0x1 : drop_position ? 0xd : 0xf;
        if (in->captures_clip && drop_position == 2 && pr_world_spill)
            live[pr_temporary_index(program->world.temporary)] |= 0xe;
        pr_reads(&made, WRITE_ALL, reads);
        for (which = 0; which < 3; which++)
            if (in->operands[which].mux == _mux_temporary)
                live[pr_temporary_index(in->operands[which].index)] |= reads[which];
    }
    (void)t;
}

/* (each output's components the instructions write, bit 3 x: pr_defaults') */
static unsigned pr_written[8];
/* (fused: the default stores of the outputs' fields, each its op in pr_body and its bytes in
WlXboxVshOutput, so that a store the epilogue never loads back is left out (pr_defaults_finish);
group: the "li 0"/"lis 0" op the stw's word came from, -1 for a psq_st. pr_defaults_off: every one
stored at each call, as before: xbox_vsh_codegen --no-defaults) */
enum { PR_DEFAULTS = 48 };
static struct {
    int op, offset, size, group;
} pr_default_ops[PR_DEFAULTS];
static int pr_default_count, pr_defaults_off;

static void pr_default_op(const char *field, int component, int size, int group) {
    if (!pr_fuse || pr_default_count >= PR_DEFAULTS)
        return;
    pr_default_ops[pr_default_count].op = pr_body_count - 1;
    pr_default_ops[pr_default_count].offset = pr_field_offset(field) + 4 * component;
    pr_default_ops[pr_default_count].size = size;
    pr_default_ops[pr_default_count].group = group;
    pr_default_count++;
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
    memcpy(pr_written, written, sizeof(pr_written));
    pr_default_count = 0;
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
                    pr_default_op(names[which], component, 8, -1);
                    written[which] |= 0xcu >> component;
                }
        if (!captured) {
            values[0] = pairs[0];
            pr_op(0, 0, 1, values, "psq_st @0, %%[oclip](%%[o]), 0, 0");
            pr_op(0, 0, 1, values, "psq_st @0, %%[oclip]+8(%%[o]), 0, 0");
        }
    }
    for (one = 0; one < 2; one++) {
        int first = 1, group = -1;

        for (which = 0; which < 8 + 2; which++)
            for (component = 0; component < 4; component++) {
                int wanted = which < 8 ? !(written[which] & (8u >> component)) && (component == 3) == one
                                       : which == 8 ? one && !fog && component == 0
                                                    : !one && !captured && component == 0;

                if (!wanted)
                    continue;
                if (first) {
                    pr_text(one ? "lis 0, 0x3f80" : "li 0, 0");
                    group = pr_body_count - 1;
                }
                first = 0;
                if (which < 8) {
                    pr_text("stw 0, %s+%d(%%[o])", names[which], 4 * component);
                    pr_default_op(names[which], component, 4, group);
                } else
                    pr_text("stw 0, %s(%%[o])", which == 8 ? "%[ofog]" : "%[ocaptured]");
            }
    }
}

/* (fused, once the epilogue is written: the default stores whose bytes it never loads left out, and
a group's "li 0" or "lis 0" with its last stw; before pr_forward_finish, which takes the ops out) */
static void pr_defaults_finish(void) {
    int n, b, g;

    if (pr_defaults_off)
        return;
    for (n = 0; n < pr_default_count; n++) {
        int read = 0;

        for (b = pr_default_ops[n].offset; b < pr_default_ops[n].offset + pr_default_ops[n].size; b++)
            read |= b >= (int)sizeof(pr_forward_read) || pr_forward_read[b];
        if (!read)
            pr_body[pr_default_ops[n].op].text[0] = 0;
    }
    for (n = 0; n < pr_default_count; n++) {
        int kept = 0;

        if ((g = pr_default_ops[n].group) < 0 || !pr_body[g].text[0])
            continue;
        for (b = 0; b < pr_default_count; b++)
            kept |= pr_default_ops[b].group == g && pr_body[pr_default_ops[b].op].text[0];
        if (!kept)
            pr_body[g].text[0] = 0;
    }
    pr_default_count = 0;
}

/* ---- fused batches (WlXboxVshFusedProc) ----
The same loop for one form of the arrays and of GX's vertex (pr_fuse): each
vertex's inputs read from its arrays into the inputs' scratch (%[v]; the
vertex before's at %[pv]), the program run into the output's scratch (%[o]),
and GX's words written from it where %[w] says, as nv2a_gx.c's vertices_pack
writes them. Little-endian words and shorts through the byte-reversed loads;
bytes and shorts made floats by the quantized loads (GQR6: u8, GQR7: s16,
saved and restored), a colour's bytes by the quantized store (u8: below 0 0,
above 255 255, else cut down: wl_nv2a_color_byte's of value * 255 + 0.5; a NaN made 0 by a
ps_sel first, the store's own NaN being 255 in Dolphin).
The state's floats are at the same offsets everywhere (before its pointers);
its pointers at %[fdata] + %[ptr] * n. */

/* the components an array of the format gives (the others keep their start
values, the same in every vertex) */
static int pr_fused_components(unsigned format) {
    if (format >= WL_XBOX_VSH_FUSED_F1 && format <= WL_XBOX_VSH_FUSED_F4)
        return (int)(format - WL_XBOX_VSH_FUSED_F1) + 1;
    if (format == WL_XBOX_VSH_FUSED_D3DCOLOR)
        return 4;
    if (format == WL_XBOX_VSH_FUSED_CMP)
        return 3;
    if (format >= WL_XBOX_VSH_FUSED_S1 && format < WL_XBOX_VSH_FUSED_UB)
        return (int)(format - WL_XBOX_VSH_FUSED_S1) + 1;
    if (format >= WL_XBOX_VSH_FUSED_UB && format < WL_XBOX_VSH_FUSED_S32K)
        return (int)(format - WL_XBOX_VSH_FUSED_UB) + 1;
    if (format >= WL_XBOX_VSH_FUSED_S32K && format < WL_XBOX_VSH_FUSED_FORMATS)
        return (int)(format - WL_XBOX_VSH_FUSED_S32K) + 1;
    return 0;
}

/* an array of packed normals (the fetch's tables in %[t11], %[t10]) */
static int pr_fused_cmp(void) {
    int n;

    for (n = 0; n < 16; n++)
        if (pr_fuse->format[n] == WL_XBOX_VSH_FUSED_CMP)
            return 1;
    return 0;
}

/* the arrays whose element 0 and stride are operands (bit n: v[n]'s): as many as GCC's 30
operands allow past the others (a "+" operand counts twice; %[c2] and %[fdata] counted when
they may be needed) */
static unsigned pr_fused_held;

static void pr_fused_operands(const WlXboxVshProgram *program, const unsigned *needs, int reused) {
    int left = 30 - (2 + 3 + 2 + 2 + 1 + (reused ? 2 : 0)) - 6 - 2 * pr_fused_cmp(), all = 0, n, which;
    unsigned long index;

    /* (%[c2]: a row from 127 on read by its number, as pr_constant_row reads it) */
    for (index = 0; index < program->count; index++) {
        const WlXboxVshInstruction *in = &program->instructions[index];

        for (which = 0; which < 3; which++)
            if (needs[index] && in->operands[which].mux == _mux_constant && !in->relative && in->constant >= 127) {
                left--;
                index = program->count - 1;
                break;
            }
    }
    pr_fused_held = 0;
    for (n = 0; n < 16; n++)
        all += 2 * (pr_fuse->format[n] != WL_XBOX_VSH_FUSED_NONE);
    if (all > left)
        left--;
    for (n = 0; n < 16; n++)
        if (pr_fuse->format[n] != WL_XBOX_VSH_FUSED_NONE && left >= 2) {
            pr_fused_held |= 1u << n;
            left -= 2;
        }
}

/* (fused, the prefix kept) the vertex's own bytes of the inputs the prefix
reads, when they are at most 4 (a skinned mesh's node indices, two bytes, and
its weight, a short): copied to the word at 8(%[slot]) as they are read, and
that word compared with the vertex before's at 12(%[slot]) in place of the
inputs made floats; the floats are then made only where the prefix is made,
from the copies. Bytes that are the same give the same floats, so the prefix
is kept for the same vertices or more (two shorts that both make -1 differ),
and made again where it would give the same. Only when no instruction past
the prefix, nor an output of it, reads those inputs (they are left as they
were in a vertex whose prefix is kept). pr_raw_at[n]: v[n]'s bytes in the
word, -1: not copied */
static int pr_raw_on, pr_raw_at[16];

static int pr_fused_new(const char *format, ...);
static int pr_fused_op(const char *name, int a, int b, int c);
static void pr_fused_store(int value, const char *format, ...);
static int pr_fused_pack(int kind, int index);

static int pr_raw_bytes(unsigned format) {
    if (format == WL_XBOX_VSH_FUSED_UB || format == WL_XBOX_VSH_FUSED_UB + 1 || format == WL_XBOX_VSH_FUSED_UB + 3)
        return (int)(format - WL_XBOX_VSH_FUSED_UB) + 1;
    if (format == WL_XBOX_VSH_FUSED_S1 || format == WL_XBOX_VSH_FUSED_S1 + 1)
        return 2 * ((int)(format - WL_XBOX_VSH_FUSED_S1) + 1);
    return 0;
}

static void pr_raw_plan(const WlXboxVshProgram *program, const unsigned *needs, const ReusePrefix *prefix,
                        const unsigned *words) {
    unsigned long index;
    int n, at = 0, which;

    pr_raw_on = 0;
    for (n = 0; n < 16; n++) {
        pr_raw_at[n] = -1;
        if (!words[n])
            continue;
        if (!pr_raw_bytes(pr_fuse->format[n]) || at + pr_raw_bytes(pr_fuse->format[n]) > 4)
            return;
        pr_raw_at[n] = at;
        at += pr_raw_bytes(pr_fuse->format[n]);
    }
    if (!at)
        return;
    for (index = 0; index < program->count; index++) {
        const WlXboxVshInstruction *in = &program->instructions[index];
        unsigned reads[3];

        if (!needs[index])
            continue;
        pr_reads(in, index < prefix->end ? WRITE_OUTPUTS : WRITE_ALL, reads);
        for (which = 0; which < 3; which++)
            if (reads[which] && in->operands[which].mux == _mux_input && pr_raw_at[in->input & 15] >= 0)
                return;
    }
    pr_raw_on = 1;
}

/* (in the prefix's way) the copied inputs made floats from their bytes */
static void pr_raw_floats(void) {
    int n, c;

    for (n = 0; n < 16; n++) {
        unsigned format = pr_fuse->format[n];
        int size = pr_fused_components(format);

        if (pr_raw_at[n] < 0)
            continue;
        for (c = 0; c < size; c += 2) {
            int pair = c + 1 < size, value, result;

            if (format >= WL_XBOX_VSH_FUSED_UB) {
                value = pr_fused_new("psq_l @0, %d(%%[slot]), %d, 6", 8 + pr_raw_at[n] + c, !pair);
                result = pr_fused_op("ps_mul", value, pr_fused_pack(0, WL_XBOX_VSH_FUSED_BYTE), -1);
            } else {
                int product;

                value = pr_fused_new("psq_l @0, %d(%%[slot]), %d, 7", 8 + pr_raw_at[n] + 2 * c, !pair);
                product = pr_fused_op("ps_mul", value, pr_fused_pack(0, WL_XBOX_VSH_FUSED_SHORT), -1);
                result = pr_fused_op("ps_sel", pr_fused_op("ps_add", product, pr_fused_pack(0, WL_XBOX_VSH_FUSED_ONES), -1),
                                     product, pr_fused_pack(0, WL_XBOX_VSH_FUSED_NEGATIVE));
            }
            if (pair)
                pr_fused_store(result, "psq_st @0, %d(%%[v]), 0, 0", 16 * n + 4 * c);
            else
                pr_fused_store(result, "stfs @0, %d(%%[v])", 16 * n + 4 * c);
        }
    }
}

/* %[fdata] needed: an array not held */
static int pr_fused_fdata(void) {
    int n;

    for (n = 0; n < 16; n++)
        if (pr_fuse->format[n] != WL_XBOX_VSH_FUSED_NONE && !(pr_fused_held & (1u << n)))
            return 1;
    return 0;
}

/* the floats and doubles of WlXboxVshFused */
static int pr_fused_pack(int kind, int index) {
    return pr_load(kind, "%[f]", (int)offsetof(WlXboxVshFused, pack) + 4 * index, 1);
}

static int pr_fused_sphere(int index) {
    return pr_load(3, "%[f]", (int)offsetof(WlXboxVshFused, sphere) + 8 * index, 1);
}

/* (before the loop: the GQRs; the outputs no instruction writes, once) */
static void pr_fused_start(const WlXboxVshProgram *program, const unsigned *needs, int reused) {
    pr_fused_operands(program, needs, reused);
    pr_op(1, 0, 0, NULL, "mfspr 0, 918");
    pr_op(1, 0, 0, NULL, "stw 0, %d(%%[f])", (int)offsetof(WlXboxVshFused, saved));
    pr_op(1, 0, 0, NULL, "mfspr 0, 919");
    pr_op(1, 0, 0, NULL, "stw 0, %d(%%[f])", (int)offsetof(WlXboxVshFused, saved) + 4);
    pr_op(1, 0, 0, NULL, "lis 0, 4");
    pr_op(1, 0, 0, NULL, "ori 0, 0, 4");
    pr_op(1, 0, 0, NULL, "mtspr 918, 0");
    pr_op(1, 0, 0, NULL, "lis 0, 7");
    pr_op(1, 0, 0, NULL, "ori 0, 0, 7");
    pr_op(1, 0, 0, NULL, "mtspr 919, 0");
    pr_op(1, 0, 0, NULL, "isync");
    pr_defaults(program);
}

static void pr_fused_end(void) {
    pr_text("lwz 0, %d(%%[f])", (int)offsetof(WlXboxVshFused, saved));
    pr_text("mtspr 918, 0");
    pr_text("lwz 0, %d(%%[f])", (int)offsetof(WlXboxVshFused, saved) + 4);
    pr_text("mtspr 919, 0");
    pr_text("isync");
}

/* a new value written by an op of one value */
static int pr_fused_new(const char *format, ...) {
    va_list arguments;
    char text[96];
    int value = pr_new();

    va_start(arguments, format);
    vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    pr_values[value].fresh = 0;
    pr_op(0, 1, 1, &value, "%s", text);
    return value;
}

/* (fused) whether value is %[k]'s 0 in both lanes (a load of it, as a pair or in both lanes) */
static int pr_fold_off;

static int pr_is_zero(int value) {
    int n;

    if (pr_fold_off)
        return 0;
    for (n = 0; n < pr_cache_count; n++)
        if (pr_cache[n].value == value && (strcmp(pr_cache[n].key, "%[k] 0 0") == 0 || strcmp(pr_cache[n].key, "%[k] 1 0") == 0))
            return 1;
    return 0;
}

/* d = op(a, b[, c]) */
static int pr_fused_op(const char *name, int a, int b, int c) {
    int values[4] = {pr_new(), a, b, c};

    pr_values[values[0]].fresh = 0;
    pr_op(0, 1, c >= 0 ? 4 : b >= 0 ? 3 : 2, values, c >= 0 ? "%s @0, @1, @2, @3" : b >= 0 ? "%s @0, @1, @2" : "%s @0, @1",
          name);
    return values[0];
}

/* an output read back (field as pr_field_offset takes it, plus offset): a pair (kind 0),
a float in both lanes (1) or in lane 0 alone (2); from the register the loop stored it from
when it can be (pr_forward_find), else loaded */
static int pr_fused_output(int kind, const char *field, int offset) {
    int at = pr_field_offset(field) + offset, v, b;

    if (kind == 0) {
        int low, high;

        if ((v = pr_forward_find(at, 8)) >= 0)
            return v;
        low = pr_forward_find(at, 4);
        high = pr_forward_find(at + 4, 4);
        if (low >= 0 && high >= 0)
            return pr_fused_op("ps_merge00", low, high, -1);
    } else {
        if ((v = pr_forward_find(at, 4)) >= 0 || (v = pr_forward_find(at, 8)) >= 0)
            return kind == 2 || pr_values[v].splat ? v : pr_fused_op("ps_merge00", v, v, -1);
        if ((v = pr_forward_find(at - 4, 8)) >= 0)
            return pr_fused_op("ps_merge11", v, v, -1);
    }
    for (b = at; b < at + (kind == 0 ? 8 : 4) && b < (int)sizeof(pr_forward_read); b++)
        pr_forward_read[b] = 1;
    if (kind == 0)
        return pr_fused_new("psq_l @0, %s+%d(%%[o]), 0, 0", field != NULL ? field : "0", offset);
    return pr_fused_new("lfs @0, %s+%d(%%[o])", field != NULL ? field : "0", offset);
}

static void pr_fused_store(int value, const char *format, ...) {
    va_list arguments;
    char text[96];

    va_start(arguments, format);
    vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    pr_op(0, 0, 1, &value, "%s", text);
}

/* (fused) the inputs the prologue makes in registers (CMP, shorts, bytes, a D3DCOLOR), read from
them, not stored to %[v] and loaded back (but an input the prefix's compare reads, which is stored
too). pr_fetched_lane[n][c]: a value with component c in ps0 (-1: none), pr_fetched_half[n][h]:
components 2h and 2h + 1 as a pair; the components the array does not give are their start values
(0, and 1 for w). With the prefix kept, the prologue is written twice (the first vertex's before the
loop, each vertex's at its top) and each value's two copies are one register (pr_union), so either
way reaches the prefix and the rest with it. pr_fetched_off: --no-forward */
static int pr_fetched_off, pr_fetched_copies;
static unsigned pr_fetched_stored;
static int pr_fetched_lane[2][16][4], pr_fetched_half[2][16][2];

static void pr_fetched_reset(unsigned stored) {
    memset(pr_fetched_lane, -1, sizeof(pr_fetched_lane));
    memset(pr_fetched_half, -1, sizeof(pr_fetched_half));
    pr_fetched_on = 0;
    pr_fetched_copies = 0;
    pr_fetched_stored = stored;
}

/* (the prologue) value made for input n's components c (and c + 1 when pair) */
static void pr_fetched_made(int n, int c, int pair, int value) {
    int copy = pr_fetched_copies > 0;

    pr_values[value].fetched = 1;
    if (pair)
        pr_fetched_half[copy][n][c >> 1] = value;
    else
        pr_fetched_lane[copy][n][c] = value;
    if (copy) {
        int other = pair ? pr_fetched_half[0][n][c >> 1] : pr_fetched_lane[0][n][c];

        if (other >= 0)
            pr_union(other, value);
    }
}

/* where component c of input n is: its value and the lane that has it (a start value: a constant) */
static int pr_fetched_source(int n, int c, int *at) {
    int copy = pr_fetched_copies > 1, v;

    *at = 0;
    if (c >= pr_fused_components(pr_fuse->format[n]))
        return pr_k(1, c == 3 ? PRK_ONE : PRK_ZERO);
    if ((v = pr_fetched_lane[copy][n][c]) >= 0)
        return v;
    *at = c & 1;
    return pr_fetched_half[copy][n][c >> 1];
}

/* lanes s0 and s1 of the input at offset (16 n) as a pair, -1 when it is not in registers */
static int pr_fetched_lanes(int offset, int s0, int s1) {
    int n = offset / 16, a, b, la, lb;
    char name[24];

    if (offset < 0 || offset % 16 || !(pr_fetched_on & (1u << n)))
        return -1;
    if (s0 >= pr_fused_components(pr_fuse->format[n]) && s1 >= pr_fused_components(pr_fuse->format[n]))
        if (s0 == s1 || s0 != 3)
            return pr_k(s0 == s1 ? 1 : 0, s0 == s1 ? (s0 == 3 ? PRK_ONE : PRK_ZERO) : s1 == 3 ? PRK_W1 : PRK_ZERO);
    a = pr_fetched_source(n, s0, &la);
    b = pr_fetched_source(n, s1, &lb);
    if (a == b && la == 0 && (lb == 1 || pr_values[a].splat))
        return a;
    if (a == b && la == lb && pr_values[a].splat)
        return a;
    snprintf(name, sizeof(name), "ps_merge%d%d", la, lb);
    return pr_op3(name, a, b, -1);
}

/* pr_load's of an input in registers (kind 0: components c c+1, 1: c in both lanes, 2: c and 1),
-1 when it is not */
static int pr_fetched_load(int kind, int offset) {
    int n = offset / 16, c = (offset % 16) / 4;

    if (offset < 0 || offset % 4 || !pr_fuse || !(pr_fetched_on & (1u << n)) || kind == 3)
        return -1;
    if (kind == 1)
        return pr_fetched_lanes(16 * n, c, c);
    if (kind == 0)
        return c < 3 ? pr_fetched_lanes(16 * n, c, c + 1) : -1;
    if (c >= pr_fused_components(pr_fuse->format[n]))
        return c == 3 ? pr_k(1, PRK_ONE) : pr_k(0, PRK_W1);
    return pr_fetched_lanes(16 * n, c, 3);
}

/* count little-endian shorts at %[ad] into the slot at at as the shorts the quantized loads read:
two at a time by a word read byte-reversed (s1 s0), its low half stored, then its high half (a
halfword store each, so the slot holds shorts in any byte order the emulation keeps), then one */
static void pr_fused_shorts(int count, int at) {
    int c;

    for (c = 0; c < count; c += 2) {
        if (c)
            pr_text("addi %%[ad], %%[ad], 4");
        if (c + 1 < count) {
            pr_text("lwbrx 0, 0, %%[ad]");
            pr_text("sth 0, %d(%%[slot])", at + 2 * c);
            pr_text("rlwinm 0, 0, 16, 16, 31");
            pr_text("sth 0, %d(%%[slot])", at + 2 * c + 2);
        } else {
            pr_text("lhbrx 0, 0, %%[ad]");
            pr_text("sth 0, %d(%%[slot])", at + 2 * c);
        }
    }
}

/* the vertex's inputs from its arrays (element: %[g]) */
static void pr_fused_prologue(void) {
    int n, c, offset = 0;

    pr_text("lwz %%[g], 0(%%[e])");
    pr_text("addi %%[e], %%[e], 4");
    for (n = 0; n < 16; n++) {
        unsigned format = pr_fuse->format[n];
        int at = 16 * n, size, kept = !(pr_fetched_off || format <= WL_XBOX_VSH_FUSED_F4 ||
                                         (pr_raw_on && pr_raw_at[n] >= 0)),
            stored = !kept || (pr_fetched_stored & (1u << n));

        if (format == WL_XBOX_VSH_FUSED_NONE)
            continue;
        if (kept)
            pr_fetched_on |= 1u << n;
        /* (its element 0 and stride in registers, %[dn] and %[sn], while GCC's 30 operands allow;
        interleaved: the element times the stride made once, into %[g]) */
        if (pr_fuse->interleaved) {
            if (!offset) {
                if (pr_fused_held & (1u << n)) {
                    pr_text("mullw %%[g], %%[s%d], %%[g]", n);
                } else {
                    pr_text("lwz 0, %d(%%[f])", (int)offsetof(WlXboxVshFused, stride) + 4 * n);
                    pr_text("mullw %%[g], 0, %%[g]");
                }
                offset = 1;
            }
            if (pr_fused_held & (1u << n)) {
                pr_text("add %%[ad], %%[d%d], %%[g]", n);
            } else {
                pr_text("lwz %%[ad], %%[fdata]+%%[ptr]*%d(%%[f])", n);
                pr_text("add %%[ad], %%[ad], %%[g]");
            }
        } else if (pr_fused_held & (1u << n)) {
            pr_text("mullw 0, %%[s%d], %%[g]", n);
            pr_text("add %%[ad], %%[d%d], 0", n);
        } else {
            pr_text("lwz %%[ad], %%[fdata]+%%[ptr]*%d(%%[f])", n);
            pr_text("lwz 0, %d(%%[f])", (int)offsetof(WlXboxVshFused, stride) + 4 * n);
            pr_text("mullw 0, 0, %%[g]");
            pr_text("add %%[ad], %%[ad], 0");
        }
        /* (an input the prefix alone reads: its bytes copied, pr_raw_plan's) */
        if (pr_raw_on && pr_raw_at[n] >= 0) {
            int bytes = pr_raw_bytes(format);

            if (format >= WL_XBOX_VSH_FUSED_UB) {
                static const char *const loads[5] = {"", "lbz", "lhz", "", "lwz"};
                static const char *const stores[5] = {"", "stb", "sth", "", "stw"};

                pr_text("%s 0, 0(%%[ad])", loads[bytes]);
                pr_text("%s 0, %d(%%[slot])", stores[bytes], 8 + pr_raw_at[n]);
            } else {
                pr_fused_shorts(bytes / 2, 8 + pr_raw_at[n]);
            }
            continue;
        }
        if (format >= WL_XBOX_VSH_FUSED_F1 && format <= WL_XBOX_VSH_FUSED_F4) {
            size = (int)(format - WL_XBOX_VSH_FUSED_F1) + 1;
            for (c = 0; c < size; c++) {
                if (c)
                    pr_text("addi %%[ad], %%[ad], 4");
                pr_text("lwbrx 0, 0, %%[ad]");
                pr_text("stw 0, %d(%%[v])", at + 4 * c);
            }
        } else if (format == WL_XBOX_VSH_FUSED_CMP) {
            /* (11, 11 and 10 bits through the fetch's tables) */
            static const char *const fields[3] = {"2, 19, 29", "23, 19, 29", "12, 20, 29"};

            pr_text("lwbrx %%[ad], 0, %%[ad]");
            for (c = 0; c < 3; c++) {
                int value;

                pr_text("rlwinm 0, %%[ad], %s", fields[c]);
                value = pr_fused_new("lfsx @0, %s, 0", c < 2 ? "%[t11]" : "%[t10]");
                /* (lfsx: the float in both lanes) */
                pr_values[value].splat = 1;
                if (kept)
                    pr_fetched_made(n, c, 0, value);
                if (stored)
                    pr_fused_store(value, "stfs @0, %d(%%[v])", at + 4 * c);
            }
        } else if (format >= WL_XBOX_VSH_FUSED_S1 && format < WL_XBOX_VSH_FUSED_UB) {
            /* (normalized: s / 32767, at least -1) */
            int scale = pr_fused_pack(0, WL_XBOX_VSH_FUSED_SHORT), one = pr_fused_pack(0, WL_XBOX_VSH_FUSED_ONES),
                low = pr_fused_pack(0, WL_XBOX_VSH_FUSED_NEGATIVE);

            size = (int)(format - WL_XBOX_VSH_FUSED_S1) + 1;
            pr_fused_shorts(size, 0);
            for (c = 0; c < size; c += 2) {
                int pair = c + 1 < size, value = pr_fused_new("psq_l @0, %d(%%[slot]), %d, 7", 2 * c, !pair);
                int product = pr_fused_op("ps_mul", value, scale, -1);
                int above = pr_fused_op("ps_add", product, one, -1);
                int result = pr_fused_op("ps_sel", above, product, low);

                if (kept)
                    pr_fetched_made(n, c, pair, result);
                if (stored && pair)
                    pr_fused_store(result, "psq_st @0, %d(%%[v]), 0, 0", at + 4 * c);
                else if (stored)
                    pr_fused_store(result, "stfs @0, %d(%%[v])", at + 4 * c);
            }
        } else if (format >= WL_XBOX_VSH_FUSED_S32K) {
            /* (shorts as they are: GQR7's s16, unscaled) */
            size = (int)(format - WL_XBOX_VSH_FUSED_S32K) + 1;
            pr_fused_shorts(size, 0);
            for (c = 0; c < size; c += 2) {
                int pair = c + 1 < size, value = pr_fused_new("psq_l @0, %d(%%[slot]), %d, 7", 2 * c, !pair);

                if (kept)
                    pr_fetched_made(n, c, pair, value);
                if (stored && pair)
                    pr_fused_store(value, "psq_st @0, %d(%%[v]), 0, 0", at + 4 * c);
                else if (stored)
                    pr_fused_store(value, "stfs @0, %d(%%[v])", at + 4 * c);
            }
        } else if (format >= WL_XBOX_VSH_FUSED_UB) {
            int scale = pr_fused_pack(0, WL_XBOX_VSH_FUSED_BYTE);

            size = (int)(format - WL_XBOX_VSH_FUSED_UB) + 1;
            for (c = 0; c < size; c += 2) {
                int pair = c + 1 < size, value = pr_fused_new("psq_l @0, %d(%%[ad]), %d, 6", c, !pair);
                int result = pr_fused_op("ps_mul", value, scale, -1);

                if (kept)
                    pr_fetched_made(n, c, pair, result);
                if (stored && pair)
                    pr_fused_store(result, "psq_st @0, %d(%%[v]), 0, 0", at + 4 * c);
                else if (stored)
                    pr_fused_store(result, "stfs @0, %d(%%[v])", at + 4 * c);
            }
        } else {
            /* a D3DCOLOR: bytes B, G, R, A */
            int scale = pr_fused_pack(0, WL_XBOX_VSH_FUSED_BYTE);
            int bg = pr_fused_new("psq_l @0, 0(%%[ad]), 0, 6"), ra = pr_fused_new("psq_l @0, 2(%%[ad]), 0, 6");
            int rg = pr_fused_op("ps_merge01", ra, bg, -1), ba = pr_fused_op("ps_merge01", bg, ra, -1);

            rg = pr_fused_op("ps_mul", rg, scale, -1);
            ba = pr_fused_op("ps_mul", ba, scale, -1);
            if (kept) {
                pr_fetched_made(n, 0, 1, rg);
                pr_fetched_made(n, 2, 1, ba);
            }
            if (stored) {
                pr_fused_store(rg, "psq_st @0, %d(%%[v]), 0, 0", at);
                pr_fused_store(ba, "psq_st @0, %d(%%[v]), 0, 0", at + 8);
            }
        }
    }
    pr_fetched_copies++;
}

/* 1 / sqrt(x) in single precision for x a float, given x / 2: frsqrte's estimate (Broadway's
table: 1/2^12 or better, as Dolphin's model of it) and two of Newton's steps, y (3/2 - x/2 y y),
each fused (a float's worth: the sphere map's coordinates need no more) */
static int pr_fused_root(int x, int half_x) {
    int y = pr_fused_op("frsqrte", x, -1, -1), three_halves = pr_fused_sphere(2), step;

    for (step = 0; step < 2; step++) {
        int t = pr_fused_op("fmuls", y, y, -1);

        t = pr_fused_op("fnmsubs", half_x, t, three_halves);
        y = pr_fused_op("fmuls", y, t, -1);
    }
    return y;
}

/* GX's words from the outputs (vertices_pack's), where %[w] says */
static void pr_fused_epilogue(void) {
    const WlXboxVshFusedKey *key = pr_fuse;
    char other[24], done[24];
    int word = 3, n;

    if (!pr_early) {
        pr_text("lwz %%[ad], 0*%%[ptr](%%[w])");
        pr_text("addi %%[w], %%[w], %%[ptr]");
    }
    /* the position: (x w, y w, -w) in pixels from the clip-space position
    the program captured, else the screen-space position and the depth */
    pr_new_label(other, sizeof(other));
    pr_new_label(done, sizeof(done));
    if (key->perspective == WL_XBOX_VSH_FUSED_XF) {
        /* (the transform unit's: v0's words as the fetch made them, the vertex's inputs) */
        for (n = 0; n < 3; n++) {
            pr_text("lwz 0, %d(%%[v])", 4 * n);
            pr_text("stw 0, %d(%%[ad])", 4 * n);
        }
    } else if (key->perspective == WL_XBOX_VSH_FUSED_WORLD && pr_world_spill && pr_early) {
        /* (stored to the vertex where the program captured) */
    } else if (key->perspective == WL_XBOX_VSH_FUSED_WORLD && pr_world_spill) {
        /* (the transform unit's from the world position, stored where the program captured) */
        for (n = 0; n < 3; n++) {
            pr_text("lwz 0, %d(%%[o])", 4 * n);
            pr_text("stw 0, %d(%%[ad])", 4 * n);
        }
    } else if (key->perspective == WL_XBOX_VSH_FUSED_WORLD) {
        /* (the transform unit's from the world position: its x, y and z, the clip-space w over the
        one at world_slot) */
        pr_fused_store(pr_world_values[0], "psq_st @0, 0(%%[ad]), 0, 0");
        pr_fused_store(pr_world_values[1], "stfs @0, 8(%%[ad])");
        /* (its offset in a register for the whole loop: [ws]) */
        pr_fused_store(pr_world_w, "stfsx @0, %%[ad], %%[ws]");
    } else {
        /* (the captured way alone when the program always captures, the other alone without
        perspective; else the program's flag tested) */
        int clip = key->perspective && pr_fused_captured, screen = !key->perspective || !pr_fused_captured;
        int to_samples = pr_fused_pack(0, WL_XBOX_VSH_FUSED_TO_SAMPLES), half = pr_fused_pack(0, WL_XBOX_VSH_FUSED_HALF);
        int mark = pr_cache_count;

        if (key->perspective) {
            int xy, zw, x, h;

            if (!clip) {
                pr_text("lwz 0, %%[ocaptured](%%[o])");
                pr_text("cmpwi 0, 0");
                pr_text("beq %s", other);
            }
            xy = pr_fused_output(0, "%[oclip]", 0);
            zw = pr_fused_output(0, "%[oclip]", 8);
            /* (the scale and offset already in samples: 1 / the samples is a power of two, so
            (x scale + w offset) / samples is x (scale / samples) + w (offset / samples) to the bit) */
            x = pr_fused_op("ps_mul", xy, pr_fused_pack(0, WL_XBOX_VSH_FUSED_SCALE), -1);
            x = pr_fused_op("ps_add", x, pr_fused_op("ps_muls1", pr_fused_pack(0, WL_XBOX_VSH_FUSED_OFFSET), zw, -1),
                            -1);
            h = pr_fused_op("ps_muls1", half, zw, -1);
            /* (-w as (-z, -w) at word 1, which x and y then write over) */
            pr_fused_store(pr_fused_op("ps_neg", zw, -1, -1), "psq_st @0, 4(%%[ad]), 0, 0");
            pr_fused_store(pr_fused_op("ps_add", x, h, -1), "psq_st @0, 0(%%[ad]), 0, 0");
            if (screen)
                pr_text("b %s", done);
        }
        if (screen) {
            int xy, z, one, depth, limited;

            pr_place_label(other);
            pr_cache_trim(mark);
            xy = pr_fused_output(0, NULL, 0);
            z = pr_fused_output(2, NULL, 8);
            one = pr_fused_pack(1, WL_XBOX_VSH_FUSED_ONES);
            xy = pr_fused_op("ps_mul", xy, to_samples, -1);
            pr_fused_store(pr_fused_op("ps_add", xy, half, -1), "psq_st @0, 0(%%[ad]), 0, 0");
            /* (as_depth: z / the maximum, 0 below 0 or for a NaN, 1 above 1; less 1) */
            depth = pr_fused_op("fdivs", z, pr_fused_pack(1, WL_XBOX_VSH_FUSED_DEPTH), -1);
            depth = pr_fused_op("fsel", depth, depth, pr_k(1, PRK_ZERO));
            limited = pr_fused_op("fsubs", depth, one, -1);
            depth = pr_fused_op("fsel", limited, one, depth);
            pr_fused_store(pr_fused_op("fsubs", depth, one, -1), "stfs @0, 8(%%[ad])");
            pr_place_label(done);
        }
        if (key->perspective && screen)
            pr_cache_trim(mark);
    }
    /* a lit form's normal (the temporary's x, y and z as the program ends) */
    if (key->lit) {
        pr_fused_store(pr_lit_values[0], "psq_st @0, %d(%%[ad]), 0, 0", 4 * word);
        pr_fused_store(pr_lit_values[1], "stfs @0, %d(%%[ad])", 4 * word + 8);
        word += 3;
    }
    /* the projected coordinates ((s, t, q) undivided, a linear texture's s and t to a fraction:
    stage_texcoord_projected's) and the sphere maps GX makes (their direction of unit length), first,
    after a lit form's normal; for two or three in all, nine words, the unused zero */
    {
        int projected = key->lit ? 1 : 0;

        for (n = 0; n < key->stage_count; n++) {
            unsigned stage = key->stages[n] & 3u, kind = key->stages[n] >> 2;
            int at = 16 * (int)stage, st;

            if (kind == WL_XBOX_VSH_FUSED_STAGE_SPHERE_XF) {
                /* (d / |d| for vertices_pack's sphere_direction: one root of two steps, single
                precision; 0 for a direction too short or too long, which GX makes the disc's centre) */
                int z = pr_fused_output(2, "%[ot]", at + 8), half = pr_fused_pack(0, WL_XBOX_VSH_FUSED_HALF);
                int zero = pr_k(0, PRK_ZERO), squares, across, length2, root, values[2], mark;

                st = pr_fused_output(0, "%[ot]", at);
                pr_new_label(other, sizeof(other));
                pr_new_label(done, sizeof(done));
                squares = pr_fused_op("ps_mul", st, st, -1);
                across = pr_fused_op("ps_sum0", squares, squares, squares);
                length2 = pr_fused_op("fmadds", z, z, across);
                values[0] = length2;
                values[1] = pr_fused_sphere(0);
                pr_op(0, 0, 2, values, "fcmpu 0, @0, @1");
                mark = pr_cache_count;
                pr_text("bng %s", other);
                values[1] = pr_fused_sphere(1);
                pr_op(0, 0, 2, values, "fcmpu 0, @0, @1");
                pr_text("bnl %s", other);
                root = pr_fused_root(length2, pr_fused_op("fmuls", length2, half, -1));
                pr_fused_store(pr_fused_op("ps_muls0", st, root, -1), "psq_st @0, %d(%%[ad]), 0, 0", 4 * word);
                pr_fused_store(pr_fused_op("fmuls", z, root, -1), "stfs @0, %d(%%[ad])", 4 * word + 8);
                pr_text("b %s", done);
                pr_place_label(other);
                pr_fused_store(zero, "psq_st @0, %d(%%[ad]), 0, 0", 4 * word);
                pr_fused_store(zero, "stfs @0, %d(%%[ad])", 4 * word + 8);
                pr_place_label(done);
                pr_cache_trim(mark);
                word += 3;
                projected++;
                continue;
            }
            if (kind != WL_XBOX_VSH_FUSED_STAGE_PROJECTED && kind != WL_XBOX_VSH_FUSED_STAGE_PROJECTED_LINEAR)
                continue;
            st = pr_fused_output(0, "%[ot]", at);
            if (kind == WL_XBOX_VSH_FUSED_STAGE_PROJECTED_LINEAR)
                st = pr_fused_op("ps_mul", st, pr_fused_pack(0, WL_XBOX_VSH_FUSED_TO_TEXELS + 2 * (int)stage), -1);
            pr_fused_store(st, "psq_st @0, %d(%%[ad]), 0, 0", 4 * word);
            pr_fused_store(pr_fused_output(2, "%[ot]", at + 12), "stfs @0, %d(%%[ad])", 4 * word + 8);
            word += 3;
            projected++;
        }
        for (; projected > 1 && projected < 3; projected++, word += 3) {
            int zero = pr_k(0, PRK_ZERO);

            pr_fused_store(zero, "psq_st @0, %d(%%[ad]), 0, 0", 4 * word);
            pr_fused_store(zero, "stfs @0, %d(%%[ad])", 4 * word + 8);
        }
    }
    /* the colours: each component's byte, value * 255 + 1/2 cut down (a lit
    form's d0 its alpha alone: r, g and b whatever the output held; a value that is %[k]'s 0, the
    output's start value or a write of it, 0 itself, its bytes 0: pr_fold_off, not, as before) */
    for (n = 0; n < key->colors; n++, word++) {
        const char *field = n == 0 ? "%[od0]" : "%[od1]";
        int half;

        if (!(key->channels & (1u << n))) {
            pr_text("li 0, 0");
            pr_text("stw 0, %d(%%[ad])", 4 * word);
            continue;
        }
        if (n == 0 && key->lit) {
            /* (the alpha's byte alone, at the word's last byte: GX lights r, g and b) */
            int value = pr_fused_output(1, field, 12);

            if (!pr_is_zero(value)) {
                value = pr_fused_op("ps_madd", value, pr_fused_pack(0, WL_XBOX_VSH_FUSED_255),
                                    pr_fused_pack(0, WL_XBOX_VSH_FUSED_HALF));
                value = pr_fused_op("ps_sel", value, value, pr_k(0, PRK_ZERO));
            }
            pr_fused_store(value, "psq_st @0, %d(%%[ad]), 1, 6", 4 * word + 3);
            continue;
        }
        for (half = 0; half < 2; half++) {
            int value = pr_fused_output(0, field, 8 * half);

            if (pr_is_zero(value)) {
                pr_fused_store(value, "psq_st @0, %d(%%[ad]), 0, 6", 4 * word + 2 * half);
                continue;
            }
            /* (fused: a byte apart from the general path's two roundings only where value * 255
               + 1/2 is within an ulp of a whole number) */
            value = pr_fused_op("ps_madd", value, pr_fused_pack(0, WL_XBOX_VSH_FUSED_255),
                                pr_fused_pack(0, WL_XBOX_VSH_FUSED_HALF));
            /* a NaN made 0 before the store, as wl_nv2a_color_byte's selects make it (Dolphin
               quantizes a NaN to 255) */
            value = pr_fused_op("ps_sel", value, value, pr_k(0, PRK_ZERO));
            pr_fused_store(value, "psq_st @0, %d(%%[ad]), 0, 6", 4 * word + 2 * half);
        }
    }
    /* the texture coordinates (stage_texcoord's) */
    for (n = 0; n < key->stage_count; n++, word += 2) {
        unsigned stage = key->stages[n] & 3u, kind = key->stages[n] >> 2;
        int at, st;

        if (kind == WL_XBOX_VSH_FUSED_STAGE_PROJECTED || kind == WL_XBOX_VSH_FUSED_STAGE_PROJECTED_LINEAR ||
            kind == WL_XBOX_VSH_FUSED_STAGE_SPHERE_XF) {
            word -= 2;
            continue;
        }
        if (kind == WL_XBOX_VSH_FUSED_STAGE_INPUT) {
            /* (GX's from the input register's x and y as the fetch made them, sent in the first stage
            made from it; the later ones not sent) */
            int k, first = 1;

            for (k = 0; k < n; k++)
                if ((key->stages[k] >> 2) == WL_XBOX_VSH_FUSED_STAGE_INPUT &&
                    pr_texgen_from[key->stages[k] & 3u] == pr_texgen_from[stage])
                    first = 0;
            if (!first) {
                word -= 2;
                continue;
            }
            pr_fused_store(pr_load(0, "%[v]", 16 * pr_texgen_from[stage], 0), "psq_st @0, %d(%%[ad]), 0, 0",
                           4 * word);
            continue;
        }
        at = 16 * (int)stage;
        st = pr_fused_output(0, "%[ot]", at);
        int linear = kind == WL_XBOX_VSH_FUSED_STAGE_LINEAR || kind == WL_XBOX_VSH_FUSED_STAGE_PROJECTIVE_LINEAR;
        int to = linear ? pr_fused_pack(0, WL_XBOX_VSH_FUSED_TO_TEXELS + 2 * (int)stage) : -1;
        int mark = pr_cache_count;

        /* (projective with a q no instruction writes: its default 1, never divided by) */
        if (!(pr_written[4 + stage] & 1u)) {
            if (kind == WL_XBOX_VSH_FUSED_STAGE_PROJECTIVE)
                kind = WL_XBOX_VSH_FUSED_STAGE_PLAIN;
            else if (kind == WL_XBOX_VSH_FUSED_STAGE_PROJECTIVE_LINEAR)
                kind = WL_XBOX_VSH_FUSED_STAGE_LINEAR;
        }
        if (kind == WL_XBOX_VSH_FUSED_STAGE_PLAIN) {
            pr_fused_store(st, "psq_st @0, %d(%%[ad]), 0, 0", 4 * word);
        } else if (kind == WL_XBOX_VSH_FUSED_STAGE_LINEAR) {
            pr_fused_store(pr_fused_op("ps_mul", st, to, -1), "psq_st @0, %d(%%[ad]), 0, 0", 4 * word);
        } else if (kind == WL_XBOX_VSH_FUSED_STAGE_PROJECTIVE || kind == WL_XBOX_VSH_FUSED_STAGE_PROJECTIVE_LINEAR) {
            /* (divided by q unless q is 1 or within 1e-12 of 0, or a NaN) */
            int q = pr_fused_output(1, "%[ot]", at + 12), values[2], divided;
            int one = pr_fused_pack(1, WL_XBOX_VSH_FUSED_ONES), tiny = pr_fused_pack(1, WL_XBOX_VSH_FUSED_TINY);

            mark = pr_cache_count;
            pr_new_label(other, sizeof(other));
            pr_new_label(done, sizeof(done));
            values[0] = q;
            values[1] = one;
            pr_op(0, 0, 2, values, "fcmpu 0, @0, @1");
            pr_text("beq %s", other);
            values[0] = pr_fused_op("fabs", q, -1, -1);
            values[1] = tiny;
            pr_op(0, 0, 2, values, "fcmpu 0, @0, @1");
            pr_text("bng %s", other);
            divided = pr_fused_op("ps_div", st, q, -1);
            if (linear)
                divided = pr_fused_op("ps_mul", divided, to, -1);
            pr_fused_store(divided, "psq_st @0, %d(%%[ad]), 0, 0", 4 * word);
            pr_text("b %s", done);
            pr_place_label(other);
            pr_fused_store(linear ? pr_fused_op("ps_mul", st, to, -1) : st, "psq_st @0, %d(%%[ad]), 0, 0", 4 * word);
            pr_place_label(done);
            pr_cache_trim(mark);
        } else {
            /* a sphere map (wl_nv2a_sphere_coordinates' coordinates, in single precision): with
            n = d / |d|, x k + 1/2, y k + 1/2 for k = 1 / (2 sqrt(2 (1 + z))); the disc's centre
            for a direction too short or too long, its edge straight behind. Toward the back
            (z < 0), 1 + z is r / (1 - z), r = x x + y y, which cancels nothing: k is
            a / 2 / sqrt(2 r a) there, a = 1 + |z|, and 1 / 2 / sqrt(2 a) in front. (A float's
            worth: within 2^-20 of the general path's double, below what GX's coordinates show) */
            char edge[24];
            int z = pr_fused_output(2, "%[ot]", at + 8), half = pr_fused_pack(0, WL_XBOX_VSH_FUSED_HALF);
            int one = pr_fused_pack(1, WL_XBOX_VSH_FUSED_ONES);
            int squares, across, length2, root, xy, r, a, s, u, k, values[2];

            pr_new_label(other, sizeof(other));
            pr_new_label(edge, sizeof(edge));
            pr_new_label(done, sizeof(done));
            squares = pr_fused_op("ps_mul", st, st, -1);
            across = pr_fused_op("ps_sum0", squares, squares, squares);
            length2 = pr_fused_op("fmadds", z, z, across);
            values[0] = length2;
            values[1] = pr_fused_sphere(0);
            pr_op(0, 0, 2, values, "fcmpu 0, @0, @1");
            mark = pr_cache_count;
            pr_text("bng %s", other);
            values[1] = pr_fused_sphere(1);
            pr_op(0, 0, 2, values, "fcmpu 0, @0, @1");
            pr_text("bnl %s", other);
            root = pr_fused_root(length2, pr_fused_op("fmuls", length2, half, -1));
            xy = pr_fused_op("ps_muls0", st, root, -1);
            z = pr_fused_op("fmuls", z, root, -1);
            r = pr_fused_op("fmuls", across, pr_fused_op("fmuls", root, root, -1), -1);
            a = pr_fused_op("fadds", one, pr_fused_op("fabs", z, -1, -1), -1);
            /* (s: 1 in front, r behind, where r below 2.5e-13 is the edge; the root's
            argument 2 a s, its half a s) */
            s = pr_fused_op("fsel", z, one, r);
            values[0] = s;
            values[1] = pr_fused_sphere(4);
            pr_op(0, 0, 2, values, "fcmpu 0, @0, @1");
            pr_text("bng %s", edge);
            u = pr_fused_op("fmuls", a, s, -1);
            k = pr_fused_root(pr_fused_op("fadds", u, u, -1), u);
            k = pr_fused_op("fmuls", k, pr_fused_op("fsel", z, half, pr_fused_op("fmuls", a, half, -1)), -1);
            pr_fused_store(pr_fused_op("ps_madds0", xy, k, half), "psq_st @0, %d(%%[ad]), 0, 0", 4 * word);
            pr_text("b %s", done);
            pr_place_label(other);
            pr_fused_store(pr_fused_pack(0, WL_XBOX_VSH_FUSED_HALF), "psq_st @0, %d(%%[ad]), 0, 0", 4 * word);
            pr_text("b %s", done);
            pr_place_label(edge);
            pr_fused_store(pr_fused_pack(0, WL_XBOX_VSH_FUSED_EDGE), "psq_st @0, %d(%%[ad]), 0, 0", 4 * word);
            pr_place_label(done);
            pr_cache_trim(mark);
        }
    }
}

/* the instructions as ops (pr_pre, pr_body); 0 when one can't be */
static int pr_generate(const WlXboxVshProgram *program, const unsigned *needs, int reused,
                       const ReusePrefix *prefix, int *compare_count) {
    static unsigned after[WL_XBOX_VSH_MAX_INSTRUCTIONS][13];
    unsigned long index, start = reused ? prefix->end : 0;
    unsigned open = ~0u;
    char skip[24] = "", top[24], made[24], again[24] = "";
    int t, h;

    pr_value_count = pr_pre_count = pr_body_count = pr_cache_count = 0;
    pr_prefix_first = pr_prefix_end = 0;
    pr_region = 0;
    pr_failed = 0;
    pr_label = 0;
    pr_ra_key[0] = 0;
    pr_a0_version = pr_uses_a0 = pr_uses_ra = pr_uses_c2 = pr_uses_slot = 0;
    memset(pr_def, -1, sizeof(pr_def));
    memset(pr_kept, 0, sizeof(pr_kept));
    /* (value 0: a placeholder no op names) */
    pr_new();
    pr_needs = ~0u;
    pr_fused_captured = 0;
    pr_early = pr_fuse && !pr_early_off;
    pr_forward_reset();
    /* (fused: live is the form's, each instruction always written or left out) */
    if (pr_fuse) {
        static unsigned fused_needs[WL_XBOX_VSH_MAX_INSTRUCTIONS];

        /* (a "w" form: the instructions the position needs kept, the world position among them; the
        clip-space position's own then dead, pr_liveness') */
        unsigned keep = pr_fuse->live | (pr_fuse->perspective == WL_XBOX_VSH_FUSED_WORLD ? WL_XBOX_VSH_LIVE_POSITION : 0u);

        if (pr_fuse->perspective == WL_XBOX_VSH_FUSED_WORLD &&
            (!program->world.found || !program->captures_clip || (!program->world.to_end && !pr_world_spill)))
            return 0;
        pr_program_world = program->world.temporary;
        /* (the stages GX makes from an input register: the register each reads, the form not written
        for a stage not of that shape) */
        {
            WlXboxVshDependencies dependencies;
            unsigned n;

            memset(pr_texgen_from, -1, sizeof(pr_texgen_from));
            wl_xbox_vsh_analyze(program, &dependencies);
            for (n = 0; n < pr_fuse->stage_count; n++) {
                unsigned stage = pr_fuse->stages[n] & 3u;

                if ((pr_fuse->stages[n] >> 2) != WL_XBOX_VSH_FUSED_STAGE_INPUT)
                    continue;
                pr_texgen_from[stage] = wl_xbox_vsh_texgen_source(&dependencies, pr_fuse, stage);
                if (pr_texgen_from[stage] < 0 || (pr_fuse->live & (WL_XBOX_VSH_LIVE_T0 << stage)))
                    return 0;
            }
        }
        for (index = 0; index < program->count; index++)
            fused_needs[index] = needs[index] & keep ? ~0u : 0u;
        needs = fused_needs;
    }
    for (index = 0; index < program->count; index++)
        if (needs[index] && !pr_supported(&program->instructions[index]))
            return 0;
    if (pr_fuse && pr_fuse->lit && !program->light.count)
        return 0;
    {
        int drop = 0;

        if (pr_fuse && pr_fuse->perspective)
            for (index = 0; index < program->count; index++)
                drop |= needs[index] && program->instructions[index].captures_clip;
        if (pr_fuse && pr_fuse->perspective == WL_XBOX_VSH_FUSED_WORLD)
            drop = 2;
        pr_liveness(program, needs, 0, after, drop);
    }
    /* (each block's test: a condition register bit set once a batch) */
    pr_need_count = 0;
    for (index = 0; index < program->count && !pr_fuse; index++)
        if (needs[index])
            pr_need_bit(needs[index]);
    if (!pr_fuse)
        pr_need_bit(WL_XBOX_VSH_LIVE_POSITION);
    *compare_count = 0;
    pr_new_label(top, sizeof(top));
    pr_new_label(made, sizeof(made));
    if (reused)
        pr_new_label(again, sizeof(again));
    /* (fused: the vertex's inputs read from its arrays first; the first
    vertex's prefix always made) */
    /* (the prefix's inputs, the components it reads: bit 3 x) */
    unsigned words[16] = {0};

    pr_raw_on = 0;
    if (reused) {
        int which;

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
        if (pr_fuse)
            pr_raw_plan(program, needs, prefix, words);
    }
    {
        /* (the inputs the prefix's compare reads in memory: stored as well) */
        unsigned stored = 0;

        for (index = 0; index < 16; index++)
            if (reused && !pr_raw_on && words[index])
                stored |= 1u << index;
        pr_fetched_reset(stored);
    }
    if (pr_fuse) {
        pr_fused_start(program, needs, reused);
        if (reused) {
            pr_fused_prologue();
            pr_forget();
            /* (the first vertex's bytes the vertex before's for the next) */
            if (pr_raw_on) {
                pr_text("lwz 0, 8(%%[slot])");
                pr_text("stw 0, 12(%%[slot])");
            }
            pr_text("b %s", again);
        }
    }
    pr_place_label(top);
    if (pr_fuse)
        pr_fused_prologue();
    /* the prefix: made, or kept while the inputs it reads are the vertex before's */
    if (reused) {
        int which;

        snprintf(pr_first, sizeof(pr_first), "%s", pr_fuse ? "" : again);
        if (pr_raw_on) {
            pr_text("lwz 0, 8(%%[slot])");
            pr_text("lwz %%[g], 12(%%[slot])");
            pr_text("stw 0, 12(%%[slot])");
            pr_text("cmpw 0, %%[g]");
            /* (the same bytes, most vertices, past the prefix; else on into it) */
            pr_text("beq %s", made);
            (*compare_count)++;
        }
        for (index = 0; index < 16 && !pr_raw_on; index++)
            for (which = 0; which < 4; which++)
                if ((words[index] & (8u >> which)) &&
                    (!pr_fuse || which < pr_fused_components(pr_fuse->format[index]))) {
                    /* (the vertex before's: %[vs] bytes before) */
                    pr_text("lwz 0, %lu(%%[v])", 16 * index + 4 * (unsigned long)which);
                    if (pr_fuse)
                        pr_text("lwz %%[g], %lu(%%[pv])", 16 * index + 4 * (unsigned long)which);
                    else
                        pr_text("lwz %%[g], %lu-%%[vs](%%[v])", 16 * index + 4 * (unsigned long)which);
                    pr_text("cmpw 0, %%[g]");
                    pr_text("bne %s", again);
                    (*compare_count)++;
                }
        if (!pr_raw_on)
            pr_text("b %s", made);
        pr_place_label(again);
        pr_use_weight = pr_weighted ? 2 : 4;
        pr_prefix_first = pr_body_count;
        pr_region = 1;
        if (pr_raw_on) {
            /* (what the prologue loaded is not there for the first vertex, which comes from the
            other) */
            pr_forget();
            pr_raw_floats();
        }
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
            pr_dead_now = pr_dead[index];
            pr_instruction(in, WRITE_TEMPORARIES);
        }
        if (open != ~0u)
            pr_place_label(skip);
        open = ~0u;
        pr_needs = ~0u;
        /* (pr_spill: the halves the rest reads of the prefix stored in the slot, and loaded from it
        at each vertex, so that its registers are free past their last reads) */
        if (pr_spill) {
            int spilled = 0, values[1];

            for (t = 0; t < 12; t++)
                for (h = 0; h < 2; h++)
                    if ((prefix->live_out[t] & (0xcu >> (2 * h))) && pr_def[t][h] >= 0) {
                        if (spilled >= PR_SPILLS) {
                            pr_failed = 1;
                            return 0;
                        }
                        values[0] = pr_def[t][h];
                        pr_op(0, 0, 1, values, "psq_st @0, %d(%%[slot]), 0, 0", PR_SPILL_AT + 8 * spilled++);
                    }
        }
        pr_resident_loads();
        pr_place_label(made);
        pr_region = 2;
        if (pr_early) {
            pr_text("lwz %%[ad], 0*%%[ptr](%%[w])");
            pr_text("addi %%[w], %%[w], %%[ptr]");
        }
        pr_use_weight = 4;
        pr_prefix_end = pr_body_count;
        pr_forward_on = 1;
        /* (the halves the rest reads of the prefix: kept for the vertex after) */
        {
            int spilled = 0, values[1];

            for (t = 0; t < 12; t++)
                for (h = 0; h < 2; h++)
                    if ((prefix->live_out[t] & (0xcu >> (2 * h))) && pr_def[t][h] >= 0) {
                        if (pr_spill) {
                            values[0] = pr_new();
                            pr_values[values[0]].fresh = 0;
                            pr_op(0, 1, 1, values, "psq_l @0, %d(%%[slot]), 0, 0", PR_SPILL_AT + 8 * spilled++);
                            pr_def[t][h] = values[0];
                            continue;
                        }
                        pr_kept[t][h] = 1;
                        pr_values[pr_def[t][h]].loop = 1;
                    }
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
            pr_dead_now = pr_dead[index];
            pr_instruction(in, WRITE_OUTPUTS);
        }
    } else {
        pr_first[0] = 0;
        pr_place_label(made);
        if (pr_early) {
            pr_text("lwz %%[ad], 0*%%[ptr](%%[w])");
            pr_text("addi %%[w], %%[w], %%[ptr]");
        }
        pr_forward_on = 1;
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
        pr_dead_now = pr_dead[index];
        pr_instruction(&program->instructions[index], WRITE_ALL);
    }
    pr_dead_now = 0;
    if (open != ~0u)
        pr_place_label(skip);
    pr_needs = ~0u;
    /* the position (as the program left p when it is live; else (0, 0, 0, 1));
    fused: none when GX's takes the clip-space one, else the one way live gives */
    if (pr_fuse) {
        int values[1], live = (pr_fuse->live & WL_XBOX_VSH_LIVE_POSITION) != 0;

        if (!(pr_fuse->perspective && pr_fused_captured) && pr_fuse->perspective != WL_XBOX_VSH_FUSED_XF &&
            pr_fuse->perspective != WL_XBOX_VSH_FUSED_WORLD) {
            values[0] = live ? pr_half(12, 0) : pr_k(0, PRK_ZERO);
            pr_op(0, 0, 1, values, "psq_st @0, 0(%%[o]), 0, 0");
            pr_forward_store(0, 8, values[0]);
            values[0] = live ? pr_half(12, 1) : pr_k(0, PRK_W1);
            pr_op(0, 0, 1, values, "psq_st @0, 8(%%[o]), 0, 0");
            pr_forward_store(8, 8, values[0]);
        }
    } else {
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
    if (pr_fuse) {
        if (pr_fuse->lit) {
            pr_lit_values[0] = pr_half(program->light.normal, 0);
            pr_lit_values[1] = pr_half(program->light.normal, 1);
        }
        if (pr_fuse->perspective == WL_XBOX_VSH_FUSED_WORLD && !pr_world_spill) {
            pr_world_values[0] = pr_half(program->world.temporary, 0);
            pr_world_values[1] = pr_half(program->world.temporary, 1);
        }
        pr_fused_epilogue();
        pr_defaults_finish();
        pr_forward_finish();
        /* (the inputs' two sets swapped: this vertex's are the vertex before's; not when the
        prefix's compare reads the copied bytes, and nothing reads the vertex before's inputs) */
        if (reused && !pr_raw_on) {
            pr_text("mr %%[ad], %%[v]");
            pr_text("mr %%[v], %%[pv]");
            pr_text("mr %%[pv], %%[ad]");
        }
        pr_text("bdnz %s", top);
        pr_fused_end();
        if (!pr_failed)
            pr_sweep();
        if (!pr_failed && pr_spill)
            pr_dead_spills();
        return !pr_failed;
    }
    pr_defaults(program);
    pr_text("addi %%[v], %%[v], %%[vs]");
    pr_text("addi %%[o], %%[o], %%[os]");
    pr_text("bdnz %s", top);
    return !pr_failed;
}

/* (the ops whose values nothing reads taken out, until none is: a load or an operation on the
floating-point registers alone, which writes nothing else. pr_sweep_off: none, as before:
xbox_vsh_codegen --no-sweep) */
static int pr_sweep_off;

static int pr_pure(const char *text) {
    static const char *const names[] = {"ps_", "psq_l ", "lfs ", "lfd ", "lfsx ", "fadds ", "fsubs ", "fmuls ",
                                        "fdivs ", "fmadds ", "fmsubs ", "fnmadds ", "fnmsubs ", "fabs ", "fneg ",
                                        "fsel ", "fres ", "frsqrte ", "frsp ", "fctiwz ", "fmr "};
    size_t n;

    for (n = 0; n < sizeof(names) / sizeof(names[0]); n++)
        if (strncmp(text, names[n], strlen(names[n])) == 0)
            return 1;
    return 0;
}

/* (whether op reads only ps0 of its kth value) */
static int pr_reads_ps0(const PrOp *op, int k) {
    static const char *const names[] = {"fadds ", "fsubs ", "fmuls ", "fdivs ", "fmadds ", "fmsubs ", "fnmadds ",
                                        "fnmsubs ", "fabs ", "fneg ", "frsqrte ", "fres ", "frsp ", "fctiwz ",
                                        "fcmpu ", "fsel ", "stfs ", "stfsx "};
    size_t n;

    if (op->defines & (1u << k))
        return 0;
    for (n = 0; n < sizeof(names) / sizeof(names[0]); n++)
        if (strncmp(op->text, names[n], strlen(names[n])) == 0)
            return 1;
    return k == 2 && (strncmp(op->text, "ps_muls0 ", 9) == 0 || strncmp(op->text, "ps_madds0 ", 10) == 0);
}

/* (a value in no web but its own: no in-place write joined it) */
static int pr_alone(int v) {
    int n;

    if (v <= 0 || pr_root(v) != v || pr_values[v].loop || pr_values[v].fetched)
        return 0;
    for (n = 0; n < pr_value_count; n++)
        if (n != v && pr_root(n) == v)
            return 0;
    return 1;
}

/* (a dot product's ps_sum1 whose every reader moves its sum to ps0 (ps_merge11 x, v, v) for readers of
ps0 alone: a ps_sum0, the sum in ps0, read by them, the merges then unread: pr_sweep. The same sum's
bits. Under pr_splat_off: not, as before) */
static PrOp *pr_op_at(int n) {
    return n < pr_pre_count ? &pr_pre[n] : &pr_body[n - pr_pre_count];
}

static void pr_lanes(void) {
    int all = pr_pre_count + pr_body_count, n, m, k, j;

    for (n = 0; n < all && !pr_splat_off; n++) {
        PrOp *sum = pr_op_at(n);
        int v = sum->values[0], ok = 1;

        if (strncmp(sum->text, "ps_sum1 ", 8) != 0 || sum->count != 4 || sum->defines != 1u || !pr_alone(v))
            continue;
        /* (every reader of v a merge of its ps1 twice, each merge's value read in ps0 alone) */
        for (m = 0; m < all && ok; m++) {
            const PrOp *op = pr_op_at(m);

            for (k = 0; k < op->count && ok; k++) {
                if (op->values[k] != v || (op->defines & (1u << k)))
                    continue;
                if (strncmp(op->text, "ps_merge11 ", 11) != 0 || op->count != 3 || op->defines != 1u ||
                    op->values[1] != v || op->values[2] != v || !pr_alone(op->values[0]))
                    ok = 0;
                else
                    for (j = 0; j < all && ok; j++) {
                        const PrOp *reader = pr_op_at(j);
                        int i;

                        for (i = 0; i < reader->count; i++)
                            if (reader->values[i] == op->values[0] && !(reader->defines & (1u << i)) &&
                                !pr_reads_ps0(reader, i))
                                ok = 0;
                    }
            }
        }
        if (!ok)
            continue;
        for (m = 0; m < all; m++) {
            const PrOp *op = pr_op_at(m);

            if (strncmp(op->text, "ps_merge11 ", 11) != 0 || op->count != 3 || op->values[1] != v)
                continue;
            for (j = 0; j < all; j++) {
                PrOp *reader = pr_op_at(j);
                int i;

                for (i = 0; i < reader->count; i++)
                    if (reader->values[i] == op->values[0] && !(reader->defines & (1u << i)))
                        reader->values[i] = v;
            }
        }
        sum->text[6] = '0';
        sum->values[2] = sum->values[1];
    }
}

static void pr_sweep(void) {
    static int reads[PR_VALUES];
    int swept = 1, n, k;

    pr_lanes();

    while (swept && !pr_sweep_off) {
        swept = 0;
        memset(reads, 0, sizeof(int) * (size_t)pr_value_count);
        for (n = 0; n < pr_pre_count + pr_body_count; n++) {
            const PrOp *op = n < pr_pre_count ? &pr_pre[n] : &pr_body[n - pr_pre_count];

            for (k = 0; k < op->count; k++)
                if (!(op->defines & (1u << k)))
                    reads[pr_root(op->values[k])]++;
        }
        for (n = 0; n < pr_pre_count + pr_body_count; n++) {
            int body = n >= pr_pre_count, at = body ? n - pr_pre_count : n, dead = 1;
            PrOp *ops = body ? pr_body : pr_pre;
            const PrOp *op = &ops[at];

            if (!op->defines || !pr_pure(op->text))
                continue;
            for (k = 0; k < op->count; k++)
                if ((op->defines & (1u << k)) && reads[pr_root(op->values[k])])
                    dead = 0;
            if (!dead)
                continue;
            if (body) {
                memmove(&pr_body[at], &pr_body[at + 1], sizeof(PrOp) * (size_t)(pr_body_count - at - 1));
                pr_body_count--;
                if (pr_prefix_first > at)
                    pr_prefix_first--;
                if (pr_prefix_end > at)
                    pr_prefix_end--;
            } else {
                memmove(&pr_pre[at], &pr_pre[at + 1], sizeof(PrOp) * (size_t)(pr_pre_count - at - 1));
                pr_pre_count--;
            }
            n--;
            swept = 1;
        }
    }
}

/* registers for the values (a linear scan); 0 when there are too few */
/* (pr_allocate with resident values: each register's positions held, a bit each, and a value given
the first register none of its positions is held in: a resident one holds its register but from the
again label to its load) */
static int pr_allocate_holes(const int *order, int count, unsigned *used, int loop_start, int end) {
    enum { WORDS = (2 * PR_OPS + 1 + 63) / 64 };
    static unsigned long long held[32][WORDS];
    int n, k, w;

    if (end + 1 > 64 * WORDS)
        return 0;
    memset(held, 0, sizeof(held));
    *used = 0;
    for (n = 0; n < count; n++) {
        PrValue *v = &pr_values[order[n]];
        unsigned long long mine[WORDS];
        int from = v->first, to = v->loop || v->resident ? end + 1 : v->last > v->first ? v->last : v->first + 1, b;

        memset(mine, 0, sizeof(mine));
        for (b = from; b < to; b++)
            mine[b >> 6] |= 1ull << (b & 63);
        if (v->resident)
            for (b = loop_start; b < loop_start + pr_prefix_first; b++)
                mine[b >> 6] |= 1ull << (b & 63);
        for (k = 0; k < 32; k++) {
            for (w = 0; w < WORDS && !(held[k][w] & mine[w]); w++)
                ;
            if (w == WORDS)
                break;
        }
        if (k == 32)
            return 0;
        for (w = 0; w < WORDS; w++)
            held[k][w] |= mine[w];
        v->phys = k;
        *used |= 1u << k;
    }
    return 1;
}

static int pr_allocate(unsigned *used) {
    static int order[PR_VALUES];
    int position = 0, n, k, count = 0, loop_start = pr_pre_count, end = pr_pre_count + pr_body_count;
    int busy[32], residents = 0;

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
    for (n = 0; n < pr_value_count; n++) {
        if (pr_values[n].resident) {
            PrValue *v = &pr_values[n];

            /* (never a web's: pr_adopt copies a value the cache holds as hoisted) */
            if (pr_root(n) != n)
                return 0;
            if (v->first < 0)
                continue;
            v->last = end;
            residents = 1;
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
    if (residents)
        return pr_allocate_holes(order, count, used, loop_start, end);
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

/* (a "w" form's world slot in a register for the whole loop: pr_print. 1: the operand [ws]; 2: r12,
loaded before the loop and clobbered, where [ws] would pass GCC's 30 operands. pr_r12_off: never r12,
the slot loaded at each use there, as before: xbox_vsh_codegen --no-r12) */
static int pr_ws_register, pr_r12_off;

/* the batch function's body for the Wii as one asm statement (pr_print
writes it), or 0 when the program can't be written so */
static unsigned pr_used;

static int pr_program_once(const WlXboxVshProgram *program, const unsigned *needs, int reused,
                           const ReusePrefix *prefix) {
    int limit, n, k, compares;
    unsigned used = 0;

    /* (which constants are loaded most: those first loaded once a batch) */
    pr_key_count = 0;
    pr_counting = 1;
    pr_hoist_limit = 0;
    memset(pr_hoist_extra, 0, sizeof(pr_hoist_extra));
    memset(pr_resident, 0, sizeof(pr_resident));
    if (!pr_generate(program, needs, reused, prefix, &compares)) {
        pr_counting = 0;
        return 0;
    }
    pr_counting = 0;
    for (n = 1; n < pr_key_count; n++) {
        char key[80];
        int uses = pr_key_uses[n];
        unsigned char regions = pr_key_regions[n];
        PrKeyLoad load = pr_key_loads[n];

        snprintf(key, sizeof(key), "%s", pr_keys[n]);
        for (k = n; k > 0 && pr_key_uses[k - 1] < uses; k--) {
            memcpy(pr_keys[k], pr_keys[k - 1], sizeof(pr_keys[k]));
            pr_key_uses[k] = pr_key_uses[k - 1];
            pr_key_regions[k] = pr_key_regions[k - 1];
            pr_key_loads[k] = pr_key_loads[k - 1];
        }
        memcpy(pr_keys[k], key, sizeof(key));
        pr_key_uses[k] = uses;
        pr_key_regions[k] = regions;
        pr_key_loads[k] = load;
    }
    for (limit = pr_key_count; limit >= 0; limit--) {
        pr_hoist_limit = limit;
        if (pr_generate(program, needs, reused, prefix, &compares) && pr_allocate(&used))
            break;
    }
    if (limit < 0)
        return 0;
    /* (a fused form's loop: each later load hoisted where the registers still allow it, most used first;
    the same arithmetic, a load a vertex fewer each) */
    if (pr_fuse && limit < pr_key_count) {
        int extra;

        for (extra = limit + 1; extra < pr_key_count; extra++) {
            pr_hoist_extra[extra] = 1;
            if (!(pr_generate(program, needs, reused, prefix, &compares) && pr_allocate(&used)))
                pr_hoist_extra[extra] = 0;
        }
        /* (the code of the loads kept made again: the last try may have failed) */
        if (!(pr_generate(program, needs, reused, prefix, &compares) && pr_allocate(&used))) {
            memset(pr_hoist_extra, 0, sizeof(pr_hoist_extra));
            if (!(pr_generate(program, needs, reused, prefix, &compares) && pr_allocate(&used)))
                return 0;
        }
    }
    if (pr_fuse && !pr_swap_off && pr_key_count > 0) {
        unsigned char kept[PR_KEYS];
        long best = pr_loop_cost(), cost;
        int i, j;

        for (n = 0; n < pr_key_count; n++)
            pr_hoist_extra[n] = (unsigned char)(n < pr_hoist_limit || pr_hoist_extra[n]);
        pr_hoist_limit = 0;
        memcpy(kept, pr_hoist_extra, sizeof(kept));
        for (j = 0; j < pr_key_count; j++) {
            if (kept[j])
                continue;
            for (i = pr_key_count - 1; i >= 0; i--) {
                if (!kept[i])
                    continue;
                pr_hoist_extra[i] = 0;
                pr_hoist_extra[j] = 1;
                if (pr_generate(program, needs, reused, prefix, &compares) && pr_allocate(&used) &&
                    (cost = pr_loop_cost()) < best) {
                    best = cost;
                    kept[i] = 0;
                    kept[j] = 1;
                    break;
                }
                pr_hoist_extra[i] = 1;
                pr_hoist_extra[j] = 0;
            }
        }
        if (!(pr_generate(program, needs, reused, prefix, &compares) && pr_allocate(&used)))
            return 0;
    }
    if (pr_fuse && reused && !pr_resident_off && pr_prefix_first < pr_prefix_end && pr_key_count > 0) {
        /* (pr_resident: each constant only the rest reads, not hoisted, loaded at the prefix's end where
        the registers allow it; then each hoisted one only the prefix reads loaded where the prefix runs,
        so that one of those has its register: cheaper by pr_cost with the prefix's ops three sixteenths,
        about the share of a skinned mesh's vertices that run it) */
        long best, cost;
        int i, j;

        for (n = 0; n < pr_key_count; n++)
            pr_hoist_extra[n] = (unsigned char)(n < pr_hoist_limit || pr_hoist_extra[n]);
        pr_hoist_limit = 0;
        if (!(pr_generate(program, needs, reused, prefix, &compares) && pr_allocate(&used)))
            return 0;
        best = pr_cost(3);
        for (j = 0; j < pr_key_count; j++) {
            if (pr_hoist_extra[j] || pr_key_regions[j] != 4)
                continue;
            pr_resident[j] = 1;
            if (pr_generate(program, needs, reused, prefix, &compares) && pr_allocate(&used) && (cost = pr_cost(3)) < best)
                best = cost;
            else
                pr_resident[j] = 0;
        }
        for (i = 0; i < pr_key_count; i++) {
            if (!pr_hoist_extra[i] || pr_key_regions[i] != 2)
                continue;
            for (j = 0; j < pr_key_count; j++) {
                if (pr_hoist_extra[j] || pr_resident[j] || pr_key_regions[j] != 4)
                    continue;
                pr_hoist_extra[i] = 0;
                pr_resident[j] = 1;
                if (pr_generate(program, needs, reused, prefix, &compares) && pr_allocate(&used) &&
                    (cost = pr_cost(3)) < best) {
                    best = cost;
                    break;
                }
                pr_hoist_extra[i] = 1;
                pr_resident[j] = 0;
            }
        }
        if (!(pr_generate(program, needs, reused, prefix, &compares) && pr_allocate(&used)))
            return 0;
    }
    pr_used = used;
    return 1;
}

/* the loop's cost as pr_program_once made it: its ops (labels aside), a reused prefix's a quarter */
static long pr_loop_cost(void) {
    long cost = 0;
    int n;

    for (n = 0; n < pr_body_count; n++) {
        size_t length = strlen(pr_body[n].text);

        if (length && pr_body[n].text[length - 1] != ':')
            cost += n >= pr_prefix_first && n < pr_prefix_end ? 2 : 4;
    }
    return cost;
}

/* (the same with a reused prefix's ops weighing prefix in 16ths of the rest's) */
static long pr_cost(int prefix) {
    long cost = 0;
    int n;

    for (n = 0; n < pr_body_count; n++) {
        size_t length = strlen(pr_body[n].text);

        if (length && pr_body[n].text[length - 1] != ':')
            cost += n >= pr_prefix_first && n < pr_prefix_end ? prefix : 16;
    }
    return cost;
}

/* a program's asm (pr_program_once): with a reused prefix, the loads hoisted chosen by their count
and by their count weighted (pr_weighted), the cheaper loop kept */
static int pr_program(const WlXboxVshProgram *program, const unsigned *needs, int reused,
                      const ReusePrefix *prefix) {
    long plain;

    pr_weighted = 0;
    if (!pr_program_once(program, needs, reused, prefix))
        return 0;
    if (!reused || pr_weights_off)
        return 1;
    plain = pr_loop_cost();
    pr_weighted = 1;
    if (pr_program_once(program, needs, reused, prefix) && pr_loop_cost() < plain) {
        pr_weighted = 0;
        return 1;
    }
    pr_weighted = 0;
    return pr_program_once(program, needs, reused, prefix);
}

/* an output offset's operand (%[od0] ...) and its value: a fused function
writes the values (its operands would pass GCC's 30) */
static const struct {
    const char *name;
    int offset;
} pr_output_offsets[] = {
    {"%[od0]", (int)offsetof(WlXboxVshOutput, d0)},       {"%[od1]", (int)offsetof(WlXboxVshOutput, d1)},
    {"%[ob0]", (int)offsetof(WlXboxVshOutput, b0)},       {"%[ob1]", (int)offsetof(WlXboxVshOutput, b1)},
    {"%[ot]", (int)offsetof(WlXboxVshOutput, t)},         {"%[ofog]", (int)offsetof(WlXboxVshOutput, fog)},
    {"%[opoint]", (int)offsetof(WlXboxVshOutput, point_size)}, {"%[oclip]", (int)offsetof(WlXboxVshOutput, clip)},
    {"%[ocaptured]", (int)offsetof(WlXboxVshOutput, clip_captured)},
};

/* the asm pr_program made (a batch function's, or with pr_fuse a fused
one's; reused: the prefix kept) */
static void pr_print(FILE *out, int reused) {
    unsigned used = pr_used;
    int n, k;

    if (pr_fuse) {
        /* (each array's element 0 and stride, and the packed normal's tables, in registers; the
        count through slot, live unused: GCC's 30 operands and the GPRs) */
        fprintf(out, "\tif (count) {\n\t\tconst char *vp = (const char *)fused->inputs[1];\n"
                     "\t\tconst char *pvp = (const char *)fused->inputs[0];\n\t\tchar *op = (char *)&fused->output;\n"
                     "\t\tconst uint32_t *ep = fused->elements;\n\t\tuint32_t *const *wp = fused->words;\n"
                     "\t\tint slot[%d] __attribute__((aligned(8)));\n\t\tlong g, ra, a0, ad;\n",
                pr_spill ? PR_SPILL_AT / 4 + 2 * PR_SPILLS : 4);
        for (k = 0; k < 16; k++)
            if (pr_fused_held & (1u << k))
                fprintf(out, "\t\tconst char *d%d = (const char *)fused->data[%d];\n\t\tlong s%d = (long)fused->stride[%d];\n",
                        k, k, k, k);
        if (pr_fused_cmp())
            fprintf(out, "\t\tconst float *t11 = fused->cmp11, *t10 = fused->cmp10;\n");
        fprintf(out, "\n\t\t(void)pvp;\n\t\t(void)live;\n\t\tslot[0] = (int)count;\n\t\tslot[2] = 0;\n");
    } else
        fprintf(out, "\tif (count) {\n\t\tconst char *vp = (const char *)vs;\n\t\tchar *op = (char *)os;\n"
                     "\t\tint slot[2];\n\t\tlong g, ra, a0;\n\n");
    /* (a "w" form's world slot in a register, [ws], where the statement's operands stay within GCC's
    30, an in-and-out one ("+") counted twice; else r12) */
    pr_ws_register = 0;
    if (pr_fuse && pr_fuse->perspective == WL_XBOX_VSH_FUSED_WORLD) {
        int operands = 7 + 3 + (reused ? 2 : 0) + 6 + (pr_uses_c2 ? 1 : 0) + (pr_fused_fdata() ? 1 : 0) +
                       (pr_fused_cmp() ? 2 : 0) + 1;

        for (k = 0; k < 16; k++)
            operands += (pr_fused_held & (1u << k)) ? 2 : 0;
        pr_ws_register = operands <= 30 ? 1 : pr_r12_off ? 0 : 2;
    }
    fprintf(out, "\t\t__asm__ volatile(\n");
    for (n = 0; n < pr_pre_count + pr_body_count + 2; n++) {
        const PrOp *op;
        const char *t;

        if (n == pr_pre_count) {
            /* (the blocks' bits: live & needs != 0 into bit 4 + k; none in a fused function) */
            if (!pr_fuse) {
                fprintf(out, "\t\t\t\"li %%[g], 0\\n\\t\"\n");
                for (k = 0; k < pr_need_count && k < PR_NEED_BITS; k++)
                    fprintf(out,
                            "\t\t\t\"andi. 0, %%[live], %u\\n\\t\"\n\t\t\t\"neg 0, 0\\n\\t\"\n"
                            "\t\t\t\"srwi 0, 0, 31\\n\\t\"\n\t\t\t\"rlwimi %%[g], 0, %d, %d, %d\\n\\t\"\n",
                            pr_need_values[k], 27 - k, 4 + k, 4 + k);
                fprintf(out, "\t\t\t\"mtcrf 127, %%[g]\\n\\t\"\n");
            }
            if (pr_ws_register == 2)
                fprintf(out, "\t\t\t\"lwz 12, %d(%%[f])\\n\\t\"\n", (int)offsetof(WlXboxVshFused, world_slot));
            if (pr_fuse)
                fprintf(out, "\t\t\t\"lwz 0, 0(%%[slot])\\n\\t\"\n\t\t\t\"mtctr 0\\n\\t\"\n");
            else
                fprintf(out, "\t\t\t\"mtctr %%[n]\\n\\t\"\n");
            if (pr_first[0])
                fprintf(out, "\t\t\t\"b %s\\n\\t\"\n", pr_first);
            continue;
        }
        if (n == pr_pre_count + 1)
            continue;
        op = n < pr_pre_count ? &pr_pre[n] : &pr_body[n - pr_pre_count - 2];
        if (!pr_ws_register && strstr(op->text, "%[ws]") != NULL)
            fprintf(out, "\t\t\t\"lwz 0, %d(%%[f])\\n\\t\"\n", (int)offsetof(WlXboxVshFused, world_slot));
        fprintf(out, "\t\t\t\"");
        for (t = op->text; *t; t++) {
            size_t o;

            if (pr_ws_register != 1 && strncmp(t, "%[ws]", 5) == 0) {
                fputs(pr_ws_register ? "12" : "0", out);
                t += 4;
                continue;
            }

            for (o = 0; pr_fuse && *t == '%' && o < sizeof(pr_output_offsets) / sizeof(pr_output_offsets[0]); o++) {
                size_t length = strlen(pr_output_offsets[o].name);

                if (strncmp(t, pr_output_offsets[o].name, length) == 0) {
                    fprintf(out, "%d", pr_output_offsets[o].offset);
                    t += length;
                    break;
                }
            }
            if (!*t)
                break;
            if (*t == '@' && t[1] >= '0' && t[1] <= '3') {
                fprintf(out, "%d", pr_values[pr_root(op->values[t[1] - '0'])].phys);
                t++;
            } else {
                fputc(*t, out);
            }
        }
        fprintf(out, "\\n\\t\"\n");
    }
    if (pr_fuse)
        fprintf(out, "\t\t\t: [v] \"+b\"(vp), [g] \"=&r\"(g), [ra] \"=&b\"(ra), [a0] \"=&r\"(a0),\n"
                     "\t\t\t  [e] \"+b\"(ep), [w] \"+b\"(wp), [ad] \"=&b\"(ad)%s",
                reused ? ", [pv] \"+b\"(pvp)" : "");
    else
        fprintf(out, "\t\t\t: [v] \"+b\"(vp), [o] \"+b\"(op), [g] \"=&r\"(g), [ra] \"=&b\"(ra), [a0] \"=&r\"(a0)");
    if (pr_fuse) {
        fprintf(out, "\n\t\t\t: [o] \"b\"(op), [c] \"b\"(c), %s[k] \"b\"(wl_xbox_vsh_pairs_k), [slot] \"b\"(slot), "
                     "[f] \"b\"(fused), [ptr] \"i\"(sizeof(void *))%s",
                pr_uses_c2 ? "[c2] \"b\"(c + 128), " : "",
                pr_fused_fdata() ? ", [fdata] \"i\"(offsetof(WlXboxVshFused, data))" : "");
        for (k = 0; k < 16; k++)
            if (pr_fused_held & (1u << k))
                fprintf(out, ",\n\t\t\t  [d%d] \"b\"(d%d), [s%d] \"r\"(s%d)", k, k, k, k);
        if (pr_fused_cmp())
            fprintf(out, ",\n\t\t\t  [t11] \"b\"(t11), [t10] \"b\"(t10)");
        if (pr_ws_register == 1)
            fprintf(out, ",\n\t\t\t  [ws] \"r\"(fused->world_slot)");
        fprintf(out, "\n");
    } else {
        fprintf(out, "\n\t\t\t: [c] \"b\"(c), [c2] \"b\"(c + 128), [k] \"b\"(wl_xbox_vsh_pairs_k), "
                     "[live] \"r\"(live), [n] \"r\"(count), [slot] \"b\"(slot),\n");
    }
    if (!pr_fuse)
        fprintf(out, "\t\t\t  [vs] \"i\"(sizeof(vs[0])), [os] \"i\"(sizeof(*os)),\n");
    if (pr_fuse)
        fprintf(out, "\t\t\t: \"r0\", %s\"cr0\", \"cr1\", \"cr2\", \"cr3\", \"cr4\", \"cr5\", "
                     "\"cr6\", \"cr7\", \"ctr\", \"memory\"", pr_ws_register == 2 ? "\"r12\", " : "");
    else
        fprintf(out, "\t\t\t  "
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
    fprintf(out, ");\n\t\t(void)g;\n\t\t(void)ra;\n\t\t(void)a0;\n%s\t}\n",
            pr_fuse ? "\t\t(void)ad;\n" : "");
}
