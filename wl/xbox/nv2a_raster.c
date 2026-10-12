// SPDX-License-Identifier: MIT
// The software reference of the NV2A backend on GX (nv2a_raster.h).
//
// A draw's vertices are run through the pipeline as the GX backend runs them; a triangle is clipped
// at the eye in clip space when its program captured the clip-space position (w below 1/1024),
// brought to the surface's pixels with the viewport's constants as the GX backend's perspective
// path does (pixel centres on integers, so a pixel is sampled at its centre plus a half), culled by
// the NV2A's rule, and filled with edge functions (the top-left rule), its attributes interpolated
// for the perspective and its depth across the screen. Each pixel: the depth test, the TEV program
// psh made of the combiners (wl_tev_eval) over texels sampled from the converted textures decoded
// back from GX's formats, the alpha test, GX's blending as the GX backend maps it, the masks.

#include "wl/xbox/nv2a_raster.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "wl/codec/gx_tex.h"
#include "wl/platform/tev.h"
#include "wl/xbox/nv2a_kelvin.h"
#include "wl/xbox/nv2a_pipeline.h"
#include "wl/xbox/nv2a_texcache.h"
#include "wl/xbox/nv2a_texture.h"
#include "wl/xbox/psh_cache.h"

enum { SURFACES = 8, DECODED = 16, CLIP_MAX = 12, COORDS = 8 };

#define NEAR_W (1.0f / 1024.0f)

typedef struct Surface {
    uint32_t phys, width, height, used;
    uint8_t *rgba;
    bool drawn;
} Surface;

typedef struct Depth {
    uint32_t phys, width, height, used;
    float *z;
} Depth;

typedef struct Decoded {
    const void *image;
    uint32_t serial, width, height, used;
    uint8_t *rgba;
    uint8_t *alpha; // a separate alpha (DXT3/5), as intensity
} Decoded;

typedef struct Sampler {
    const uint8_t *rgba;
    uint32_t width, height;
    uint8_t wrap_s, wrap_t; // WlGxWrap
    bool bilinear;
} Sampler;

// a vertex in clip space with its attributes
typedef struct RVertex {
    float clip[4];   // (x, y, z, w); for a draw in screen space (x, y, z) in pixels and depth, w 1
    float color[2][4];
    float tex[COORDS][2];
} RVertex;

// a vertex on the surface
typedef struct SVertex {
    float x, y, z, inv_w;
    float color[2][4];
    float tex[COORDS][2];
} SVertex;

struct WlNv2aRaster {
    WlNv2aRasterConfig cfg;
    WlNv2a *dev;
    WlNv2aPipeline pipeline;
    WlNv2aTexCache textures;
    WlXboxPshCache psh;
    WlNv2aRasterStats stats;
    Surface surfaces[SURFACES];
    Depth depths[SURFACES];
    Decoded decoded[DECODED];
    uint32_t use;
    RVertex *vertices;
    uint32_t vertex_capacity;
    // the draw's
    const WlNv2aTexEntry *entries[WL_NV2A_TEXTURES];
    WlNv2aTexEntry surface_entries[WL_NV2A_TEXTURES];
    Sampler samplers[8];
    bool fog_ramp;
    WlTevProgram tev;
    unsigned texcoords, channels;
    bool perspective;
    float sx, sy, depth_max;
    const float *scale, *offset;
    Surface *target;
    Depth *depth;
    uint32_t clip_x0, clip_y0, clip_x1, clip_y1;
    WlGxBlendMode blend;
    WlGxBlendFactor source, destination;
    bool color_update, alpha_update, depth_test, depth_write, alpha_test;
    WlGxCompare depth_func, alpha_func;
    uint8_t alpha_ref;
    unsigned cull;
    uint32_t pixels;
};

// ---- creation -----------------------------------------------------------------------------------

WlNv2aRaster *wl_nv2a_raster_create(const WlNv2aRasterConfig *cfg) {
    WlNv2aRaster *r = calloc(1, sizeof *r);

    if (r == NULL) {
        return NULL;
    }
    if (cfg != NULL) {
        r->cfg = *cfg;
    }
    {
        WlNv2aTexCacheConfig textures;

        memset(&textures, 0, sizeof textures);
        textures.budget = 64u * 1024u * 1024u;
        wl_nv2a_texcache_init(&r->textures, &textures);
    }
    wl_nv2a_pipeline_init(&r->pipeline, NULL, 0);
    wl_xbox_psh_cache_init(&r->psh, NULL);
    return r;
}

void wl_nv2a_raster_destroy(WlNv2aRaster *r) {
    unsigned i;

    if (r == NULL) {
        return;
    }
    wl_nv2a_texcache_clear(&r->textures);
    for (i = 0; i < SURFACES; i++) {
        free(r->surfaces[i].rgba);
        free(r->depths[i].z);
    }
    for (i = 0; i < DECODED; i++) {
        free(r->decoded[i].rgba);
        free(r->decoded[i].alpha);
    }
    free(r->vertices);
    free(r);
}

void wl_nv2a_raster_attach(WlNv2aRaster *r, WlNv2a *dev) {
    r->dev = dev;
}

