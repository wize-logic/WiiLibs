// SPDX-License-Identifier: MIT
// The processor's part of every NV2A backend (nv2a_pipeline.h).

#include "wl/xbox/nv2a_pipeline.h"

#include <math.h>
#include <string.h>

#define TAG "nv2a"

void wl_nv2a_pipeline_init(WlNv2aPipeline *p, const WlXboxVshNative *natives, unsigned long native_count) {
    memset(p, 0, sizeof *p);
    p->natives = natives;
    p->native_count = native_count;
    p->program_generation = 0xFFFFFFFFu;
}

bool wl_nv2a_pipeline_begin(WlNv2aPipeline *p, const WlNv2aKelvin *k) {
    const uint32_t *words;
    unsigned long count, hash;
    unsigned i, slot = 0;

    p->point_size = (float)wl_nv2a_k(k, WL_NV2A_K_SET_POINT_SIZE) * (1.0f / 8.0f);
    if (!wl_nv2a_program_mode(k)) {
        p->program = NULL;
        p->program_generation = k->program_generation;
        return true;
    }
    if (p->program != NULL && p->program_generation == k->program_generation) {
        return true;
    }
    wl_nv2a_program(k, &words, &count);
    if (count == 0) {
        p->program = NULL;
        return false;
    }
    hash = wl_xbox_vsh_hash(words, count);
    p->use++;
    for (i = 0; i < WL_NV2A_PIPELINE_PROGRAMS; i++) {
        if (p->programs[i].used != 0 && p->programs[i].hash == hash && p->programs[i].count == count) {
            p->programs[i].used = p->use;
            p->program = &p->programs[i].program;
            p->program_generation = k->program_generation;
            p->found++;
            return true;
        }
        if (p->programs[i].used < p->programs[slot].used) {
            slot = i;
        }
    }
    wl_xbox_vsh_compile(words, count, p->natives, p->native_count, &p->programs[slot].program);
    p->programs[slot].hash = hash;
    p->programs[slot].count = count;
    p->programs[slot].used = p->use;
    p->program = &p->programs[slot].program;
    p->program_generation = k->program_generation;
    p->compiled++;
    return true;
}

// v by a 4x4 matrix of 16 method words: out[j] = sum of v[i] m[4 i + j] (Direct3D's row vector by
// its row-major matrix, which is OpenGL's column-major matrix by a column vector)
static void transform(const WlNv2aKelvin *k, uint32_t method, const float v[4], float out[4]) {
    unsigned i, j;

    for (j = 0; j < 4; j++) {
        float sum = 0.0f;

        for (i = 0; i < 4; i++) {
            sum += v[i] * wl_nv2a_kf(k, method + 4u * (4u * i + j));
        }
        out[j] = sum;
    }
}

static void fixed_function(const WlNv2aKelvin *k, const float inputs[WL_NV2A_ATTRIBS][4], WlXboxVshOutput *out) {
    float clip[4];
    unsigned t;

    transform(k, WL_NV2A_K_SET_COMPOSITE_MATRIX, inputs[0], clip);
    memcpy(out->clip, clip, sizeof out->clip);
    out->clip_captured = 0;
    if (clip[3] != 0.0f) {
        out->position[0] = clip[0] / clip[3];
        out->position[1] = clip[1] / clip[3];
        out->position[2] = clip[2] / clip[3];
    } else {
        out->position[0] = clip[0];
        out->position[1] = clip[1];
        out->position[2] = clip[2];
    }
    out->position[3] = 1.0f;
    memcpy(out->d0, inputs[3], sizeof out->d0);
    memcpy(out->d1, inputs[4], sizeof out->d1);
    memcpy(out->b0, inputs[3], sizeof out->b0);
    memcpy(out->b1, inputs[4], sizeof out->b1);
    for (t = 0; t < 4; t++) {
        if (wl_nv2a_k(k, WL_NV2A_K_SET_TEXTURE_MATRIX_ENABLE + 4u * t)) {
            transform(k, WL_NV2A_K_SET_TEXTURE_MATRIX + 64u * t, inputs[9 + t], out->t[t]);
        } else {
            memcpy(out->t[t], inputs[9 + t], sizeof out->t[t]);
        }
    }
    switch (wl_nv2a_k(k, WL_NV2A_K_SET_FOG_GEN_MODE)) {
    case 0: // the specular colour's alpha
        out->fog = inputs[4][3];
        break;
    case 6: // the fog coordinate
        out->fog = inputs[5][0];
        break;
    default: // (radial and planar: the distance, as clip-space w has it)
        out->fog = clip[3];
        break;
    }
    out->point_size = 1.0f;
}

