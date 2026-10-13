// SPDX-License-Identifier: MIT
// The NV2A's register combiners (an Xbox pixel shader) on GX's texture environment (TEV).
//
// An Xbox pixel shader runs up to eight general combiner stages, each computing A*B and C*D on
// its registers, for the colour and the alpha apart, and writing them or their sum (or a choice
// of the two) back; then the final combiner, A*B + (1-A)*C + D for the colour and G for the
// alpha. A TEV stage computes d + (1-c)*a + c*b, for the colour and the alpha apart, on 8-bit
// inputs, with one texture and one rasterized colour, and writes it to one of four registers. So:
//
// - each product is a stage (c*b, or (1-c)*a for an inverted input), a sum two, and the final
//   combiner one, its lerp TEV's own;
// - registers are renamed as they are written: a combiner register is wherever its value last
//   went (a TEV register, or the texture or colour it still is until written), so a stage's
//   inputs are all read before its outputs land;
// - an input the stage cannot reach as it is (a second texture or colour, a second constant, both
//   inputs of a product inverted, a signed mapping) is first computed into a free register by a
//   stage of its own;
// - the colour and alpha halves of a combiner stage share TEV stages where their textures and
//   colours agree;
// - a half bias negate or expand negate (0.5 - x, 1 - 2x) is made in its product's own stage,
//   without a register (emit_negated_product), and the alpha's read of the blue of a colour made
//   as a product of a texture, colour or constant in the stage that reads it (inputs_folding);
// - the alpha half reads the blue of a colour that is the same in every channel from a copy made
//   in an alpha (blue_copy), and a colour read only as that blue is not made (elided);
// - the colour's and the alpha's operations of a combiner stage go into TEV stages side by side,
//   in the order an alpha written and read as the colour's input needs (queues_flush);
// - once the stages are made, halves whose results nothing reads are taken out, and a half left
//   alone in its stage moves into another's unused half (stages_compact).
//
// GX's inputs are unsigned and clamped: the signed mappings (expand, half bias) come out clamped
// to [0, 1], and a negation as 0, except in a dot product, whose signed terms TEV's lerp, bias and
// scale make exactly (the bump mapping it serves: signed dot products). A dot product of a
// register is taken as 1 (no TEV swap reaches a register's channels); a caller that knows such a
// program makes it by hand instead (WlXboxPshRecipe).
//
// A bumped reflection's cube map (DOT_RFLCT_SPEC) is read through GX's indirect unit, two stages
// before each lookup making its offset from the bump map (bumped_lookups_place), and its normal's
// lookup (DOT_RFLCT_DIFF) is the flat normal, per vertex, in colour 1: the device makes both.
//
// The stages are WiiLibs' TEV program (wl/platform/tev.h), which builds them, merges what their
// inputs need of a stage's order, hands out the registers, checks the result (wl_tev_validate)
// and applies it. The translation works in one static structure: one thread at a time.

#include "wl/xbox/psh.h"

#include <string.h>

/* ---------- constants */

enum {
    _register_zero = 0,
    _register_c0 = 1,
    _register_c1 = 2,
    _register_fog = 3,
    _register_v0 = 4,
    _register_v1 = 5,
    _register_t0 = 8,
    _register_t1 = 9,
    _register_t2 = 10,
    _register_t3 = 11,
    _register_r0 = 12,
    _register_r1 = 13,
    _register_v1r0_sum = 14,
    _register_ef_product = 15,
};

/* the combiner registers a stage can write, by name */
enum {
    _name_v0,
    _name_v1,
    _name_t0,
    _name_t1,
    _name_t2,
    _name_t3,
    _name_r0,
    _name_r1,
    NUMBER_OF_NAMES
};

enum { _portion_color, _portion_alpha, NUMBER_OF_PORTIONS };

/* an operand: where a value is */
enum {
    _kind_zero,
    _kind_one,
    _kind_half,
    /* TEV's registers: PREV, REG0, REG1, REG2 */
    _kind_register,
    /* a texture map's texel (index: the map) */
    _kind_texture,
    /* a rasterized colour (index: 0 diffuse, 1 specular) */
    _kind_ras,
    /* a constant (index: its slot) */
    _kind_konst,
    /* a blue the colour half made as a product, made again by the stage that
    reads it, in place of a register (index: its slot, products) */
    _kind_product,
};

/* what an input may leave to the stage that reads it (inputs_folding) */
enum {
    /* its half bias negate or expand negate: no register of its own */
    _fold_mapping = 1,
    /* the blue it reads, made again as a product there */
    _fold_product = 2,
};

enum {
    /* the colour (in the colour half), or the alpha (in the alpha half) */
    _channel_natural,
    /* the alpha, read in the colour half */
    _channel_alpha,
    /* the blue, read in the alpha half */
    _channel_blue,
    /* the red, green or blue in all three of the colour half's channels
    (a dot product's terms, through the swap tables or the constant's
    selection) */
    _channel_red_all,
    _channel_green_all,
    _channel_blue_all,
};

#define MAXIMUM_OPERATIONS 48

/* ---------- structures */

struct operand {
    unsigned char kind, index, channel, invert;
    /* signed negation: a product with it is subtracted (TEV's SUB) */
    unsigned char negative;
    /* a signed mapping left to the stage that reads it (emit_product): 0xa0
    half bias negate, 0.5 - x, or 0x60 expand negate, 1 - 2x, below 0 as 0 */
    unsigned char mapping;
};

struct operation {
    struct operand in[4];
    unsigned char subtract, bias, scale, dest;
    /* (its result kept signed, for a later operation's d) */
    unsigned char unclamped;
    /* (its place among the combiner stage's operations as queued, both
    portions': queues_flush keeps what the order says of their registers) */
    unsigned char sequence;
};

struct translation {
    const uint32_t *state;
    const WlXboxPshTextures *textures;
    WlXboxPshProgram *program;
    bool failed;
    const char *reason;
    /* the second attempt, for a program the exact one does not fit: the
    mappings that would take a register of their own, and the dot
    products that would take three stages, approximated */
    bool economical;
    /* inputs left to the stages that read them (inputs_folding); the last
    attempts go without, as the translation was before it had them */
    bool folding;
    /* the signed dot products (emit_signed_dot): 2 in the alpha half where
    the alpha half reads the result's blue, else in the colour half; 1 in
    the colour half; 0 taken as 1; -1 as before they were made, their
    inputs clamped */
    int signed_dots;
    /* where each name is, for each portion (a colour's may be a register's
    alpha: a value the same in every channel) */
    struct operand names[NUMBER_OF_PORTIONS][NUMBER_OF_NAMES];
    /* what holds each TEV register, for each portion (names and temporaries) */
    WlTevRegPool pools[NUMBER_OF_PORTIONS];
    /* registers whose names moved this combiner stage, freed at its end */
    unsigned char released[NUMBER_OF_PORTIONS][4];
    /* the combiner stage being translated, and its portion */
    int stage, portion_now;
    /* how each name's colour was last made, as products the alpha half can
    read the blue of (blue_recipe_set): the alpha half's reads of a blue the
    colour half wrote to a register recompute it */
    struct blue_recipe {
        bool valid;
        int count;
        struct operand x[2], y[2];
        unsigned flags;
        /* a dot product of x[0] and y[0] (the colour half's operands): the same
        in every channel */
        bool dot;
    } blue[NUMBER_OF_NAMES];
    /* the colour's names and their recipes as this combiner stage began: the
    alpha portion reads blues of those (NV2A reads every input before it
    writes any) */
    struct operand color_before[NUMBER_OF_NAMES];
    struct blue_recipe blue_before[NUMBER_OF_NAMES];
    /* an alpha register holding a name's colour, the same in every channel,
    made for the alpha half's reads of its blue (the register + 1; 0 none),
    and as this combiner stage began; whether its last reader took it over */
    unsigned char blue_copy[NUMBER_OF_NAMES], blue_copy_before[NUMBER_OF_NAMES];
    bool blue_copy_taken[NUMBER_OF_NAMES];
    /* a colour not made, read only as the alpha half's blue, made again from
    its products there; and as this combiner stage began */
    bool elided[NUMBER_OF_NAMES], elided_before[NUMBER_OF_NAMES];
    /* the products of the combiner portion's _kind_product operands */
    struct {
        struct operand x, y;
    } products[4];
    unsigned product_count;
    /* this combiner stage's operations, for each portion, merged at its end */
    struct operation queue[NUMBER_OF_PORTIONS][MAXIMUM_OPERATIONS];
    unsigned queue_count[NUMBER_OF_PORTIONS];
    unsigned char sequence;
};

/* ---------- operands */

static void fail(struct translation *translation, const char *reason) {
    if (!translation->failed)
        translation->reason = reason;
    translation->failed = true;
}

static struct operand operand(unsigned char kind, unsigned char index, unsigned char channel) {
    struct operand result;

    result.kind = kind;
    result.index = index;
    result.channel = channel;
    result.invert = 0;
    result.negative = 0;
    result.mapping = 0;
    return result;
}

static const struct operand zero = {_kind_zero, 0, 0, 0, 0, 0};
static const struct operand one = {_kind_one, 0, 0, 0, 0, 0};

/* an operand with its inversion folded where it can be (constants) */
static struct operand folded(struct operand value) {
    if (value.invert) {
        if (value.kind == _kind_zero)
            return one;
        if (value.kind == _kind_one)
            return zero;
        if (value.kind == _kind_half)
            value.invert = 0;
    }
    return value;
}

/* something approximated, and the first thing why */
static void approximate(WlXboxPshProgram *program, const char *why) {
    if (!program->approximate)
        program->approximation = why;
    program->approximate = true;
}

/* Direct3D's ARGB as the TEV program's RGBA */
static uint32_t rgba(uint32_t color) {
    return ((uint32_t)color << 8) | ((uint32_t)color >> 24);
}

static unsigned char konst_slot(struct translation *translation, uint32_t color) {
    int slot = wl_tev_konst_slot(&translation->program->tev, rgba(color));

    if (slot >= 0)
        return (unsigned char)slot;
    /* (more than four constants: the last stands in) */
    approximate(translation->program, "more than four constants");
    return 3;
}

/* a texture stage's texel, read in the portion */
static struct operand texture_operand(struct translation *translation, int stage, int portion) {
    const WlXboxPshTextures *textures = translation->textures;

    /* (what the device makes per vertex in colour 1: a normal's lookup) */
    if (textures->vertex_color & (1U << stage))
        return operand(_kind_ras, 1, _channel_natural);
    if (!(textures->present & (1U << stage)))
        return zero;
    if (portion == _portion_color) {
        if (textures->contents[stage] == WL_XBOX_PSH_TEXTURE_ALPHA)
            return one;
        return operand(_kind_texture, (unsigned char)stage, _channel_natural);
    }
    if (textures->contents[stage] == WL_XBOX_PSH_TEXTURE_COLOR)
        return one;
    if (textures->separate_alpha & (1U << stage))
        return operand(_kind_texture, (unsigned char)(stage + 4), _channel_natural);
    return operand(_kind_texture, (unsigned char)stage, _channel_natural);
}

/* where the names start: t0-t3 their textures, v0 and v1 the colours, r0
the alpha of t0 (as the hardware starts it), r1 nothing */
static void names_initialize(struct translation *translation) {
    int portion, stage;

    for (portion = 0; portion < NUMBER_OF_PORTIONS; portion++) {
        for (stage = 0; stage < 4; stage++)
            translation->names[portion][_name_t0 + stage] =
                texture_operand(translation, stage, portion);
        translation->names[portion][_name_v0] = operand(_kind_ras, 0, _channel_natural);
        translation->names[portion][_name_v1] = operand(_kind_ras, 1, _channel_natural);
        translation->names[portion][_name_r1] = zero;
    }
    /* (v0's alpha a texture's, where the device makes it one) */
    if (translation->textures->d0_alpha_map)
        translation->names[_portion_alpha][_name_v0] =
            operand(_kind_texture,
                    (unsigned char)(WL_XBOX_PSH_D0_ALPHA_MAP + translation->textures->d0_alpha_map),
                    _channel_natural);
    translation->names[_portion_color][_name_r0] = zero;
    translation->names[_portion_alpha][_name_r0] = texture_operand(translation, 0, _portion_alpha);
}

static int register_of_name(int name) {
    static const unsigned char registers[NUMBER_OF_NAMES] = {4, 5, 8, 9, 10, 11, 12, 13};

    return registers[name];
}

static int name_of(unsigned long reg) {
    switch (reg) {
    case _register_v0:
        return _name_v0;
    case _register_v1:
        return _name_v1;
    case _register_t0:
        return _name_t0;
    case _register_t1:
        return _name_t1;
    case _register_t2:
        return _name_t2;
    case _register_t3:
        return _name_t3;
    case _register_r0:
        return _name_r0;
    case _register_r1:
        return _name_r1;
    default:
        return -1;
    }
}

/* ---------- constants

GX has four konst registers. A program with more distinct constants keeps
in them those it needs in every channel (dot products, through the konst
selections) and then the rest it can, and gives the others to TEV
registers, set before its stages run and kept for it (constants_place). */

/* a constant as it is read in a portion, when that is 0 or 1 in every
channel read: no konst register for it */
static bool constant_trivial(uint32_t color, int portion, bool alpha_channel, struct operand *out) {
    unsigned r = (color >> 16) & 0xFF, g = (color >> 8) & 0xFF, b = color & 0xFF, a = color >> 24;
    unsigned value;

    if (portion == _portion_color && !alpha_channel) {
        if (r != g || g != b)
            return false;
        value = r;
    } else if (portion == _portion_alpha && !alpha_channel) {
        value = b;
    } else {
        value = a;
    }
    if (value != 0 && value != 255)
        return false;
    *out = value ? one : zero;
    return true;
}

static int preloaded_register(const struct translation *translation, uint32_t color) {
    const WlTevProgram *tev = &translation->program->tev;
    int reg;

    for (reg = WL_TEV_REG2; reg >= WL_TEV_REG0; reg--) {
        if ((tev->preload_mask & (1u << reg)) && tev->preload[reg] == rgba(color))
            return reg;
    }
    return -1;
}

static void constants_place(struct translation *translation) {
    const uint32_t *state = translation->state;
    WlXboxPshProgram *program = translation->program;
    uint32_t values[24];
    bool broadcast[24], placed[24];
    unsigned count = 0, index, konst = 0, preloads = 0;
    unsigned long combiner_count = state[WL_XBOX_PSH_COMBINER_COUNT] & 0xff;
    uint32_t combiners = state[WL_XBOX_PSH_COMBINER_COUNT];
    unsigned long stage;
    int portion, shift;

#define CONSTANT_ADD(value, wide)                                                                  \
    do {                                                                                           \
        uint32_t constant_value_ = (value);                                                        \
        unsigned constant_index_;                                                                  \
        for (constant_index_ = 0;                                                                  \
             constant_index_ < count && values[constant_index_] != constant_value_;                \
             constant_index_++)                                                                    \
            ;                                                                                      \
        if (constant_index_ == count && count < 24) {                                              \
            values[count] = constant_value_;                                                       \
            broadcast[count++] = (wide);                                                           \
        } else if (constant_index_ < count && (wide))                                              \
            broadcast[constant_index_] = true;                                                     \
    } while (0)

    if (combiner_count > 8)
        combiner_count = 8;
    for (stage = 0; stage < combiner_count; stage++) {
        for (portion = 0; portion < NUMBER_OF_PORTIONS; portion++) {
            uint32_t inputs = state[(portion == _portion_alpha ? WL_XBOX_PSH_ALPHA_INPUTS0
                                                               : WL_XBOX_PSH_RGB_INPUTS0) +
                                    stage];
            unsigned flags =
                (unsigned)(state[(portion == _portion_alpha ? WL_XBOX_PSH_ALPHA_OUTPUTS0
                                                            : WL_XBOX_PSH_RGB_OUTPUTS0) +
                                 stage] >>
                           12);

            for (shift = 24, index = 0; shift >= 0; shift -= 8, index++) {
                unsigned long reg = (inputs >> shift) & 0x0f;
                bool wide = portion == _portion_color &&
                            ((index < 2 && (flags & 0x02)) || (index >= 2 && (flags & 0x01)));
                bool alpha_channel = ((inputs >> shift) & 0x10) != 0;
                struct operand trivial;

                if ((reg == _register_c0 &&
                     constant_trivial(
                         state[WL_XBOX_PSH_CONSTANT0_0 + ((combiners & 0x1000) ? stage : 0)],
                         portion, alpha_channel, &trivial)) ||
                    (reg == _register_c1 &&
                     constant_trivial(
                         state[WL_XBOX_PSH_CONSTANT1_0 + ((combiners & 0x10000) ? stage : 0)],
                         portion, alpha_channel, &trivial)))
                    continue;
                if (reg == _register_c0)
                    CONSTANT_ADD(
                        state[WL_XBOX_PSH_CONSTANT0_0 + ((combiners & 0x1000) ? stage : 0)], wide);
                else if (reg == _register_c1)
                    CONSTANT_ADD(
                        state[WL_XBOX_PSH_CONSTANT1_0 + ((combiners & 0x10000) ? stage : 0)], wide);
                else if (reg == _register_fog && portion == _portion_color &&
                         !((inputs >> shift) & 0x10))
                    CONSTANT_ADD(translation->textures->fog_color | 0xFF000000UL, false);
            }
        }
    }
    for (shift = 0; shift < 32; shift += 8) {
        unsigned long input = (state[WL_XBOX_PSH_FINAL_INPUTS_ABCD] >> shift) & 0xff;
        unsigned long reg = input & 0x0f;
        struct operand trivial;

        if ((reg == _register_c0 &&
             constant_trivial(state[WL_XBOX_PSH_FINAL_CONSTANT0], _portion_color,
                              (input & 0x10) != 0, &trivial)) ||
            (reg == _register_c1 &&
             constant_trivial(state[WL_XBOX_PSH_FINAL_CONSTANT1], _portion_color,
                              (input & 0x10) != 0, &trivial)))
            continue;
        if (reg == _register_c0)
            CONSTANT_ADD(state[WL_XBOX_PSH_FINAL_CONSTANT0], false);
        else if (reg == _register_c1)
            CONSTANT_ADD(state[WL_XBOX_PSH_FINAL_CONSTANT1], false);
        else if (reg == _register_fog)
            CONSTANT_ADD(translation->textures->fog_color | 0xFF000000UL, false);
    }
    for (shift = 8; shift < 32; shift += 8) {
        unsigned long input = (state[WL_XBOX_PSH_FINAL_INPUTS_EFG] >> shift) & 0xff;
        unsigned long reg = input & 0x0f;
        struct operand trivial;
        /* (E and F are read as colour, G as alpha) */
        int portion_read = shift == 8 ? _portion_alpha : _portion_color;

        if ((reg == _register_c0 &&
             constant_trivial(state[WL_XBOX_PSH_FINAL_CONSTANT0], portion_read,
                              portion_read == _portion_alpha || (input & 0x10) != 0, &trivial)) ||
            (reg == _register_c1 &&
             constant_trivial(state[WL_XBOX_PSH_FINAL_CONSTANT1], portion_read,
                              portion_read == _portion_alpha || (input & 0x10) != 0, &trivial)))
            continue;
        if (reg == _register_c0)
            CONSTANT_ADD(state[WL_XBOX_PSH_FINAL_CONSTANT0], false);
        else if (reg == _register_c1)
            CONSTANT_ADD(state[WL_XBOX_PSH_FINAL_CONSTANT1], false);
    }
#undef CONSTANT_ADD
    if (count <= 4)
        return;
    /* the four konst registers: those needed in every channel, then the
    first others; the rest to REG2, REG1, REG0, kept for them */
    memset(placed, 0, sizeof(placed));
    for (index = 0; index < count && konst < 4; index++) {
        if (broadcast[index]) {
            wl_tev_konst_slot(&program->tev, rgba(values[index]));
            placed[index] = true;
            konst++;
        }
    }
    for (index = 0; index < count && konst < 4; index++) {
        if (!broadcast[index]) {
            wl_tev_konst_slot(&program->tev, rgba(values[index]));
            placed[index] = true;
            konst++;
        }
    }
    for (index = 0; index < count; index++) {
        unsigned char reg;

        if (placed[index])
            continue;
        if (preloads >= 3) {
            approximate(program, "more than seven constants");
            continue;
        }
        reg = (unsigned char)(WL_TEV_REG2 - preloads++);
        wl_tev_preload(&program->tev, reg, rgba(values[index]));
        wl_tev_pool_retain(&translation->pools[_portion_color], reg);
        wl_tev_pool_retain(&translation->pools[_portion_alpha], reg);
    }
}