void wl_nv2a_raster_stats(const WlNv2aRaster *r, WlNv2aRasterStats *out) {
    *out = r->stats;
}

const uint8_t *wl_nv2a_raster_surface(const WlNv2aRaster *r, uint32_t phys, uint32_t *width, uint32_t *height) {
    unsigned i;

    for (i = 0; i < SURFACES; i++) {
        if (r->surfaces[i].rgba != NULL && r->surfaces[i].phys == phys) {
            *width = r->surfaces[i].width;
            *height = r->surfaces[i].height;
            return r->surfaces[i].rgba;
        }
    }
    return NULL;
}

// ---- surfaces -------------------------------------------------------------------------------------

static void surface_size(const WlNv2aSurface *s, uint32_t *w, uint32_t *h) {
    if (s->swizzled) {
        *w = 1u << s->log2_width;
        *h = 1u << s->log2_height;
    } else {
        *w = (s->clip_x + s->clip_width) / s->samples_x;
        *h = (s->clip_y + s->clip_height) / s->samples_y;
    }
    if (*w == 0 || *w > 4096u) {
        *w = 640;
    }
    if (*h == 0 || *h > 4096u) {
        *h = 480;
    }
}

static Surface *color_surface(WlNv2aRaster *r, uint32_t phys, uint32_t w, uint32_t h) {
    Surface *slot = NULL;
    unsigned i;

    r->use++;
    for (i = 0; i < SURFACES; i++) {
        Surface *s = &r->surfaces[i];

        if (s->rgba != NULL && s->phys == phys) {
            if (s->width != w || s->height != h) {
                uint8_t *grown = calloc((size_t)w * h, 4);

                if (grown == NULL) {
                    return NULL;
                }
                free(s->rgba);
                s->rgba = grown;
                s->width = w;
                s->height = h;
            }
            s->used = r->use;
            return s;
        }
        if (slot == NULL || s->rgba == NULL || (slot->rgba != NULL && s->used < slot->used)) {
            slot = s;
        }
    }
    free(slot->rgba);
    memset(slot, 0, sizeof *slot);
    slot->rgba = calloc((size_t)w * h, 4);
    if (slot->rgba == NULL) {
        return NULL;
    }
    slot->phys = phys;
    slot->width = w;
    slot->height = h;
    slot->used = r->use;
    return slot;
}

static Depth *depth_surface(WlNv2aRaster *r, uint32_t phys, uint32_t w, uint32_t h) {
    Depth *slot = NULL;
    unsigned i;
    size_t n;

    r->use++;
    for (i = 0; i < SURFACES; i++) {
        Depth *d = &r->depths[i];

        if (d->z != NULL && d->phys == phys && d->width >= w && d->height >= h) {
            d->used = r->use;
            return d;
        }
        if (slot == NULL || d->z == NULL || (slot->z != NULL && d->used < slot->used)) {
            slot = d;
        }
    }
    free(slot->z);
    memset(slot, 0, sizeof *slot);
    slot->z = malloc((size_t)w * h * sizeof *slot->z);
    if (slot->z == NULL) {
        return NULL;
    }
    for (n = 0; n < (size_t)w * h; n++) {
        slot->z[n] = 1.0f;
    }
    slot->phys = phys;
    slot->width = w;
    slot->height = h;
    slot->used = r->use;
    return slot;
}

// ---- textures --------------------------------------------------------------------------------------

static const Decoded *decode(WlNv2aRaster *r, const WlNv2aTexEntry *e) {
    Decoded *slot = NULL;
    const WlNv2aGxLayout *l = &e->layout;
    size_t texels = (size_t)l->width * l->height;
    unsigned i;

    r->use++;
    for (i = 0; i < DECODED; i++) {
        Decoded *d = &r->decoded[i];

        if (d->rgba != NULL && d->image == e->image && d->serial == e->serial) {
            d->used = r->use;
            return d;
        }
        if (slot == NULL || d->rgba == NULL || (slot->rgba != NULL && d->used < slot->used)) {
            slot = d;
        }
    }
    free(slot->rgba);
    free(slot->alpha);
    memset(slot, 0, sizeof *slot);
    slot->rgba = malloc(texels * 4u);
    if (slot->rgba == NULL) {
        return NULL;
    }
    if (l->format == WL_GXC_C8) {
        uint8_t *indices = malloc(texels), *palette = malloc(256u * 4u);
        size_t t;

        if (indices == NULL || palette == NULL ||
            wl_gxc_decode_ci(WL_GXC_C8, e->image, l->image_bytes, l->width, l->height, indices) == 0) {
            free(indices);
            free(palette);
            return NULL;
        }
        memset(palette, 0, 256u * 4u);
        wl_gxc_tlut_decode(l->tlut_format, (const uint8_t *)e->image + l->tlut_offset, l->tlut_entries, palette);
        for (t = 0; t < texels; t++) {
            memcpy(slot->rgba + 4u * t, palette + 4u * indices[t], 4);
        }
        free(indices);
        free(palette);
    } else if (wl_gxc_decode(l->format, e->image, l->image_bytes, l->width, l->height, slot->rgba) == 0) {
        return NULL;
    }
    if (l->separate_alpha) {
        slot->alpha = malloc(texels * 4u);
        if (slot->alpha != NULL) {
            wl_gxc_decode(WL_GXC_I4, (const uint8_t *)e->image + ((l->image_bytes + 31u) & ~(size_t)31u), l->alpha_bytes,
                          l->width, l->height, slot->alpha);
        }
    }
    slot->image = e->image;
    slot->serial = e->serial;
    slot->width = l->width;
    slot->height = l->height;
    slot->used = r->use;
    return slot;
}