void wl_nv2a_pipeline_vertex(WlNv2aPipeline *p, const WlNv2aKelvin *k, const float inputs[WL_NV2A_ATTRIBS][4],
                             unsigned live, WlXboxVshOutput *out) {
    if (p->program == NULL) {
        fixed_function(k, inputs, out);
        return;
    }
    wl_xbox_vsh_run_live(p->program, (const float (*)[4])k->constants, inputs, p->point_size, out, live);
}

float wl_nv2a_fog_factor(const WlNv2aKelvin *k, float coordinate) {
    float bias = wl_nv2a_kf(k, WL_NV2A_K_SET_FOG_PARAMS), multiplier = wl_nv2a_kf(k, WL_NV2A_K_SET_FOG_PARAMS + 4);
    float factor, c = coordinate;

    switch (wl_nv2a_k(k, WL_NV2A_K_SET_FOG_MODE)) {
    case WL_NV2A_FOG_EXP_ABS:
        c = fabsf(c);
        /* fall through */
    case WL_NV2A_FOG_EXP:
        factor = bias - 1.5f + exp2f(16.0f * multiplier * c);
        break;
    case WL_NV2A_FOG_EXP2_ABS:
        c = fabsf(c);
        /* fall through */
    case WL_NV2A_FOG_EXP2: {
        float mc = multiplier * c;
        factor = bias - 1.5f + exp2f(-32.0f * mc * mc);
        break;
    }
    case WL_NV2A_FOG_LINEAR_ABS:
        c = fabsf(c);
        /* fall through */
    default:
        factor = bias - 1.0f + multiplier * c;
        break;
    }
    if (!(factor > 0.0f)) {
        return 0.0f;
    }
    return factor > 1.0f ? 1.0f : factor;
}

void wl_nv2a_psh_textures(const WlNv2aKelvin *k, const WlNv2aTexEntry *const entries[WL_NV2A_TEXTURES],
                          WlXboxPshTextures *out) {
    unsigned stage;

    memset(out, 0, sizeof *out);
    for (stage = 0; stage < WL_NV2A_TEXTURES; stage++) {
        unsigned mode = (wl_nv2a_k(k, WL_NV2A_K_SET_SHADER_STAGE_PROGRAM) >> (5u * stage)) & 0x1Fu;
        const WlNv2aTexEntry *e = entries[stage];

        if (mode == WL_NV2A_STAGE_NONE) {
            continue;
        }
        if (e == NULL) {
            out->unavailable |= 1u << stage;
            continue;
        }
        switch (mode) {
        case WL_NV2A_STAGE_2D_PROJECTIVE:
        case WL_NV2A_STAGE_3D_PROJECTIVE:
            if (e->sphere) {
                out->unavailable |= 1u << stage;
                break;
            }
            out->present |= 1u << stage;
            out->contents[stage] = e->contents;
            if (e->layout.separate_alpha) {
                out->separate_alpha |= 1u << stage;
            }
            break;
        case WL_NV2A_STAGE_CUBE_MAP:
            if (!e->sphere) {
                out->unavailable |= 1u << stage;
                break;
            }
            out->present |= 1u << stage;
            out->cube |= 1u << stage;
            out->contents[stage] = e->contents;
            break;
        default:
            out->unavailable |= 1u << stage;
            break;
        }
    }
    // (the fog ramp is map 7, which a fourth stage's DXT3/5 alpha would take: no fog then)
    out->fog_color = wl_nv2a_fog_rgb(k);
    if (wl_nv2a_k(k, WL_NV2A_K_SET_FOG_ENABLE) && !(out->separate_alpha & 8u)) {
        out->fog = 1;
    }
}

