// SPDX-License-Identifier: MIT
// The combiners' signed sums made unsigned (psh_bias.h).
//
// Each colour register r is followed as an unsigned value s (what the program written again
// computes in it, within [0, 1]) and an offset k: the NV2A holds s + k. A product the program
// writes again is one of an input and the constant 1 (the zero register inverted):
//  - a half bias of an unsigned input x, x - 1/2: x, offset -1/2 (of the zero register: 0, -1/2);
//  - a half bias negate of the zero register, 1/2: 0, offset 1/2;
//  - a signed read of a register with an offset: its s, its offset;
// and any other product is left as it is (offset 0) where it reads no register with an offset.
// An output adds the products' offsets, halves them with the values (shift right) or takes 1/2
// from the offset (bias); a sum of two values neither of which is known to be 0 is written only
// halved (it stays within [0, 1]); a register is left with an offset only from -1 to 0 (or any up
// to 1 where its value is 0), where s + k is within the NV2A's [-1, 1]. Everything else that reads
// a register with an offset (a product of two inputs, a dot product, the mux, the alpha half's
// blue, the final combiner) refuses the program. The alpha halves are left as they are.
#include "wl/xbox/psh_bias.h"

#include <string.h>

enum {
    REGISTERS = 16,
    REGISTER_ZERO = 0,
    REGISTER_V1 = 5,
    REGISTER_R0 = 12,
    REGISTER_SUM = 15,
    // the input mappings (an input byte's 0xe0)
    MAP_IDENTITY = 0x00,
    MAP_INVERT = 0x20,
    MAP_HALFBIAS = 0x80,
    MAP_HALFBIAS_NEGATE = 0xa0,
    MAP_SIGNED = 0xc0,
    // an output word's flags (its bits 12 on): the dot products, the mux, the mapping, the blues
    // to the alpha
    OUT_DOTS = 0x03,
    OUT_MUX = 0x04,
    OUT_MAPPING = 0x38,
    OUT_BLUE_TO_ALPHA = 0xc0,
    OUT_BIAS = 0x08,
    OUT_HALF = 0x30
};

// a product as written again: its two input bytes, its unsigned value's offset, whether that
// value is known to be 0, whether it was changed
typedef struct {
    unsigned x, y;
    float k;
    bool zero, changed;
} Product;

static bool is_one(unsigned input) {
    return (input & 0xef) == (MAP_INVERT | REGISTER_ZERO);
}

// whether an input byte is the constant 0
static bool is_zero(unsigned input) {
    unsigned mapping = input & 0xe0;

    return (input & 0x0f) == REGISTER_ZERO &&
           (mapping == MAP_IDENTITY || mapping == MAP_SIGNED || mapping == 0xe0);
}

// whether an input reads the colour of a register with an offset (k: the offsets)
static bool reads_offset(unsigned input, const float *k) {
    return !(input & 0x10) && k[input & 0x0f] != 0.0f;
}

// one product of a colour half: x * y, written again; false where it cannot be
static bool product_make(unsigned x, unsigned y, const float *k, Product *p) {
    unsigned v, reg, mapping;

    p->x = x;
    p->y = y;
    p->k = 0.0f;
    p->changed = false;
    p->zero = is_zero(x) || is_zero(y);
    if (!is_one(x) && !is_one(y)) {
        // (left as it is, reading no register with an offset)
        return !reads_offset(x, k) && !reads_offset(y, k);
    }
    // (v, times 1: the input written again, in x's place where it was)
    v = is_one(x) ? y : x;
    reg = v & 0x0f;
    mapping = v & 0xe0;
    if (mapping == MAP_HALFBIAS && !reads_offset(v, k)) {
        v = (v & 0x1f) | MAP_IDENTITY;
        p->k = -0.5f;
    } else if (mapping == MAP_HALFBIAS_NEGATE && reg == REGISTER_ZERO) {
        v = REGISTER_ZERO;
        p->k = 0.5f;
    } else if (mapping == MAP_SIGNED && reads_offset(v, k)) {
        p->k = k[reg];
        v = (v & 0x1f) | MAP_IDENTITY;
    } else {
        return !reads_offset(v, k);
    }
    p->changed = true;
    p->zero = is_zero(v);
    if (is_one(x)) {
        p->y = v;
    } else {
        p->x = v;
    }
    return true;
}

// an output's mapping on an offset (flags: the output word's bits 12 on); false where the value
// would leave [0, 1]
static bool output_offset(unsigned flags, float *k) {
    switch (flags & OUT_MAPPING) {
    case 0:
        return true;
    case OUT_BIAS:
        *k -= 0.5f;
        return true;
    case OUT_HALF:
        *k *= 0.5f;
        return true;
    default:
        return false;
    }
}

// whether a register may hold an offset k: its value plus k within [-1, 1]
static bool offset_fits(float k, bool zero) {
    return (k >= -1.0f && k <= 0.0f) || (zero && k >= -1.0f && k <= 1.0f);
}