static uint8_t gx_wrap_of(uint8_t wrap, bool pow2) {
    if (!pow2) {
        return WL_GX_WRAP_CLAMP;
    }
    return wrap == WL_NV2A_WRAP_REPEAT ? (uint8_t)WL_GX_WRAP_REPEAT
           : wrap == WL_NV2A_WRAP_MIRROR ? (uint8_t)WL_GX_WRAP_MIRROR
                                         : (uint8_t)WL_GX_WRAP_CLAMP;
}

// a raster surface at a texture's address (a render target), as an entry
static const WlNv2aTexEntry *surface_texture(WlNv2aRaster *r, unsigned stage, const WlNv2aTexture *t) {
    unsigned i;

    for (i = 0; i < SURFACES; i++) {
        Surface *s = &r->surfaces[i];

        if (s->rgba != NULL && s->phys == t->phys && s != r->target) {
            WlNv2aTexEntry *e = &r->surface_entries[stage];

            memset(e, 0, sizeof *e);
            e->contents = WL_XBOX_PSH_TEXTURE_ALL;
            e->linear = t->linear;
            e->width = (uint16_t)t->width;
            e->height = (uint16_t)t->height;
            e->layout.width = s->width;
            e->layout.height = s->height;
            e->layout.levels = 1;
            e->image = s->rgba; // (raw RGBA: sampled directly)
            e->serial = 0xFFFFFFFFu;
            return e;
        }
    }
    return NULL;
}

static void textures_bind(WlNv2aRaster *r, const WlNv2aKelvin *k) {
    unsigned stage;

    memset(r->samplers, 0, sizeof r->samplers);
    for (stage = 0; stage < WL_NV2A_TEXTURES; stage++) {
        WlNv2aTexture t;
        const WlNv2aTexEntry *e;
        bool pow2;

        r->entries[stage] = NULL;
        if (wl_nv2a_stage_program(k, stage) == WL_NV2A_STAGE_NONE || !wl_nv2a_texture(k, stage, &t)) {
            continue;
        }
        e = surface_texture(r, stage, &t);
        if (e != NULL) {
            r->samplers[stage].rgba = e->image;
        } else {
            const Decoded *d;

            e = wl_nv2a_texcache_get(&r->textures, r->dev, &t, false, NULL);
            if (e == NULL || (d = decode(r, e)) == NULL) {
                continue;
            }
            r->samplers[stage].rgba = d->rgba;
            if (d->alpha != NULL && stage + 4u < 8u) {
                r->samplers[stage + 4u] = r->samplers[stage];
                r->samplers[stage + 4u].rgba = d->alpha;
            }
        }
        pow2 = (e->layout.width & (e->layout.width - 1u)) == 0 && (e->layout.height & (e->layout.height - 1u)) == 0;
        r->samplers[stage].width = e->layout.width;
        r->samplers[stage].height = e->layout.height;
        r->samplers[stage].wrap_s = e->sphere ? (uint8_t)WL_GX_WRAP_CLAMP : gx_wrap_of(t.wrap_u, pow2);
        r->samplers[stage].wrap_t = e->sphere ? (uint8_t)WL_GX_WRAP_CLAMP : gx_wrap_of(t.wrap_v, pow2);
        r->samplers[stage].bilinear = r->cfg.bilinear && t.mag_filter != WL_NV2A_FILTER_NEAREST;
        if (r->samplers[stage + 4u].rgba != NULL && stage + 4u < 8u) {
            r->samplers[stage + 4u].width = e->layout.width;
            r->samplers[stage + 4u].height = e->layout.height;
            r->samplers[stage + 4u].wrap_s = r->samplers[stage].wrap_s;
            r->samplers[stage + 4u].wrap_t = r->samplers[stage].wrap_t;
            r->samplers[stage + 4u].bilinear = r->samplers[stage].bilinear;
        }
        r->entries[stage] = e;
    }
}

static int wrap_coordinate(int i, uint32_t size, uint8_t wrap) {
    int n = (int)size;

    switch (wrap) {
    case WL_GX_WRAP_REPEAT:
        i %= n;
        return i < 0 ? i + n : i;
    case WL_GX_WRAP_MIRROR: {
        int period = 2 * n, m = i % period;

        m = m < 0 ? m + period : m;
        return m < n ? m : period - 1 - m;
    }
    default:
        return i < 0 ? 0 : i >= n ? n - 1 : i;
    }
}