void wl_nv2a_texcoord(const WlNv2aTexEntry *entry, unsigned stage_program, const float t[4], float st[2]) {
    if (entry != NULL && entry->sphere) {
        wl_nv2a_sphere_coordinates(t, st);
        return;
    }
    st[0] = t[0];
    st[1] = t[1];
    if ((stage_program == WL_NV2A_STAGE_2D_PROJECTIVE || stage_program == WL_NV2A_STAGE_3D_PROJECTIVE) &&
        t[3] != 1.0f && fabsf(t[3]) > 1.0e-12f) {
        st[0] /= t[3];
        st[1] /= t[3];
    }
    if (entry != NULL && entry->linear && entry->width != 0 && entry->height != 0) {
        st[0] *= 1.0f / (float)entry->width;
        st[1] *= 1.0f / (float)entry->height;
    }
}

// ---- the raster state as GX takes it ----------------------------------------------------------------

WlGxCompare wl_nv2a_gx_compare(uint32_t func) {
    return func >= WL_NV2A_FUNC_NEVER && func <= WL_NV2A_FUNC_ALWAYS ? (WlGxCompare)(func - WL_NV2A_FUNC_NEVER)
                                                                      : WL_GX_ALWAYS;
}

// GX's factor for the NV2A's (OpenCE-Wii's gx_blend_factor: GX's colour factors are the other
// side's, a source's factor of the destination colour, a destination's of the source colour)
static WlGxBlendFactor gx_blend_factor(uint32_t factor, bool source, bool *exact) {
    switch (factor) {
    case WL_NV2A_BLEND_ZERO:
        return WL_GX_BL_ZERO;
    case WL_NV2A_BLEND_ONE:
        return WL_GX_BL_ONE;
    case WL_NV2A_BLEND_SRC_COLOR:
        if (source) {
            *exact = false;
            return WL_GX_BL_ONE;
        }
        return WL_GX_BL_OTHERCLR;
    case WL_NV2A_BLEND_ONE_MINUS_SRC_COLOR:
        if (source) {
            *exact = false;
            return WL_GX_BL_ZERO;
        }
        return WL_GX_BL_INVOTHERCLR;
    case WL_NV2A_BLEND_SRC_ALPHA_SATURATE:
        *exact = false;
        return WL_GX_BL_SRCALPHA;
    case WL_NV2A_BLEND_SRC_ALPHA:
        return WL_GX_BL_SRCALPHA;
    case WL_NV2A_BLEND_ONE_MINUS_SRC_ALPHA:
        return WL_GX_BL_INVSRCALPHA;
    case WL_NV2A_BLEND_DST_ALPHA:
        return WL_GX_BL_DSTALPHA;
    case WL_NV2A_BLEND_ONE_MINUS_DST_ALPHA:
        return WL_GX_BL_INVDSTALPHA;
    case WL_NV2A_BLEND_DST_COLOR:
        if (!source) {
            *exact = false;
            return WL_GX_BL_ONE;
        }
        return WL_GX_BL_OTHERCLR;
    case WL_NV2A_BLEND_ONE_MINUS_DST_COLOR:
        if (!source) {
            *exact = false;
            return WL_GX_BL_ZERO;
        }
        return WL_GX_BL_INVOTHERCLR;
    default:
        // (no constant colour on GX's blender)
        *exact = false;
        return WL_GX_BL_ONE;
    }
}

