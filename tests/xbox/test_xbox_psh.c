// SPDX-License-Identifier: MIT
// wl/xbox/psh: the NV2A's register combiners translated to TEV programs. Random combiner programs
// (synthetic: random inputs, mappings, outputs, constants and final combiners) are translated, and
// each exact one's TEV program, simulated here, is compared with the combiners evaluated directly,
// in the translation's own model of them: signed mappings clamped to [0, 1] outside dot products,
// registers clamped to [0, 1], an unsigned dot product saturating at 1, a signed one NV2A's (below
// 0 as 0), the mux a blend by r0's alpha. The simulation is TEV's: a, b and c read 8 bits (a
// register's value outside [0, 1] wraps), d the whole signed value, an unclamped result kept from
// -4 to 4. Every program is valid to WiiLibs' validator, and the programs are the same on every
// target (a digest of them all, the same on the big-endian ppc-be build). Then the recipe hook,
// the fog, D0's alpha from a texture and a bumped lookup through GX's indirect unit.
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "greatest.h"
#include "wl/xbox/psh.h"

static uint32_t g_rng;
static uint32_t rnd(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 8;
}

typedef struct {
    float c[4]; // r g b a
} V4;

static float clamp01(float x) {
    return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
}

static float frand(void) {
    return (float)(rnd() % 256u) / 255.0f;
}

static V4 rand4(void) {
    V4 v;
    for (int i = 0; i < 4; i++) {
        v.c[i] = frand();
    }
    return v;
}

static V4 argb_to_v4(uint32_t d) {
    V4 v = {{(float)((d >> 16) & 255u) / 255.0f, (float)((d >> 8) & 255u) / 255.0f,
             (float)(d & 255u) / 255.0f, (float)(d >> 24) / 255.0f}};
    return v;
}

static V4 rgba_to_v4(uint32_t d) {
    V4 v = {{(float)(d >> 24) / 255.0f, (float)((d >> 16) & 255u) / 255.0f,
             (float)((d >> 8) & 255u) / 255.0f, (float)(d & 255u) / 255.0f}};
    return v;
}

// ---- the combiners, evaluated ------------------------------------------------------------------

typedef struct {
    V4 t[4];
    V4 v0, v1;
} Inputs;

// the fog: on or not, its colour, and a pixel's factor; D0's alpha read from this map (0: none)
static int g_fog_on;
static uint32_t g_fog_color;
static float g_fog_factor;
static unsigned g_d0_alpha_map;

static V4 reg_value(const uint32_t *s, unsigned reg, int stage, const V4 *regs) {
    V4 zero = {{0, 0, 0, 0}};
    uint32_t count = s[WL_XBOX_PSH_COMBINER_COUNT];
    switch (reg) {
    case 1:
        return argb_to_v4(stage < 0 ? s[WL_XBOX_PSH_FINAL_CONSTANT0]
                                    : s[WL_XBOX_PSH_CONSTANT0_0 + ((count & 0x1000u) ? stage : 0)]);
    case 2:
        return argb_to_v4(stage < 0
                              ? s[WL_XBOX_PSH_FINAL_CONSTANT1]
                              : s[WL_XBOX_PSH_CONSTANT1_0 + ((count & 0x10000u) ? stage : 0)]);
    case 3: {
        V4 f = argb_to_v4(g_fog_color);
        f.c[3] = g_fog_on ? g_fog_factor : 1.0f;
        return f;
    }
    case 4:
    case 5:
    case 8:
    case 9:
    case 10:
    case 11:
    case 12:
    case 13:
        return regs[reg];
    default:
        return zero;
    }
}

static float map_input(float x, unsigned mapping) {
    switch (mapping) {
    case 0x00:
    case 0xc0:
        return clamp01(x);
    case 0x20:
        return 1.0f - clamp01(x);
    case 0x40:
        return clamp01(2.0f * x - 1.0f);
    case 0x60:
        return clamp01(1.0f - 2.0f * clamp01(x));
    case 0x80:
        return clamp01(x - 0.5f);
    case 0xa0:
        return clamp01(0.5f - clamp01(x));
    default:
        return -clamp01(x);
    }
}

static float out_map(float x, unsigned flags) {
    switch (flags & 0x38u) {
    case 0x08:
        x = x - 0.5f;
        break;
    case 0x10:
        x = x * 2.0f;
        break;
    case 0x18:
        x = (x - 0.5f) * 2.0f;
        break;
    case 0x20:
        x = x * 4.0f;
        break;
    case 0x30:
        x = x * 0.5f;
        break;
    default:
        break;
    }
    return clamp01(x);
}

// a combiner input mapping, NV2A's, signed (for dot products)
static float signed_input(float x, unsigned mapping) {
    x = clamp01(x);
    switch (mapping) {
    case 0x20:
        return 1.0f - x;
    case 0x40:
        return 2.0f * x - 1.0f;
    case 0x60:
        return 1.0f - 2.0f * x;
    case 0x80:
        return x - 0.5f;
    case 0xa0:
        return 0.5f - x;
    case 0xe0:
        return -x;
    default:
        return x;
    }
}