static void texel(const Sampler *s, int x, int y, float out[4]) {
    const uint8_t *p;

    x = wrap_coordinate(x, s->width, s->wrap_s);
    y = wrap_coordinate(y, s->height, s->wrap_t);
    p = s->rgba + ((size_t)y * s->width + (size_t)x) * 4u;
    out[0] = p[0], out[1] = p[1], out[2] = p[2], out[3] = p[3];
}

static void sample(const Sampler *s, float u, float v, uint8_t out[4]) {
    float fx = u * (float)s->width, fy = v * (float)s->height, c[4];
    unsigned i;

    if (!(fx > -1.0e6f && fx < 1.0e6f)) {
        fx = 0.0f;
    }
    if (!(fy > -1.0e6f && fy < 1.0e6f)) {
        fy = 0.0f;
    }
    if (s->bilinear) {
        float x = fx - 0.5f, y = fy - 0.5f, ax, ay, t00[4], t10[4], t01[4], t11[4];
        int x0 = (int)floorf(x), y0 = (int)floorf(y);

        ax = x - (float)x0;
        ay = y - (float)y0;
        texel(s, x0, y0, t00);
        texel(s, x0 + 1, y0, t10);
        texel(s, x0, y0 + 1, t01);
        texel(s, x0 + 1, y0 + 1, t11);
        for (i = 0; i < 4; i++) {
            c[i] = (t00[i] * (1.0f - ax) + t10[i] * ax) * (1.0f - ay) + (t01[i] * (1.0f - ax) + t11[i] * ax) * ay;
        }
    } else {
        texel(s, (int)floorf(fx), (int)floorf(fy), c);
    }
    for (i = 0; i < 4; i++) {
        out[i] = (uint8_t)(c[i] <= 0.0f ? 0 : c[i] >= 255.0f ? 255 : (int)(c[i] + 0.5f));
    }
}

// ---- the pixel stage ---------------------------------------------------------------------------------

static void tev_setup(WlNv2aRaster *r, const WlNv2aKelvin *k) {
    WlXboxPshTextures textures;
    uint32_t words[WL_XBOX_PSH_WORDS];
    const WlXboxPshProgram *program;
    bool usable, translated;

    wl_nv2a_psh_textures(k, r->entries, &textures);
    r->fog_ramp = textures.fog != 0;
    wl_nv2a_psh_words(k, words);
    program = wl_xbox_psh_cache_program(&r->psh, words, wl_nv2a_k(k, WL_NV2A_K_SET_SHADER_STAGE_PROGRAM), &textures,
                                        &usable, &translated);
    (void)translated;
    if (usable) {
        r->tev = program->tev;
        r->texcoords = program->texcoords;
        r->channels = program->channels;
        return;
    }
    r->stats.programs_unusable++;
    wl_tev_recipe(&r->tev, r->entries[0] != NULL ? WL_GX_RECIPE_TEX_MODULATE : WL_GX_RECIPE_VERTEX_COLOR);
    // (the recipes' konst slot 0 is GX's white)
    r->tev.konst[0] = 0xFFFFFFFFu;
    r->texcoords = r->entries[0] != NULL ? 1u : 0u;
    r->channels = 1u;
}

// ---- pixels ------------------------------------------------------------------------------------------

static bool compare(WlGxCompare func, float a, float b) {
    switch (func) {
    case WL_GX_NEVER:
        return false;
    case WL_GX_LESS:
        return a < b;
    case WL_GX_EQUAL:
        return a == b;
    case WL_GX_LEQUAL:
        return a <= b;
    case WL_GX_GREATER:
        return a > b;
    case WL_GX_NEQUAL:
        return a != b;
    case WL_GX_GEQUAL:
        return a >= b;
    default:
        return true;
    }
}

static float factor(WlGxBlendFactor f, bool source, const float src[4], const float dst[4], unsigned channel) {
    switch (f) {
    case WL_GX_BL_ZERO:
        return 0.0f;
    case WL_GX_BL_ONE:
        return 1.0f;
    case WL_GX_BL_OTHERCLR:
        return channel == 3 ? (source ? dst[3] : src[3]) : (source ? dst[channel] : src[channel]);
    case WL_GX_BL_INVOTHERCLR:
        return 1.0f - (channel == 3 ? (source ? dst[3] : src[3]) : (source ? dst[channel] : src[channel]));
    case WL_GX_BL_SRCALPHA:
        return src[3];
    case WL_GX_BL_INVSRCALPHA:
        return 1.0f - src[3];
    case WL_GX_BL_DSTALPHA:
        return dst[3];
    default:
        return 1.0f - dst[3];
    }
}