/* the colour of a name whose blue the alpha portion reads, and how it was
made: as the combiner stage began (NV2A reads every input before it writes
any), unless that is a register's colour whose products are not known: then
as the colour portion left it */
static const struct operand *blue_location(const struct translation *translation, int name,
                                           const struct blue_recipe **recipe) {
    const struct operand *before = &translation->color_before[name];

    *recipe = &translation->blue_before[name];
    return before;
}

/* a register read in a portion (alpha_channel: its alpha in the colour
portion, in the alpha portion its alpha rather than its blue) */
static struct operand register_operand(struct translation *translation, unsigned long reg,
                                       bool alpha_channel, int portion, int stage) {
    const uint32_t *state = translation->state;
    struct operand result;
    int name = name_of(reg);

    if (reg == _register_c0 || reg == _register_c1) {
        uint32_t color;

        if (stage < 0) {
            color = state[reg == _register_c0 ? WL_XBOX_PSH_FINAL_CONSTANT0
                                              : WL_XBOX_PSH_FINAL_CONSTANT1];
        } else {
            uint32_t count = state[WL_XBOX_PSH_COMBINER_COUNT];
            bool unique = reg == _register_c0 ? (count & 0x1000) != 0 : (count & 0x10000) != 0;

            color =
                state[(reg == _register_c0 ? WL_XBOX_PSH_CONSTANT0_0 : WL_XBOX_PSH_CONSTANT1_0) +
                      (unique ? stage : 0)];
        }
        {
            int preloaded = preloaded_register(translation, color);
            struct operand trivial;

            if (constant_trivial(color, portion, alpha_channel, &trivial))
                return trivial;
            if (preloaded >= 0) {
                /* (a constant in a TEV register: its blue unreachable from the
                alpha half, its alpha read instead) */
                if (portion == _portion_alpha && !alpha_channel)
                    approximate(translation->program, "a preloaded constant's blue");
                return operand(_kind_register, (unsigned char)preloaded,
                               portion == _portion_color && alpha_channel ? _channel_alpha
                                                                          : _channel_natural);
            }
        }
        result = operand(_kind_konst, konst_slot(translation, color), _channel_natural);
    } else if (reg == _register_fog) {
        /* the fog register: its colour the fog's, its alpha the vertex's fog
        factor, read from the fog ramp (WL_XBOX_PSH_FOG_MAP: an intensity equal to
        its coordinate, the factor); 1 without fog */
        const WlXboxPshTextures *textures = translation->textures;

        if ((portion == _portion_alpha && alpha_channel) ||
            (portion == _portion_color && alpha_channel)) {
            if (!textures->fog)
                return one;
            result = operand(_kind_texture, WL_XBOX_PSH_FOG_MAP, _channel_natural);
            if (portion == _portion_color)
                result.channel = _channel_alpha;
            return result;
        }
        {
            int preloaded = preloaded_register(translation, textures->fog_color | 0xFF000000UL);
            struct operand trivial;

            /* (the alpha half reads the colour's blue) */
            if (constant_trivial(textures->fog_color | 0xFF000000UL, portion, false, &trivial))
                return trivial;
            if (preloaded >= 0) {
                if (portion == _portion_alpha)
                    approximate(translation->program, "a preloaded constant's blue");
                return operand(_kind_register, (unsigned char)preloaded, _channel_natural);
            }
        }
        result = operand(_kind_konst, konst_slot(translation, textures->fog_color | 0xFF000000UL),
                         portion == _portion_alpha ? _channel_blue : _channel_natural);
        return result;
    } else if (name >= 0) {
        if (portion == _portion_color) {
            result = translation->names[alpha_channel ? _portion_alpha : _portion_color][name];
            /* (an alpha that is a texture's, a colour's or a constant's blue:
            that blue in all three) */
            if (alpha_channel && result.kind >= _kind_register)
                result.channel =
                    result.channel == _channel_blue ? _channel_blue_all : _channel_alpha;
        } else if (alpha_channel) {
            result = translation->names[_portion_alpha][name];
        } else {
            /* the blue: a texture's, a colour's or a constant's through the
            swap tables; a register's alpha stands in for its blue */
            const struct blue_recipe *recipe;

            result = *blue_location(translation, name, &recipe);
            if (result.kind == _kind_register && result.channel == _channel_alpha)
                /* (a colour that is a register's alpha: its blue too) */
                result.channel = _channel_natural;
            else if (result.kind == _kind_register)
                result = translation->names[_portion_alpha][name];
            else if (result.kind >= _kind_texture)
                /* (a replicated alpha's blue is the alpha) */
                result.channel =
                    result.channel == _channel_alpha ? _channel_natural : _channel_blue;
        }
        return result;
    } else {
        return zero;
    }
    if (portion == _portion_color)
        result.channel = alpha_channel ? _channel_alpha : _channel_natural;
    else
        result.channel = alpha_channel ? _channel_natural : _channel_blue;
    return result;
}

/* ---------- registers */

/* the registers whose names moved this combiner stage: not handed out
until it ends */
static uint32_t registers_released(const struct translation *translation, int portion) {
    uint32_t mask = 0;
    int reg;

    for (reg = 0; reg < 4; reg++) {
        if (translation->released[portion][reg])
            mask |= 1u << reg;
    }
    return mask;
}

/* (PREV last: the final combiner's) */
static int register_allocate(struct translation *translation, int portion) {
    return wl_tev_pool_alloc(&translation->pools[portion],
                             registers_released(translation, portion));
}

static void register_free(struct translation *translation, int portion, int reg) {
    if (reg >= 0)
        wl_tev_pool_release(&translation->pools[portion], (uint8_t)reg);
}

/* the pool a portion's name in a register is held in: a colour's name may
stand for a register's alpha (the same in every channel), held in the
alpha's */
static int location_pool(int portion, const struct operand *location) {
    return portion == _portion_color && location->channel == _channel_alpha ? _portion_alpha
                                                                            : portion;
}

/* ---------- operations */

static void queue(struct translation *translation, int portion, const struct operation *operation) {
    if (translation->queue_count[portion] >= MAXIMUM_OPERATIONS) {
        fail(translation, "too many operations");
        return;
    }
    translation->queue[portion][translation->queue_count[portion]] = *operation;
    translation->queue[portion][translation->queue_count[portion]++].sequence =
        translation->sequence++;
}

static struct operation operation_new(int dest) {
    struct operation result;

    memset(&result, 0, sizeof(result));
    result.in[0] = result.in[1] = result.in[2] = result.in[3] = zero;
    result.dest = (unsigned char)dest;
    return result;
}

/* the constant selection an operand takes in a portion (WL_TEV_KSEL_NONE:
none) */
static unsigned char konst_selection(const struct operand *value, int portion) {
    if (portion == _portion_color) {
        if (value->kind != _kind_konst)
            return WL_TEV_KSEL_NONE;
        switch (value->channel) {
        case _channel_alpha:
            return wl_tev_kcsel(value->index, WL_TEV_K_A);
        case _channel_red_all:
            return wl_tev_kcsel(value->index, WL_TEV_K_R);
        case _channel_green_all:
            return wl_tev_kcsel(value->index, WL_TEV_K_G);
        case _channel_blue_all:
            return wl_tev_kcsel(value->index, WL_TEV_K_B);
        default:
            return wl_tev_kcsel(value->index, WL_TEV_K_RGB);
        }
    }
    switch (value->kind) {
    case _kind_one:
        return WL_TEV_KSEL_1;
    case _kind_half:
        return WL_TEV_KSEL_1_2;
    case _kind_konst:
        switch (value->channel) {
        case _channel_red_all:
            return wl_tev_kasel(value->index, WL_TEV_K_R);
        case _channel_green_all:
            return wl_tev_kasel(value->index, WL_TEV_K_G);
        case _channel_blue:
        case _channel_blue_all:
            return wl_tev_kasel(value->index, WL_TEV_K_B);
        default:
            return wl_tev_kasel(value->index, WL_TEV_K_A);
        }
    default:
        return WL_TEV_KSEL_NONE;
    }
}

/* adds what an operand takes of a stage (WlTevNeeds: a texture and the swap
tables that serve its read, a rasterized colour and its, the portion's
constant selection); false when it clashes with what is there */
static bool resources_add(WlTevNeeds *needs, const struct operand *value, int portion) {
    unsigned char konst = konst_selection(value, portion);
    unsigned char swap;

    if (konst != WL_TEV_KSEL_NONE)
        return (portion == _portion_color ? wl_tev_needs_konst_color
                                          : wl_tev_needs_konst_alpha)(needs, konst);
    if (value->kind != _kind_texture && value->kind != _kind_ras)
        return true;
    /* the swap tables the read can take (swap_tables_set), as a set: 0 as
    it is (RGBA), 1 the red everywhere, 2 the green, 3 the blue in the alpha
    (RGBB, the colour as it is) */
    switch (value->channel) {
    case _channel_red_all:
        swap = 1u << 1;
        break;
    case _channel_green_all:
        swap = 1u << 2;
        break;
    case _channel_blue_all:
    case _channel_blue:
        swap = 1u << 3;
        break;
    case _channel_alpha:
        swap = 1u << 0;
        break;
    default:
        swap = portion == _portion_color ? (1u << 0) | (1u << 3) : 1u << 0;
        break;
    }
    if (value->kind == _kind_ras)
        return wl_tev_needs_channel(needs, value->index ? WL_TEV_CH_COLOR1A1 : WL_TEV_CH_COLOR0A0,
                                    swap);
    /* (a texture stage's map, or four after for its separate alpha, at the
    stage's coordinate; the fog ramp's; D0's alpha's map) */
    if (value->index == WL_XBOX_PSH_FOG_MAP)
        return wl_tev_needs_texture(needs, WL_XBOX_PSH_FOG_TEXCOORD, WL_XBOX_PSH_FOG_TEXMAP, swap);
    if (value->index >= WL_XBOX_PSH_D0_ALPHA_MAP)
        return wl_tev_needs_texture(needs, WL_XBOX_PSH_D0_ALPHA_TEXCOORD,
                                    (uint8_t)(value->index - WL_XBOX_PSH_D0_ALPHA_MAP), swap);
    return wl_tev_needs_texture(needs, (uint8_t)(value->index & 3), value->index, swap);
}

static struct operand materialized(struct translation *translation, int portion,
                                   struct operand value);
static bool register_available(const struct translation *translation, int portion);
static bool operand_reads(const struct operand *value, int reading_portion, int reg, int pool);

/* whether an operation reads a register of its portion's pool, other than
through input `except` */
static bool operation_reads(const struct operation *operation, int portion, int reg, int except) {
    int index;

    for (index = 0; index < 4; index++) {
        if (index != except && operand_reads(&operation->in[index], portion, reg, portion))
            return true;
    }
    return false;
}

/* queues an operation whose inputs may clash: the ones that do are first
computed into registers of their own */
static void emit(struct translation *translation, int portion, struct operation *operation) {
    WlTevNeeds resources;
    int temporaries[4] = {-1, -1, -1, -1};
    int index;

    wl_tev_needs_clear(&resources);
    for (index = 0; index < 4; index++) {
        if (resources_add(&resources, &operation->in[index], portion))
            continue;
        if (register_available(translation, portion)) {
            operation->in[index] = materialized(translation, portion, operation->in[index]);
            if (operation->in[index].kind == _kind_register)
                temporaries[index] = operation->in[index].index;
        } else if (!operation_reads(operation, portion, operation->dest, index)) {
            /* (no free register: the input goes to the operation's own
            destination first, which nothing else it reads is) */
            struct operation copy = operation_new(operation->dest);
            unsigned char negative = operation->in[index].negative;

            copy.in[3] = operation->in[index];
            copy.in[3].negative = 0;
            if (copy.in[3].invert) {
                copy.in[3].invert = 0;
                copy.in[0] = one;
                copy.in[2] = copy.in[3];
                copy.in[3] = zero;
            }
            queue(translation, portion, &copy);
            operation->in[index] = operand(_kind_register, operation->dest, _channel_natural);
            operation->in[index].negative = negative;
        } else {
            fail(translation, "no free register");
        }
    }
    queue(translation, portion, operation);
    for (index = 0; index < 4; index++)
        register_free(translation, portion, temporaries[index]);
}

/* the value in a register of its own (an inverted value inverted there):
the caller frees it */
static struct operand materialized(struct translation *translation, int portion,
                                   struct operand value) {
    struct operation operation;
    struct operand copy;
    unsigned char negative;
    int reg = register_allocate(translation, portion);

    if (reg < 0) {
        fail(translation, "no free register");
        return zero;
    }
    operation = operation_new(reg);
    /* (a negation stays with the copy: -x is x's copy, negated) */
    negative = value.negative;
    value.negative = 0;
    if (value.invert) {
        /* (1 - c) * a */
        value.invert = 0;
        operation.in[0] = one;
        operation.in[2] = value;
    } else {
        operation.in[3] = value;
    }
    emit(translation, portion, &operation);
    copy = operand(_kind_register, (unsigned char)reg, _channel_natural);
    copy.negative = negative;
    return copy;
}

/* a combiner output's mapping (its flags' 0x38) as TEV's bias and scale */
static void output_mapping(struct operation *operation, unsigned flags) {
    switch (flags & 0x38) {
    case 0x08:
        operation->bias = WL_TEV_BIAS_SUBHALF;
        break;
    case 0x10:
        operation->scale = WL_TEV_SCALE_2;
        break;
    case 0x18:
        operation->bias = WL_TEV_BIAS_SUBHALF;
        operation->scale = WL_TEV_SCALE_2;
        break;
    case 0x20:
        operation->scale = WL_TEV_SCALE_4;
        break;
    case 0x30:
        operation->scale = WL_TEV_DIVIDE_2;
        break;
    default:
        break;
    }
}

/* x * y as TEV's lerp, in an operation's a, b and c: c * b, or (1 - c) * a
for an inverted input (both inverted: x first in a register of its own,
returned for the caller to free) */
static int product_lerp(struct translation *translation, int portion, struct operand x,
                        struct operand y, struct operation *operation) {
    int temporary = -1;

    if (x.invert && y.invert) {
        x = materialized(translation, portion, x);
        temporary = x.kind == _kind_register ? x.index : -1;
    }
    if (x.invert) {
        struct operand swap = x;

        x = y;
        y = swap;
    }
    if (x.kind == _kind_zero || y.kind == _kind_zero) {
        /* (nothing) */
    } else if (y.invert) {
        y.invert = 0;
        operation->in[0] = x;
        operation->in[2] = y;
    } else if (y.kind == _kind_one) {
        operation->in[0] = x;
    } else if (x.kind == _kind_one) {
        operation->in[0] = y;
    } else {
        operation->in[1] = x;
        operation->in[2] = y;
    }
    return temporary;
}

/* whether a negated mapping's product, times `scale` (1 for the half bias
negate, 2 for the expand negate), takes a combiner output's mapping in the
same stage (emit_negated_product) */
static bool negated_mapping_composes(int scale, unsigned output) {
    return scale == 1 || output == 0x00 || output == 0x10 || output == 0x30;
}

static void emit_product(struct translation *translation, int portion, struct operand x,
                         struct operand y, struct operand d, unsigned flags, int dest);

/* x * y, mapped, into dest, x's value v under a negated mapping: the half
bias negate's y * max(0, 0.5 - v), the expand negate's y * max(0, 1 - 2v).
TEV makes y * (0.5 - v) in one stage as 0 - ((1 - y) * 1/2 + y * v) + 1/2
(for an inverted y, 1 - z: 0 - ((1 - z) * v + z * 1/2) + 1/2; for y = 1,
0 - v + 1/2, v itself maybe a product), then times 2 for the expand negate,
and the stage's own clamp is the mapping's: y is not below 0, so the
product is below 0 just when 0.5 - v is, and every output mapping keeps a
value below 0 there. So the stage must write the product alone: d is 0, and
y neither negated nor mapped (inputs_folding sees to it). */
static void emit_negated_product(struct translation *translation, int portion, struct operand x,
                                 struct operand y, struct operand d, unsigned flags, int dest) {
    struct operation operation = operation_new(dest);
    struct operand value, half = operand(_kind_half, 0, 0);
    unsigned output = flags & 0x38;
    int scale, factor, temporary = -1, index;
    WlTevNeeds resources;
    bool fits = true;

    if (y.mapping) {
        struct operand swap = x;

        x = y;
        y = swap;
    }
    scale = x.mapping == 0x60 ? 2 : 1;
    value = x;
    value.mapping = 0;
    if (y.kind == _kind_zero) {
        emit_product(translation, portion, zero, zero, d, flags, dest);
        return;
    }
    if (!y.mapping && !y.negative && y.kind != _kind_product &&
        (value.kind != _kind_product || y.kind == _kind_one) && d.kind == _kind_zero &&
        negated_mapping_composes(scale, output)) {
        operation.subtract = 1;
        if (y.kind == _kind_one) {
            if (value.kind == _kind_product)
                temporary = product_lerp(translation, portion, translation->products[value.index].x,
                                         translation->products[value.index].y, &operation);
            else
                operation.in[0] = value;
        } else if (y.invert) {
            operation.in[0] = value;
            operation.in[1] = half;
            operation.in[2] = y;
            operation.in[2].invert = 0;
        } else {
            operation.in[0] = half;
            operation.in[1] = value;
            operation.in[2] = y;
        }
        /* (the 1/2 and an output's -1/2 cancel) */
        operation.bias = output == 0x08 || output == 0x18 ? WL_TEV_BIAS_ZERO : WL_TEV_BIAS_ADDHALF;
        factor = (output == 0x10 || output == 0x18 ? 2 : output == 0x20 ? 4 : 1) * scale;
        operation.scale = output == 0x30 ? (factor == 2 ? WL_TEV_SCALE_1 : WL_TEV_DIVIDE_2)
                          : factor == 4  ? WL_TEV_SCALE_4
                          : factor == 2  ? WL_TEV_SCALE_2
                                         : WL_TEV_SCALE_1;
        /* (what the stage cannot read, emit computes into a register first:
        without one, as before) */
        wl_tev_needs_clear(&resources);
        for (index = 0; index < 4; index++)
            fits = fits && resources_add(&resources, &operation.in[index], portion);
        if (fits || register_available(translation, portion)) {
            emit(translation, portion, &operation);
            register_free(translation, portion, temporary);
            return;
        }
        register_free(translation, portion, temporary);
    }
    if (value.kind == _kind_konst && !y.mapping && !y.negative && y.kind != _kind_product) {
        /* (of a constant, where 1/2 and it do not fit one stage: the constant it
        maps to, each channel, in a slot of its own) */
        uint32_t color = translation->program->tev.konst[value.index], mapped = 0;
        int shift, slot;

        for (shift = 0; shift < 32; shift += 8) {
            float v = (float)((color >> shift) & 0xFF) / 255.0f;

            v = scale == 2 ? 1.0f - 2.0f * v : 0.5f - v;
            mapped |= (uint32_t)((v < 0.0f ? 0.0f : v) * 255.0f + 0.5f) << shift;
        }
        slot = wl_tev_konst_slot(&translation->program->tev, mapped);
        if (slot >= 0) {
            value.index = (unsigned char)slot;
            emit_product(translation, portion, value, y, d, flags, dest);
            return;
        }
    }
    /* (inputs_folding makes this rare: the attempts without folding map it
    in a register of its own, as before) */
    fail(translation, "a negated mapping its stage cannot make");
}