static bool mapping_is_signed(unsigned mapping) {
    return mapping == 0x40 || mapping == 0x60 || mapping == 0x80 || mapping == 0xa0;
}

// a dot product of inputs k and k + 1
static float dot_of(uint32_t inputs, int k, float raw[4][4], float mapped[4][4]) {
    unsigned mk = (inputs >> (24 - 8 * k)) & 0xe0u, ml = (inputs >> (16 - 8 * k)) & 0xe0u;
    float dot = 0.0f;
    if (mapping_is_signed(mk) || mapping_is_signed(ml)) {
        for (int c = 0; c < 3; c++) {
            dot += signed_input(raw[k][c], mk) * signed_input(raw[k + 1][c], ml);
        }
        return dot < 0.0f ? 0.0f : dot > 1.0f ? 1.0f : dot;
    }
    for (int c = 0; c < 3; c++) {
        dot += mapped[k][c] * mapped[k + 1][c];
    }
    return dot > 1.0f ? 1.0f : dot < -1.0f ? -1.0f : dot;
}

static V4 reference(const uint32_t *s, const Inputs *in, unsigned present) {
    V4 regs[16];
    memset(regs, 0, sizeof regs);
    for (int i = 0; i < 4; i++) {
        if (present & (1u << i)) {
            regs[8 + i] = in->t[i];
        }
    }
    regs[4] = in->v0;
    regs[5] = in->v1;
    regs[12].c[3] = regs[8].c[3];
    int count = (int)(s[WL_XBOX_PSH_COMBINER_COUNT] & 0xffu);
    for (int st = 0; st < count; st++) {
        V4 next[16];
        memcpy(next, regs, sizeof regs);
        for (int portion = 0; portion < 2; portion++) {
            uint32_t inputs =
                s[(portion ? WL_XBOX_PSH_ALPHA_INPUTS0 : WL_XBOX_PSH_RGB_INPUTS0) + st];
            uint32_t outputs =
                s[(portion ? WL_XBOX_PSH_ALPHA_OUTPUTS0 : WL_XBOX_PSH_RGB_OUTPUTS0) + st];
            unsigned flags = outputs >> 12;
            int channels = portion ? 1 : 3, first = portion ? 3 : 0;
            float ab[4] = {0}, cd[4] = {0}, sum[4] = {0}, mapped[4][4] = {{0}}, raw[4][4] = {{0}};
            for (int ch = first; ch < first + channels; ch++) {
                for (int k = 0; k < 4; k++) {
                    unsigned byte = (inputs >> (24 - 8 * k)) & 0xffu;
                    V4 r = reg_value(s, byte & 15u, st, regs);
                    float x = portion ? ((byte & 0x10u) ? r.c[3] : r.c[2])
                                      : ((byte & 0x10u) ? r.c[3] : r.c[ch]);
                    raw[k][ch] = x;
                    mapped[k][ch] = map_input(x, byte & 0xe0u);
                }
            }
            for (int ch = first; ch < first + channels; ch++) {
                ab[ch] = mapped[0][ch] * mapped[1][ch];
                cd[ch] = mapped[2][ch] * mapped[3][ch];
                // (dot products: over red, green and blue; saturating at 1, as the TEV's sum of
                // the three terms does)
                if (!portion && (flags & 2u)) {
                    ab[ch] = dot_of(inputs, 0, raw, mapped);
                }
                if (!portion && (flags & 1u)) {
                    cd[ch] = dot_of(inputs, 2, raw, mapped);
                }
                if (flags & 4u) {
                    float m = regs[12].c[3];
                    sum[ch] = (1.0f - m) * ab[ch] + m * cd[ch];
                } else {
                    sum[ch] = ab[ch] + cd[ch];
                }
            }
            unsigned abn = (outputs >> 4) & 15u, cdn = outputs & 15u, sumn = (outputs >> 8) & 15u;
            for (int ch = first; ch < first + channels; ch++) {
                if (abn >= 4 && abn != 6 && abn != 7) {
                    next[abn].c[ch] = out_map(ab[ch], flags);
                }
                if (cdn >= 4 && cdn != 6 && cdn != 7) {
                    next[cdn].c[ch] = out_map(cd[ch], flags);
                }
                if (sumn >= 4 && sumn != 6 && sumn != 7) {
                    next[sumn].c[ch] = out_map(sum[ch], flags);
                }
            }
        }
        memcpy(regs, next, sizeof regs);
    }
    uint32_t abcd = s[WL_XBOX_PSH_FINAL_INPUTS_ABCD], efg = s[WL_XBOX_PSH_FINAL_INPUTS_EFG];
    if (!abcd && !efg) {
        return regs[12];
    }
    V4 result, ef = {{0}}, sumv = {{0}};
    unsigned settings = efg & 0xffu;
    for (int ch = 0; ch < 3; ch++) {
        unsigned e = (efg >> 24) & 0xffu, f = (efg >> 16) & 0xffu;
        V4 re = reg_value(s, e & 15u, -1, regs), rf = reg_value(s, f & 15u, -1, regs);
        float xe = clamp01((e & 0x10u) ? re.c[3] : re.c[ch]),
              xf = clamp01((f & 0x10u) ? rf.c[3] : rf.c[ch]);
        if (e & 0x20u) {
            xe = 1.0f - xe;
        }
        if (f & 0x20u) {
            xf = 1.0f - xf;
        }
        ef.c[ch] = xe * xf;
        float a = clamp01(regs[5].c[ch]), b = clamp01(regs[12].c[ch]);
        if (settings & 0x40u) {
            a = 1.0f - a;
        }
        if (settings & 0x20u) {
            b = 1.0f - b;
        }
        sumv.c[ch] = clamp01(a + b);
    }
    regs[14] = sumv;
    regs[15] = ef;
    for (int ch = 0; ch < 3; ch++) {
        float v[4];
        for (int k = 0; k < 4; k++) {
            unsigned byte = (abcd >> (24 - 8 * k)) & 0xffu;
            unsigned reg = byte & 15u;
            V4 r = (reg == 14 || reg == 15) ? regs[reg] : reg_value(s, reg, -1, regs);
            float x = clamp01((byte & 0x10u) ? r.c[3] : r.c[ch]);
            if (byte & 0x20u) {
                x = 1.0f - x;
            }
            v[k] = x;
        }
        result.c[ch] = clamp01(v[0] * v[1] + (1.0f - v[0]) * v[2] + v[3]);
    }
    unsigned g = (efg >> 8) & 0xffu;
    V4 r = reg_value(s, g & 15u, -1, regs);
    float x = clamp01((g & 0x10u) ? r.c[3] : r.c[2]);
    if (g & 0x20u) {
        x = 1.0f - x;
    }
    result.c[3] = x;
    return result;
}