bool wl_nv2a_gx_blend(const WlNv2aKelvin *k, WlGxBlendMode *mode, WlGxBlendFactor *source,
                      WlGxBlendFactor *destination) {
    bool exact = true;
    uint32_t equation;

    if (!wl_nv2a_k(k, WL_NV2A_K_SET_BLEND_ENABLE)) {
        *mode = WL_GX_BM_NONE;
        *source = WL_GX_BL_ONE;
        *destination = WL_GX_BL_ZERO;
        return true;
    }
    equation = wl_nv2a_k(k, WL_NV2A_K_SET_BLEND_EQUATION);
    *source = gx_blend_factor(wl_nv2a_k(k, WL_NV2A_K_SET_BLEND_FUNC_SFACTOR), true, &exact);
    *destination = gx_blend_factor(wl_nv2a_k(k, WL_NV2A_K_SET_BLEND_FUNC_DFACTOR), false, &exact);
    *mode = WL_GX_BM_BLEND;
    if (equation == WL_NV2A_BLEND_EQ_REVERSE_SUBTRACT || equation == WL_NV2A_BLEND_EQ_REVERSE_SUBTRACT_SIGNED) {
        // (GX subtracts the source from the destination, its factors one)
        *mode = WL_GX_BM_SUBTRACT;
        exact = exact && *source == WL_GX_BL_ONE && *destination == WL_GX_BL_ONE;
    } else if (equation != WL_NV2A_BLEND_EQ_ADD) {
        exact = false;
    }
    return exact;
}

unsigned wl_nv2a_cull(const WlNv2aKelvin *k) {
    uint32_t face;
    bool front_positive;

    if (!wl_nv2a_k(k, WL_NV2A_K_SET_CULL_FACE_ENABLE)) {
        return WL_NV2A_CULLS_NONE;
    }
    face = wl_nv2a_k(k, WL_NV2A_K_SET_CULL_FACE);
    // (CW as the screen shows it: a positive area in its y-down coordinates)
    front_positive = wl_nv2a_k(k, WL_NV2A_K_SET_FRONT_FACE) == WL_NV2A_FRONT_CW;
    if (face == WL_NV2A_CULL_FRONT_AND_BACK) {
        return WL_NV2A_CULLS_ALL;
    }
    return (face == WL_NV2A_CULL_FRONT) == front_positive ? WL_NV2A_CULLS_POSITIVE : WL_NV2A_CULLS_NEGATIVE;
}

uint32_t wl_nv2a_clear_rgba(const WlNv2aKelvin *k, const WlNv2aSurface *s) {
    uint32_t value = wl_nv2a_k(k, WL_NV2A_K_SET_COLOR_CLEAR_VALUE);

    if (s->color_bytes == 2) {
        uint32_t r, g, b;

        if (s->color_format == WL_NV2A_SURFACE_COLOR_R5G6B5) {
            r = (value >> 11) & 31u, g = (value >> 5) & 63u, b = value & 31u;
            r = r << 3 | r >> 2, g = g << 2 | g >> 4, b = b << 3 | b >> 2;
        } else {
            r = (value >> 10) & 31u, g = (value >> 5) & 31u, b = value & 31u;
            r = r << 3 | r >> 2, g = g << 3 | g >> 2, b = b << 3 | b >> 2;
        }
        return r << 24 | g << 16 | b << 8 | 0xFFu;
    }
    // ARGB to RGBA
    return value << 8 | value >> 24;
}

float wl_nv2a_clear_depth(const WlNv2aKelvin *k, const WlNv2aSurface *s) {
    uint32_t value = wl_nv2a_k(k, WL_NV2A_K_SET_ZSTENCIL_CLEAR_VALUE);

    if (s->zeta_format == WL_NV2A_SURFACE_ZETA_Z16) {
        return (float)(value & 0xFFFFu) * (1.0f / 65535.0f);
    }
    return (float)(value >> 8) * (1.0f / 16777215.0f);
}