/* x * y + d, mapped, into dest: c * b, or (1 - c) * a for an inverted input */
static void emit_product(struct translation *translation, int portion, struct operand x,
                         struct operand y, struct operand d, unsigned flags, int dest) {
    struct operation operation = operation_new(dest);
    int temporary = -1, d_temporary = -1;

    x = folded(x);
    y = folded(y);
    if (x.mapping || y.mapping) {
        emit_negated_product(translation, portion, x, y, d, flags, dest);
        return;
    }
    /* (a blue made again here: its own product, times 1) */
    if (y.kind == _kind_product) {
        struct operand swap = x;

        x = y;
        y = swap;
    }
    if (x.kind == _kind_product && y.kind != _kind_zero) {
        if (y.kind != _kind_one || y.negative) {
            fail(translation, "a product made again, not times 1");
            return;
        }
        emit_product(translation, portion, translation->products[x.index].x,
                     translation->products[x.index].y, d, flags, dest);
        return;
    }
    if (x.kind == _kind_product)
        x = zero;
    /* (a negated factor: the product subtracted from d) */
    operation.subtract = (unsigned char)(x.negative ^ y.negative);
    x.negative = y.negative = 0;
    if (d.invert) {
        d = materialized(translation, portion, d);
        d_temporary = d.kind == _kind_register ? d.index : -1;
    }
    temporary = product_lerp(translation, portion, x, y, &operation);
    operation.in[3] = d;
    output_mapping(&operation, flags);
    emit(translation, portion, &operation);
    register_free(translation, portion, temporary);
    register_free(translation, portion, d_temporary);
}

/* ---------- dot products

dot(x, y) over red, green and blue, in all three: TEV has no dot product,
but its swap tables put one channel of a texture or rasterized colour in
all three (swap_tables_set), and its constant selections one channel of
a constant: three products, summed in a register. Of two constants, the dot
product is a constant. Neither can be a register (no swap reaches them): the
product is then taken as 1, and the program marked approximate. */

/* an operand's value per channel, when it is a constant */
static bool constant_value(const struct translation *translation, const struct operand *value,
                           float out[3]) {
    int channel;
    float v[4];

    switch (value->kind) {
    case _kind_zero:
        v[0] = v[1] = v[2] = v[3] = 0.0f;
        break;
    case _kind_one:
        v[0] = v[1] = v[2] = v[3] = 1.0f;
        break;
    case _kind_half:
        v[0] = v[1] = v[2] = v[3] = 0.5f;
        break;
    case _kind_konst: {
        uint32_t color = translation->program->tev.konst[value->index];

        v[0] = (float)(color >> 24) / 255.0f;
        v[1] = (float)((color >> 16) & 0xFF) / 255.0f;
        v[2] = (float)((color >> 8) & 0xFF) / 255.0f;
        v[3] = (float)(color & 0xFF) / 255.0f;
        break;
    }
    default:
        return false;
    }
    for (channel = 0; channel < 3; channel++) {
        float x = value->channel == _channel_alpha       ? v[3]
                  : value->channel == _channel_red_all   ? v[0]
                  : value->channel == _channel_green_all ? v[1]
                  : value->channel == _channel_blue_all  ? v[2]
                                                         : v[channel];

        out[channel] = value->invert ? 1.0f - x : x;
    }
    return true;
}

/* one channel of an operand in all three of the colour's */
static struct operand broadcast(struct operand value, int channel) {
    static const unsigned char channels[3] = {_channel_red_all, _channel_green_all,
                                              _channel_blue_all};

    /* (the same in every channel already: constants, alphas, one channel in
    all three) */
    if (value.kind == _kind_zero || value.kind == _kind_one || value.kind == _kind_half ||
        value.channel == _channel_alpha || value.channel == _channel_red_all ||
        value.channel == _channel_green_all || value.channel == _channel_blue_all)
        return value;
    value.channel = channels[channel];
    return value;
}

static bool broadcastable(const struct operand *value) {
    return value->kind != _kind_register || value->channel == _channel_alpha;
}

/* whether a dot product's two inputs (the low two bytes of `inputs`) read a
texture stage GX cannot sample, still unwritten (a dependent lookup's): the
product is then taken as 1. (A cube map sampled as a sphere map is read as
any texture.) */
static bool dot_unavailable(struct translation *translation, unsigned long inputs, int stage) {
    int which;

    (void)stage;
    for (which = 0; which < 2; which++) {
        unsigned long reg = (inputs >> (8 * which)) & 0x0f;
        int name = name_of(reg);

        if (reg >= _register_t0 && reg <= _register_t3 &&
            (translation->textures->unavailable & (1u << (reg - _register_t0))) &&
            translation->names[_portion_color][name].kind == _kind_zero) {
            approximate(translation->program, "a dot product with a texture GX cannot sample");
            return true;
        }
    }
    return false;
}

static struct operand emit_dot(struct translation *translation, int portion, struct operand x,
                               struct operand y, int *temporary) {
    float a[3], b[3];
    unsigned char negative = (unsigned char)(x.negative ^ y.negative);
    struct operand result;
    unsigned terms;
    int reg, channel;

    *temporary = -1;
    x = folded(x);
    y = folded(y);
    x.negative = y.negative = 0;
    if (constant_value(translation, &x, a) && constant_value(translation, &y, b)) {
        float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        uint32_t level;

        if (dot > 1.0f)
            dot = 1.0f;
        level = (uint32_t)(dot * 255.0f + 0.5f);
        result =
            operand(_kind_konst,
                    konst_slot(translation, (level << 24) | (level << 16) | (level << 8) | level),
                    _channel_natural);
        result.negative = negative;
        return result;
    }
    /* (the terms whose constant is 0 are left out: a constant of one
    channel, as a program uses to pick a channel, is one stage) */
    terms = 0;
    for (channel = 0; channel < 3; channel++) {
        bool a_constant = constant_value(translation, &x, a),
             b_constant = constant_value(translation, &y, b);

        if (!((a_constant && a[channel] == 0.0f) || (b_constant && b[channel] == 0.0f)))
            terms |= 1u << channel;
    }
    if (!terms)
        return zero;
    if (!broadcastable(&x) || !broadcastable(&y) ||
        (translation->economical && (terms & (terms - 1)))) {
        approximate(translation->program, translation->economical ? "a dot product, economically"
                                                                  : "a dot product of a register");
        return one;
    }
    reg = register_allocate(translation, portion);
    if (reg < 0) {
        approximate(translation->program, "a dot product without a register");
        return one;
    }
    result = operand(_kind_register, (unsigned char)reg, _channel_natural);
    {
        bool first = true;

        for (channel = 0; channel < 3; channel++) {
            if (!(terms & (1u << channel)))
                continue;
            emit_product(translation, portion, broadcast(x, channel), broadcast(y, channel),
                         first ? zero : result, 0, reg);
            first = false;
        }
    }
    *temporary = reg;
    result.negative = negative;
    return result;
}

/* ---------- signed dot products

A dot product whose inputs are mapped to signed values (expand, 2v - 1;
half bias, v - 1/2; and their negations: each k(v - 1/2)), of values a swap
table or a constant's selection puts one channel of in all three (the bump
map against a normalization cube map's direction, or a constant). TEV's
inputs are unsigned, but its lerp, d + (1 - c)a + cb, with its bias of -1/2
and its scale, makes each term exactly:

    (x - 1/2)(y - 1/2) = (lerp(1 - x, x, y) - 1/2) / 2
    (x - 1/2) y        = lerp(1/2, x, y) - 1/2
    (x - 1/2)(1 - y)   = lerp(x, 1/2, y) - 1/2

A channel at a time, x (and 1 - x) go to registers, then the term is read
with c = y and added to the sum so far, as d. The sum stays signed (TEV keeps
-4 to 4 unclamped, for a d) until the last term, which scales it and clamps
it to [0, 1]: a dot product below 0 is 0, as any register below 0 is here.

In the colour half, 1 - x and x are one stage's colour and alpha: two stages
a channel. Where the alpha half reads the result's blue later, the sum is
made in the alpha half instead, three stages a channel (x, 1 - x, the term),
and the colour reads it as that register's alpha. */

/* a signed mapping as k(v - 1/2) */
static bool mapping_signed(unsigned mapping, int *k) {
    switch (mapping) {
    case 0x40:
        *k = 2;
        return true;
    case 0x60:
        *k = -2;
        return true;
    case 0x80:
        *k = 1;
        return true;
    case 0xa0:
        *k = -1;
        return true;
    default:
        return false;
    }
}

/* a value in [0, 1] under a combiner input's mapping, as NV2A's (signed) */
static float mapped_value(float v, unsigned mapping) {
    switch (mapping) {
    case 0x20:
        return 1.0f - v;
    case 0x40:
        return 2.0f * v - 1.0f;
    case 0x60:
        return 1.0f - 2.0f * v;
    case 0x80:
        return v - 0.5f;
    case 0xa0:
        return 0.5f - v;
    case 0xe0:
        return -v;
    default:
        return v;
    }
}

/* a colour-half operand (one channel in all three, or a value the same in
every channel) as the alpha half reads it */
static struct operand alpha_form(struct operand value) {
    if (value.kind == _kind_register || value.channel == _channel_alpha)
        value.channel = _channel_natural;
    return value;
}

/* whether one TEV stage can read what these halves read */
static bool stage_fits(const struct operation *color, const struct operation *alpha) {
    WlTevNeeds color_resources, alpha_resources;
    int index;

    wl_tev_needs_clear(&color_resources);
    wl_tev_needs_clear(&alpha_resources);
    for (index = 0; index < 4; index++) {
        if ((color && !resources_add(&color_resources, &color->in[index], _portion_color)) ||
            (alpha && !resources_add(&alpha_resources, &alpha->in[index], _portion_alpha)))
            return false;
    }
    return wl_tev_needs_merge(&color_resources, &alpha_resources);
}

static bool stage_append(struct translation *translation, const struct operation *color,
                         const struct operation *alpha);

enum { MAXIMUM_SIGNED_DOT_STAGES = 9 };

/* a signed dot product's TEV stages, for x and y in this order (x signed),
of the terms (channels) not 0: false when a stage cannot read what it must */
static bool signed_dot_stages(struct operand x, struct operand y, bool both_signed, int sign,
                              bool invert_y, int scale, bool in_alpha, unsigned terms, int sum,
                              int low, int high, struct operation *color, struct operation *alpha,
                              bool *has_color, bool *has_alpha, int *count) {
    static const unsigned char scales[] = {WL_TEV_DIVIDE_2, WL_TEV_SCALE_1, WL_TEV_SCALE_2, 0,
                                           WL_TEV_SCALE_4};
    int channel;
    bool first_term = true;

    *count = 0;
    for (channel = 0; channel < 3; channel++) {
        struct operand bx = broadcast(x, channel), by = broadcast(y, channel);
        struct operation *term;
        struct operand a, b;
        int first = *count;

        if (!(terms & (1u << channel)))
            continue;
        /* x, and 1 - x: (1 - c) * 1 */
        if (!in_alpha) {
            color[*count] = operation_new(both_signed ? low : high);
            if (both_signed) {
                color[*count].in[0] = one;
                color[*count].in[2] = bx;
                alpha[*count] = operation_new(high);
                alpha[*count].in[3] = alpha_form(bx);
            } else {
                color[*count].in[3] = bx;
            }
            has_color[*count] = true;
            has_alpha[*count] = both_signed;
            ++*count;
        } else {
            alpha[*count] = operation_new(high);
            alpha[*count].in[3] = alpha_form(bx);
            has_color[*count] = false;
            has_alpha[*count] = true;
            ++*count;
            if (both_signed) {
                alpha[*count] = operation_new(low);
                alpha[*count].in[0] = one;
                alpha[*count].in[2] = alpha_form(bx);
                has_color[*count] = false;
                has_alpha[*count] = true;
                ++*count;
            }
        }
        /* the term, added to the sum so far */
        term = in_alpha ? &alpha[*count] : &color[*count];
        *term = operation_new(sum);
        if (both_signed) {
            struct operand x_low = operand(_kind_register, (unsigned char)low, _channel_natural);
            struct operand x_high = operand(_kind_register, (unsigned char)high,
                                            in_alpha ? _channel_natural : _channel_alpha);

            /* (a negative product: lerp(x, 1 - x, y), its negation) */
            a = sign > 0 ? x_low : x_high;
            b = sign > 0 ? x_high : x_low;
            term->bias = WL_TEV_BIAS_SUBHALF;
        } else {
            struct operand x_high = operand(_kind_register, (unsigned char)high, _channel_natural);

            a = invert_y ? x_high : operand(_kind_half, 0, 0);
            b = invert_y ? operand(_kind_half, 0, 0) : x_high;
            /* (a negative product subtracted: d - lerp + 1/2) */
            term->subtract = (unsigned char)(sign < 0);
            term->bias = sign < 0 ? WL_TEV_BIAS_ADDHALF : WL_TEV_BIAS_SUBHALF;
        }
        term->in[0] = a;
        term->in[1] = b;
        term->in[2] = in_alpha ? alpha_form(by) : by;
        term->in[3] =
            first_term ? zero : operand(_kind_register, (unsigned char)sum, _channel_natural);
        first_term = false;
        /* (the last term scales the sum and clamps it) */
        if (terms >> (channel + 1))
            term->unclamped = 1;
        else
            term->scale = scales[scale];
        has_color[*count] = !in_alpha;
        has_alpha[*count] = in_alpha;
        ++*count;
        for (; first < *count; first++) {
            if (!stage_fits(has_color[first] ? &color[first] : NULL,
                            has_alpha[first] ? &alpha[first] : NULL))
                return false;
        }
    }
    return true;
}

/* dot(x, y) under their mappings (one of them signed), x and y read through
a swap or a selection: into a register of its own (*temporary), the
colour's, or (in_alpha) the alpha's, read by the colour as its alpha. false
when it cannot be made so (nothing made) */
static bool emit_signed_dot(struct translation *translation, struct operand x, unsigned x_mapping,
                            struct operand y, unsigned y_mapping, bool in_alpha,
                            struct operand *result, int *temporary) {
    struct operation color[MAXIMUM_SIGNED_DOT_STAGES], alpha[MAXIMUM_SIGNED_DOT_STAGES];
    bool has_color[MAXIMUM_SIGNED_DOT_STAGES], has_alpha[MAXIMUM_SIGNED_DOT_STAGES];
    float a[3], b[3];
    int kx = 0, ky = 0, sign, scale, count = 0, attempt, index;
    int pool = in_alpha ? _portion_alpha : _portion_color;
    int sum, low = -1, high, low_pool = in_alpha ? _portion_alpha : _portion_color;
    int high_pool = _portion_alpha;
    bool x_signed = mapping_signed(x_mapping, &kx), y_signed = mapping_signed(y_mapping, &ky),
         both_signed;
    bool invert_y, made = false;
    bool x_constant, y_constant;
    unsigned terms = 0;
    int channel;

    *temporary = -1;
    /* (any queued operation of this combiner stage would come after the
    stages made here: there must be none) */
    if ((!x_signed && !y_signed) || translation->queue_count[_portion_color] ||
        translation->queue_count[_portion_alpha])
        return false;
    if (!x_signed) {
        struct operand swap = x;
        unsigned swap_mapping = x_mapping;

        x = y;
        y = swap;
        x_mapping = y_mapping;
        y_mapping = swap_mapping;
        kx = ky;
        y_signed = false;
    }
    both_signed = y_signed;
    /* (of two constants, a constant: 0 and 1 without a konst register) */
    x_constant = constant_value(translation, &x, a);
    y_constant = constant_value(translation, &y, b);
    if (x_constant && y_constant) {
        float dot = 0.0f;
        uint32_t level;

        for (channel = 0; channel < 3; channel++)
            dot += mapped_value(a[channel], x_mapping) * mapped_value(b[channel], y_mapping);
        dot = dot < 0.0f ? 0.0f : dot > 1.0f ? 1.0f : dot;
        level = (uint32_t)(dot * 255.0f + 0.5f);
        *result = level == 0     ? zero
                  : level == 255 ? one
                                 : operand(_kind_konst,
                                           konst_slot(translation, (level << 24) | (level << 16) |
                                                                       (level << 8) | level),
                                           _channel_natural);
        return true;
    }
    /* (the terms whose constant maps to 0 are left out) */
    for (channel = 0; channel < 3; channel++) {
        if (!(x_constant && mapped_value(a[channel], x_mapping) == 0.0f) &&
            !(y_constant && mapped_value(b[channel], y_mapping) == 0.0f))
            terms |= 1u << channel;
    }
    if (!terms) {
        *result = zero;
        return true;
    }
    /* the product's sign and its scale: k(x - 1/2) k'(y - 1/2) is
    kk'/2 (lerp(...) - 1/2); k(x - 1/2) y is k (lerp(...) - 1/2) */
    invert_y = !both_signed && y_mapping == 0x20;
    if (both_signed) {
        sign = kx * ky > 0 ? 1 : -1;
        scale = (kx * ky > 0 ? kx * ky : -kx * ky) / 2;
    } else {
        sign = (kx > 0 ? 1 : -1) * (y_mapping == 0xe0 ? -1 : 1);
        scale = kx > 0 ? kx : -kx;
    }
    /* (the colour half's x is 1 - x, of x in the alpha's: x for a product
    with an unsigned value) */
    if (!in_alpha && !both_signed)
        high_pool = _portion_color;
    sum = register_allocate(translation, pool);
    high = register_allocate(translation, high_pool);
    if (both_signed)
        low = register_allocate(translation, low_pool);
    if (sum >= 0 && high >= 0 && (!both_signed || low >= 0)) {
        /* (either way round, for two signed values: the one a constant
        selection reaches is better read as c) */
        for (attempt = 0; attempt < (both_signed ? 2 : 1) && !made; attempt++) {
            made =
                attempt
                    ? signed_dot_stages(y, x, both_signed, sign, invert_y, scale, in_alpha, terms,
                                        sum, low, high, color, alpha, has_color, has_alpha, &count)
                    : signed_dot_stages(x, y, both_signed, sign, invert_y, scale, in_alpha, terms,
                                        sum, low, high, color, alpha, has_color, has_alpha, &count);
        }
    }
    for (index = 0; made && index < count; index++) {
        if (!stage_append(translation, has_color[index] ? &color[index] : NULL,
                          has_alpha[index] ? &alpha[index] : NULL))
            break;
    }
    register_free(translation, low_pool, low);
    register_free(translation, high_pool, high);
    if (!made) {
        register_free(translation, pool, sum);
        return false;
    }
    *temporary = sum;
    *result =
        operand(_kind_register, (unsigned char)sum, in_alpha ? _channel_alpha : _channel_natural);
    return true;
}