// ---- the TEV program, simulated ----------------------------------------------------------------

// a, b and c: 8 bits (a register's signed value wraps)
static float tev_abc(float x) {
    if (x >= 0.0f && x <= 1.0f) {
        return x;
    }
    return (float)(((int)lroundf(x * 255.0f)) & 255) / 255.0f;
}

// a result: clamped to [0, 1], or kept from -1024/255 to 1023/255
static float tev_result(float x, bool unclamped) {
    if (!unclamped) {
        return clamp01(x);
    }
    return x < -1024.0f / 255.0f ? -1024.0f / 255.0f : x > 1023.0f / 255.0f ? 1023.0f / 255.0f : x;
}

static float konst_sel(const WlXboxPshProgram *p, unsigned sel, int ch, bool alpha_half) {
    static const float fractions[8] = {1.0f,     7 / 8.0f, 6 / 8.0f, 5 / 8.0f,
                                       4 / 8.0f, 3 / 8.0f, 2 / 8.0f, 1 / 8.0f};
    if (sel < 8) {
        return fractions[sel];
    }
    if (sel >= 0x0c && sel <= 0x0f && !alpha_half) {
        return rgba_to_v4(p->tev.konst[sel - 0x0c]).c[ch];
    }
    if (sel >= 0x10) {
        unsigned which = (sel - 0x10) / 4, slot = (sel - 0x10) % 4;
        return rgba_to_v4(p->tev.konst[slot]).c[which];
    }
    return 0.0f;
}

static V4 swapped(const WlXboxPshProgram *p, V4 value, unsigned table) {
    V4 out;
    for (int i = 0; i < 4; i++) {
        out.c[i] = value.c[p->tev.swap[table][i] & 3u];
    }
    return out;
}

static float half_result(const WlTevHalf *h, const float v[4]) {
    float r = v[3] + (1.0f - v[2]) * v[0] + v[2] * v[1];
    if (h->op == WL_TEV_SUB) {
        r = v[3] - ((1.0f - v[2]) * v[0] + v[2] * v[1]);
    }
    r += h->bias == WL_TEV_BIAS_ADDHALF ? 0.5f : h->bias == WL_TEV_BIAS_SUBHALF ? -0.5f : 0.0f;
    r *= h->scale == WL_TEV_SCALE_2    ? 2.0f
         : h->scale == WL_TEV_SCALE_4  ? 4.0f
         : h->scale == WL_TEV_DIVIDE_2 ? 0.5f
                                       : 1.0f;
    return tev_result(r, !h->clamp);
}