static void shade(WlNv2aRaster *r, uint32_t x, uint32_t y, float z, const float color[2][4],
                  const float tex[COORDS][2]) {
    size_t at = (size_t)y * r->target->width + x;
    uint8_t *dst8 = r->target->rgba + at * 4u;
    WlTevPixel px;
    uint32_t rgba;
    unsigned s, c;
    float src[4], dst[4], out[4];

    if (r->depth != NULL && r->depth_test) {
        float *stored = &r->depth->z[(size_t)y * r->depth->width + x];

        if (!compare(r->depth_func, z, *stored)) {
            return;
        }
    }
    memset(&px, 0, sizeof px);
    for (c = 0; c < 2; c++) {
        for (s = 0; s < 4; s++) {
            float v = color[c][s];

            px.ras[c][s] = (uint8_t)(v <= 0.0f ? 0 : v >= 1.0f ? 255 : (int)(v * 255.0f + 0.5f));
        }
    }
    for (s = 0; s < r->tev.stage_count; s++) {
        uint8_t map = r->tev.stages[s].texmap, coord = r->tev.stages[s].texcoord;

        if (map >= 8 || coord >= COORDS) {
            continue;
        }
        if (map == WL_XBOX_PSH_FOG_TEXMAP && r->fog_ramp) {
            int v = (int)floorf(tex[coord][0] * 256.0f);
            uint8_t value = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);

            px.tex[map][0] = px.tex[map][1] = px.tex[map][2] = px.tex[map][3] = value;
        } else if (r->samplers[map].rgba != NULL) {
            sample(&r->samplers[map], tex[coord][0], tex[coord][1], px.tex[map]);
        }
    }
    rgba = wl_tev_eval_rgba(&r->tev, &px);
    src[0] = (float)(rgba >> 24) / 255.0f;
    src[1] = (float)((rgba >> 16) & 0xFFu) / 255.0f;
    src[2] = (float)((rgba >> 8) & 0xFFu) / 255.0f;
    src[3] = (float)(rgba & 0xFFu) / 255.0f;
    if (r->alpha_test && !compare(r->alpha_func, (float)(rgba & 0xFFu), (float)r->alpha_ref)) {
        return;
    }
    for (c = 0; c < 4; c++) {
        dst[c] = (float)dst8[c] / 255.0f;
    }
    for (c = 0; c < 4; c++) {
        switch (r->blend) {
        case WL_GX_BM_BLEND:
            out[c] = src[c] * factor(r->source, true, src, dst, c) + dst[c] * factor(r->destination, false, src, dst, c);
            break;
        case WL_GX_BM_SUBTRACT:
            out[c] = dst[c] - src[c];
            break;
        default:
            out[c] = src[c];
            break;
        }
        out[c] = out[c] < 0.0f ? 0.0f : out[c] > 1.0f ? 1.0f : out[c];
    }
    if (r->color_update) {
        for (c = 0; c < 3; c++) {
            dst8[c] = (uint8_t)(out[c] * 255.0f + 0.5f);
        }
    }
    if (r->alpha_update) {
        dst8[3] = (uint8_t)(out[3] * 255.0f + 0.5f);
    }
    if (r->depth != NULL && r->depth_write) {
        r->depth->z[(size_t)y * r->depth->width + x] = z;
    }
    r->pixels++;
    r->stats.pixels++;
}

// ---- triangles ---------------------------------------------------------------------------------------

static void to_surface(const WlNv2aRaster *r, const RVertex *v, SVertex *out) {
    unsigned c, t;

    if (r->perspective) {
        float w = v->clip[3], inv = 1.0f / w;

        out->x = (v->clip[0] * r->scale[0] * inv + r->offset[0]) / r->sx + 0.5f;
        out->y = (v->clip[1] * r->scale[1] * inv + r->offset[1]) / r->sy + 0.5f;
        out->z = (v->clip[2] * r->scale[2] * inv + r->offset[2]) / r->depth_max;
        out->inv_w = inv;
    } else {
        out->x = v->clip[0];
        out->y = v->clip[1];
        out->z = v->clip[2];
        out->inv_w = 1.0f;
    }
    out->z = out->z < 0.0f ? 0.0f : out->z > 1.0f ? 1.0f : out->z;
    for (c = 0; c < 2; c++) {
        for (t = 0; t < 4; t++) {
            out->color[c][t] = v->color[c][t] * out->inv_w;
        }
    }
    for (t = 0; t < COORDS; t++) {
        out->tex[t][0] = v->tex[t][0] * out->inv_w;
        out->tex[t][1] = v->tex[t][1] * out->inv_w;
    }
}

static float edge(const SVertex *a, const SVertex *b, float x, float y) {
    return (b->x - a->x) * (y - a->y) - (b->y - a->y) * (x - a->x);
}

static bool top_left(const SVertex *a, const SVertex *b) {
    // (for a triangle of positive area by edge(), in y-down coordinates: the interior is on an
    // edge's positive side, so a top edge runs right and a left edge runs up)
    return (a->y == b->y && b->x > a->x) || b->y < a->y;
}