/* ---------- combiner inputs */

/* ---------- blues for the alpha half

The alpha half of a TEV stage reads alphas, and the blue of a texture, a
rasterized colour or a constant through the swap tables and the konst
selections, but not a register's colour. A combiner's alpha portion may read
the blue of what its colour portion wrote: the products that made it are
kept, when the alpha half can read their blues, and computed again there. */

/* a colour-half operand as the alpha half reads its blue; false when it
cannot (a register's colour, which may change) */
static bool blue_operand(struct operand value, struct operand *out) {
    switch (value.kind) {
    case _kind_zero:
    case _kind_one:
    case _kind_half:
        break;
    case _kind_texture:
    case _kind_ras:
    case _kind_konst:
        if (value.channel == _channel_alpha)
            value.channel = _channel_natural;
        else if (value.channel == _channel_natural)
            value.channel = _channel_blue;
        else
            return false;
        break;
    default:
        return false;
    }
    *out = value;
    return true;
}

/* a name's colour just written as x0*y0 (+ x1*y1), mapped by flags */
static void blue_recipe_set(struct translation *translation, unsigned long reg, int count,
                            const struct operand *x, const struct operand *y, unsigned flags) {
    int name = name_of(reg), index;
    struct blue_recipe *recipe;

    if (name < 0)
        return;
    recipe = &translation->blue[name];
    recipe->valid = true;
    recipe->dot = false;
    recipe->count = count;
    recipe->flags = flags;
    for (index = 0; index < count; index++) {
        if (!blue_operand(x[index], &recipe->x[index]) ||
            !blue_operand(y[index], &recipe->y[index]))
            recipe->valid = false;
    }
}

/* a name's colour just written as a dot product (unmapped: its blue is the
dot product itself, made again in the alpha half) */
static void blue_recipe_dot(struct translation *translation, unsigned long reg, struct operand x,
                            struct operand y, unsigned flags) {
    int name = name_of(reg);
    struct blue_recipe *recipe;

    if (name < 0)
        return;
    recipe = &translation->blue[name];
    recipe->valid = !(flags & 0x38) && broadcastable(&x) && broadcastable(&y);
    recipe->dot = true;
    recipe->count = 1;
    recipe->flags = flags;
    recipe->x[0] = x;
    recipe->y[0] = y;
}

static void blue_recipe_forget(struct translation *translation, unsigned long reg) {
    int name = name_of(reg);

    if (name >= 0)
        translation->blue[name].valid = false;
}

static struct operand mapped_input(struct translation *translation, struct operand value,
                                   unsigned mapping, int portion, int *temporary);

static bool blue_read_later(const struct translation *translation, int after_stage, int name);

/* one general combiner input, mapped: signed mappings clamped (folding:
what inputs_folding leaves to the stage that reads it) */
static struct operand combiner_input(struct translation *translation, unsigned long input,
                                     int portion, int stage, unsigned folding, int *temporary) {
    struct operand value =
        register_operand(translation, input & 0x0f, (input & 0x10) != 0, portion, stage);
    unsigned mapping = input & 0xe0;
    const struct blue_recipe *recipe = NULL;
    const struct operand *location =
        portion == _portion_alpha && !(input & 0x10) && name_of(input & 0x0f) >= 0
            ? blue_location(translation, name_of(input & 0x0f), &recipe)
            : NULL;

    *temporary = -1;
    if (location && translation->blue_copy_before[name_of(input & 0x0f)]) {
        /* (a copy of the colour in an alpha: its blue, taken over by its last
        reader, freed after it) */
        int name = name_of(input & 0x0f), copy = translation->blue_copy_before[name] - 1;

        value = operand(_kind_register, (unsigned char)copy, _channel_natural);
        location = NULL;
        if (stage >= 0 && !translation->blue_copy_taken[name] &&
            translation->blue_copy[name] == copy + 1 &&
            !blue_read_later(translation, stage, name)) {
            translation->blue_copy[name] = 0;
            translation->blue_copy_taken[name] = true;
            *temporary = copy;
        }
    }
    /* (the blue of a name the colour half wrote to a register: made again
    from its products, or its alpha in its place) */
    if (location &&
        ((location->kind == _kind_register && location->channel != _channel_alpha) ||
         translation->elided_before[name_of(input & 0x0f)]) &&
        (folding & _fold_product) && recipe->valid && !recipe->dot && recipe->count == 1 &&
        !(recipe->flags & 0x38) && !recipe->x[0].negative && !recipe->y[0].negative &&
        !recipe->x[0].mapping && !recipe->y[0].mapping && translation->product_count < 4) {
        /* (one product, times 1 where it is read: made again in that stage,
        no register of its own) */
        unsigned slot = translation->product_count++;

        translation->products[slot].x = recipe->x[0];
        translation->products[slot].y = recipe->y[0];
        value = operand(_kind_product, (unsigned char)slot, _channel_natural);
    } else if (location &&
               ((location->kind == _kind_register && location->channel != _channel_alpha) ||
                translation->elided_before[name_of(input & 0x0f)])) {
        int blue =
            recipe->valid && !recipe->dot ? register_allocate(translation, _portion_alpha) : -1;

        if (recipe->valid && recipe->dot) {
            value = emit_dot(translation, _portion_alpha, recipe->x[0], recipe->y[0], temporary);
            /* (a negated dot product of values not below 0: the colour wrote
            it clamped, 0) */
            if (value.negative) {
                register_free(translation, _portion_alpha, *temporary);
                *temporary = -1;
                value = zero;
            }
        } else if (blue >= 0) {
            emit_product(translation, _portion_alpha, recipe->x[0], recipe->y[0], zero,
                         recipe->count == 1 ? recipe->flags : 0, blue);
            if (recipe->count == 2)
                emit_product(translation, _portion_alpha, recipe->x[1], recipe->y[1],
                             operand(_kind_register, (unsigned char)blue, _channel_natural),
                             recipe->flags, blue);
            value = operand(_kind_register, (unsigned char)blue, _channel_natural);
            *temporary = blue;
        } else {
            approximate(translation->program, "the blue of a register, read as its alpha");
        }
    }
    switch (mapping) {
    case 0x00:
    case 0xc0:
        return value;
    case 0x20:
        value.invert = 1;
        return folded(value);
    case 0xe0:
        /* signed negation: -x, which a sum takes as a subtraction */
        if (value.kind == _kind_zero)
            return zero;
        value.negative = 1;
        return value;
    default:
        break;
    }
    /* the mappings computed into a register of their own, clamped to [0, 1]
    as TEV's registers are: expand 2x - 1, half bias x - 0.5, expand
    negate 1 - 2x, half bias negate 0.5 - x */
    if (value.kind == _kind_zero)
        return mapping == 0x40 || mapping == 0x80 ? zero
               : mapping == 0x60                  ? one
                                                  : operand(_kind_half, 0, 0);
    if (value.kind == _kind_one)
        return mapping == 0x40 ? one : mapping == 0x80 ? operand(_kind_half, 0, 0) : zero;
    if (value.kind == _kind_half)
        return zero;
    if ((folding & _fold_mapping) && (mapping == 0x60 || mapping == 0xa0)) {
        /* (left to the stage that reads it, emit_negated_product; a blue
        made again stays in its register until then) */
        value.mapping = (unsigned char)mapping;
        return value;
    }
    if (*temporary >= 0) {
        /* (a blue made again, now read by the mapping's own stage: its
        register free after it) */
        int blue = *temporary;
        struct operand mapped;

        *temporary = -1;
        mapped = mapped_input(translation, value, mapping, portion, temporary);
        register_free(translation, portion, blue);
        return mapped;
    }
    return mapped_input(translation, value, mapping, portion, temporary);
}

/* the mappings computed into a register of their own (combiner_input) */
static struct operand mapped_input(struct translation *translation, struct operand value,
                                   unsigned mapping, int portion, int *temporary) {
    struct operation operation;
    int reg;

    reg = translation->economical && (mapping == 0x60 || mapping == 0xa0)
              ? -1
              : register_allocate(translation, portion);
    if (reg < 0 && (mapping == 0x60 || mapping == 0xa0)) {
        /* (no register: expand negate as an inversion, half bias negate as
        0, as they were before they had one) */
        approximate(translation->program, "a negated mapping");
        if (mapping == 0xa0)
            return zero;
        value.invert = 1;
        return folded(value);
    }
    if (reg < 0) {
        fail(translation, "no free register");
        return zero;
    }
    operation = operation_new(reg);
    if (mapping == 0x40 || mapping == 0x80) {
        /* (x - 0.5), times 2 for the expansion */
        operation.in[3] = value;
        operation.bias = WL_TEV_BIAS_SUBHALF;
        operation.scale = mapping == 0x40 ? WL_TEV_SCALE_2 : WL_TEV_SCALE_1;
    } else {
        /* (0.5 - x), times 2 for the expansion: TEV's subtraction, d - a */
        operation.in[3] = operand(_kind_half, 0, 0);
        operation.in[0] = value;
        operation.subtract = 1;
        operation.scale = mapping == 0x60 ? WL_TEV_SCALE_2 : WL_TEV_SCALE_1;
    }
    emit(translation, portion, &operation);
    *temporary = reg;
    return operand(_kind_register, (unsigned char)reg, _channel_natural);
}

/* ---------- liveness */

/* which of a name's channels an input byte reads in a portion: its colour
(_portion_color) or its alpha; the alpha portion's blue reads the colour */
static int input_reads(unsigned long input, int portion) {
    (void)portion;
    return (input & 0x10) ? _portion_alpha : _portion_color;
}

/* whether a name's channel (portion) is read after the combiner stage (the
later stages and the final combiner), as far as can be told */
static bool name_read_later(const struct translation *translation, int after_stage, int name,
                            int channel) {
    const uint32_t *state = translation->state;
    unsigned long count = state[WL_XBOX_PSH_COMBINER_COUNT] & 0xff;
    uint32_t abcd = state[WL_XBOX_PSH_FINAL_INPUTS_ABCD];
    uint32_t efg = state[WL_XBOX_PSH_FINAL_INPUTS_EFG];
    unsigned long stage;
    int portion, shift;

    if (count > 8)
        count = 8;
    for (stage = (unsigned long)(after_stage + 1); stage < count; stage++) {
        uint32_t outputs = state[WL_XBOX_PSH_RGB_OUTPUTS0 + stage];

        for (portion = 0; portion < NUMBER_OF_PORTIONS; portion++) {
            uint32_t inputs = state[(portion == _portion_alpha ? WL_XBOX_PSH_ALPHA_INPUTS0
                                                               : WL_XBOX_PSH_RGB_INPUTS0) +
                                    stage];

            for (shift = 0; shift < 32; shift += 8) {
                unsigned long input = (inputs >> shift) & 0xff;

                if (name_of(input & 0x0f) == name && input_reads(input, portion) == channel)
                    return true;
            }
            outputs = state[(portion == _portion_alpha ? WL_XBOX_PSH_ALPHA_OUTPUTS0
                                                       : WL_XBOX_PSH_RGB_OUTPUTS0) +
                            stage];
            /* (the mux reads r0's alpha) */
            if (((outputs >> 12) & 0x04) && name == _name_r0 && channel == _portion_alpha)
                return true;
        }
        /* written again by this stage, which does not read it: what it held
        is read no more (a stage reads every input before it writes) */
        outputs = state[(channel == _portion_alpha ? WL_XBOX_PSH_ALPHA_OUTPUTS0
                                                   : WL_XBOX_PSH_RGB_OUTPUTS0) +
                        stage];
        if (name_of(outputs & 0x0f) == name || name_of((outputs >> 4) & 0x0f) == name ||
            name_of((outputs >> 8) & 0x0f) == name)
            return false;
    }
    if (!abcd && !efg)
        return name == _name_r0;
    for (shift = 0; shift < 32; shift += 8) {
        unsigned long input = (abcd >> shift) & 0xff;

        if (name_of(input & 0x0f) == name && input_reads(input, _portion_color) == channel)
            return true;
        if ((input & 0x0f) == _register_v1r0_sum && channel == _portion_color &&
            (name == _name_r0 || name == _name_v1))
            return true;
        if ((input & 0x0f) == _register_ef_product) {
            unsigned long e = (efg >> 24) & 0xff, f = (efg >> 16) & 0xff;

            if ((name_of(e & 0x0f) == name && input_reads(e, _portion_color) == channel) ||
                (name_of(f & 0x0f) == name && input_reads(f, _portion_color) == channel))
                return true;
        }
    }
    {
        unsigned long g = (efg >> 8) & 0xff;

        if (name_of(g & 0x0f) == name && input_reads(g, _portion_alpha) == channel)
            return true;
    }
    return false;
}

/* a value the same in every channel */
static bool uniform(const struct operand *value) {
    return value->kind <= _kind_half || value->channel == _channel_alpha;
}

/* whether a combiner stage's alpha portion reads a name's blue */
static bool blue_read_now(const struct translation *translation, int stage, int name) {
    uint32_t inputs = translation->state[WL_XBOX_PSH_ALPHA_INPUTS0 + stage];
    int shift;

    for (shift = 0; shift < 32; shift += 8) {
        unsigned long input = (inputs >> shift) & 0xff;

        if (name_of(input & 0x0f) == name && !(input & 0x10))
            return true;
    }
    return false;
}

/* whether a combiner stage's colour output (4 bits a name, at shift) is a
product of the name it writes, read as its colour, and alphas or 0: a
value the same in every channel where the name's was (made in the alpha
half where that was, as combiner_portion makes it) */
static bool uniform_product(const struct translation *translation, int stage, int shift, int name) {
    uint32_t inputs = translation->state[WL_XBOX_PSH_RGB_INPUTS0 + stage];
    uint32_t outputs = translation->state[WL_XBOX_PSH_RGB_OUTPUTS0 + stage];
    int which = shift == 4 ? 0 : 1, index;

    if (shift == 8 || (outputs & (which ? 0x1000u : 0x2000u)))
        return false;
    for (index = 0; index < 2; index++) {
        unsigned long input = (inputs >> (24 - 16 * which - 8 * index)) & 0xff;

        if (!(input & 0x10) && (input & 0x0f) && name_of(input & 0x0f) != name)
            return false;
        if ((input & 0xe0) != 0x00 && (input & 0xe0) != 0x20 && (input & 0xe0) != 0xc0 &&
            (input & 0xe0) != 0xe0)
            return false;
    }
    return true;
}

/* whether the alpha half reads a name's blue after a combiner stage, before
the colour half writes it again other than as a uniform_product (the alpha
portions' inputs, the final combiner's G) */
static bool blue_read_later(const struct translation *translation, int after_stage, int name) {
    const uint32_t *state = translation->state;
    unsigned long count = state[WL_XBOX_PSH_COMBINER_COUNT] & 0xff;
    uint32_t efg = state[WL_XBOX_PSH_FINAL_INPUTS_EFG];
    unsigned long stage, g;
    int shift;

    if (count > 8)
        count = 8;
    for (stage = (unsigned long)(after_stage + 1); stage < count; stage++) {
        uint32_t inputs = state[WL_XBOX_PSH_ALPHA_INPUTS0 + stage];
        uint32_t outputs = state[WL_XBOX_PSH_RGB_OUTPUTS0 + stage];

        for (shift = 0; shift < 32; shift += 8) {
            unsigned long input = (inputs >> shift) & 0xff;

            if (name_of(input & 0x0f) == name && !(input & 0x10))
                return true;
        }
        for (shift = 0; shift <= 8; shift += 4) {
            if (name_of((outputs >> shift) & 0x0f) == name &&
                !uniform_product(translation, (int)stage, shift, name))
                return false;
        }
    }
    if (!state[WL_XBOX_PSH_FINAL_INPUTS_ABCD] && !efg)
        return false;
    g = (efg >> 8) & 0xff;
    return name_of(g & 0x0f) == name && !(g & 0x10);
}

/* after a combiner stage: the registers of names nothing reads again are free */
static void names_retire(struct translation *translation, int stage) {
    int portion, name;

    /* (and the blue copies whose blue is read no more) */
    for (name = 0; name < NUMBER_OF_NAMES; name++) {
        if (translation->blue_copy[name] && !blue_read_later(translation, stage, name)) {
            register_free(translation, _portion_alpha, translation->blue_copy[name] - 1);
            translation->blue_copy[name] = 0;
        }
    }

    for (portion = 0; portion < NUMBER_OF_PORTIONS; portion++) {
        for (name = 0; name < NUMBER_OF_NAMES; name++) {
            struct operand *location = &translation->names[portion][name];

            if (location->kind == _kind_register &&
                !name_read_later(translation, stage, name, portion)) {
                register_free(translation, location_pool(portion, location), location->index);
                *location = zero;
            }
        }
    }
}

/* an operand left to the stage that reads it (inputs_folding) */
static bool special(const struct operand *value) {
    return value->mapping || value->kind == _kind_product;
}

/* whether a product of operands left to its stage can be made there with an
output's mapping */
static bool special_composes(const struct operand *x, const struct operand *y, unsigned output) {
    int scale = x->mapping == 0x60 || y->mapping == 0x60 ? 2 : 1;

    return negated_mapping_composes(scale, output);
}

/* a product that is one of its operands as it is (or 0): no stage */
static bool product_operand(struct operand x, struct operand y, struct operand *result) {
    x = folded(x);
    y = folded(y);
    if (x.kind == _kind_zero || y.kind == _kind_zero) {
        *result = zero;
        return true;
    }
    /* (a mapping or a product left to the stage: that stage is made) */
    if (x.negative || y.negative || special(&x) || special(&y))
        return false;
    if (x.kind == _kind_one && !y.invert) {
        *result = y;
        return true;
    }
    if (y.kind == _kind_one && !x.invert) {
        *result = x;
        return true;
    }
    return false;
}