// maps: by texture map (the fog ramp at WL_XBOX_PSH_FOG_TEXMAP)
static V4 simulate(const WlXboxPshProgram *p, const Inputs *in, const V4 *maps) {
    V4 regs[4];
    memset(regs, 0, sizeof regs);
    for (unsigned i = 0; i < 4; i++) {
        if (p->tev.preload_mask & (1u << i)) {
            regs[i] = rgba_to_v4(p->tev.preload[i]);
        }
    }
    for (unsigned i = 0; i < p->tev.stage_count; i++) {
        const WlTevStage *st = &p->tev.stages[i];
        V4 tex = {{0}}, ras = {{0}}, nregs[4];
        if (st->texmap != WL_TEV_NONE) {
            tex = maps[st->texmap];
        }
        tex = swapped(p, tex, st->tex_swap);
        if (st->channel != WL_TEV_NONE) {
            ras = st->channel == WL_TEV_CH_COLOR1A1 ? in->v1 : in->v0;
        }
        ras = swapped(p, ras, st->ras_swap);
        memcpy(nregs, regs, sizeof regs);
        for (int ch = 0; ch < 3; ch++) {
            float v[4];
            for (int k = 0; k < 4; k++) {
                float x;
                switch (st->color.in[k]) {
                case WL_TEV_CC_CPREV:
                    x = regs[0].c[ch];
                    break;
                case WL_TEV_CC_APREV:
                    x = regs[0].c[3];
                    break;
                case WL_TEV_CC_C0:
                    x = regs[1].c[ch];
                    break;
                case WL_TEV_CC_A0:
                    x = regs[1].c[3];
                    break;
                case WL_TEV_CC_C1:
                    x = regs[2].c[ch];
                    break;
                case WL_TEV_CC_A1:
                    x = regs[2].c[3];
                    break;
                case WL_TEV_CC_C2:
                    x = regs[3].c[ch];
                    break;
                case WL_TEV_CC_A2:
                    x = regs[3].c[3];
                    break;
                case WL_TEV_CC_TEXC:
                    x = tex.c[ch];
                    break;
                case WL_TEV_CC_TEXA:
                    x = tex.c[3];
                    break;
                case WL_TEV_CC_RASC:
                    x = ras.c[ch];
                    break;
                case WL_TEV_CC_RASA:
                    x = ras.c[3];
                    break;
                case WL_TEV_CC_ONE:
                    x = 1.0f;
                    break;
                case WL_TEV_CC_HALF:
                    x = 0.5f;
                    break;
                case WL_TEV_CC_KONST:
                    x = konst_sel(p, st->kcsel, ch, false);
                    break;
                default:
                    x = 0.0f;
                    break;
                }
                v[k] = k < 3 ? tev_abc(x) : x;
            }
            nregs[st->color.dest].c[ch] = half_result(&st->color, v);
        }
        float v[4];
        for (int k = 0; k < 4; k++) {
            float x;
            switch (st->alpha.in[k]) {
            case WL_TEV_CA_APREV:
                x = regs[0].c[3];
                break;
            case WL_TEV_CA_A0:
                x = regs[1].c[3];
                break;
            case WL_TEV_CA_A1:
                x = regs[2].c[3];
                break;
            case WL_TEV_CA_A2:
                x = regs[3].c[3];
                break;
            case WL_TEV_CA_TEXA:
                x = tex.c[3];
                break;
            case WL_TEV_CA_RASA:
                x = ras.c[3];
                break;
            case WL_TEV_CA_KONST:
                x = konst_sel(p, st->kasel, 3, true);
                break;
            default:
                x = 0.0f;
                break;
            }
            v[k] = k < 3 ? tev_abc(x) : x;
        }
        nregs[st->alpha.dest].c[3] = half_result(&st->alpha, v);
        memcpy(regs, nregs, sizeof regs);
    }
    return regs[0];
}

// ---- random programs ---------------------------------------------------------------------------

static unsigned rand_input(int alpha_portion) {
    static const unsigned regs[] = {0, 1, 2, 3, 4, 5, 8, 9, 10, 12, 13};
    static const unsigned maps[] = {0x00, 0x20, 0x40, 0x60, 0x80, 0xa0, 0xc0, 0xe0};
    unsigned reg = regs[rnd() % 11u];
    unsigned alpha = rnd() % 3u == 0 ? 0x10u : 0u;
    if (alpha_portion && rnd() % 4u) {
        alpha = 0x10u; // (and blues, a quarter of the time)
    }
    return reg | alpha | maps[rnd() % 8u];
}

static unsigned rand_output(void) {
    static const unsigned names[] = {0, 0, 0, 12, 13, 8, 9, 4};
    return names[rnd() % 8u];
}