bool wl_xbox_psh_unbias(const uint32_t *state, uint32_t *out) {
    float k[REGISTERS], next[REGISTERS];
    bool zero[REGISTERS], next_zero[REGISTERS], changed = false;
    unsigned count = state[WL_XBOX_PSH_COMBINER_COUNT] & 0xffu, stage, i;

    if (count > 8u) {
        return false;
    }
    // (only a half bias or a biased output makes an offset: most programs have none, and are left
    // at once)
    for (stage = 0; stage < count; stage++) {
        uint32_t inputs = state[WL_XBOX_PSH_RGB_INPUTS0 + stage];

        if (((state[WL_XBOX_PSH_RGB_OUTPUTS0 + stage] >> 12) & OUT_MAPPING) == OUT_BIAS) {
            break;
        }

        for (i = 0; i < 4; i++) {
            unsigned mapping = (inputs >> (8 * i)) & 0xe0u;

            if (mapping == MAP_HALFBIAS || mapping == MAP_HALFBIAS_NEGATE) {
                break;
            }
        }
        if (i < 4) {
            break;
        }
    }
    if (stage == count) {
        return false;
    }
    memcpy(out, state, WL_XBOX_PSH_WORDS * sizeof *out);
    memset(k, 0, sizeof k);
    memset(zero, 0, sizeof zero);
    for (stage = 0; stage < count; stage++) {
        uint32_t inputs = state[WL_XBOX_PSH_RGB_INPUTS0 + stage];
        uint32_t outputs = state[WL_XBOX_PSH_RGB_OUTPUTS0 + stage];
        uint32_t alpha = state[WL_XBOX_PSH_ALPHA_INPUTS0 + stage];
        unsigned flags = (unsigned)(outputs >> 12), dest[3], d;
        Product ab, cd;
        float result[3];
        bool result_zero[3], touched;

        // (the alpha half reads its inputs' alphas, or a colour's blue)
        for (i = 0; i < 4; i++) {
            if (reads_offset((alpha >> (24 - 8 * i)) & 0xffu, k)) {
                return false;
            }
        }
        if (!product_make((inputs >> 24) & 0xffu, (inputs >> 16) & 0xffu, k, &ab) ||
            !product_make((inputs >> 8) & 0xffu, inputs & 0xffu, k, &cd)) {
            return false;
        }
        // (a biased output, too: its -1/2 is an offset)
        touched = ab.changed || cd.changed || (flags & OUT_MAPPING) == OUT_BIAS;
        memcpy(next, k, sizeof next);
        memcpy(next_zero, zero, sizeof next_zero);
        dest[0] = (outputs >> 4) & 0x0fu; // AB
        dest[1] = outputs & 0x0fu;        // CD
        dest[2] = (outputs >> 8) & 0x0fu; // the sum (or the mux)
        if (touched) {
            if (flags & (OUT_DOTS | OUT_MUX | OUT_BLUE_TO_ALPHA)) {
                return false;
            }
            result[0] = ab.k;
            result[1] = cd.k;
            result[2] = ab.k + cd.k;
            result_zero[0] = ab.zero;
            result_zero[1] = cd.zero;
            result_zero[2] = ab.zero && cd.zero;
            // (a sum of two values above 0 may pass 1 unless halved)
            if (dest[2] != REGISTER_ZERO && !ab.zero && !cd.zero &&
                (flags & OUT_MAPPING) != OUT_HALF) {
                return false;
            }
            for (d = 0; d < 3; d++) {
                if (dest[d] == REGISTER_ZERO) {
                    continue;
                }
                if (!output_offset(flags, &result[d]) || !offset_fits(result[d], result_zero[d])) {
                    return false;
                }
                next[dest[d]] = result[d];
                next_zero[dest[d]] = result_zero[d];
            }
            out[WL_XBOX_PSH_RGB_INPUTS0 + stage] = ab.x << 24 | ab.y << 16 | cd.x << 8 | cd.y;
            // (a bias is the offset's now)
            if ((flags & OUT_MAPPING) == OUT_BIAS) {
                out[WL_XBOX_PSH_RGB_OUTPUTS0 + stage] = outputs & ~((uint32_t)OUT_MAPPING << 12);
            }
            changed = true;
        } else {
            for (d = 0; d < 3; d++) {
                next[dest[d]] = 0.0f;
                next_zero[dest[d]] = false;
            }
        }
        next[REGISTER_ZERO] = 0.0f;
        next_zero[REGISTER_ZERO] = false;
        memcpy(k, next, sizeof k);
        memcpy(zero, next_zero, sizeof zero);
    }
    if (!changed) {
        return false;
    }
    // the final combiner reads the true values (with it off, r0)
    if (state[WL_XBOX_PSH_FINAL_INPUTS_ABCD] == 0 && state[WL_XBOX_PSH_FINAL_INPUTS_EFG] == 0) {
        return k[REGISTER_R0] == 0.0f;
    }
    for (i = 0; i < 7; i++) {
        uint32_t word = state[i < 4 ? WL_XBOX_PSH_FINAL_INPUTS_ABCD : WL_XBOX_PSH_FINAL_INPUTS_EFG];
        unsigned reg = (word >> (24 - 8 * (i % 4))) & 0x0fu;

        if (k[reg] != 0.0f ||
            (reg == REGISTER_SUM && (k[REGISTER_R0] != 0.0f || k[REGISTER_V1] != 0.0f))) {
            return false;
        }
    }
    return true;
}