/* whether an operand, read in a portion, reads a register of a portion's
pool (the colour half reads the alpha pool through an alpha channel) */
/* A*B + C*D as x/2 + y/2: one of each pair a half, the others plain (not
inverted or negative) */
static bool half_pair(const struct operand *inputs, struct operand *x, struct operand *y) {
    int first, second;

    for (first = 0; first < 2; first++) {
        for (second = 2; second < 4; second++) {
            const struct operand *a = &inputs[first], *c = &inputs[second];
            const struct operand *b = &inputs[1 - first], *d = &inputs[5 - second];

            if (a->kind == _kind_half && !a->invert && !a->negative && c->kind == _kind_half &&
                !c->invert && !c->negative && !b->invert && !b->negative && !d->invert &&
                !d->negative) {
                *x = *b;
                *y = *d;
                return true;
            }
        }
    }
    return false;
}

/* A*B + C*D as (1 - x) p + x q: one of each pair x, inverted in one of them
only, the others plain (not inverted or negative) */
static bool lerp_pair(const struct operand *inputs, struct operand *p, struct operand *q,
                      struct operand *x) {
    int first, second;

    for (first = 0; first < 2; first++) {
        for (second = 2; second < 4; second++) {
            struct operand a = inputs[first], c = inputs[second];
            const struct operand *b = &inputs[1 - first], *d = &inputs[5 - second];

            if (a.negative || c.negative || b->invert || b->negative || d->invert || d->negative ||
                a.invert == c.invert || a.kind < _kind_register)
                continue;
            a.invert = c.invert = 0;
            if (memcmp(&a, &c, sizeof(a)))
                continue;
            *x = a;
            *p = inputs[first].invert ? *b : *d;
            *q = inputs[first].invert ? *d : *b;
            return true;
        }
    }
    return false;
}

static bool operand_reads(const struct operand *value, int reading_portion, int reg, int pool) {
    int read_pool = reading_portion == _portion_color && value->channel != _channel_alpha
                        ? _portion_color
                        : _portion_alpha;

    return value->kind == _kind_register && value->index == reg && read_pool == pool;
}

/* the register a combiner output's name goes to: where it was, when nothing
after reads that (later: the operands still to be used in this portion; the
colour portion's reads of an alpha written over come first, queues_flush),
else a free one */
static bool aliasable(const struct operand *value, int portion) {
    /* (a register's alpha, for the colour, the same in every channel: held
    in the alpha's pool, location_pool) */
    return value->kind != _kind_register || portion == _portion_alpha ||
           value->channel == _channel_natural || value->channel == _channel_alpha;
}

/* a name now standing for an operand (a register's shared) */
static void name_alias(struct translation *translation, int portion, unsigned long reg,
                       struct operand value) {
    struct operand *location = &translation->names[portion][name_of(reg)];

    if (location->kind == _kind_register)
        translation->released[location_pool(portion, location)][location->index]++;
    if (value.kind == _kind_register)
        wl_tev_pool_retain(&translation->pools[location_pool(portion, &value)], value.index);
    *location = value;
}

static bool register_available(const struct translation *translation, int portion) {
    int reg;

    for (reg = 0; reg < 4; reg++) {
        if (wl_tev_pool_free(&translation->pools[portion], (uint8_t)reg) &&
            !translation->released[portion][reg])
            return true;
    }
    return false;
}

/* whether a name can be written over its own register */
static bool in_place(const struct translation *translation, int portion, unsigned long reg,
                     const struct operand *later, int later_count) {
    int name = name_of(reg);
    const struct operand *location;
    int index;

    if (name < 0)
        return false;
    location = &translation->names[portion][name];
    if (location->kind != _kind_register || location_pool(portion, location) != portion ||
        translation->pools[portion].holders[location->index] != 1)
        return false;
    for (index = 0; index < later_count; index++) {
        if (operand_reads(&later[index], portion, location->index, portion))
            return false;
    }
    return true;
}

static int output_register(struct translation *translation, int portion, unsigned long reg,
                           const struct operand *later, int later_count) {
    int name = name_of(reg);
    int dest;
    struct operand *location;

    if (name < 0)
        return -1;
    location = &translation->names[portion][name];
    if (in_place(translation, portion, reg, later, later_count))
        return location->index;
    dest = register_allocate(translation, portion);
    if (dest < 0) {
        /* (none free: the register of a name read no more after this
        stage, when nothing still to come here reads it) */
        int other;

        for (other = 0; other < NUMBER_OF_NAMES && dest < 0; other++) {
            struct operand *victim = &translation->names[portion][other];

            if (other != name && victim->kind == _kind_register &&
                location_pool(portion, victim) == portion &&
                !name_read_later(translation, translation->stage, other, portion) &&
                in_place(translation, portion, (unsigned long)register_of_name(other), later,
                         later_count)) {
                /* (the victim's hold on it passes to the name) */
                dest = victim->index;
                *victim = zero;
                if (location->kind == _kind_register)
                    translation->released[location_pool(portion, location)][location->index]++;
                *location = operand(_kind_register, (unsigned char)dest, _channel_natural);
                return dest;
            }
        }
        fail(translation, "no free register");
        return -1;
    }
    if (location->kind == _kind_register)
        translation->released[location_pool(portion, location)][location->index]++;
    *location = operand(_kind_register, (unsigned char)dest, _channel_natural);
    return dest;
}

/* an input byte that is 1 whatever the registers hold (0, inverted or expand
negated) */
static bool input_one(unsigned input) {
    return (input & 0x0f) == 0 && ((input & 0xe0) == 0x20 || (input & 0xe0) == 0x60);
}

/* an input byte under a negated mapping (half bias negate, expand negate) of
a value that may change */
static bool input_negated(unsigned input) {
    return (input & 0x0f) != 0 && ((input & 0xe0) == 0xa0 || (input & 0xe0) == 0x60);
}

/* What each of a combiner portion's inputs may leave to the stage that reads
it, by the bytes alone (folding[4], combiner_input):

- _fold_mapping: a negated mapping, made in the stage of its product
  (emit_negated_product), which must then write that product alone, d 0: so
  not in a dot product, its other factor neither negated nor mapped likewise,
  the output's mapping one that stage can take; in a sum (not a mux) of two
  products, only one of them so made, computed first (the other added to it).
- _fold_product: in the alpha portion, the blue of a name the colour half
  made as a product, times 1: that product made again in the stage that
  reads it (a sum's two products may both be: the first goes to a register).

Neither needs a register of its own; an input left out is mapped as before. */
static void inputs_folding(uint32_t inputs, unsigned flags, int portion, unsigned long ab_name,
                           unsigned long cd_name, unsigned long sum_name, unsigned char folding[4]) {
    unsigned bytes[4], output = flags & 0x38;
    bool sum = name_of(sum_name) >= 0, mux = sum && (flags & 0x04);
    bool same;
    int index;

    for (index = 0; index < 4; index++) {
        bytes[index] = (unsigned)(inputs >> (24 - 8 * index)) & 0xff;
        folding[index] = 0;
    }
    same = bytes[0] == bytes[2] && bytes[1] == bytes[3];
    for (index = 0; index < 4; index++) {
        int pair = index / 2, scale = (bytes[index] & 0xe0) == 0x60 ? 2 : 1;
        unsigned partner = bytes[index ^ 1];

        if (portion == _portion_color && (flags & (pair ? 0x01u : 0x02u)))
            continue;
        if (!input_negated(bytes[index]) || input_negated(partner) || (partner & 0xe0) == 0xe0)
            continue;
        if (name_of(pair ? cd_name : ab_name) >= 0 && !negated_mapping_composes(scale, output))
            continue;
        if (sum && mux && same && !negated_mapping_composes(scale, output))
            continue;
        /* (A*B + A*B is A*B made once, its output's scale doubled: for the
        mappings combiner_portion doubles) */
        if (sum && !mux && same &&
            !(output == 0x00 || (output == 0x10 && scale == 1) || output == 0x30))
            continue;
        folding[index] |= _fold_mapping;
    }
    /* (in a sum of two products, one of them made with its mapping: the first) */
    if (sum && !mux && !same && (folding[0] | folding[1]) & _fold_mapping) {
        folding[2] &= (unsigned char)~_fold_mapping;
        folding[3] &= (unsigned char)~_fold_mapping;
    }
    for (index = 0; portion == _portion_alpha && index < 4; index++) {
        unsigned input = bytes[index], mapping = input & 0xe0;

        if (!(input & 0x10) && name_of(input & 0x0f) >= 0 && input_one(bytes[index ^ 1]) &&
            (mapping == 0x00 || mapping == 0xc0 || (folding[index] & _fold_mapping)))
            folding[index] |= _fold_product;
    }
}

/* whether a name's colour is read after a combiner stage as a colour (by a
colour portion or the final combiner's colour), not only as the alpha
half's blue */
static bool color_read_later(const struct translation *translation, int after_stage, int name) {
    const uint32_t *state = translation->state;
    unsigned long count = state[WL_XBOX_PSH_COMBINER_COUNT] & 0xff;
    uint32_t abcd = state[WL_XBOX_PSH_FINAL_INPUTS_ABCD];
    uint32_t efg = state[WL_XBOX_PSH_FINAL_INPUTS_EFG];
    unsigned long stage;
    int shift;

    if (count > 8)
        count = 8;
    for (stage = (unsigned long)(after_stage + 1); stage < count; stage++) {
        uint32_t inputs = state[WL_XBOX_PSH_RGB_INPUTS0 + stage];
        uint32_t outputs = state[WL_XBOX_PSH_RGB_OUTPUTS0 + stage];
        int sum = name_of((outputs >> 8) & 0x0f);
        bool sum_live =
            sum >= 0 && name_read_later(translation, (int)stage, sum, _portion_color);

        for (shift = 0; shift < 32; shift += 8) {
            unsigned long input = (inputs >> shift) & 0xff;
            /* (a pair whose product nothing reads, its own output's or the
            sum's, does not read it) */
            int own = name_of((outputs >> (shift >= 16 ? 4 : 0)) & 0x0f);

            if (name_of(input & 0x0f) == name && !(input & 0x10) &&
                (sum_live ||
                 (own >= 0 && name_read_later(translation, (int)stage, own, _portion_color))))
                return true;
        }
        if (name_of(outputs & 0x0f) == name || name_of((outputs >> 4) & 0x0f) == name ||
            name_of((outputs >> 8) & 0x0f) == name)
            return false;
    }
    if (!abcd && !efg)
        return name == _name_r0;
    for (shift = 0; shift < 32; shift += 8) {
        unsigned long input = (abcd >> shift) & 0xff;

        if (name_of(input & 0x0f) == name && !(input & 0x10))
            return true;
        if ((input & 0x0f) == _register_v1r0_sum && (name == _name_r0 || name == _name_v1))
            return true;
        if ((input & 0x0f) == _register_ef_product) {
            unsigned long e = (efg >> 24) & 0xff, f = (efg >> 16) & 0xff;

            if ((name_of(e & 0x0f) == name && !(e & 0x10)) ||
                (name_of(f & 0x0f) == name && !(f & 0x10)))
                return true;
        }
    }
    return false;
}

/* which product of a sum A*B + C*D is made first (0 or 2), as
combiner_portion makes it: a negative product second, one with a mapping
first (made alone); -1 when both have a mapping or are negative */
static int sum_first(const struct operand *inputs) {
    bool ab_negative = (inputs[0].negative ^ inputs[1].negative) && inputs[0].kind != _kind_zero &&
                       inputs[1].kind != _kind_zero;
    bool cd_negative = (inputs[2].negative ^ inputs[3].negative) && inputs[2].kind != _kind_zero &&
                       inputs[3].kind != _kind_zero;
    bool ab_mapped = inputs[0].mapping || inputs[1].mapping;
    bool cd_mapped = inputs[2].mapping || inputs[3].mapping;

    if ((ab_negative && cd_negative) || (ab_mapped && cd_mapped))
        return -1;
    return ab_negative || cd_mapped ? 2 : 0;
}

/* a register for one of a mux's products (inputs `first` and `first` + 1)
when none is free: an input's own register (a temporary) that nothing after
it reads, not the other product, the chooser nor what follows (TEV reads a
stage's inputs before it writes); -1 if none. It passes from the input to
the product */
static int mux_product_register(int portion,
                                const struct operand *inputs, int first, int *temporaries,
                                const struct operand *chooser, const struct operand *later,
                                int later_count) {
    int index, other;

    for (index = first; index < first + 2; index++) {
        int reg = temporaries[index];
        bool read = reg < 0 || operand_reads(chooser, portion, reg, portion);

        for (other = 0; other < 4 && !read; other++)
            read = (other / 2 != first / 2 && operand_reads(&inputs[other], portion, reg, portion)) ||
                   (other == (index ^ 1) && temporaries[other] == reg);
        for (other = 0; other < later_count && !read; other++)
            read = operand_reads(&later[other], portion, reg, portion);
        if (!read) {
            temporaries[index] = -1;
            return reg;
        }
    }
    return -1;
}