static void program_random(uint32_t *state) {
    memset(state, 0, WL_XBOX_PSH_WORDS * sizeof *state);
    g_fog_on = (int)(rnd() % 2u);
    g_fog_color = rnd() * 2654435761u;
    unsigned count = 1 + rnd() % 4u;
    state[WL_XBOX_PSH_COMBINER_COUNT] =
        count | (rnd() % 2u ? 0x1000u : 0u) | (rnd() % 2u ? 0x10000u : 0u);
    for (int i = 0; i < 8; i++) {
        state[WL_XBOX_PSH_CONSTANT0_0 + i] = rnd() * 2654435761u;
        state[WL_XBOX_PSH_CONSTANT1_0 + i] = rnd() * 40503u * 65537u;
    }
    state[WL_XBOX_PSH_FINAL_CONSTANT0] = rnd() * 2246822519u;
    state[WL_XBOX_PSH_FINAL_CONSTANT1] = rnd() * 3266489917u;
    for (unsigned st = 0; st < count; st++) {
        for (int portion = 0; portion < 2; portion++) {
            static const unsigned mapflags[] = {0, 0, 0x08, 0x10, 0x18, 0x20, 0x30};
            uint32_t in = 0;
            for (int k = 0; k < 4; k++) {
                in = (in << 8) | rand_input(portion);
            }
            unsigned ab = rand_output(), cd = rand_output(), sum = rand_output();
            if (ab && ab == cd) {
                cd = 0;
            }
            if (sum && (sum == ab || sum == cd)) {
                sum = 0;
            }
            unsigned flags = mapflags[rnd() % 7u] | (sum && rnd() % 4u == 0 ? 0x04u : 0u) |
                             (!portion && rnd() % 5u == 0 ? 0x02u : 0u) |
                             (!portion && rnd() % 5u == 0 ? 0x01u : 0u);
            state[(portion ? WL_XBOX_PSH_ALPHA_INPUTS0 : WL_XBOX_PSH_RGB_INPUTS0) + st] = in;
            state[(portion ? WL_XBOX_PSH_ALPHA_OUTPUTS0 : WL_XBOX_PSH_RGB_OUTPUTS0) + st] =
                flags << 12 | sum << 8 | ab << 4 | cd;
        }
    }
    if (rnd() % 3u) {
        static const unsigned r[] = {0, 1, 2, 3, 4, 5, 8, 9, 12, 13, 14, 15};
        static const unsigned es[] = {0, 1, 4, 8, 12}, fs[] = {0, 2, 5, 9, 13},
                              gs[] = {0x1c, 0x18, 0x14, 0x1d, 0x11};
        uint32_t abcd = 0;
        for (int k = 0; k < 4; k++) {
            unsigned reg = r[rnd() % 12u], alpha = rnd() % 3u == 0 ? 0x10u : 0u;
            // (the sum and the product E F have no alpha: NV2A refuses their alpha as an input)
            if (reg >= 14) {
                alpha = 0;
            }
            abcd = (abcd << 8) | reg | alpha | (rnd() % 3u == 0 ? 0x20u : 0u);
        }
        unsigned e = es[rnd() % 5u], f = fs[rnd() % 5u];
        unsigned g = gs[rnd() % 5u] | (rnd() % 3u == 0 ? 0x20u : 0u);
        state[WL_XBOX_PSH_FINAL_INPUTS_ABCD] = abcd;
        state[WL_XBOX_PSH_FINAL_INPUTS_EFG] =
            e << 24 | f << 16 | g << 8 | (rnd() % 2u ? 0x20u : 0u) | (rnd() % 2u ? 0x40u : 0u);
    }
}

static void textures_make(WlXboxPshTextures *textures, unsigned present) {
    memset(textures, 0, sizeof *textures);
    textures->present = present;
    textures->fog = (unsigned char)g_fog_on;
    textures->fog_color = g_fog_color & 0x00FFFFFFu;
    textures->d0_alpha_map = (unsigned char)g_d0_alpha_map;
}

// a program's fields folded into a digest (FNV-1a over the values, not the bytes: the same on
// every target)
static uint32_t g_digest = 2166136261u;
static void fold(uint32_t value) {
    for (int i = 0; i < 4; i++) {
        g_digest = (g_digest ^ ((value >> (8 * i)) & 255u)) * 16777619u;
    }
}

static void fold_half(const WlTevHalf *h) {
    for (int k = 0; k < 4; k++) {
        fold(h->in[k]);
    }
    fold((uint32_t)h->op << 24 | (uint32_t)h->bias << 16 | (uint32_t)h->scale << 8 | h->clamp);
    fold(h->dest);
}