static void fill(WlNv2aRaster *r, const SVertex *v0, const SVertex *v1, const SVertex *v2) {
    float area = edge(v0, v1, v2->x, v2->y);
    float minx, maxx, miny, maxy;
    int x0, x1, y0, y1, x, y;
    bool tl0, tl1, tl2;
    unsigned cull = r->cull;

    if (area == 0.0f || !(area == area)) {
        return;
    }
    if (cull == WL_NV2A_CULLS_ALL || (cull == WL_NV2A_CULLS_POSITIVE && area > 0.0f) ||
        (cull == WL_NV2A_CULLS_NEGATIVE && area < 0.0f)) {
        r->stats.culled++;
        return;
    }
    if (area < 0.0f) {
        const SVertex *t = v1;

        v1 = v2;
        v2 = t;
        area = -area;
    }
    r->stats.triangles++;
    minx = fminf(v0->x, fminf(v1->x, v2->x));
    maxx = fmaxf(v0->x, fmaxf(v1->x, v2->x));
    miny = fminf(v0->y, fminf(v1->y, v2->y));
    maxy = fmaxf(v0->y, fmaxf(v1->y, v2->y));
    x0 = (int)floorf(minx);
    x1 = (int)ceilf(maxx);
    y0 = (int)floorf(miny);
    y1 = (int)ceilf(maxy);
    x0 = x0 < (int)r->clip_x0 ? (int)r->clip_x0 : x0;
    y0 = y0 < (int)r->clip_y0 ? (int)r->clip_y0 : y0;
    x1 = x1 > (int)r->clip_x1 ? (int)r->clip_x1 : x1;
    y1 = y1 > (int)r->clip_y1 ? (int)r->clip_y1 : y1;
    tl0 = top_left(v1, v2);
    tl1 = top_left(v2, v0);
    tl2 = top_left(v0, v1);
    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = edge(v1, v2, px, py), w1 = edge(v2, v0, px, py), w2 = edge(v0, v1, px, py);
            float b0, b1, b2, inv, z, color[2][4], tex[COORDS][2];
            unsigned c, t;

            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f || (w0 == 0.0f && !tl0) || (w1 == 0.0f && !tl1) ||
                (w2 == 0.0f && !tl2)) {
                continue;
            }
            b0 = w0 / area;
            b1 = w1 / area;
            b2 = w2 / area;
            z = b0 * v0->z + b1 * v1->z + b2 * v2->z;
            inv = b0 * v0->inv_w + b1 * v1->inv_w + b2 * v2->inv_w;
            inv = inv != 0.0f ? 1.0f / inv : 1.0f;
            for (c = 0; c < 2; c++) {
                for (t = 0; t < 4; t++) {
                    color[c][t] = (b0 * v0->color[c][t] + b1 * v1->color[c][t] + b2 * v2->color[c][t]) * inv;
                }
            }
            for (t = 0; t < COORDS; t++) {
                tex[t][0] = (b0 * v0->tex[t][0] + b1 * v1->tex[t][0] + b2 * v2->tex[t][0]) * inv;
                tex[t][1] = (b0 * v0->tex[t][1] + b1 * v1->tex[t][1] + b2 * v2->tex[t][1]) * inv;
            }
            shade(r, (uint32_t)x, (uint32_t)y, z, (const float (*)[4])color, (const float (*)[2])tex);
        }
    }
}

static void lerp_vertex(const RVertex *a, const RVertex *b, float t, RVertex *out) {
    unsigned i, c;

    for (i = 0; i < 4; i++) {
        out->clip[i] = a->clip[i] + (b->clip[i] - a->clip[i]) * t;
    }
    for (c = 0; c < 2; c++) {
        for (i = 0; i < 4; i++) {
            out->color[c][i] = a->color[c][i] + (b->color[c][i] - a->color[c][i]) * t;
        }
    }
    for (i = 0; i < COORDS; i++) {
        out->tex[i][0] = a->tex[i][0] + (b->tex[i][0] - a->tex[i][0]) * t;
        out->tex[i][1] = a->tex[i][1] + (b->tex[i][1] - a->tex[i][1]) * t;
    }
}

// a triangle: clipped at the eye (w >= NEAR_W) when it is in clip space, then filled as a fan
static void triangle(WlNv2aRaster *r, const RVertex *a, const RVertex *b, const RVertex *c) {
    RVertex in[CLIP_MAX], out[CLIP_MAX];
    SVertex s[CLIP_MAX];
    unsigned n = 3, m = 0, i;

    in[0] = *a;
    in[1] = *b;
    in[2] = *c;
    if (r->perspective && (a->clip[3] < NEAR_W || b->clip[3] < NEAR_W || c->clip[3] < NEAR_W)) {
        for (i = 0; i < n; i++) {
            const RVertex *p = &in[i], *q = &in[(i + 1) % n];
            bool p_in = p->clip[3] >= NEAR_W, q_in = q->clip[3] >= NEAR_W;

            if (p_in) {
                out[m++] = *p;
            }
            if (p_in != q_in && m < CLIP_MAX) {
                lerp_vertex(p, q, (NEAR_W - p->clip[3]) / (q->clip[3] - p->clip[3]), &out[m++]);
            }
        }
        n = m;
        memcpy(in, out, n * sizeof *in);
    }
    if (n < 3) {
        return;
    }
    for (i = 0; i < n; i++) {
        to_surface(r, &in[i], &s[i]);
    }
    for (i = 1; i + 1 < n; i++) {
        fill(r, &s[0], &s[i], &s[i + 1]);
    }
}

// ---- the backend ----------------------------------------------------------------------------------