static void combiner_portion(struct translation *translation, int stage, int portion) {
    const uint32_t *state = translation->state;
    uint32_t inputs =
        state[(portion == _portion_alpha ? WL_XBOX_PSH_ALPHA_INPUTS0 : WL_XBOX_PSH_RGB_INPUTS0) +
              stage];
    uint32_t outputs =
        state[(portion == _portion_alpha ? WL_XBOX_PSH_ALPHA_OUTPUTS0 : WL_XBOX_PSH_RGB_OUTPUTS0) +
              stage];
    unsigned flags = (unsigned)(outputs >> 12);
    struct operand inputs_mapped[4], later[4], sum_saved[4];
    int later_count;
    int dot_temporaries[2] = {-1, -1};
    struct operand dot_operands[4];
    bool dot_unlit[2] = {false, false};
    bool signed_dot[2] = {false, false};
    struct operand signed_results[2];
    int signed_temporaries[2] = {-1, -1};
    int temporaries[4];
    unsigned char folding[4];
    unsigned long ab_name = (outputs >> 4) & 0x0f, cd_name = outputs & 0x0f,
                  sum_name = (outputs >> 8) & 0x0f;
    int dest, index;

    translation->product_count = 0;
    /* (a colour written again: its blue copy, still read by this combiner
    stage's alpha portion, freed at its end) */
    for (index = 0; portion == _portion_color && index < 3; index++) {
        int written = name_of((outputs >> (4 * index)) & 0x0f);

        if (written >= 0)
            translation->elided[written] = false;
        if (written >= 0 && translation->blue_copy[written]) {
            translation->released[_portion_alpha][translation->blue_copy[written] - 1]++;
            translation->blue_copy[written] = 0;
        }
    }

    /* (writes nothing reads again are left out) */
    if (name_of(ab_name) >= 0 && !name_read_later(translation, stage, name_of(ab_name), portion))
        ab_name = 0;
    if (name_of(cd_name) >= 0 && !name_read_later(translation, stage, name_of(cd_name), portion))
        cd_name = 0;
    if (name_of(sum_name) >= 0 && !name_read_later(translation, stage, name_of(sum_name), portion))
        sum_name = 0;
    if (name_of(ab_name) < 0 && name_of(cd_name) < 0 && name_of(sum_name) < 0)
        return;
    /* signed dot products, of values a swap or selection reaches: first, as
    their stages come before any this combiner stage queues */
    for (index = 0; portion == _portion_color && index < 2; index++) {
        unsigned long x_input = (inputs >> (24 - 16 * index)) & 0xff,
                      y_input = (inputs >> (16 - 16 * index)) & 0xff;
        unsigned long own = index ? cd_name : ab_name;
        struct operand x, y;
        int k;
        bool in_alpha;

        if (!(flags & (index ? 0x01u : 0x02u)) ||
            (!mapping_signed((unsigned)(x_input & 0xe0), &k) &&
             !mapping_signed((unsigned)(y_input & 0xe0), &k)) ||
            dot_unavailable(translation, (x_input << 8) | y_input, stage))
            continue;
        /* (the last attempts: as the translation was before it had them, its
        inputs mapped as anywhere else) */
        if (translation->signed_dots < 0) {
            approximate(translation->program, "a signed dot product, its inputs clamped");
            continue;
        }
        x = register_operand(translation, x_input & 0x0f, (x_input & 0x10) != 0, portion, stage);
        y = register_operand(translation, y_input & 0x0f, (y_input & 0x10) != 0, portion, stage);
        /* (a register's colour: as before, a dot product of a register, its
        signed inputs clamped) */
        if (!broadcastable(&x) || !broadcastable(&y)) {
            approximate(translation->program, "a signed dot product of a register");
            continue;
        }
        if (name_of(own) < 0 && name_of(sum_name) < 0) {
            signed_dot[index] = true;
            signed_results[index] = zero;
            continue;
        }
        in_alpha = translation->signed_dots == 2 && name_of(own) >= 0 && !(flags & 0x38) &&
                   blue_read_later(translation, stage, name_of(own));
        signed_dot[index] =
            translation->signed_dots && !translation->economical &&
            (emit_signed_dot(translation, x, (unsigned)(x_input & 0xe0), y,
                             (unsigned)(y_input & 0xe0), in_alpha, &signed_results[index],
                             &signed_temporaries[index]) ||
             (in_alpha && emit_signed_dot(translation, x, (unsigned)(x_input & 0xe0), y,
                                          (unsigned)(y_input & 0xe0), false, &signed_results[index],
                                          &signed_temporaries[index])));
        /* (else as before: of 0, 1/2 or 1, its signed inputs clamped to [0, 1],
        as a register's are; of anything else 1, as a dot product of the
        register a mapping would take) */
        if (!signed_dot[index] &&
            (translation->economical || x.kind > _kind_half || y.kind > _kind_half)) {
            signed_dot[index] = true;
            signed_results[index] = one;
            approximate(translation->program, "a signed dot product");
        } else if (!signed_dot[index]) {
            approximate(translation->program, "a signed dot product, its inputs clamped");
        }
    }
    if (translation->folding)
        inputs_folding(inputs, flags, portion, ab_name, cd_name, sum_name, folding);
    else
        memset(folding, 0, sizeof(folding));
    for (index = 0; index < 4; index++) {
        unsigned long input = (inputs >> (24 - 8 * index)) & 0xff;
        int earlier;

        if (signed_dot[index / 2]) {
            inputs_mapped[index] = index & 1 ? one : signed_results[index / 2];
            temporaries[index] = -1;
            continue;
        }
        /* (an input given twice is mapped once) */
        for (earlier = 0; earlier < index; earlier++) {
            if (!signed_dot[earlier / 2] && ((inputs >> (24 - 8 * earlier)) & 0xff) == input &&
                folding[earlier] == folding[index])
                break;
        }
        if (earlier < index) {
            inputs_mapped[index] = inputs_mapped[earlier];
            temporaries[index] = -1;
        } else {
            inputs_mapped[index] = combiner_input(translation, input, portion, stage,
                                                  folding[index], &temporaries[index]);
        }
    }
    /* dot products: into a register of their own (emit_dot), then a
product with 1 */
    memcpy(dot_operands, inputs_mapped, sizeof(dot_operands));
    /* (one nothing reads, neither its own output nor the sum: 0) */
    if (portion == _portion_color && (flags & 0x02) && !signed_dot[0] && name_of(ab_name) < 0 &&
        name_of(sum_name) < 0) {
        inputs_mapped[0] = zero;
        inputs_mapped[1] = one;
    } else if (portion == _portion_color && (flags & 0x02) && !signed_dot[0]) {
        dot_unlit[0] = dot_unavailable(translation, inputs >> 16, stage);
        inputs_mapped[0] = dot_unlit[0] ? one
                                        : emit_dot(translation, _portion_color, inputs_mapped[0],
                                                   inputs_mapped[1], &dot_temporaries[0]);
        inputs_mapped[1] = one;
    }
    if (portion == _portion_color && (flags & 0x01) && !signed_dot[1] && name_of(cd_name) < 0 &&
        name_of(sum_name) < 0) {
        inputs_mapped[2] = zero;
        inputs_mapped[3] = one;
    } else if (portion == _portion_color && (flags & 0x01) && !signed_dot[1]) {
        dot_unlit[1] = dot_unavailable(translation, inputs, stage);
        inputs_mapped[2] = dot_unlit[1] ? one
                                        : emit_dot(translation, _portion_color, inputs_mapped[2],
                                                   inputs_mapped[3], &dot_temporaries[1]);
        inputs_mapped[3] = one;
    }
    if (portion == _portion_color) {
        /* (what the colour half's outputs are, for the alpha half's reads of
        their blue: a product, or a sum of two, not a mux) */
        if (name_of(ab_name) >= 0) {
            if ((flags & 0x02) && !dot_unlit[0] && !signed_dot[0])
                blue_recipe_dot(translation, ab_name, dot_operands[0], dot_operands[1], flags);
            else
                blue_recipe_set(translation, ab_name, 1, &inputs_mapped[0], &inputs_mapped[1],
                                flags);
        }
        if (name_of(cd_name) >= 0) {
            if ((flags & 0x01) && !dot_unlit[1] && !signed_dot[1])
                blue_recipe_dot(translation, cd_name, dot_operands[2], dot_operands[3], flags);
            else
                blue_recipe_set(translation, cd_name, 1, &inputs_mapped[2], &inputs_mapped[3],
                                flags);
        }
        if (name_of(sum_name) >= 0) {
            struct operand x[2], y[2];

            /* (in the order the sum makes them, below) */
            int first = sum_first(inputs_mapped) == 2 ? 2 : 0;

            x[0] = inputs_mapped[first];
            y[0] = inputs_mapped[first + 1];
            x[1] = inputs_mapped[2 - first];
            y[1] = inputs_mapped[3 - first];
            if (flags & 0x04)
                blue_recipe_forget(translation, sum_name);
            else
                blue_recipe_set(translation, sum_name, 2, x, y, flags);
        }
        /* a colour read later only as the alpha half's blue, which its
        products make again there: not made (elided) */
        for (index = 0; index < 3; index++) {
            unsigned long *name = index == 0 ? &ab_name : index == 1 ? &cd_name : &sum_name;
            struct operand *location;

            if (name_of(*name) < 0 || !translation->blue[name_of(*name)].valid ||
                color_read_later(translation, stage, name_of(*name)))
                continue;
            location = &translation->names[_portion_color][name_of(*name)];
            if (location->kind == _kind_register)
                translation->released[location_pool(_portion_color, location)][location->index]++;
            *location = zero;
            translation->elided[name_of(*name)] = true;
            *name = 0;
        }
    }
    /* what the writes after the sum still read: A and B for A*B's own
    output, C and D for C*D's */
    later_count = 0;
    if (name_of(ab_name) >= 0) {
        later[later_count++] = inputs_mapped[0];
        later[later_count++] = inputs_mapped[1];
    }
    if (name_of(cd_name) >= 0) {
        later[later_count++] = inputs_mapped[2];
        later[later_count++] = inputs_mapped[3];
    }

    /* a sum of products of values the same in every channel, one of them a
    register's alpha, that the alpha half reads the blue of later: made in
    the alpha half too, a copy for those reads (blue_copy), freed after the
    last */
    if (portion == _portion_color && translation->signed_dots == 2 && name_of(sum_name) >= 0 &&
        !(flags & 0x07) && blue_read_later(translation, stage, name_of(sum_name)) &&
        !blue_read_now(translation, stage, name_of(sum_name))) {
        bool all_uniform = true, alpha_register = false;
        int first, reg;

        for (index = 0; index < 4; index++) {
            all_uniform = all_uniform && uniform(&inputs_mapped[index]) &&
                          inputs_mapped[index].kind != _kind_product;
            alpha_register = alpha_register || (inputs_mapped[index].kind == _kind_register &&
                                                inputs_mapped[index].channel == _channel_alpha);
        }
        /* (the order the sum makes them in: a negative product second, one
        with a mapping first) */
        first = sum_first(inputs_mapped);
        if (all_uniform && alpha_register && first >= 0 &&
            (reg = register_allocate(translation, _portion_alpha)) >= 0) {
            emit_product(translation, _portion_alpha, alpha_form(inputs_mapped[first]),
                         alpha_form(inputs_mapped[first + 1]), zero, 0, reg);
            emit_product(translation, _portion_alpha, alpha_form(inputs_mapped[2 - first]),
                         alpha_form(inputs_mapped[3 - first]),
                         operand(_kind_register, (unsigned char)reg, _channel_natural), flags,
                         reg);
            translation->blue_copy[name_of(sum_name)] = (unsigned char)(reg + 1);
            /* (a colour read later only through its blue: the copy alone) */
            if (!color_read_later(translation, stage, name_of(sum_name))) {
                struct operand *location = &translation->names[_portion_color][name_of(sum_name)];

                if (location->kind == _kind_register)
                    translation->released[location_pool(_portion_color, location)]
                                         [location->index]++;
                *location = zero;
                blue_recipe_forget(translation, sum_name);
                sum_name = 0;
            }
        }
    }
    if (name_of(sum_name) >= 0) {
        struct operand ab_value, cd_value;
        bool ab_folds = product_operand(inputs_mapped[0], inputs_mapped[1], &ab_value);
        bool cd_folds = product_operand(inputs_mapped[2], inputs_mapped[3], &cd_value);
        bool same = !memcmp(&inputs_mapped[0], &inputs_mapped[2], 2 * sizeof(inputs_mapped[0]));

        if ((flags & 0x04) && same) {
            /* (a choice of one product and itself: the product) */
            dest = output_register(translation, portion, sum_name, later, later_count);
            if (dest >= 0)
                emit_product(translation, portion, inputs_mapped[0], inputs_mapped[1], zero, flags,
                             dest);
            sum_name = 0;
        } else if (!(flags & 0x38) && !(flags & 0x04) && ab_folds && cd_folds &&
                   (ab_value.kind == _kind_zero || cd_value.kind == _kind_zero) &&
                   aliasable(ab_value.kind == _kind_zero ? &cd_value : &ab_value, portion)) {
            /* (a value and nothing: the value) */
            name_alias(translation, portion, sum_name,
                       ab_value.kind == _kind_zero ? cd_value : ab_value);
            sum_name = 0;
        }
    }
    /* (the sum may order its products: the inputs as they were after it) */
    memcpy(sum_saved, inputs_mapped, sizeof(sum_saved));
    if (name_of(sum_name) >= 0) {
        if (flags & 0x04) {
            /* the mux: r0's alpha chooses, as a blend of the two (each product
            in a register of its own, unless it is a value as it is; a
            negative product clamps to 0 there) */
            struct operand ab, cd;
            int ab_register = -1, cd_register = -1;
            struct operand chooser = translation->names[_portion_alpha][_name_r0];
            struct operation operation;

            for (index = 0; index < 4; index++) {
                if (inputs_mapped[index].negative)
                    approximate(translation->program, "a negative product in a mux");
            }

            if (portion == _portion_color && chooser.kind >= _kind_register)
                chooser.channel =
                    chooser.channel == _channel_blue ? _channel_blue_all : _channel_alpha;
            if (!product_operand(inputs_mapped[0], inputs_mapped[1], &ab)) {
                ab_register = register_allocate(translation, portion);
                if (ab_register < 0)
                    ab_register = mux_product_register(portion, inputs_mapped, 0,
                                                       temporaries, &chooser, later, later_count);
                if (ab_register < 0) {
                    fail(translation, "no free register");
                    return;
                }
                emit_product(translation, portion, inputs_mapped[0], inputs_mapped[1], zero, 0,
                             ab_register);
                ab = operand(_kind_register, (unsigned char)ab_register, _channel_natural);
            }
            if (!product_operand(inputs_mapped[2], inputs_mapped[3], &cd)) {
                cd_register = register_allocate(translation, portion);
                if (cd_register < 0)
                    cd_register = mux_product_register(portion, inputs_mapped, 2,
                                                       temporaries, &chooser, later, later_count);
                if (cd_register < 0) {
                    fail(translation, "no free register");
                    return;
                }
                emit_product(translation, portion, inputs_mapped[2], inputs_mapped[3], zero, 0,
                             cd_register);
                cd = operand(_kind_register, (unsigned char)cd_register, _channel_natural);
            }
            /* (the inputs' own registers are done with, if nothing after
            reads them) */
            if (!later_count) {
                for (index = 0; index < 4; index++) {
                    register_free(translation, portion, temporaries[index]);
                    temporaries[index] = -1;
                }
            }
            dest = output_register(translation, portion, sum_name, later, later_count);
            if (dest < 0)
                return;
            operation = operation_new(dest);
            operation.in[0] = ab;
            operation.in[1] = cd;
            operation.in[2] = chooser;
            output_mapping(&operation, flags);
            emit(translation, portion, &operation);
            register_free(translation, portion, ab_register);
            register_free(translation, portion, cd_register);
        } else {
            struct operand ab;
            int ab_register = -1;
            unsigned mapping = flags & 0x38;
            bool ab_negative = (inputs_mapped[0].negative ^ inputs_mapped[1].negative) &&
                               inputs_mapped[0].kind != _kind_zero &&
                               inputs_mapped[1].kind != _kind_zero;
            bool cd_negative = (inputs_mapped[2].negative ^ inputs_mapped[3].negative) &&
                               inputs_mapped[2].kind != _kind_zero &&
                               inputs_mapped[3].kind != _kind_zero;

            /* (a negative product is the one subtracted: second; both
            negative, the sum is below 0, which clamps to 0) */
            if (ab_negative && cd_negative) {
                inputs_mapped[0] = inputs_mapped[2] = zero;
            } else if (ab_negative ||
                       (!cd_negative && (inputs_mapped[2].mapping || inputs_mapped[3].mapping))) {
                /* (a product with a mapping is made alone, first:
                emit_negated_product) */
                struct operand swap[2];

                memcpy(swap, inputs_mapped, sizeof(swap));
                memcpy(inputs_mapped, inputs_mapped + 2, sizeof(swap));
                memcpy(inputs_mapped + 2, swap, sizeof(swap));
            }

            struct operand half_x, half_y, lerp_c, cd_value;
            bool specials = special(&inputs_mapped[0]) || special(&inputs_mapped[1]) ||
                            special(&inputs_mapped[2]) || special(&inputs_mapped[3]);

            if (specials && product_operand(inputs_mapped[2], inputs_mapped[3], &cd_value) &&
                cd_value.kind == _kind_zero &&
                special_composes(&inputs_mapped[0], &inputs_mapped[1], mapping)) {
                /* (A*B + 0: A*B, mapped) */
                dest = output_register(translation, portion, sum_name, later, later_count);
                if (dest >= 0)
                    emit_product(translation, portion, inputs_mapped[0], inputs_mapped[1], zero,
                                 flags, dest);
            } else if (!specials && half_pair(inputs_mapped, &half_x, &half_y)) {
                /* (x/2 + y/2: TEV's blend of the two, by a half) */
                struct operation operation;

                dest = output_register(translation, portion, sum_name, later, later_count);
                if (dest >= 0) {
                    operation = operation_new(dest);
                    operation.in[0] = half_x;
                    operation.in[1] = half_y;
                    operation.in[2] = operand(_kind_half, 0, 0);
                    output_mapping(&operation, flags);
                    emit(translation, portion, &operation);
                }
            } else if (!specials && lerp_pair(inputs_mapped, &half_x, &half_y, &lerp_c)) {
                /* ((1 - x) p + x q: TEV's own lerp) */
                struct operation operation;

                dest = output_register(translation, portion, sum_name, later, later_count);
                if (dest >= 0) {
                    operation = operation_new(dest);
                    operation.in[0] = half_x;
                    operation.in[1] = half_y;
                    operation.in[2] = lerp_c;
                    output_mapping(&operation, flags);
                    emit(translation, portion, &operation);
                }
            } else if (!memcmp(&inputs_mapped[0], &inputs_mapped[2],
                               2 * sizeof(inputs_mapped[0])) &&
                       (mapping == 0 || mapping == 0x10 || mapping == 0x30)) {
                /* (A*B + A*B: A*B twice) */
                dest = output_register(translation, portion, sum_name, later, later_count);
                if (dest >= 0) {
                    emit_product(translation, portion, inputs_mapped[0], inputs_mapped[1], zero,
                                 (flags & ~0x38u) | (mapping == 0      ? 0x10u
                                                     : mapping == 0x10 ? 0x20u
                                                                       : 0x00u),
                                 dest);
                }
            } else if (product_operand(inputs_mapped[0], inputs_mapped[1], &ab)) {
                /* (A*B is a value as it is: the sum's d) */
                dest = output_register(translation, portion, sum_name, later, later_count);
                if (dest >= 0)
                    emit_product(translation, portion, inputs_mapped[2], inputs_mapped[3], ab,
                                 flags, dest);
            } else if ((ab_register = register_allocate(translation, portion)) >= 0) {
                emit_product(translation, portion, inputs_mapped[0], inputs_mapped[1], zero, 0,
                             ab_register);
                dest = output_register(translation, portion, sum_name, later, later_count);
                if (dest >= 0) {
                    emit_product(
                        translation, portion, inputs_mapped[2], inputs_mapped[3],
                        operand(_kind_register, (unsigned char)ab_register, _channel_natural),
                        flags, dest);
                }
                register_free(translation, portion, ab_register);
            } else {
                /* (no register for A*B: it goes to the sum's own, which C and
                D must not read) */
                struct operand sum_later[2];

                sum_later[0] = inputs_mapped[2];
                sum_later[1] = inputs_mapped[3];
                dest = output_register(translation, portion, sum_name, later, later_count);
                if (dest < 0)
                    return;
                if (operand_reads(&sum_later[0], portion, dest, portion) ||
                    operand_reads(&sum_later[1], portion, dest, portion)) {
                    fail(translation, "no free register");
                    return;
                }
                emit_product(translation, portion, inputs_mapped[0], inputs_mapped[1], zero, 0,
                             dest);
                emit_product(translation, portion, inputs_mapped[2], inputs_mapped[3],
                             operand(_kind_register, (unsigned char)dest, _channel_natural), flags,
                             dest);
            }
        }
    }
    memcpy(inputs_mapped, sum_saved, sizeof(sum_saved));
    /* a product of values the same in every channel, one of them a
    register's alpha (a signed dot product made there, for the alpha half's
    reads of its blue): made in the alpha half, the colour standing for that
    register's alpha, unless this combiner stage's alpha portion reads the
    blue it had */
    for (index = 0; portion == _portion_color && translation->signed_dots == 2 && index < 2;
         index++) {
        unsigned long *name = index ? &cd_name : &ab_name;
        struct operand x = inputs_mapped[2 * index], y = inputs_mapped[2 * index + 1];
        struct operand *location;
        int reg;

        if (name_of(*name) < 0 || (flags & (index ? 0x01u : 0x02u)) || !uniform(&x) ||
            !uniform(&y) ||
            !((x.kind == _kind_register && x.channel == _channel_alpha) ||
              (y.kind == _kind_register && y.channel == _channel_alpha)) ||
            blue_read_now(translation, stage, name_of(*name)) ||
            (reg = register_allocate(translation, _portion_alpha)) < 0)
            continue;
        emit_product(translation, _portion_alpha, alpha_form(x), alpha_form(y), zero, flags, reg);
        location = &translation->names[_portion_color][name_of(*name)];
        if (location->kind == _kind_register)
            translation->released[location_pool(_portion_color, location)][location->index]++;
        *location = operand(_kind_register, (unsigned char)reg, _channel_alpha);
        blue_recipe_forget(translation, *name);
        *name = 0;
    }
    /* a product that is an operand as it is, unmapped, is that operand: the
    name stands for it, no stage made (a register's alpha too, for the
    colour: held in the alpha's pool) */
    if (!(flags & 0x38)) {
        struct operand value;

        if (name_of(ab_name) >= 0 && product_operand(inputs_mapped[0], inputs_mapped[1], &value) &&
            aliasable(&value, portion)) {
            name_alias(translation, portion, ab_name, value);
            ab_name = 0;
        }
        if (name_of(cd_name) >= 0 && product_operand(inputs_mapped[2], inputs_mapped[3], &value) &&
            aliasable(&value, portion)) {
            name_alias(translation, portion, cd_name, value);
            cd_name = 0;
        }
    }
    /* A*B, then C*D, or C*D first when A*B has no register to go to and
    C*D can go over its name's own (A and B not reading it) */
    {
        bool cd_first = false;

        if (name_of(ab_name) >= 0 && name_of(cd_name) >= 0 &&
            !register_available(translation, portion) &&
            !in_place(translation, portion, ab_name, &inputs_mapped[2], 2)) {
            cd_first = in_place(translation, portion, cd_name, inputs_mapped, 2);
        }
        if (cd_first) {
            dest = output_register(translation, portion, cd_name, inputs_mapped, 2);
            if (dest >= 0)
                emit_product(translation, portion, inputs_mapped[2], inputs_mapped[3], zero, flags,
                             dest);
            cd_name = 0;
        }
        if (name_of(ab_name) >= 0) {
            /* (C and D are still to be read) */
            dest = output_register(translation, portion, ab_name, &inputs_mapped[2],
                                   name_of(cd_name) >= 0 ? 2 : 0);
            if (dest >= 0)
                emit_product(translation, portion, inputs_mapped[0], inputs_mapped[1], zero, flags,
                             dest);
        }
        if (name_of(cd_name) >= 0) {
            dest = output_register(translation, portion, cd_name, inputs_mapped, 0);
            if (dest >= 0)
                emit_product(translation, portion, inputs_mapped[2], inputs_mapped[3], zero, flags,
                             dest);
        }
    }
    for (index = 0; index < 4; index++)
        register_free(translation, portion, temporaries[index]);
    register_free(translation, portion, dot_temporaries[0]);
    register_free(translation, portion, dot_temporaries[1]);
    /* (a signed dot product in an alpha, which the colour's queued
    operations may still read: free once they are TEV stages) */
    for (index = 0; index < 2; index++) {
        if (signed_temporaries[index] < 0)
            continue;
        if (signed_results[index].channel == _channel_alpha)
            translation->released[_portion_alpha][signed_temporaries[index]]++;
        else
            register_free(translation, _portion_color, signed_temporaries[index]);
    }
}

/* ---------- stages */