static void fold_program(bool usable, const WlXboxPshProgram *p) {
    fold(usable);
    if (!usable) {
        return;
    }
    fold(p->tev.stage_count);
    for (unsigned i = 0; i < p->tev.stage_count; i++) {
        const WlTevStage *s = &p->tev.stages[i];
        fold((uint32_t)s->texcoord << 24 | (uint32_t)s->texmap << 16 | (uint32_t)s->channel << 8 |
             s->ras_swap);
        fold((uint32_t)s->tex_swap << 16 | (uint32_t)s->kcsel << 8 | s->kasel);
        fold_half(&s->color);
        fold_half(&s->alpha);
        fold(p->indirect[i]);
    }
    for (unsigned t = 0; t < WL_TEV_SWAP_TABLES; t++) {
        fold((uint32_t)p->tev.swap[t][0] << 24 | (uint32_t)p->tev.swap[t][1] << 16 |
             (uint32_t)p->tev.swap[t][2] << 8 | p->tev.swap[t][3]);
    }
    for (unsigned k = 0; k < WL_TEV_KONST_SLOTS; k++) {
        fold(p->tev.konst_mask & (1u << k) ? p->tev.konst[k] : 0u);
    }
    for (unsigned r = 0; r < WL_TEV_REG_COUNT; r++) {
        fold(p->tev.preload_mask & (1u << r) ? p->tev.preload[r] : 0u);
    }
    fold((uint32_t)p->tev.konst_mask << 8 | p->tev.preload_mask);
    fold((uint32_t)p->texcoords << 16 | (uint32_t)p->channels << 8 | p->vertices);
    fold(p->approximate);
}

// 1: exact and agrees, 0: differs, -1: not translated, -2: approximate
static int check(const uint32_t *state, unsigned present, const char **why) {
    WlXboxPshTextures textures;
    static WlXboxPshProgram program;
    textures_make(&textures, present);
    bool usable = wl_xbox_psh_translate(state, &textures, NULL, &program);
    fold_program(usable, &program);
    if (!usable) {
        *why = program.reason;
        return -1;
    }
    if (!wl_tev_validate(&program.tev, NULL)) {
        *why = "refused by the validator";
        return 0;
    }
    if (program.approximate) {
        return -2;
    }
    for (int trial = 0; trial < 50; trial++) {
        Inputs in;
        V4 maps[8];
        for (int i = 0; i < 4; i++) {
            in.t[i] = rand4();
        }
        in.t[0].c[3] = (float)(rnd() % 2u); // (r0.a starts as t0.a: binary, for the mux)
        in.v0 = rand4();
        in.v1 = rand4();
        memset(maps, 0, sizeof maps);
        g_fog_factor = frand();
        for (int ch = 0; ch < 4; ch++) {
            maps[WL_XBOX_PSH_FOG_TEXMAP].c[ch] = g_fog_factor;
        }
        // (an intensity texture holding the vertex's alpha)
        if (g_d0_alpha_map) {
            for (int ch = 0; ch < 4; ch++) {
                maps[g_d0_alpha_map].c[ch] = in.v0.c[3];
            }
        }
        for (int i = 0; i < 4; i++) {
            maps[i] = in.t[i];
        }
        V4 want = reference(state, &in, present), got = simulate(&program, &in, maps);
        for (int ch = 0; ch < 4; ch++) {
            if (fabsf(want.c[ch] - got.c[ch]) > 0.01f) {
                *why = "the TEV program differs from the combiners";
                return 0;
            }
        }
    }
    return 1;
}

enum { RANDOM_PROGRAMS = 1500 };

TEST random_programs_agree_with_the_combiners(void) {
    static uint32_t state[WL_XBOX_PSH_WORDS];
    int exact = 0, approximate = 0, untranslated = 0, d0_exact = 0;
    g_rng = 0x5EED1234u;
    g_digest = 2166136261u;
    for (int n = 0; n < RANDOM_PROGRAMS; n++) {
        const char *why = NULL;
        program_random(state);
        int r = check(state, 0x7u, &why);
        if (r == 0) {
            FAILm(why);
        }
        exact += r == 1;
        approximate += r == -2;
        untranslated += r == -1;
        if (r == 1) {
            // the same with D0's alpha from a texture (map 7, or 6 where the fog ramp has 7)
            g_d0_alpha_map = g_fog_on ? 6u : 7u;
            r = check(state, 0x7u, &why);
            g_d0_alpha_map = 0;
            if (r == 0) {
                FAILm(why);
            }
            d0_exact += r == 1;
        }
    }
    // (most are exact (897 of them, 813 before a name written again was seen to free its register);
    // the rest beyond GX or approximated, never wrong)
    ASSERT(exact > RANDOM_PROGRAMS / 2);
    ASSERT(d0_exact > exact * 9 / 10);
    ASSERT(approximate + untranslated > 0);
    // the same programs, field for field, on every target
    ASSERT_EQ_FMT(0x5E3D0355u, g_digest, "%08x");
    PASS();
}

// ---- particular programs -----------------------------------------------------------------------

// r0 = t0 * v0, the final combiner off
static void modulate(uint32_t *state) {
    memset(state, 0, WL_XBOX_PSH_WORDS * sizeof *state);
    state[WL_XBOX_PSH_COMBINER_COUNT] = 1;
    state[WL_XBOX_PSH_RGB_INPUTS0] = 0x08040000u;   // A t0, B v0
    state[WL_XBOX_PSH_ALPHA_INPUTS0] = 0x18140000u; // their alphas
    state[WL_XBOX_PSH_RGB_OUTPUTS0] = 0x000000C0u;  // AB to r0
    state[WL_XBOX_PSH_ALPHA_OUTPUTS0] = 0x000000C0u;
}