static bool surface_prepare(WlNv2aRaster *r, const WlNv2aKelvin *k, WlNv2aSurface *s) {
    uint32_t w, h;

    wl_nv2a_surface(k, s);
    surface_size(s, &w, &h);
    r->target = color_surface(r, s->color_phys, w, h);
    if (r->target == NULL) {
        return false;
    }
    r->target->drawn = true;
    r->depth = s->zeta_format != 0 ? depth_surface(r, s->zeta_phys, w, h) : NULL;
    r->clip_x0 = s->clip_x / s->samples_x;
    r->clip_y0 = s->clip_y / s->samples_y;
    r->clip_x1 = (s->clip_x + s->clip_width) / s->samples_x;
    r->clip_y1 = (s->clip_y + s->clip_height) / s->samples_y;
    r->clip_x1 = r->clip_x1 > w || r->clip_x1 == 0 ? w : r->clip_x1;
    r->clip_y1 = r->clip_y1 > h || r->clip_y1 == 0 ? h : r->clip_y1;
    return true;
}

static uint32_t raster_draw(void *user, const WlNv2aKelvin *k, const WlNv2aDraw *draw) {
    WlNv2aRaster *r = user;
    WlNv2aSurface s;
    WlNv2aPrimInfo info;
    float inputs[WL_NV2A_ATTRIBS][4];
    unsigned live = WL_XBOX_VSH_LIVE_ALL, stage_programs[WL_NV2A_TEXTURES], st;
    uint32_t v, mask;

    if (wl_nv2a_k(k, WL_NV2A_K_SET_STENCIL_TEST_ENABLE)) {
        r->stats.stencil_skipped++;
        return 0;
    }
    if (r->dev == NULL || !wl_nv2a_pipeline_begin(&r->pipeline, k) || !surface_prepare(r, k, &s)) {
        return 0;
    }
    r->stats.draws++;
    r->pixels = 0;
    // the raster state, as the GX backend maps it
    (void)wl_nv2a_gx_blend(k, &r->blend, &r->source, &r->destination);
    mask = wl_nv2a_k(k, WL_NV2A_K_SET_COLOR_MASK);
    r->color_update = (mask & 0x00010101u) != 0;
    r->alpha_update = (mask & 0x01000000u) != 0;
    r->depth_test = r->depth != NULL && wl_nv2a_k(k, WL_NV2A_K_SET_DEPTH_TEST_ENABLE) != 0;
    r->depth_write = r->depth_test && wl_nv2a_k(k, WL_NV2A_K_SET_DEPTH_MASK) != 0;
    r->depth_func = wl_nv2a_gx_compare(wl_nv2a_k(k, WL_NV2A_K_SET_DEPTH_FUNC));
    r->alpha_test = wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_TEST_ENABLE) != 0;
    r->alpha_func = wl_nv2a_gx_compare(wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_FUNC));
    r->alpha_ref = (uint8_t)wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_REF);
    r->cull = wl_nv2a_cull(k);
    textures_bind(r, k);
    tev_setup(r, k);
    r->sx = (float)s.samples_x;
    r->sy = (float)s.samples_y;
    r->depth_max = s.depth_max > 0.0f ? s.depth_max : 16777215.0f;
    r->scale = k->constants[WL_NV2A_CONSTANT_VIEWPORT_SCALE];
    r->offset = k->constants[WL_NV2A_CONSTANT_VIEWPORT_OFFSET];
    for (st = 0; st < WL_NV2A_TEXTURES; st++) {
        stage_programs[st] = wl_nv2a_stage_program(k, st);
    }
    if (draw->count > r->vertex_capacity) {
        RVertex *grown = realloc(r->vertices, draw->count * sizeof *grown);

        if (grown == NULL) {
            return 0;
        }
        r->vertices = grown;
        r->vertex_capacity = draw->count;
    }
    // the vertices: in clip space when the program captures it (the first vertex says), else on
    // the surface
    r->perspective = false;
    for (v = 0; v < draw->count; v++) {
        RVertex *rv = &r->vertices[v];
        WlXboxVshOutput o;
        unsigned c;

        memset(&o, 0, sizeof o);
        wl_nv2a_draw_fetch(draw, k, v, inputs);
        wl_nv2a_pipeline_vertex(&r->pipeline, k, (const float (*)[4])inputs, live, &o);
        if (v == 0) {
            r->perspective = o.clip_captured && r->pipeline.program != NULL;
        }
        if (r->perspective) {
            memcpy(rv->clip, o.clip, sizeof rv->clip);
        } else {
            rv->clip[0] = o.position[0] / r->sx + 0.5f;
            rv->clip[1] = o.position[1] / r->sy + 0.5f;
            rv->clip[2] = o.position[2] / r->depth_max;
            rv->clip[3] = 1.0f;
        }
        for (c = 0; c < 4; c++) {
            rv->color[0][c] = o.d0[c] < 0.0f ? 0.0f : o.d0[c] > 1.0f ? 1.0f : o.d0[c];
            rv->color[1][c] = o.d1[c] < 0.0f ? 0.0f : o.d1[c] > 1.0f ? 1.0f : o.d1[c];
        }
        memset(rv->tex, 0, sizeof rv->tex);
        for (st = 0; st < WL_NV2A_TEXTURES; st++) {
            wl_nv2a_texcoord(r->entries[st], stage_programs[st], o.t[st], rv->tex[st]);
        }
        rv->tex[WL_XBOX_PSH_FOG_TEXCOORD][0] = wl_gx_ramp_coordinate(wl_nv2a_fog_factor(k, o.fog));
    }
    wl_nv2a_prim_info(draw->primitive, &info);
    if (info.lines || info.points) {
        return 0;
    }
    if (draw->primitive == WL_NV2A_PRIM_TRIANGLES) {
        for (v = 0; v + 2 < draw->count; v += 3) {
            triangle(r, &r->vertices[v], &r->vertices[v + 1], &r->vertices[v + 2]);
        }
    } else if (draw->primitive == WL_NV2A_PRIM_QUADS) {
        for (v = 0; v + 3 < draw->count; v += 4) {
            triangle(r, &r->vertices[v], &r->vertices[v + 1], &r->vertices[v + 2]);
            triangle(r, &r->vertices[v], &r->vertices[v + 2], &r->vertices[v + 3]);
        }
    } else if (info.strip) {
        for (v = 0; v + 2 < draw->count; v++) {
            // (a strip's odd triangles turn the other way)
            if (v & 1u) {
                triangle(r, &r->vertices[v + 1], &r->vertices[v], &r->vertices[v + 2]);
            } else {
                triangle(r, &r->vertices[v], &r->vertices[v + 1], &r->vertices[v + 2]);
            }
        }
    } else if (info.fan) {
        for (v = 1; v + 1 < draw->count; v++) {
            triangle(r, &r->vertices[0], &r->vertices[v], &r->vertices[v + 1]);
        }
    }
    return r->pixels;
}