static unsigned char input_code(const struct operand *value, int portion) {
    static const unsigned char color_registers[4] = {WL_TEV_CC_CPREV, WL_TEV_CC_C0, WL_TEV_CC_C1,
                                                     WL_TEV_CC_C2};
    static const unsigned char alpha_registers[4] = {WL_TEV_CC_APREV, WL_TEV_CC_A0, WL_TEV_CC_A1,
                                                     WL_TEV_CC_A2};
    static const unsigned char alpha_half_registers[4] = {WL_TEV_CA_APREV, WL_TEV_CA_A0,
                                                          WL_TEV_CA_A1, WL_TEV_CA_A2};

    if (portion == _portion_color) {
        switch (value->kind) {
        case _kind_one:
            return WL_TEV_CC_ONE;
        case _kind_half:
            return WL_TEV_CC_HALF;
        case _kind_register:
            return value->channel == _channel_alpha ? alpha_registers[value->index & 3]
                                                    : color_registers[value->index & 3];
        /* (the blue everywhere: the alpha, which table 3 makes the blue) */
        case _kind_texture:
            return value->channel == _channel_alpha || value->channel == _channel_blue_all
                       ? WL_TEV_CC_TEXA
                       : WL_TEV_CC_TEXC;
        case _kind_ras:
            return value->channel == _channel_alpha || value->channel == _channel_blue_all
                       ? WL_TEV_CC_RASA
                       : WL_TEV_CC_RASC;
        case _kind_konst:
            return WL_TEV_CC_KONST;
        default:
            return WL_TEV_CC_ZERO;
        }
    }
    switch (value->kind) {
    case _kind_one:
    case _kind_half:
    case _kind_konst:
        return WL_TEV_CA_KONST;
    case _kind_register:
        return alpha_half_registers[value->index & 3];
    case _kind_texture:
        return WL_TEV_CA_TEXA;
    case _kind_ras:
        return WL_TEV_CA_RASA;
    default:
        return WL_TEV_CA_ZERO;
    }
}

static void operation_resources(const struct operation *operation, int portion,
                                WlTevNeeds *resources) {
    int index;

    wl_tev_needs_clear(resources);
    for (index = 0; index < 4; index++)
        resources_add(resources, &operation->in[index], portion);
}

static void stage_half(WlTevStage *stage, const struct operation *operation, int portion) {
    unsigned char in[4];
    int index;

    for (index = 0; index < 4; index++)
        in[index] = input_code(&operation->in[index], portion);
    (portion == _portion_color ? wl_tev_stage_color : wl_tev_stage_alpha)(
        stage, in[0], in[1], in[2], in[3], operation->subtract ? WL_TEV_SUB : WL_TEV_ADD,
        operation->bias, operation->scale, !operation->unclamped, operation->dest);
}

/* a TEV stage of a colour operation, an alpha one or both (a half with
nothing to do keeps PREV as it is) */
static bool stage_append(struct translation *translation, const struct operation *color,
                         const struct operation *alpha) {
    WlTevStage *stage = wl_tev_add_stage(&translation->program->tev);
    WlTevNeeds resources, other;

    if (!stage) {
        fail(translation, "more than 16 stages");
        return false;
    }
    wl_tev_needs_clear(&resources);
    if (color)
        operation_resources(color, _portion_color, &resources);
    if (alpha) {
        operation_resources(alpha, _portion_alpha, &other);
        wl_tev_needs_merge(&resources, &other);
    }
    wl_tev_stage_set_needs(stage, &resources);
    if (color)
        stage_half(stage, color, _portion_color);
    if (alpha)
        stage_half(stage, alpha, _portion_alpha);
    return true;
}

/* whether a colour operation reads a register's alpha */
static bool color_reads_alpha(const struct operation *operation, int reg) {
    int index;

    for (index = 0; index < 4; index++) {
        if (operand_reads(&operation->in[index], _portion_color, reg, _portion_alpha))
            return true;
    }
    return false;
}

/* whether the alpha operation `alpha` may be made now, with the colour one
`with` (an index, or -1) beside it: every colour operation queued before it
that reads the alpha it writes made already, or beside it (TEV reads a
stage's inputs before it writes) */
static bool alpha_ready(const struct translation *translation, unsigned color, int with,
                        const struct operation *alpha) {
    unsigned index;

    for (index = color; index < translation->queue_count[_portion_color]; index++) {
        const struct operation *reader = &translation->queue[_portion_color][index];

        if ((int)index != with && reader->sequence < alpha->sequence &&
            color_reads_alpha(reader, alpha->dest))
            return false;
    }
    return true;
}

/* whether the colour operation `color_operation` may be made now: every
alpha operation queued before it that writes an alpha it reads made
already, in an earlier stage */
static bool color_ready(const struct translation *translation, unsigned alpha,
                        const struct operation *color_operation) {
    unsigned index;

    for (index = alpha; index < translation->queue_count[_portion_alpha]; index++) {
        const struct operation *writer = &translation->queue[_portion_alpha][index];

        if (writer->sequence < color_operation->sequence &&
            color_reads_alpha(color_operation, writer->dest))
            return false;
    }
    return true;
}

/* this combiner stage's operations into TEV stages: the colour's and the
alpha's side by side where their textures and colours agree, the shorter
list's at the end of the longer's (so the alpha's last writes, PREV's in the
final combiner, land no earlier than the colour's last reads of them); where
the two cannot share a stage, the colour's goes first. What the order they
were queued in says of an alpha written and read as the colour's input
holds: a colour operation queued before the alpha one that writes what it
reads is made no later, one queued after it later (else the one queued
first goes alone) */
static void queues_flush(struct translation *translation) {
    unsigned color = 0, alpha = 0;
    int portion, reg;

    while (color < translation->queue_count[_portion_color] ||
           alpha < translation->queue_count[_portion_alpha]) {
        unsigned color_left = translation->queue_count[_portion_color] - color;
        unsigned alpha_left = translation->queue_count[_portion_alpha] - alpha;
        const struct operation *color_operation = color_left >= alpha_left && color_left
                                                      ? &translation->queue[_portion_color][color]
                                                      : NULL;
        const struct operation *alpha_operation = alpha_left >= color_left && alpha_left
                                                      ? &translation->queue[_portion_alpha][alpha]
                                                      : NULL;

        if (color_operation && alpha_operation) {
            WlTevNeeds color_resources, alpha_resources;

            operation_resources(color_operation, _portion_color, &color_resources);
            operation_resources(alpha_operation, _portion_alpha, &alpha_resources);
            if (!wl_tev_needs_merge(&color_resources, &alpha_resources))
                alpha_operation = NULL;
        }
        if (color_operation && !color_ready(translation, alpha, color_operation))
            color_operation = NULL;
        if (alpha_operation &&
            !alpha_ready(translation, color, color_operation ? (int)color : -1, alpha_operation))
            alpha_operation = NULL;
        if (!color_operation && !alpha_operation) {
            /* (the one queued first: always ready) */
            const struct operation *next_color =
                color_left ? &translation->queue[_portion_color][color] : NULL;
            const struct operation *next_alpha =
                alpha_left ? &translation->queue[_portion_alpha][alpha] : NULL;

            if (next_color && (!next_alpha || next_color->sequence < next_alpha->sequence))
                color_operation = next_color;
            else
                alpha_operation = next_alpha;
        }
        if (!stage_append(translation, color_operation, alpha_operation))
            return;
        if (color_operation)
            color++;
        if (alpha_operation)
            alpha++;
    }
    translation->sequence = 0;
    for (portion = 0; portion < NUMBER_OF_PORTIONS; portion++) {
        translation->queue_count[portion] = 0;
        for (reg = 0; reg < 4; reg++) {
            while (translation->released[portion][reg]) {
                translation->released[portion][reg]--;
                register_free(translation, portion, reg);
            }
        }
    }
}

/* ---------- the final combiner */

static struct operand final_input(struct translation *translation, unsigned long input,
                                  int portion) {
    struct operand value =
        register_operand(translation, input & 0x0f, (input & 0x10) != 0, portion, -1);

    if (input & 0x20)
        value.invert = 1;
    return folded(value);
}

/* the names the final combiner's colour still reads through its inputs
`abcd` (those made already marked 0x80), but for the input at `skip` (a
shift: the product or sum being made), directly or through the product E*F
or the sum v1 + r0 */
static unsigned final_color_reads(uint32_t abcd, uint32_t efg, int skip) {
    unsigned reads = 0;
    int shift, name;

    for (shift = 0; shift < 32; shift += 8) {
        unsigned long input = (abcd >> shift) & 0xff;

        if ((input & 0x80) || shift == skip)
            continue;
        if ((input & 0x0f) == _register_ef_product) {
            if ((name = name_of((efg >> 24) & 0x0f)) >= 0)
                reads |= 1u << name;
            if ((name = name_of((efg >> 16) & 0x0f)) >= 0)
                reads |= 1u << name;
        } else if ((input & 0x0f) == _register_v1r0_sum) {
            reads |= 1u << _name_v1 | 1u << _name_r0;
        } else if ((name = name_of(input & 0x0f)) >= 0) {
            reads |= 1u << name;
        }
    }
    return reads;
}

/* A register for the final combiner's product or sum: a free one, or else
one that holds only names the rest of the final combiner does not read
(`reads`): the product's own inputs among them, as TEV reads a stage's
inputs before it writes (E*F into E's register, say, when nothing else reads
E). -1 when there is none. */
static int final_register(struct translation *translation, unsigned reads) {
    int reg = register_allocate(translation, _portion_color), candidate, name;

    if (reg >= 0)
        return reg;
    for (candidate = 0; candidate < 4; candidate++) {
        unsigned held = 0, holders = 0;

        for (name = 0; name < NUMBER_OF_NAMES; name++) {
            const struct operand *location = &translation->names[_portion_color][name];

            if (location->kind == _kind_register && location->index == candidate &&
                location_pool(_portion_color, location) == _portion_color) {
                held |= 1u << name;
                holders++;
            }
        }
        if (held == 0 || (held & reads) != 0 ||
            translation->pools[_portion_color].holders[candidate] != holders)
            continue;
        /* (the names' holds on it pass to the value made: read no more) */
        for (name = 0; name < NUMBER_OF_NAMES; name++) {
            if (held & (1u << name))
                translation->names[_portion_color][name] = zero;
        }
        return candidate;
    }
    return -1;
}

static void final_combiner(struct translation *translation) {
    const uint32_t *state = translation->state;
    uint32_t abcd = state[WL_XBOX_PSH_FINAL_INPUTS_ABCD];
    uint32_t efg = state[WL_XBOX_PSH_FINAL_INPUTS_EFG];
    unsigned long settings = efg & 0xff;
    struct operand a, b, c, d, g;
    struct operation color, alpha;
    unsigned long input;
    int shift;

    if (!abcd && !efg) {
        /* the result is r0 */
        color = operation_new(0);
        color.in[3] = translation->names[_portion_color][_name_r0];
        emit(translation, _portion_color, &color);
        alpha = operation_new(0);
        alpha.in[3] = translation->names[_portion_alpha][_name_r0];
        emit(translation, _portion_alpha, &alpha);
        queues_flush(translation);
        return;
    }
    /* (the sum's and the product's alpha is 0: the zero register's) */
    for (shift = 0; shift < 32; shift += 8) {
        input = (abcd >> shift) & 0xff;
        if (((input & 0x0f) == _register_v1r0_sum || (input & 0x0f) == _register_ef_product) &&
            (input & 0x10))
            abcd = (uint32_t)((abcd & ~(0xffUL << shift)) | ((input & 0x20UL) << shift));
    }
    /* the sum and the product the inputs name, first, where they are named */
    for (shift = 0; shift < 32; shift += 8) {
        input = (abcd >> shift) & 0x0f;
        if ((abcd >> shift) & 0x80)
            continue;
        if (input == _register_ef_product) {
            struct operand e = final_input(translation, (efg >> 24) & 0xff, _portion_color);
            struct operand f = final_input(translation, (efg >> 16) & 0xff, _portion_color);
            struct operand value;
            int reg;

            /* (a register as it is: read where it is) */
            if (product_operand(e, f, &value) && value.kind == _kind_register) {
                abcd = (uint32_t)((abcd & ~(0xffUL << shift)) |
                                  ((unsigned long)(0x80 | value.index |
                                                   (value.channel == _channel_alpha ? 0x10 : 0))
                                   << shift) |
                                  (((abcd >> shift) & 0x30UL) << shift));
                continue;
            }
            reg = final_register(translation, final_color_reads(abcd, efg, shift));
            if (reg < 0) {
                fail(translation, "no free register");
                return;
            }
            emit_product(translation, _portion_color, e, f, zero, 0, reg);
            /* (marked 0x80: the input is in reg now) */
            abcd = (uint32_t)((abcd & ~(0xffUL << shift)) | ((unsigned long)(0x80 | reg) << shift) |
                              (((abcd >> shift) & 0x30UL) << shift));
        } else if (input == _register_v1r0_sum) {
            struct operand v1 =
                register_operand(translation, _register_v1, false, _portion_color, -1);
            struct operand r0 =
                register_operand(translation, _register_r0, false, _portion_color, -1);
            int reg = final_register(translation, final_color_reads(abcd, efg, shift));

            if (reg < 0) {
                fail(translation, "no free register");
                return;
            }
            v1.invert = (settings & 0x40) != 0;
            r0.invert = (settings & 0x20) != 0;
            emit_product(translation, _portion_color, v1, one, r0, 0, reg);
            abcd = (uint32_t)((abcd & ~(0xffUL << shift)) | ((unsigned long)(0x80 | reg) << shift) |
                              (((abcd >> shift) & 0x30UL) << shift));
        }
    }

#define FINAL_OPERAND(value, byte)                                                                 \
    do {                                                                                           \
        unsigned long final_byte = (byte);                                                         \
        if (final_byte & 0x80) {                                                                   \
            (value) = operand(_kind_register, (unsigned char)(final_byte & 3),                     \
                              (final_byte & 0x10) ? _channel_alpha : _channel_natural);            \
            (value).invert = (final_byte & 0x20) ? 1 : 0;                                          \
        } else                                                                                     \
            (value) = final_input(translation, final_byte, _portion_color);                        \
    } while (0)

    FINAL_OPERAND(a, (abcd >> 24) & 0xff);
    FINAL_OPERAND(b, (abcd >> 16) & 0xff);
    FINAL_OPERAND(c, (abcd >> 8) & 0xff);
    FINAL_OPERAND(d, abcd & 0xff);
#undef FINAL_OPERAND

    /* A*B + (1-A)*C + D: TEV's lerp, d + (1-c)*a + c*b */
    color = operation_new(0);
    if (a.invert) {
        struct operand swap = b;

        b = c;
        c = swap;
        a.invert = 0;
    }
    if (b.invert)
        b = materialized(translation, _portion_color, b);
    if (c.invert)
        c = materialized(translation, _portion_color, c);
    if (d.invert)
        d = materialized(translation, _portion_color, d);
    color.in[0] = c;
    color.in[1] = b;
    color.in[2] = a;
    color.in[3] = d;
    emit(translation, _portion_color, &color);

    {
        /* (a register's blue as a combiner stage's alpha portion reads it:
        made again from its products, or its alpha, approximated) */
        unsigned long blue_input = (efg >> 8) & 0xff;
        int temporary;

        g = combiner_input(translation, blue_input & 0x1f, _portion_alpha, -1, 0, &temporary);
        if (blue_input & 0x20) {
            g.invert = 1;
            g = folded(g);
        }
    }
    alpha = operation_new(0);
    if (g.invert) {
        g.invert = 0;
        alpha.in[0] = one;
        alpha.in[2] = g;
    } else {
        alpha.in[3] = g;
    }
    emit(translation, _portion_alpha, &alpha);
    queues_flush(translation);
}

/* ---------- dead stages

The translation leaves halves of stages whose results nothing reads (a
value made for a portion that ends up not using it, a register written
again before it is read) and halves that only pass a register on. Once the
stages are made these are taken out (stages_compact): a stage with nothing
left in it goes, and a half left alone in its stage moves into the unused
half of another, where nothing between them reads or writes what it does
and the two halves' textures and colours agree. Every half that stays reads
what it read before, so PREV's colour and alpha after the last stage, the
program's result, are as they were. */

/* a register's colour (alpha 0) or alpha, as a bit of a set */
#define REGISTER_CHANNEL(reg, alpha) (1u << ((reg) * 2 + ((alpha) ? 1 : 0)))
#define HALF(stage, which) ((which) ? &(stage)->alpha : &(stage)->color)

/* the register channel an input reads (0: none) */
static unsigned input_channel(unsigned char code, int alpha) {
    if (alpha)
        return code <= WL_TEV_CA_A2 ? REGISTER_CHANNEL(code, 1) : 0;
    return code <= WL_TEV_CC_A2 ? REGISTER_CHANNEL(code >> 1, code & 1) : 0;
}

static bool input_zero(unsigned char code, int alpha) {
    return code == (alpha ? WL_TEV_CA_ZERO : WL_TEV_CC_ZERO);
}

/* the register channels a half's result depends on: d, and of the lerp
(1 - c) * a + c * b, a unless c is 1, b unless c is 0, and c unless a and b
are 0 (all of a compare's) */
static unsigned half_reads(const WlTevHalf *half, int alpha) {
    unsigned reads = input_channel(half->in[3], alpha);
    bool lerp = half->op == WL_TEV_ADD || half->op == WL_TEV_SUB;

    if (!lerp || alpha || half->in[2] != WL_TEV_CC_ONE)
        reads |= input_channel(half->in[0], alpha);
    if (!lerp || !input_zero(half->in[2], alpha))
        reads |= input_channel(half->in[1], alpha);
    if (!lerp || !input_zero(half->in[0], alpha) || !input_zero(half->in[1], alpha))
        reads |= input_channel(half->in[2], alpha);
    return reads;
}

static unsigned half_writes(const WlTevHalf *half, int alpha) {
    return REGISTER_CHANNEL(half->dest & 3, alpha);
}

/* whether a half leaves its register as it was: d its own channel, nothing
added to it, not scaled, and clamped only where it is in [0, 1] (in_range:
the channels that are, before the stage) */
static bool half_passes(const WlTevHalf *half, int alpha, unsigned in_range) {
    unsigned char own = (unsigned char)(alpha ? half->dest : half->dest * 2);

    return (half->op == WL_TEV_ADD || half->op == WL_TEV_SUB) && half->bias == WL_TEV_BIAS_ZERO &&
           half->scale == WL_TEV_SCALE_1 && input_zero(half->in[0], alpha) &&
           input_zero(half->in[1], alpha) && half->in[3] == own &&
           (!half->clamp || (in_range & half_writes(half, alpha)));
}

/* the channels in [0, 1] before each stage: registers start in it (0, or a
preloaded constant), a clamped result is in it, an unclamped one may not be */
static void stages_in_range(const WlTevProgram *tev, unsigned in_range[WL_TEV_MAX_STAGES]) {
    unsigned range = 0xFF, index;
    int alpha;

    for (index = 0; index < tev->stage_count; index++) {
        unsigned next = range;

        in_range[index] = range;
        for (alpha = 0; alpha < 2; alpha++) {
            const WlTevHalf *half = HALF(&tev->stages[index], alpha);

            if (half_passes(half, alpha, range))
                continue;
            if (half->clamp)
                next |= half_writes(half, alpha);
            else
                next &= ~half_writes(half, alpha);
        }
        range = next;
    }
}