TEST a_modulation(void) {
    uint32_t state[WL_XBOX_PSH_WORDS];
    WlXboxPshTextures textures;
    WlXboxPshProgram program;
    modulate(state);
    g_fog_on = 0;
    g_d0_alpha_map = 0;
    textures_make(&textures, 1u);
    ASSERT(wl_xbox_psh_translate(state, &textures, NULL, &program));
    ASSERT(!program.approximate);
    ASSERT(program.reason == NULL);
    // (r0 into a register, then into PREV, the result's)
    ASSERT_EQ(2u, program.tev.stage_count);
    ASSERT_EQ(1u, program.texcoords); // GX_TEXCOORD0
    ASSERT_EQ(1u, program.channels);  // COLOR0A0
    ASSERT_EQ(WL_XBOX_PSH_VERTICES_PROGRAM, program.vertices);
    ASSERT_EQ(0u, program.tev.stages[0].texmap);
    ASSERT_EQ(WL_TEV_CH_COLOR0A0, program.tev.stages[0].channel);
    PASS();
}

static int g_recipe_calls;
static bool recipe_make(const uint32_t *state, const WlXboxPshTextures *textures,
                        WlXboxPshProgram *program, void *user) {
    (void)textures;
    g_recipe_calls++;
    if (state[WL_XBOX_PSH_RGB_INPUTS0] != 0x08040000u || user != &g_recipe_calls) {
        return false;
    }
    // (texture 1 through colour 1: what no translation would make of it)
    memset(program, 0, sizeof *program);
    wl_tev_program_init(&program->tev);
    WlTevStage *stage = wl_tev_add_stage(&program->tev);
    wl_tev_stage_order(stage, 1, 1, WL_TEV_CH_COLOR1A1);
    wl_tev_stage_color(stage, WL_TEV_CC_ZERO, WL_TEV_CC_TEXC, WL_TEV_CC_RASC, WL_TEV_CC_ZERO,
                       WL_TEV_ADD, WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, true, WL_TEV_PREV);
    program->vertices = WL_XBOX_PSH_VERTICES_RECIPE;
    return true;
}

TEST a_recipe_comes_first(void) {
    uint32_t state[WL_XBOX_PSH_WORDS];
    WlXboxPshTextures textures;
    WlXboxPshProgram program;
    WlXboxPshRecipe recipe = {recipe_make, &g_recipe_calls};
    modulate(state);
    textures_make(&textures, 1u);
    g_recipe_calls = 0;
    ASSERT(wl_xbox_psh_translate(state, &textures, &recipe, &program));
    ASSERT_EQ(1, g_recipe_calls);
    ASSERT_EQ(WL_XBOX_PSH_VERTICES_RECIPE, program.vertices);
    // (the texture coordinates and colour channels its stages read)
    ASSERT_EQ(2u, program.texcoords);
    ASSERT_EQ(2u, program.channels);
    // a recipe that declines: the translation's own
    state[WL_XBOX_PSH_RGB_INPUTS0] = 0x04080000u;
    ASSERT(wl_xbox_psh_translate(state, &textures, &recipe, &program));
    ASSERT_EQ(2, g_recipe_calls);
    ASSERT_EQ(WL_XBOX_PSH_VERTICES_PROGRAM, program.vertices);
    ASSERT_EQ(1u, program.channels);
    PASS();
}

TEST fog_reads_the_ramp(void) {
    uint32_t state[WL_XBOX_PSH_WORDS];
    WlXboxPshTextures textures;
    WlXboxPshProgram program;
    // the final combiner: A fog's alpha, B r0, C the fog's colour (lerp to it by the factor)
    modulate(state);
    state[WL_XBOX_PSH_FINAL_INPUTS_ABCD] = 0x130C0300u;
    state[WL_XBOX_PSH_FINAL_INPUTS_EFG] = 0x00001C00u;
    g_fog_on = 1;
    g_fog_color = 0x00336699u;
    textures_make(&textures, 1u);
    ASSERT(wl_xbox_psh_translate(state, &textures, NULL, &program));
    bool ramp = false;
    for (unsigned i = 0; i < program.tev.stage_count; i++) {
        if (program.tev.stages[i].texmap == WL_XBOX_PSH_FOG_TEXMAP) {
            ASSERT_EQ(WL_XBOX_PSH_FOG_TEXCOORD, program.tev.stages[i].texcoord);
            ramp = true;
        }
    }
    ASSERT(ramp);
    ASSERT(program.texcoords & (1u << WL_XBOX_PSH_FOG_TEXCOORD));
    const char *why = NULL;
    ASSERT_EQ(1, check(state, 1u, &why));
    g_fog_on = 0;
    PASS();
}