static void raster_clear(void *user, const WlNv2aKelvin *k, uint32_t flags) {
    WlNv2aRaster *r = user;
    WlNv2aSurface s;
    uint32_t h = wl_nv2a_k(k, WL_NV2A_K_SET_CLEAR_RECT_HORIZONTAL), v = wl_nv2a_k(k, WL_NV2A_K_SET_CLEAR_RECT_VERTICAL);
    uint32_t x0, x1, y0, y1, x, y, rgba;
    float z;

    if (!surface_prepare(r, k, &s)) {
        return;
    }
    r->stats.clears++;
    x0 = (h & 0xFFFFu) / s.samples_x;
    x1 = ((h >> 16) + 1u) / s.samples_x;
    y0 = (v & 0xFFFFu) / s.samples_y;
    y1 = ((v >> 16) + 1u) / s.samples_y;
    x1 = x1 > r->target->width ? r->target->width : x1;
    y1 = y1 > r->target->height ? r->target->height : y1;
    rgba = wl_nv2a_clear_rgba(k, &s);
    z = wl_nv2a_clear_depth(k, &s);
    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            if (flags & WL_NV2A_CLEAR_COLOR) {
                uint8_t *p = r->target->rgba + ((size_t)y * r->target->width + x) * 4u;

                p[0] = (uint8_t)(rgba >> 24);
                p[1] = (uint8_t)(rgba >> 16);
                p[2] = (uint8_t)(rgba >> 8);
                p[3] = (uint8_t)rgba;
            }
            if ((flags & WL_NV2A_CLEAR_Z) && r->depth != NULL && x < r->depth->width && y < r->depth->height) {
                r->depth->z[(size_t)y * r->depth->width + x] = z;
            }
        }
    }
}

static void show(WlNv2aRaster *r, Surface *s) {
    r->stats.frames++;
    s->drawn = false;
    if (r->cfg.frame != NULL) {
        r->cfg.frame(r->cfg.user, s->phys, s->rgba, s->width, s->height);
    }
    wl_nv2a_texcache_frame_done(&r->textures);
}

static void raster_frame_done(void *user, const WlNv2aKelvin *k) {
    WlNv2aRaster *r = user;
    uint32_t phys = k->dma_color.base + wl_nv2a_k(k, WL_NV2A_K_SET_SURFACE_COLOR_OFFSET);
    unsigned i;

    for (i = 0; i < SURFACES; i++) {
        if (r->surfaces[i].rgba != NULL && r->surfaces[i].phys == phys && r->surfaces[i].drawn) {
            show(r, &r->surfaces[i]);
        }
    }
}

static void raster_flip(void *user, const WlNv2aKelvin *k, uint32_t phys) {
    WlNv2aRaster *r = user;
    unsigned i;

    (void)k;
    for (i = 0; i < SURFACES; i++) {
        if (r->surfaces[i].rgba != NULL && r->surfaces[i].phys == phys && r->surfaces[i].drawn) {
            show(r, &r->surfaces[i]);
        }
    }
}

void wl_nv2a_raster_backend(WlNv2aRaster *r, WlNv2aBackend *out) {
    memset(out, 0, sizeof *out);
    out->user = r;
    out->clear = raster_clear;
    out->draw = raster_draw;
    out->frame_done = raster_frame_done;
    out->flip = raster_flip;
}