/* the halves used: those that change what they write, and whose result a
used half reads before it is written again, or that is the program's
(PREV's colour and alpha after the last stage). (A value a used half reads
is then always a used half's, or a register's start.) */
static void stages_used(const WlTevProgram *tev, unsigned char used[WL_TEV_MAX_STAGES][2]) {
    unsigned in_range[WL_TEV_MAX_STAGES];
    unsigned live = REGISTER_CHANNEL(WL_TEV_PREV, 0) | REGISTER_CHANNEL(WL_TEV_PREV, 1), index;
    int alpha;

    stages_in_range(tev, in_range);
    for (index = tev->stage_count; index-- > 0;) {
        unsigned reads = 0, writes = 0;

        for (alpha = 0; alpha < 2; alpha++) {
            const WlTevHalf *half = HALF(&tev->stages[index], alpha);

            used[index][alpha] = (unsigned char)(!half_passes(half, alpha, in_range[index]) &&
                                                 (live & half_writes(half, alpha)));
            if (used[index][alpha]) {
                reads |= half_reads(half, alpha);
                writes |= half_writes(half, alpha);
            }
        }
        live = (live & ~writes) | reads;
    }
}

/* what a half reads of its stage's order and constant selection */
enum { _stage_texture = 1, _stage_ras = 2, _stage_konst = 4 };

static unsigned half_stage_reads(const WlTevHalf *half, int alpha) {
    unsigned reads = 0;
    int index;

    for (index = 0; index < 4; index++) {
        unsigned char code = half->in[index];

        if (alpha)
            reads |= code == WL_TEV_CA_TEXA    ? _stage_texture
                     : code == WL_TEV_CA_RASA  ? _stage_ras
                     : code == WL_TEV_CA_KONST ? _stage_konst
                                               : 0;
        else
            reads |= code == WL_TEV_CC_TEXC || code == WL_TEV_CC_TEXA   ? _stage_texture
                     : code == WL_TEV_CC_RASC || code == WL_TEV_CC_RASA ? _stage_ras
                     : code == WL_TEV_CC_KONST                          ? _stage_konst
                                                                        : 0;
    }
    return reads;
}

/* whether a half that reads and writes these register channels can move
past a stage: none of its used halves reads what the half writes, or writes
what it reads or writes */
static bool stage_passable(const WlTevStage *stage, const unsigned char used[2], unsigned reads,
                           unsigned writes) {
    int alpha;

    for (alpha = 0; alpha < 2; alpha++) {
        const WlTevHalf *half = HALF(stage, alpha);

        if (used[alpha] &&
            ((half_writes(half, alpha) & (reads | writes)) || (half_reads(half, alpha) & writes)))
            return false;
    }
    return true;
}

/* moves a used half of stage `from` into stage `to`, whose half of its kind
is unused, if the texture and colour its other half reads are those the
moving half reads: false when they are not */
static bool half_move(WlTevProgram *tev, unsigned char used[WL_TEV_MAX_STAGES][2], unsigned from,
                      unsigned to, int alpha) {
    const WlTevStage *source = &tev->stages[from];
    WlTevStage *target = &tev->stages[to];
    unsigned moving = half_stage_reads(HALF(source, alpha), alpha);
    unsigned staying = used[to][!alpha] ? half_stage_reads(HALF(target, !alpha), !alpha) : 0;

    if ((moving & staying & _stage_texture) &&
        (source->texcoord != target->texcoord || source->texmap != target->texmap ||
         source->tex_swap != target->tex_swap))
        return false;
    if ((moving & staying & _stage_ras) &&
        (source->channel != target->channel || source->ras_swap != target->ras_swap))
        return false;
    if (moving & _stage_texture) {
        target->texcoord = source->texcoord;
        target->texmap = source->texmap;
        target->tex_swap = source->tex_swap;
    }
    if (moving & _stage_ras) {
        target->channel = source->channel;
        target->ras_swap = source->ras_swap;
    }
    if (alpha)
        target->kasel = source->kasel;
    else
        target->kcsel = source->kcsel;
    *HALF(target, alpha) = *HALF(source, alpha);
    used[to][alpha] = 1;
    used[from][alpha] = 0;
    return true;
}

/* moves the half alone in stage `from` into the unused half of a later or
earlier stage, past stages that do not see it move: false when there is
none */
static bool half_merge(WlTevProgram *tev, unsigned char used[WL_TEV_MAX_STAGES][2], unsigned from) {
    const WlTevHalf *half;
    unsigned reads, writes, to;
    int alpha;

    if (used[from][0] == used[from][1])
        return false;
    alpha = used[from][1];
    half = HALF(&tev->stages[from], alpha);
    reads = half_reads(half, alpha);
    writes = half_writes(half, alpha);
    /* (later: the stage's other half reads its inputs before the moved half
    writes, so it must not read what that writes) */
    for (to = from + 1; to < tev->stage_count; to++) {
        const WlTevStage *target = &tev->stages[to];

        if (!used[to][alpha] &&
            !(used[to][!alpha] && (half_reads(HALF(target, !alpha), !alpha) & writes)) &&
            half_move(tev, used, from, to, alpha))
            return true;
        if (!stage_passable(target, used[to], reads, writes))
            break;
    }
    /* (earlier: the moved half reads its inputs before the other half
    writes, so that must not write what it reads) */
    for (to = from; to-- > 0;) {
        const WlTevStage *target = &tev->stages[to];

        if (!used[to][alpha] &&
            !(used[to][!alpha] && (half_writes(HALF(target, !alpha), !alpha) & reads)) &&
            half_move(tev, used, from, to, alpha))
            return true;
        if (!stage_passable(target, used[to], reads, writes))
            break;
    }
    return false;
}

static void stage_remove(WlTevProgram *tev, unsigned char used[WL_TEV_MAX_STAGES][2],
                         unsigned index) {
    memmove(&tev->stages[index], &tev->stages[index + 1],
            (tev->stage_count - index - 1) * sizeof(tev->stages[0]));
    memmove(used[index], used[index + 1], (tev->stage_count - index - 1) * sizeof(used[0]));
    tev->stage_count--;
}

/* a used half's lerp with the inputs it does not depend on 0; an unused half
passing PREV on, exactly (clamped in stages_compact where that is the same) */
static void half_tidy(WlTevStage *stage, int alpha, bool used) {
    WlTevHalf *half = HALF(stage, alpha);
    unsigned char zero_input = (unsigned char)(alpha ? WL_TEV_CA_ZERO : WL_TEV_CC_ZERO);

    if (!used) {
        (alpha ? wl_tev_stage_alpha : wl_tev_stage_color)(
            stage, zero_input, zero_input, zero_input,
            (unsigned char)(alpha ? WL_TEV_CA_APREV : WL_TEV_CC_CPREV), WL_TEV_ADD,
            WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, false, WL_TEV_PREV);
        return;
    }
    if (half->op != WL_TEV_ADD && half->op != WL_TEV_SUB)
        return;
    if (!alpha && half->in[2] == WL_TEV_CC_ONE)
        half->in[0] = zero_input;
    if (input_zero(half->in[2], alpha))
        half->in[1] = zero_input;
    if (input_zero(half->in[0], alpha) && input_zero(half->in[1], alpha))
        half->in[2] = zero_input;
}

/* takes out the halves and stages the result does not need. A half left
unused passes PREV on, as wl_tev_add_stage leaves it (clamped where PREV is
in [0, 1], else exactly), and a stage's order and constant selections are
only those its halves read. */
static void stages_compact(WlTevProgram *tev) {
    unsigned char used[WL_TEV_MAX_STAGES][2];
    unsigned in_range[WL_TEV_MAX_STAGES];
    unsigned index;
    int alpha;

    stages_used(tev, used);
    for (index = 0; index < tev->stage_count; index++) {
        for (alpha = 0; alpha < 2; alpha++)
            half_tidy(&tev->stages[index], alpha, used[index][alpha]);
    }
    for (index = tev->stage_count; index-- > 0 && tev->stage_count > 1;) {
        if (!used[index][0] && !used[index][1])
            stage_remove(tev, used, index);
    }
    for (index = 0; index < tev->stage_count;) {
        if (half_merge(tev, used, index)) {
            stage_remove(tev, used, index);
            index = 0;
        } else
            index++;
    }
    stages_in_range(tev, in_range);
    for (index = 0; index < tev->stage_count; index++) {
        WlTevStage *stage = &tev->stages[index];
        unsigned color_reads = half_stage_reads(&stage->color, 0),
                 alpha_reads = half_stage_reads(&stage->alpha, 1);

        for (alpha = 0; alpha < 2; alpha++) {
            if (!used[index][alpha])
                HALF(stage, alpha)->clamp =
                    (in_range[index] & REGISTER_CHANNEL(WL_TEV_PREV, alpha)) != 0;
        }
        if (!((color_reads | alpha_reads) & _stage_texture)) {
            stage->texcoord = stage->texmap = WL_TEV_NONE;
            stage->tex_swap = 0;
        }
        if (!((color_reads | alpha_reads) & _stage_ras)) {
            stage->channel = WL_TEV_NONE;
            stage->ras_swap = 0;
        }
        if (!(color_reads & _stage_konst))
            stage->kcsel = WL_TEV_KSEL_1;
        if (!(alpha_reads & _stage_konst))
            stage->kasel = WL_TEV_KSEL_1;
    }
}

/* ---------- bumped lookups

A stage that reads a bumped map (WlXboxPshTextures.bumped: a bumped
reflection's cube map) looks it up where GX's indirect unit puts it, as GX's
environment-mapped bump does (GX_SetTevIndBumpST): over the two stages before
it, the first takes texture coordinate 1 times the bump map's x, the second
adds coordinate 2 times its y, and the lookup adds its own coordinate. The
two read no texture (their own lookups are not made). A lookup right after
another repeats its coordinate. Where the stages before a lookup read a
texture, stages that pass PREV on are put in. (The device makes the three
coordinates per vertex, and sets the indirect unit.) */

static bool stage_free_for_offset(const WlTevProgram *tev, const unsigned char *indirect,
                                  unsigned index) {
    return tev->stages[index].texmap == WL_TEV_NONE && indirect[index] == WL_XBOX_PSH_INDIRECT_NONE;
}

/* a stage that passes PREV on, exactly, put in before stage `index` */
static bool stage_insert_pass(WlTevProgram *tev, unsigned char *indirect, unsigned index) {
    WlTevStage *stage;

    if (tev->stage_count >= WL_TEV_MAX_STAGES)
        return false;
    memmove(&tev->stages[index + 1], &tev->stages[index],
            (tev->stage_count - index) * sizeof(tev->stages[0]));
    memmove(&indirect[index + 1], &indirect[index], tev->stage_count - index);
    tev->stage_count++;
    stage = &tev->stages[index];
    memset(stage, 0, sizeof(*stage));
    stage->texcoord = stage->texmap = stage->channel = WL_TEV_NONE;
    stage->kcsel = stage->kasel = WL_TEV_KSEL_1;
    wl_tev_stage_color(stage, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_CPREV,
                       WL_TEV_ADD, WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, false, WL_TEV_PREV);
    wl_tev_stage_alpha(stage, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_APREV,
                       WL_TEV_ADD, WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, false, WL_TEV_PREV);
    indirect[index] = WL_XBOX_PSH_INDIRECT_NONE;
    return true;
}

/* each bumped lookup's two stages before it: false when they do not fit */
static bool bumped_lookups_place(WlXboxPshProgram *program, unsigned bumped) {
    WlTevProgram *tev = &program->tev;
    unsigned char *indirect = program->indirect;
    unsigned index;

    memset(program->indirect, 0, sizeof(program->indirect));
    for (index = 0; index < tev->stage_count; index++) {
        const WlTevStage *stage = &tev->stages[index];

        if (stage->texmap >= WL_XBOX_PSH_TEXTURES || !(bumped & (1u << stage->texmap)))
            continue;
        if (index > 0 &&
            (indirect[index - 1] == WL_XBOX_PSH_INDIRECT_LOOKUP ||
             indirect[index - 1] == WL_XBOX_PSH_INDIRECT_REPEAT) &&
            tev->stages[index - 1].texmap == stage->texmap &&
            tev->stages[index - 1].texcoord == stage->texcoord) {
            indirect[index] = WL_XBOX_PSH_INDIRECT_REPEAT;
            continue;
        }
        while (index < 2 || !stage_free_for_offset(tev, indirect, index - 1) ||
               !stage_free_for_offset(tev, indirect, index - 2)) {
            unsigned at =
                index >= 1 && stage_free_for_offset(tev, indirect, index - 1) ? index - 1 : index;

            if (!stage_insert_pass(tev, indirect, at))
                return false;
            index++;
        }
        indirect[index - 2] = WL_XBOX_PSH_INDIRECT_S;
        indirect[index - 1] = WL_XBOX_PSH_INDIRECT_T;
        indirect[index] = WL_XBOX_PSH_INDIRECT_LOOKUP;
    }
    return true;
}

/* ---------- public code */

/* the swap tables the stages read through (resources_add): as it is, the
red everywhere, the green everywhere, the blue in the alpha */
static void swap_tables_set(WlTevProgram *tev) {
    wl_tev_set_swap(tev, 1, WL_TEV_SWAP_R, WL_TEV_SWAP_R, WL_TEV_SWAP_R, WL_TEV_SWAP_R);
    wl_tev_set_swap(tev, 2, WL_TEV_SWAP_G, WL_TEV_SWAP_G, WL_TEV_SWAP_G, WL_TEV_SWAP_G);
    wl_tev_set_swap(tev, 3, WL_TEV_SWAP_R, WL_TEV_SWAP_G, WL_TEV_SWAP_B, WL_TEV_SWAP_B);
}

static bool translate_once(const uint32_t *render_state, const WlXboxPshTextures *textures,
                           WlXboxPshProgram *program, bool economical, bool folding, bool preload,
                           int signed_dots) {
    static struct translation translation;
    unsigned long combiner_count = render_state[WL_XBOX_PSH_COMBINER_COUNT] & 0xff;
    unsigned long stage;
    int portion;

    memset(&translation, 0, sizeof(translation));
    memset(program, 0, sizeof(*program));
    wl_tev_program_init(&program->tev);
    swap_tables_set(&program->tev);
    translation.state = render_state;
    translation.textures = textures;
    translation.program = program;
    translation.economical = economical;
    translation.folding = folding;
    translation.signed_dots = signed_dots;
    wl_tev_pool_init(&translation.pools[_portion_color]);
    wl_tev_pool_init(&translation.pools[_portion_alpha]);
    names_initialize(&translation);
    if (preload)
        constants_place(&translation);
    if (combiner_count > 8)
        combiner_count = 8;
    for (stage = 0; stage < combiner_count && !translation.failed; stage++) {
        memcpy(translation.color_before, translation.names[_portion_color],
               sizeof(translation.color_before));
        memcpy(translation.blue_before, translation.blue, sizeof(translation.blue_before));
        memcpy(translation.blue_copy_before, translation.blue_copy,
               sizeof(translation.blue_copy_before));
        memset(translation.blue_copy_taken, 0, sizeof(translation.blue_copy_taken));
        memcpy(translation.elided_before, translation.elided, sizeof(translation.elided_before));
        translation.stage = (int)stage;
        for (portion = 0; portion < NUMBER_OF_PORTIONS; portion++) {
            translation.portion_now = portion;
            combiner_portion(&translation, (int)stage, portion);
        }
        queues_flush(&translation);
        names_retire(&translation, (int)stage);
    }
    memcpy(translation.color_before, translation.names[_portion_color],
           sizeof(translation.color_before));
    memcpy(translation.blue_before, translation.blue, sizeof(translation.blue_before));
    memcpy(translation.blue_copy_before, translation.blue_copy,
           sizeof(translation.blue_copy_before));
    memcpy(translation.elided_before, translation.elided, sizeof(translation.elided_before));
    if (!translation.failed)
        final_combiner(&translation);
    program->reason = translation.failed ? translation.reason : NULL;
    if (translation.failed || program->tev.stage_count == 0)
        return false;
    stages_compact(&program->tev);
    if (textures->bumped && !bumped_lookups_place(program, textures->bumped)) {
        program->reason = "more than 16 stages with the bumped lookups' offsets";
        return false;
    }
    /* (what the hardware would reject: a fault of the translation's) */
    if (!wl_tev_validate(&program->tev, NULL)) {
        program->reason = "an invalid TEV program";
        return false;
    }
    return true;
}

bool wl_xbox_psh_translate(const uint32_t *render_state, const WlXboxPshTextures *textures,
                           const WlXboxPshRecipe *recipe, WlXboxPshProgram *program) {
    /* the signed dot products: where the alpha half reads them, then in the
    colour half only (the alpha half's blue of one approximated), then as the
    translation was before it made them, their inputs clamped; economically,
    1, then as before */
    static const struct {
        bool economical, folding;
        signed char signed_dots;
    } attempts[] = {{false, true, 2},  {false, true, 1},  {false, true, -1}, {false, false, 2},
                    {false, false, 1}, {false, false, -1}, {true, true, 0},  {true, true, -1},
                    {true, false, 0},  {true, false, -1}};
    unsigned attempt, stage;
    int preload;

    /* (the caller's programs made by hand) */
    if (recipe && recipe->make && recipe->make(render_state, textures, program, recipe->user)) {
        program->texcoords = (unsigned char)wl_tev_texcoords_mask(&program->tev);
        program->channels = (unsigned char)wl_tev_channels_mask(&program->tev);
        return true;
    }
    /* each exact, then with constants beyond four approximated (their
    registers free for the program); the first exact program, or else the
    first approximated */
    static WlXboxPshProgram approximated;
    bool have_approximated = false, done = false;

    for (attempt = 0; attempt < sizeof(attempts) / sizeof(attempts[0]) && !done; attempt++) {
        for (preload = 1; preload >= 0 && !done; preload--) {
            if (!translate_once(render_state, textures, program, attempts[attempt].economical,
                                attempts[attempt].folding, preload != 0,
                                attempts[attempt].signed_dots))
                continue;
            if (!program->approximate)
                done = true;
            else if (!have_approximated) {
                approximated = *program;
                have_approximated = true;
            }
        }
    }
    if (!done && !have_approximated)
        return false;
    if (!done)
        *program = approximated;
    /* (what the stages read of the vertices; a bumped lookup's offsets
    read the bump map at coordinate 0 and their terms at 1 and 2) */
    program->texcoords = (unsigned char)wl_tev_texcoords_mask(&program->tev);
    program->channels = (unsigned char)wl_tev_channels_mask(&program->tev);
    for (stage = 0; stage < program->tev.stage_count; stage++) {
        if (program->indirect[stage] != WL_XBOX_PSH_INDIRECT_NONE)
            program->texcoords |= 1u << 0 | 1u << WL_XBOX_PSH_BUMP_TEXCOORD_S |
                                  1u << WL_XBOX_PSH_BUMP_TEXCOORD_T;
    }
    if (textures->bumped || textures->vertex_color || textures->reflected)
        program->vertices = WL_XBOX_PSH_VERTICES_BUMPED_REFLECTION;
    return true;
}

/* (a device that sets GX itself throughout tells WiiLibs' shadow of it
when it is done) */
void wl_xbox_psh_apply(const WlXboxPshProgram *program) {
    wl_tev_apply_unchecked(&program->tev);
}