TEST a_bumped_lookup_goes_through_the_indirect_unit(void) {
    uint32_t state[WL_XBOX_PSH_WORDS];
    WlXboxPshTextures textures;
    WlXboxPshProgram program;
    // r0 = t1 (a bumped reflection's cube map, looked up at the bump map's offset) * v0
    modulate(state);
    state[WL_XBOX_PSH_RGB_INPUTS0] = 0x09040000u;
    state[WL_XBOX_PSH_ALPHA_INPUTS0] = 0x19140000u;
    g_fog_on = 0;
    textures_make(&textures, 3u);
    textures.bumped = 2u;
    ASSERT(wl_xbox_psh_translate(state, &textures, NULL, &program));
    ASSERT_EQ(WL_XBOX_PSH_VERTICES_BUMPED_REFLECTION, program.vertices);
    unsigned lookups = 0;
    for (unsigned i = 0; i < program.tev.stage_count; i++) {
        const WlTevStage *st = &program.tev.stages[i];
        if (st->texmap == 1) {
            ASSERT(program.indirect[i] == WL_XBOX_PSH_INDIRECT_LOOKUP ||
                   program.indirect[i] == WL_XBOX_PSH_INDIRECT_REPEAT);
        }
        if (program.indirect[i] == WL_XBOX_PSH_INDIRECT_LOOKUP) {
            // (the offset's two terms in the two stages before it, which read no texture)
            ASSERT(i >= 2);
            ASSERT_EQ(WL_XBOX_PSH_INDIRECT_T, program.indirect[i - 1]);
            ASSERT_EQ(WL_XBOX_PSH_INDIRECT_S, program.indirect[i - 2]);
            ASSERT_EQ(WL_TEV_NONE, program.tev.stages[i - 1].texmap);
            ASSERT_EQ(WL_TEV_NONE, program.tev.stages[i - 2].texmap);
            lookups++;
        }
    }
    ASSERT_EQ(1u, lookups);
    ASSERT_EQ(7u, program.texcoords & 7u);
    PASS();
}

// The final combiner's product E*F when four values are alive and every TEV register holds one:
// r0 = t0^2, r1 = r0^2, then v0 = v0 * t0 and v1 = (1 - v0) * v1.a (the colours renamed into
// registers), and the final combiner v1 * (r0 * r1) + v0, its alpha v0's. E*F goes into r0's
// register, which nothing reads after it (TEV reads a stage's inputs before it writes); a glow
// sprite's program of this shape was refused as having "no free register".
TEST a_final_product_with_every_register_held(void) {
    uint32_t state[WL_XBOX_PSH_WORDS];
    WlXboxPshTextures textures;
    WlXboxPshProgram program;
    const char *why = NULL;

    memset(state, 0, sizeof state);
    state[WL_XBOX_PSH_COMBINER_COUNT] = 3;
    state[WL_XBOX_PSH_RGB_INPUTS0 + 0] = 0x08080000u;  // A t0, B t0
    state[WL_XBOX_PSH_RGB_OUTPUTS0 + 0] = 0x000000C0u; // AB to r0
    state[WL_XBOX_PSH_RGB_INPUTS0 + 1] = 0x0C0C0000u;  // A r0, B r0
    state[WL_XBOX_PSH_RGB_OUTPUTS0 + 1] = 0x000000D0u; // AB to r1
    state[WL_XBOX_PSH_RGB_INPUTS0 + 2] = 0x04082415u;  // A v0, B t0, C 1 - v0, D v1.a
    state[WL_XBOX_PSH_RGB_OUTPUTS0 + 2] = 0x00000045u; // AB to v0, CD to v1
    state[WL_XBOX_PSH_FINAL_INPUTS_ABCD] = 0x050F0004u; // A v1, B E*F, C 0, D v0
    state[WL_XBOX_PSH_FINAL_INPUTS_EFG] = 0x0C0D1400u;  // E r0, F r1, G v0.a
    g_fog_on = 0;
    g_d0_alpha_map = 0;
    textures_make(&textures, 1u);
    ASSERT(wl_xbox_psh_translate(state, &textures, NULL, &program));
    ASSERT(program.reason == NULL);
    ASSERT(program.tev.stage_count <= 8u);
    ASSERT_EQ(1, check(state, 1u, &why));
    // ...and the sum v1 + r0 the same way: A v1, B the sum, C 0, D v0
    state[WL_XBOX_PSH_FINAL_INPUTS_ABCD] = 0x050E0004u;
    ASSERT(wl_xbox_psh_translate(state, &textures, NULL, &program));
    ASSERT_EQ(1, check(state, 1u, &why));
    PASS();
}

SUITE(psh_suite) {
    RUN_TEST(random_programs_agree_with_the_combiners);
    RUN_TEST(a_final_product_with_every_register_held);
    RUN_TEST(a_modulation);
    RUN_TEST(a_recipe_comes_first);
    RUN_TEST(fog_reads_the_ramp);
    RUN_TEST(a_bumped_lookup_goes_through_the_indirect_unit);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(psh_suite);
    GREATEST_MAIN_END();
}
