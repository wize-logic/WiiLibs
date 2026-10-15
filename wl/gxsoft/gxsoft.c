// SPDX-License-Identifier: MIT
// The software GX (gxsoft.h): wl/platform's host GX renderer.
#include "wl/gxsoft/gxsoft.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "wl/base/hash.h"
#include "wl/codec/gx_tex.h"
#include "wl/platform/gx_direct.h"
#include "wl/platform/host/gx_direct_host.h"
#include "wl/platform/host/gx_renderer_host.h"
#include "wl/platform/host/tev_host.h"
#include "wl/platform/platform.h"
#include "wl/platform/tev.h"

#define EFB_W WL_GXSOFT_EFB_WIDTH
#define EFB_H WL_GXSOFT_EFB_HEIGHT
#define Z_FAR 0x00FFFFFFu

enum {
    // a vertex's numbers: the clip-space position, two colours (0..255), eight coordinates, their q
    // (1 but for a projected one's, which s and t are divided by at each pixel)
    A_CLIP = 0,
    A_COLOR = 4,
    A_TEX = 12,
    A_TQ = 28,
    A_COUNT = 36,
    CLIP_MAX = 16,
    DECODED_MAX = 96,
    LEVELS_MAX = 11,
    LOOKUPS_MAX = 16
};

// the pixel centre GX samples at, in pixels from a pixel's top left (Dolphin's 7/12)
#define SAMPLE_OFFSET (7.0 / 12.0)
// how far beyond the viewport x and y are clipped (in its half-sizes): far enough never to be seen
#define GUARD 8.0f

typedef struct Level {
    uint32_t w, h;
    uint8_t *rgba;
} Level;

// a texture image decoded from GX's format into RGBA8 levels
typedef struct Decoded {
    const void *image;
    uint8_t format, mips;
    uint16_t width, height;
    const void *tlut;
    uint8_t tlut_format;
    uint16_t tlut_count;
    size_t source_bytes;
    uint64_t hash;
    bool checked; // since the texture cache was last emptied
    uint32_t used;
    uint32_t levels;
    Level level[LEVELS_MAX];
    uint8_t *memory;
} Decoded;

// what one TEV stage's lookup samples
typedef struct Lookup {
    uint8_t texcoord, map;
    const Decoded *d;
    uint8_t wrap_s, wrap_t, min_filter, mag_filter;
    float lod_bias;
    bool lod; // the level of detail matters (mipmaps, or a minifying filter unlike the magnifying one)
} Lookup;

// a vertex on the screen: its position, depth (0..1), 1 / w and attributes over w
typedef struct SVertex {
    double x, y;
    float z, iw;
    float a[A_COUNT];
} SVertex;

typedef struct Plan {
    const WlHostGxDirect *st;
    const WlTevProgram *tev;
    float p[6];
    bool orthographic;
    float vx, vy, vw, vh;
    int sx0, sy0, sx1, sy1; // the scissor ∩ the EFB, exclusive ends
    unsigned channels;      // colour channels sent (0..2)
    unsigned texgens;       // texture coordinates generated
    unsigned need_colors;   // channels the stages read (bit 0, bit 1)
    unsigned need_tex;      // coordinates the lookups read
    unsigned lookup_count;
    Lookup lookups[LOOKUPS_MAX];
    int8_t stage_lookup[WL_TEV_MAX_STAGES];
    WlGxSoftDraw info;
} Plan;

static struct {
    bool attached;
    WlGxSoftConfig cfg;
    bool efb_alpha; // RGBA6_Z24 (else RGB8_Z24)
    uint8_t *color; // EFB_W * EFB_H * 4, as the pixels read back
    uint32_t *depth;
    uint32_t clear_rgba;
    WlGxSoftStats stats;
    uint32_t draw_index;
    uint32_t use;
    Decoded decoded[DECODED_MAX];
    WlTevProgram fallback;
} g;

// ---- the EFB --------------------------------------------------------------------------------------

// a channel as the EFB keeps it (RGBA6: six bits, widened as reads and copies widen them)
static uint8_t efb_channel(uint32_t v) {
    if (g.efb_alpha) {
        uint32_t six = (v & 0xFFu) >> 2;
        return (uint8_t)(six << 2 | six >> 4);
    }
    return (uint8_t)v;
}

static void efb_store(uint8_t *px, const uint8_t rgba[4], bool colour, bool alpha) {
    if (colour) {
        px[0] = efb_channel(rgba[0]);
        px[1] = efb_channel(rgba[1]);
        px[2] = efb_channel(rgba[2]);
    }
    if (alpha) {
        px[3] = g.efb_alpha ? efb_channel(rgba[3]) : 255u;
    }
}

static void efb_fill(int x0, int y0, int x1, int y1, bool colour, uint32_t rgba, bool depth, uint32_t z) {
    uint8_t c[4] = {(uint8_t)(rgba >> 24), (uint8_t)(rgba >> 16), (uint8_t)(rgba >> 8), (uint8_t)rgba};
    int x, y;

    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > (int)EFB_W ? (int)EFB_W : x1;
    y1 = y1 > (int)EFB_H ? (int)EFB_H : y1;
    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            size_t i = (size_t)y * EFB_W + (size_t)x;

            if (colour) {
                efb_store(&g.color[i * 4u], c, true, true);
            }
            if (depth) {
                g.depth[i] = z;
            }
        }
    }
}

static uint32_t depth24(float depth) {
    double d = (double)depth * 16777215.0;

    if (!(d > 0.0)) {
        return 0;
    }
    return d >= 16777215.0 ? Z_FAR : (uint32_t)(d + 0.5);
}

// ---- textures -------------------------------------------------------------------------------------

static void decoded_free(Decoded *d) {
    free(d->memory);
    memset(d, 0, sizeof *d);
}

static uint64_t source_hash(const void *image, size_t bytes, const void *tlut, uint16_t tlut_count) {
    uint64_t h = wl_fnv1a64(image, bytes);

    if (tlut != NULL) {
        h = wl_fnv1a64_update(h, tlut, (size_t)tlut_count * 2u);
    }
    return h;
}

static bool decode(Decoded *d) {
    size_t total = 0, offset = 0;
    uint32_t l, w = d->width, h = d->height;
    WlGxcFormat format = (WlGxcFormat)d->format;
    uint8_t palette[256 * 4];

    d->levels = d->mips == 0 ? 1u : d->mips;
    if (d->levels > LEVELS_MAX) {
        d->levels = LEVELS_MAX;
    }
    for (l = 0; l < d->levels; l++) {
        total += (size_t)w * h * 4u;
        w = w > 1u ? w >> 1 : 1u;
        h = h > 1u ? h >> 1 : 1u;
    }
    d->memory = malloc(total);
    if (d->memory == NULL) {
        return false;
    }
    if (format == WL_GXC_C4 || format == WL_GXC_C8) {
        uint32_t count = d->tlut != NULL ? d->tlut_count : 0u;

        memset(palette, 0, sizeof palette);
        if (count > 256u) {
            count = 256u;
        }
        if (count != 0) {
            wl_gxc_tlut_decode((WlGxcTlutFormat)d->tlut_format, d->tlut, count, palette);
        }
    }
    w = d->width;
    h = d->height;
    total = 0;
    for (l = 0; l < d->levels; l++) {
        size_t bytes = wl_gxc_tex_bytes(format, w, h);
        Level *level = &d->level[l];

        level->w = w;
        level->h = h;
        level->rgba = d->memory + total;
        if (format == WL_GXC_C4 || format == WL_GXC_C8) {
            size_t n = (size_t)w * h, i;
            uint8_t *indices = malloc(n);

            if (indices == NULL || wl_gxc_decode_ci(format, (const uint8_t *)d->image + offset, bytes, w, h, indices) == 0) {
                memset(level->rgba, 0, n * 4u);
            } else {
                for (i = 0; i < n; i++) {
                    memcpy(level->rgba + i * 4u, palette + (size_t)indices[i] * 4u, 4);
                }
            }
            free(indices);
        } else if (wl_gxc_decode(format, (const uint8_t *)d->image + offset, bytes, w, h, level->rgba) == 0) {
            memset(level->rgba, 0, (size_t)w * h * 4u);
        }
        total += (size_t)w * h * 4u;
        offset += bytes;
        w = w > 1u ? w >> 1 : 1u;
        h = h > 1u ? h >> 1 : 1u;
    }
    d->source_bytes = offset;
    g.stats.textures_decoded++;
    return true;
}

// a texture's bytes in GX's format, its levels one after the other (as decode reads them)
static size_t source_size(uint8_t format, uint16_t width, uint16_t height, uint8_t mips) {
    uint32_t l, levels = mips == 0 ? 1u : mips, w = width, h = height;
    size_t bytes = 0;

    levels = levels > LEVELS_MAX ? LEVELS_MAX : levels;
    for (l = 0; l < levels; l++) {
        bytes += wl_gxc_tex_bytes((WlGxcFormat)format, w, h);
        w = w > 1u ? w >> 1 : 1u;
        h = h > 1u ? h >> 1 : 1u;
    }
    return bytes;
}

// The decoded image of a texture object (or the ramp). Kept by what it is (its bytes' hash, format,
// size, levels and palette), not where it is: the host memory a texture lands in changes with the
// heap from build to build, and what is decoded must not. Its address only saves the hash: an image
// found at the same address since the texture cache was last emptied is taken as unchanged.
static const Decoded *decoded_get(const void *image, uint8_t format, uint16_t width, uint16_t height, uint8_t mips,
                                  const WlGxTlut *tlut) {
    Decoded *slot = NULL;
    unsigned i;
    const void *tlut_entries = tlut != NULL ? tlut->entries : NULL;
    uint16_t tlut_count = tlut != NULL ? tlut->count : 0u;
    uint8_t tlut_format = tlut != NULL ? tlut->format : 0u;
    uint64_t h;

    if (image == NULL || format >= WL_GXC_FORMAT_COUNT || width == 0 || height == 0) {
        return NULL;
    }
    mips = mips == 0 ? 1u : mips;
    g.use++;
    // (the same address, checked since the cache was emptied)
    for (i = 0; i < DECODED_MAX; i++) {
        Decoded *d = &g.decoded[i];

        if (d->memory != NULL && d->checked && d->image == image && d->format == format && d->width == width &&
            d->height == height && d->mips == mips && d->tlut == tlut_entries && d->tlut_count == tlut_count &&
            d->tlut_format == tlut_format) {
            d->used = g.use;
            return d;
        }
    }
    // (the same bytes, wherever they were)
    h = source_hash(image, source_size(format, width, height, mips), tlut_entries, tlut_count);
    for (i = 0; i < DECODED_MAX; i++) {
        Decoded *d = &g.decoded[i];

        if (d->memory != NULL && d->hash == h && d->format == format && d->width == width && d->height == height &&
            d->mips == mips && d->tlut_count == tlut_count && d->tlut_format == tlut_format) {
            d->image = image;
            d->tlut = tlut_entries;
            d->checked = true;
            d->used = g.use;
            return d;
        }
        if (slot == NULL || d->memory == NULL || (slot->memory != NULL && d->used < slot->used)) {
            slot = d;
        }
    }
    decoded_free(slot);
    slot->image = image;
    slot->format = format;
    slot->width = width;
    slot->height = height;
    slot->mips = mips;
    slot->tlut = tlut_entries;
    slot->tlut_count = tlut_count;
    slot->tlut_format = tlut_format;
    if (!decode(slot)) {
        decoded_free(slot);
        return NULL;
    }
    slot->hash = h;
    slot->checked = true;
    slot->used = g.use;
    return slot;
}

static void textures_unchecked(void) {
    unsigned i;

    for (i = 0; i < DECODED_MAX; i++) {
        g.decoded[i].checked = false;
    }
}

static int wrap_coord(int i, uint32_t size, uint8_t wrap) {
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

// one level's texel at the coordinates (s, t) (0..1 the image), nearest or bilinear, as GX samples
// (bilinear with 7-bit fractions, half a texel back)
static void level_sample(const Level *lv, const Lookup *k, float s, float t, bool linear, uint32_t out[4]) {
    double u = (double)s * lv->w, v = (double)t * lv->h;
    int c;

    if (!linear) {
        int x = wrap_coord((int)floor(u), lv->w, k->wrap_s), y = wrap_coord((int)floor(v), lv->h, k->wrap_t);
        const uint8_t *p = lv->rgba + ((size_t)y * lv->w + (size_t)x) * 4u;

        for (c = 0; c < 4; c++) {
            out[c] = (uint32_t)p[c] << 14;
        }
    } else {
        double fu = floor(u * 128.0 - 64.0), fv = floor(v * 128.0 - 64.0);
        int iu = (int)fu, iv = (int)fv;
        int x0 = iu >> 7, y0 = iv >> 7;
        uint32_t fs = (uint32_t)(iu & 0x7F), ft = (uint32_t)(iv & 0x7F);
        int xa = wrap_coord(x0, lv->w, k->wrap_s), xb = wrap_coord(x0 + 1, lv->w, k->wrap_s);
        int ya = wrap_coord(y0, lv->h, k->wrap_t), yb = wrap_coord(y0 + 1, lv->h, k->wrap_t);
        const uint8_t *p00 = lv->rgba + ((size_t)ya * lv->w + (size_t)xa) * 4u;
        const uint8_t *p10 = lv->rgba + ((size_t)ya * lv->w + (size_t)xb) * 4u;
        const uint8_t *p01 = lv->rgba + ((size_t)yb * lv->w + (size_t)xa) * 4u;
        const uint8_t *p11 = lv->rgba + ((size_t)yb * lv->w + (size_t)xb) * 4u;
        uint32_t w00 = (128u - fs) * (128u - ft), w10 = fs * (128u - ft), w01 = (128u - fs) * ft, w11 = fs * ft;

        for (c = 0; c < 4; c++) {
            out[c] = p00[c] * w00 + p10[c] * w10 + p01[c] * w01 + p11[c] * w11;
        }
    }
}

static bool filter_linear(uint8_t filter) {
    return filter == WL_GX_FILTER_LINEAR || filter == WL_GX_FILTER_LINEAR_MIP_NEAREST ||
           filter == WL_GX_FILTER_LINEAR_MIP_LINEAR;
}

// a lookup's texel at (s, t) with the level of detail `lod` (log2 of the texels a pixel spans)
static void lookup_sample(const Lookup *k, float s, float t, float lod, uint8_t out[4]) {
    const Decoded *d = k->d;
    uint32_t acc[4], other[4];
    int c;

    if (d == NULL) {
        memset(out, 0, 4);
        return;
    }
    if (!k->lod || !(lod > 0.0f)) {
        level_sample(&d->level[0], k, s, t, filter_linear(k->lod ? k->mag_filter : k->min_filter), acc);
    } else {
        uint8_t f = k->min_filter;
        float max_lod = (float)(d->levels - 1u);
        // (GX keeps the level of detail with four fractional bits)
        float q = floorf((lod > max_lod ? max_lod : lod) * 16.0f) / 16.0f;

        if (f == WL_GX_FILTER_NEAREST || f == WL_GX_FILTER_LINEAR || d->levels <= 1u) {
            level_sample(&d->level[0], k, s, t, filter_linear(f), acc);
        } else if (f == WL_GX_FILTER_NEAREST_MIP_NEAREST || f == WL_GX_FILTER_LINEAR_MIP_NEAREST) {
            uint32_t l = (uint32_t)floorf(q + 0.5f);

            level_sample(&d->level[l < d->levels ? l : d->levels - 1u], k, s, t, filter_linear(f), acc);
        } else {
            uint32_t l = (uint32_t)floorf(q), frac = (uint32_t)((q - floorf(q)) * 16.0f);

            level_sample(&d->level[l], k, s, t, filter_linear(f), acc);
            if (frac != 0 && l + 1u < d->levels) {
                level_sample(&d->level[l + 1u], k, s, t, filter_linear(f), other);
                for (c = 0; c < 4; c++) {
                    acc[c] = (acc[c] * (16u - frac) + other[c] * frac) >> 4;
                }
            }
        }
    }
    for (c = 0; c < 4; c++) {
        out[c] = (uint8_t)(acc[c] >> 14);
    }
}

// ---- the draw's plan ------------------------------------------------------------------------------

static bool power_of_two(uint32_t v) {
    return v != 0 && (v & (v - 1u)) == 0;
}

static void lookup_prepare(Plan *plan, Lookup *k) {
    const WlHostGxDirect *st = plan->st;
    uint8_t map = k->map;

    k->d = NULL;
    k->lod = false;
    if (map >= 8 || st->map_kind[map] == WL_HOST_GX_MAP_NONE) {
        plan->info.issues |= WL_GXSOFT_ISSUE_NO_TEXTURE;
        return;
    }
    if (st->map_kind[map] == WL_HOST_GX_MAP_RAMP) {
        k->d = decoded_get(wl_gx_ramp_image(), WL_GXC_I8, WL_GX_RAMP_WIDTH, WL_GX_RAMP_HEIGHT, 1, NULL);
        k->wrap_s = k->wrap_t = WL_GX_WRAP_CLAMP;
        k->min_filter = k->mag_filter = WL_GX_FILTER_NEAREST;
        k->lod_bias = 0.0f;
        return;
    }
    {
        const WlGxTexObj *tex = &st->map_objects[map];
        const WlGxSampler *sm = &tex->sampler;
        const WlGxTlut *tlut = NULL;
        bool pow2 = power_of_two(tex->width) && power_of_two(tex->height);

        if (tex->format == WL_GX_TEX_C4 || tex->format == WL_GX_TEX_C8) {
            if (!st->tlut_loaded[map]) {
                plan->info.issues |= WL_GXSOFT_ISSUE_NO_TLUT;
                return;
            }
            tlut = &st->tluts[map];
        }
        k->d = decoded_get(wl_host_gx_tex_image(tex), tex->format, tex->width, tex->height, tex->mips, tlut);
        // (as the Wii backend loads the object: clamped unless a power of two, no mip filter
        // without mipmaps)
        k->wrap_s = pow2 ? sm->wrap_s : (uint8_t)WL_GX_WRAP_CLAMP;
        k->wrap_t = pow2 ? sm->wrap_t : (uint8_t)WL_GX_WRAP_CLAMP;
        k->min_filter = sm->min_filter;
        if (tex->mips <= 1u && k->min_filter >= WL_GX_FILTER_NEAREST_MIP_NEAREST) {
            k->min_filter = (k->min_filter & 1u) != 0 ? (uint8_t)WL_GX_FILTER_LINEAR : (uint8_t)WL_GX_FILTER_NEAREST;
        }
        k->mag_filter = sm->mag_filter;
        k->lod_bias = tex->mips > 1u ? sm->lod_bias : 0.0f;
        k->lod = k->d != NULL && (k->d->levels > 1u || filter_linear(k->min_filter) != filter_linear(k->mag_filter));
    }
}

static void plan_make(Plan *plan, WlGxDirectPrim prim, uint32_t count) {
    const WlHostGxDirect *st = wl_host_gx_direct();
    const WlTevProgram *tev = wl_host_tev_last();
    unsigned n, si;

    memset(plan, 0, sizeof *plan);
    plan->st = st;
    plan->info.index = g.draw_index++;
    plan->info.prim = (uint8_t)prim;
    plan->info.vertices = count;
    plan->info.x0 = plan->info.y0 = 1e30f;
    plan->info.x1 = plan->info.y1 = -1e30f;
    if (tev == NULL) {
        tev = &g.fallback;
    }
    plan->tev = tev;
    if (!wl_tev_validate(tev, NULL)) {
        plan->info.issues |= WL_GXSOFT_ISSUE_TEV_INVALID;
    }
    // (the six numbers GX keeps of the matrix: GX_LoadProjectionMtx)
    plan->orthographic = st->orthographic;
    plan->p[0] = st->projection[0][0];
    plan->p[2] = st->projection[1][1];
    plan->p[4] = st->projection[2][2];
    plan->p[5] = st->projection[2][3];
    if (st->orthographic) {
        plan->p[1] = st->projection[0][3];
        plan->p[3] = st->projection[1][3];
    } else {
        plan->p[1] = st->projection[0][2];
        plan->p[3] = st->projection[1][2];
    }
    plan->vx = st->viewport[0];
    plan->vy = st->viewport[1];
    plan->vw = st->viewport[2];
    plan->vh = st->viewport[3];
    plan->sx0 = (int)st->scissor[0];
    plan->sy0 = (int)st->scissor[1];
    plan->sx1 = (int)(st->scissor[0] + st->scissor[2]);
    plan->sy1 = (int)(st->scissor[1] + st->scissor[3]);
    plan->sx1 = plan->sx1 > (int)EFB_W ? (int)EFB_W : plan->sx1;
    plan->sy1 = plan->sy1 > (int)EFB_H ? (int)EFB_H : plan->sy1;
    plan->channels = st->layout.colors > 2u ? 2u : st->layout.colors;
    for (n = 0; n < 8; n++) {
        if ((st->layout.texcoords | st->layout.texcoords_generated | st->layout.texcoords_projected) & (1u << n)) {
            plan->texgens = n + 1u;
        }
    }
    for (si = 0; si < tev->stage_count && si < WL_TEV_MAX_STAGES; si++) {
        const WlTevStage *s = &tev->stages[si];
        unsigned k;

        plan->stage_lookup[si] = -1;
        if (s->channel == WL_TEV_CH_COLOR0A0 || s->channel == WL_TEV_CH_COLOR1A1) {
            unsigned ch = s->channel - WL_TEV_CH_COLOR0A0;

            plan->need_colors |= 1u << ch;
            if (ch >= plan->channels) {
                plan->info.issues |= WL_GXSOFT_ISSUE_NO_CHANNEL;
            }
        }
        if (s->texmap == WL_TEV_NONE) {
            continue;
        }
        if (s->texcoord >= 8u || s->texcoord >= plan->texgens) {
            plan->info.issues |= WL_GXSOFT_ISSUE_NO_TEXCOORD;
        } else {
            plan->need_tex |= 1u << s->texcoord;
        }
        for (k = 0; k < plan->lookup_count; k++) {
            if (plan->lookups[k].texcoord == s->texcoord && plan->lookups[k].map == s->texmap) {
                break;
            }
        }
        if (k == plan->lookup_count) {
            plan->lookups[k].texcoord = s->texcoord;
            plan->lookups[k].map = s->texmap;
            lookup_prepare(plan, &plan->lookups[k]);
            plan->lookup_count++;
        }
        plan->stage_lookup[si] = (int8_t)k;
    }
}

// ---- vertices -------------------------------------------------------------------------------------

static void vertex_transform(const Plan *plan, const WlGxDirectVertex *v, float a[A_COUNT]) {
    const float *p = plan->p;
    float x = v->position[0], y = v->position[1], z = v->position[2];
    unsigned c, n;

    // (the transform unit's position matrix, when set)
    if (plan->st->position_matrix_set) {
        const float (*m)[4] = plan->st->position_matrix;

        x = m[0][0] * v->position[0] + m[0][1] * v->position[1] + m[0][2] * v->position[2] + m[0][3];
        y = m[1][0] * v->position[0] + m[1][1] * v->position[1] + m[1][2] * v->position[2] + m[1][3];
        z = m[2][0] * v->position[0] + m[2][1] * v->position[1] + m[2][2] * v->position[2] + m[2][3];
    }

    if (plan->orthographic) {
        a[A_CLIP + 0] = p[0] * x + p[1];
        a[A_CLIP + 1] = p[2] * y + p[3];
        a[A_CLIP + 3] = 1.0f;
    } else {
        a[A_CLIP + 0] = p[0] * x + p[1] * z;
        a[A_CLIP + 1] = p[2] * y + p[3] * z;
        a[A_CLIP + 3] = -z;
    }
    a[A_CLIP + 2] = p[4] * z + p[5];
    for (c = 0; c < 2; c++) {
        for (n = 0; n < 4; n++) {
            a[A_COLOR + c * 4u + n] = c < plan->channels ? (float)v->color[c][n] : 0.0f;
        }
    }
    for (n = 0; n < 8; n++) {
        float *t = &a[A_TEX + n * 2u];

        a[A_TQ + n] = 1.0f;
        if (n >= plan->texgens) {
            t[0] = t[1] = 0.0f;
        } else if (plan->st->texgen[n].set) {
            // (a generator's matrix: from the vertex's coordinate n, (s, t, 1, 1), or its position as
            // sent, (x, y, z, 1))
            const float (*m)[4] = plan->st->texgen[n].m;
            float in[4];

            if (plan->st->texgen[n].source == WL_GX_DIRECT_TG_POSITION) {
                in[0] = v->position[0], in[1] = v->position[1], in[2] = v->position[2], in[3] = 1.0f;
            } else {
                in[0] = v->texcoord[n][0], in[1] = v->texcoord[n][1], in[2] = 1.0f, in[3] = 1.0f;
            }
            t[0] = m[0][0] * in[0] + m[0][1] * in[1] + m[0][2] * in[2] + m[0][3] * in[3];
            t[1] = m[1][0] * in[0] + m[1][1] * in[1] + m[1][2] * in[2] + m[1][3] * in[3];
        } else if (plan->st->layout.texcoords_projected & (1u << n)) {
            // (a projected coordinate, through its 3x4 identity: s and t divided by q at each pixel)
            t[0] = v->texcoord[n][0];
            t[1] = v->texcoord[n][1];
            a[A_TQ + n] = v->texcoord_q[n];
        } else if (plan->st->layout.texcoords & (1u << n)) {
            t[0] = v->texcoord[n][0];
            t[1] = (plan->st->layout.texcoord_s & (1u << n)) ? 0.0f : v->texcoord[n][1];
        } else {
            // (GX_TG_POS through the identity)
            t[0] = x;
            t[1] = y;
        }
    }
}

// the signed distance of a clip-space vertex inside plane `i` (inside: >= 0)
static float plane_distance(const float *a, unsigned i) {
    float x = a[A_CLIP + 0], y = a[A_CLIP + 1], z = a[A_CLIP + 2], w = a[A_CLIP + 3];

    switch (i) {
    case 0: return z + w;            // near: z' >= -w
    case 1: return -z;               // far: z' <= 0
    case 2: return w - 1.0e-6f;      // in front of the eye
    case 3: return GUARD * w - x;
    case 4: return GUARD * w + x;
    case 5: return GUARD * w - y;
    default: return GUARD * w + y;
    }
}

// Sutherland-Hodgman on the planes; the polygon's vertex count left (0: nothing)
static unsigned clip_polygon(float poly[CLIP_MAX][A_COUNT], unsigned n) {
    float out[CLIP_MAX][A_COUNT];
    unsigned plane, i, k;

    for (plane = 0; plane < 7 && n >= 3; plane++) {
        unsigned m = 0;
        bool all_in = true;

        for (i = 0; i < n; i++) {
            if (plane_distance(poly[i], plane) < 0.0f) {
                all_in = false;
                break;
            }
        }
        if (all_in) {
            continue;
        }
        for (i = 0; i < n; i++) {
            const float *a = poly[i], *b = poly[(i + 1u) % n];
            float da = plane_distance(a, plane), db = plane_distance(b, plane);

            if (da >= 0.0f && m < CLIP_MAX) {
                memcpy(out[m++], a, sizeof out[0]);
            }
            if ((da >= 0.0f) != (db >= 0.0f) && m < CLIP_MAX) {
                float t = da / (da - db);

                for (k = 0; k < A_COUNT; k++) {
                    out[m][k] = a[k] + (b[k] - a[k]) * t;
                }
                m++;
            }
        }
        memcpy(poly, out, sizeof out[0] * m);
        n = m;
    }
    return n >= 3 ? n : 0;
}

static void to_screen(const Plan *plan, const float *a, SVertex *s) {
    float w = a[A_CLIP + 3], iw = 1.0f / w;
    unsigned k;

    s->x = (double)plan->vx + (double)plan->vw * 0.5 * (1.0 + (double)(a[A_CLIP + 0] * iw));
    s->y = (double)plan->vy + (double)plan->vh * 0.5 * (1.0 - (double)(a[A_CLIP + 1] * iw));
    s->z = 1.0f + a[A_CLIP + 2] * iw;
    s->iw = iw;
    for (k = A_COLOR; k < A_COUNT; k++) {
        s->a[k] = a[k] * iw;
    }
}

// ---- pixels ---------------------------------------------------------------------------------------

static bool compare(WlGxCompare func, uint32_t a, uint32_t b) {
    switch (func) {
    case WL_GX_NEVER: return false;
    case WL_GX_LESS: return a < b;
    case WL_GX_EQUAL: return a == b;
    case WL_GX_LEQUAL: return a <= b;
    case WL_GX_GREATER: return a > b;
    case WL_GX_NEQUAL: return a != b;
    case WL_GX_GEQUAL: return a >= b;
    default: return true;
    }
}

static uint32_t blend_factor(WlGxBlendFactor f, const uint8_t src[4], const uint8_t dst[4], bool source, unsigned c) {
    switch (f) {
    case WL_GX_BL_ZERO: return 0;
    case WL_GX_BL_ONE: return 255;
    case WL_GX_BL_OTHERCLR: return source ? dst[c] : src[c];
    case WL_GX_BL_INVOTHERCLR: return 255u - (source ? dst[c] : src[c]);
    case WL_GX_BL_SRCALPHA: return src[3];
    case WL_GX_BL_INVSRCALPHA: return 255u - src[3];
    case WL_GX_BL_DSTALPHA: return dst[3];
    default: return 255u - dst[3];
    }
}

static void blend(const WlHostGxDirect *st, const uint8_t src[4], const uint8_t dst[4], uint8_t out[4]) {
    unsigned c;

    switch (st->blend_mode) {
    case WL_GX_BM_BLEND:
        for (c = 0; c < 4; c++) {
            uint32_t fs = blend_factor(st->blend_source, src, dst, true, c);
            uint32_t fd = blend_factor(st->blend_destination, src, dst, false, c);
            uint32_t v;

            fs += fs >> 7;
            fd += fd >> 7;
            v = (src[c] * fs + dst[c] * fd + 128u) >> 8;
            out[c] = (uint8_t)(v > 255u ? 255u : v);
        }
        break;
    case WL_GX_BM_SUBTRACT:
        for (c = 0; c < 4; c++) {
            out[c] = (uint8_t)(dst[c] > src[c] ? dst[c] - src[c] : 0);
        }
        break;
    default:
        memcpy(out, src, 4);
        break;
    }
}

typedef struct Triangle {
    SVertex v[3];
    double area;
    // the barycentric weights' steps across a pixel
    double dl_dx[3], dl_dy[3];
} Triangle;

static float attribute(const Triangle *t, const double l[3], unsigned k, float inv_den) {
    return (float)(l[0] * t->v[0].a[k] + l[1] * t->v[1].a[k] + l[2] * t->v[2].a[k]) * inv_den;
}

static float inverse_w(const Triangle *t, const double l[3]) {
    double d = l[0] * t->v[0].iw + l[1] * t->v[1].iw + l[2] * t->v[2].iw;

    return d != 0.0 ? (float)(1.0 / d) : 0.0f;
}

static uint8_t color_byte(float v) {
    return (uint8_t)(v <= 0.0f ? 0 : v >= 255.0f ? 255 : (int)(v + 0.5f));
}

static void pixel(Plan *plan, const Triangle *t, int px, int py, const double l[3]) {
    const WlHostGxDirect *st = plan->st;
    size_t i = (size_t)py * EFB_W + (size_t)px;
    uint32_t z = depth24((float)(l[0] * t->v[0].z + l[1] * t->v[1].z + l[2] * t->v[2].z));
    bool z_test = st->z_test;
    uint8_t stage_tex[WL_TEV_MAX_STAGES][4], texels[LOOKUPS_MAX][4], ras[2][4], src[4], out[4];
    WlTevRegValue regs[WL_TEV_REG_COUNT];
    float inv = inverse_w(t, l);
    unsigned k, c, si;

    plan->info.pixels++;
    if (z_test && st->z_early) {
        if (!compare(st->z_func, z, g.depth[i])) {
            plan->info.z_failed++;
            return;
        }
        if (st->z_write) {
            g.depth[i] = z;
        }
    }
    for (c = 0; c < 2; c++) {
        for (k = 0; k < 4; k++) {
            ras[c][k] = (plan->need_colors & (1u << c)) ? color_byte(attribute(t, l, A_COLOR + c * 4u + k, inv)) : 0u;
        }
    }
    for (k = 0; k < plan->lookup_count; k++) {
        const Lookup *lk = &plan->lookups[k];
        float s = 0.0f, tt = 0.0f, lod = 0.0f;

        if (lk->texcoord < 8u && (plan->need_tex & (1u << lk->texcoord))) {
            unsigned a = A_TEX + lk->texcoord * 2u;

            s = attribute(t, l, a, inv);
            tt = attribute(t, l, a + 1u, inv);
            if (plan->st->layout.texcoords_projected & (1u << lk->texcoord)) {
                // (a projected coordinate: divided by its q here, at the pixel)
                float q = attribute(t, l, A_TQ + lk->texcoord, inv);

                s = q != 0.0f ? s / q : 0.0f;
                tt = q != 0.0f ? tt / q : 0.0f;
            }
            if (lk->lod && lk->d != NULL) {
                double lx[3], ly[3];
                float sx, tx, sy, ty, rho_x, rho_y, rho;
                unsigned j;

                for (j = 0; j < 3; j++) {
                    lx[j] = l[j] + t->dl_dx[j];
                    ly[j] = l[j] + t->dl_dy[j];
                }
                {
                    float ix = inverse_w(t, lx), iy = inverse_w(t, ly);

                    float qx = 1.0f, qy = 1.0f;

                    if (plan->st->layout.texcoords_projected & (1u << lk->texcoord)) {
                        qx = attribute(t, lx, A_TQ + lk->texcoord, ix);
                        qy = attribute(t, ly, A_TQ + lk->texcoord, iy);
                        qx = qx != 0.0f ? qx : 1.0f;
                        qy = qy != 0.0f ? qy : 1.0f;
                    }
                    sx = attribute(t, lx, a, ix) / qx - s;
                    tx = attribute(t, lx, a + 1u, ix) / qx - tt;
                    sy = attribute(t, ly, a, iy) / qy - s;
                    ty = attribute(t, ly, a + 1u, iy) / qy - tt;
                }
                sx *= (float)lk->d->width;
                sy *= (float)lk->d->width;
                tx *= (float)lk->d->height;
                ty *= (float)lk->d->height;
                rho_x = sqrtf(sx * sx + tx * tx);
                rho_y = sqrtf(sy * sy + ty * ty);
                rho = rho_x > rho_y ? rho_x : rho_y;
                lod = (rho > 0.0f ? log2f(rho) : -16.0f) + lk->lod_bias;
            }
        }
        lookup_sample(lk, s, tt, lod, texels[k]);
    }
    for (si = 0; si < plan->tev->stage_count && si < WL_TEV_MAX_STAGES; si++) {
        if (plan->stage_lookup[si] >= 0) {
            memcpy(stage_tex[si], texels[plan->stage_lookup[si]], 4);
        } else {
            memset(stage_tex[si], 0, 4);
        }
    }
    wl_tev_eval_stages(plan->tev, (const uint8_t (*)[4])stage_tex, (const uint8_t (*)[4])ras, regs);
    // (the TEV's output is its eight low bits, as Dolphin hands it on)
    src[0] = (uint8_t)(regs[WL_TEV_PREV].r & 0xFF);
    src[1] = (uint8_t)(regs[WL_TEV_PREV].g & 0xFF);
    src[2] = (uint8_t)(regs[WL_TEV_PREV].b & 0xFF);
    src[3] = (uint8_t)(regs[WL_TEV_PREV].a & 0xFF);
    if (!compare(st->alpha_func, src[3], st->alpha_ref)) {
        plan->info.alpha_failed++;
        return;
    }
    if (z_test && !st->z_early) {
        if (!compare(st->z_func, z, g.depth[i])) {
            plan->info.z_failed++;
            return;
        }
        if (st->z_write) {
            g.depth[i] = z;
        }
    }
    blend(st, src, &g.color[i * 4u], out);
    efb_store(&g.color[i * 4u], out, st->color_update, st->alpha_update);
    plan->info.written++;
    if ((float)px < plan->info.x0) {
        plan->info.x0 = (float)px;
    }
    if ((float)py < plan->info.y0) {
        plan->info.y0 = (float)py;
    }
    if ((float)px + 1.0f > plan->info.x1) {
        plan->info.x1 = (float)px + 1.0f;
    }
    if ((float)py + 1.0f > plan->info.y1) {
        plan->info.y1 = (float)py + 1.0f;
    }
}

// whether an edge (a to b) of a triangle wound clockwise on the screen is a top or a left edge
static bool top_left(const SVertex *a, const SVertex *b) {
    double dx = b->x - a->x, dy = b->y - a->y;

    return (dy == 0.0 && dx > 0.0) || dy < 0.0;
}

static double edge(const SVertex *a, const SVertex *b, double x, double y) {
    return (b->x - a->x) * (y - a->y) - (b->y - a->y) * (x - a->x);
}

static void triangle_fill(Plan *plan, const SVertex *a, const SVertex *b, const SVertex *c) {
    Triangle t;
    double minx, maxx, miny, maxy;
    int x0, x1, y0, y1, px, py;
    bool tl[3];
    unsigned j;

    t.v[0] = *a;
    t.v[1] = *b;
    t.v[2] = *c;
    t.area = edge(&t.v[0], &t.v[1], t.v[2].x, t.v[2].y);
    if (t.area == 0.0 || t.area != t.area) {
        return;
    }
    if (t.area < 0.0) {
        SVertex swap = t.v[1];

        t.v[1] = t.v[2];
        t.v[2] = swap;
        t.area = -t.area;
    }
    tl[0] = top_left(&t.v[1], &t.v[2]);
    tl[1] = top_left(&t.v[2], &t.v[0]);
    tl[2] = top_left(&t.v[0], &t.v[1]);
    // (the weights are affine on the screen: their steps from the edges' slopes)
    t.dl_dx[0] = -(t.v[2].y - t.v[1].y) / t.area;
    t.dl_dy[0] = (t.v[2].x - t.v[1].x) / t.area;
    t.dl_dx[1] = -(t.v[0].y - t.v[2].y) / t.area;
    t.dl_dy[1] = (t.v[0].x - t.v[2].x) / t.area;
    t.dl_dx[2] = -(t.v[1].y - t.v[0].y) / t.area;
    t.dl_dy[2] = (t.v[1].x - t.v[0].x) / t.area;
    minx = maxx = t.v[0].x;
    miny = maxy = t.v[0].y;
    for (j = 1; j < 3; j++) {
        minx = t.v[j].x < minx ? t.v[j].x : minx;
        maxx = t.v[j].x > maxx ? t.v[j].x : maxx;
        miny = t.v[j].y < miny ? t.v[j].y : miny;
        maxy = t.v[j].y > maxy ? t.v[j].y : maxy;
    }
    x0 = (int)floor(minx - SAMPLE_OFFSET);
    x1 = (int)ceil(maxx - SAMPLE_OFFSET) + 1;
    y0 = (int)floor(miny - SAMPLE_OFFSET);
    y1 = (int)ceil(maxy - SAMPLE_OFFSET) + 1;
    x0 = x0 < plan->sx0 ? plan->sx0 : x0;
    y0 = y0 < plan->sy0 ? plan->sy0 : y0;
    x1 = x1 > plan->sx1 ? plan->sx1 : x1;
    y1 = y1 > plan->sy1 ? plan->sy1 : y1;
    for (py = y0; py < y1; py++) {
        double sy = (double)py + SAMPLE_OFFSET;

        for (px = x0; px < x1; px++) {
            double sx = (double)px + SAMPLE_OFFSET, e[3], l[3];

            e[0] = edge(&t.v[1], &t.v[2], sx, sy);
            e[1] = edge(&t.v[2], &t.v[0], sx, sy);
            e[2] = edge(&t.v[0], &t.v[1], sx, sy);
            if (e[0] < 0.0 || e[1] < 0.0 || e[2] < 0.0 || (e[0] == 0.0 && !tl[0]) || (e[1] == 0.0 && !tl[1]) ||
                (e[2] == 0.0 && !tl[2])) {
                continue;
            }
            for (j = 0; j < 3; j++) {
                l[j] = e[j] / t.area;
            }
            pixel(plan, &t, px, py, l);
        }
    }
}

// A triangle (clip-space numbers): culled, clipped, filled. `cull` false for lines and points.
static void triangle(Plan *plan, const float *a, const float *b, const float *c, bool cull) {
    float poly[CLIP_MAX][A_COUNT];
    SVertex s[CLIP_MAX];
    unsigned n, i;
    double area = 0.0;

    plan->info.triangles++;
    memcpy(poly[0], a, sizeof poly[0]);
    memcpy(poly[1], b, sizeof poly[0]);
    memcpy(poly[2], c, sizeof poly[0]);
    n = clip_polygon(poly, 3);
    if (n == 0) {
        plan->info.clipped++;
        return;
    }
    for (i = 0; i < n; i++) {
        to_screen(plan, poly[i], &s[i]);
    }
    for (i = 0; i < n; i++) {
        const SVertex *p = &s[i], *q = &s[(i + 1u) % n];

        area += p->x * q->y - q->x * p->y;
    }
    if (cull) {
        // (y down: a positive area runs clockwise on the screen, GX's front face)
        WlGxCull mode = plan->st->cull;

        if (mode == WL_GX_CULL_ALL || (mode == WL_GX_CULL_FRONT && area > 0.0) ||
            (mode == WL_GX_CULL_BACK && area < 0.0)) {
            plan->info.culled++;
            return;
        }
    }
    for (i = 1; i + 1u < n; i++) {
        triangle_fill(plan, &s[0], &s[i], &s[i + 1u]);
    }
}

// a line or a point as a quad one pixel wide (in clip space, at the vertices' w)
static void thin_quad(Plan *plan, const float *a, const float *b) {
    float q[4][A_COUNT];
    float ax = a[A_CLIP] / a[A_CLIP + 3], ay = a[A_CLIP + 1] / a[A_CLIP + 3];
    float bx = b[A_CLIP] / b[A_CLIP + 3], by = b[A_CLIP + 1] / b[A_CLIP + 3];
    float px = 1.0f / (plan->vw > 0.0f ? plan->vw : 640.0f), py = 1.0f / (plan->vh > 0.0f ? plan->vh : 480.0f);
    float dx = (bx - ax) / (2.0f * px), dy = (by - ay) / (2.0f * py), len = sqrtf(dx * dx + dy * dy);
    float nx, ny;
    unsigned i;

    if (!(a[A_CLIP + 3] > 0.0f) || !(b[A_CLIP + 3] > 0.0f)) {
        return;
    }
    if (len > 0.0f) {
        nx = -dy / len * px;
        ny = dx / len * py;
    } else {
        // (a point: a pixel square)
        nx = px;
        ny = 0.0f;
        memcpy(q[0], a, sizeof q[0]);
        memcpy(q[1], a, sizeof q[0]);
        memcpy(q[2], a, sizeof q[0]);
        memcpy(q[3], a, sizeof q[0]);
        q[0][A_CLIP] -= px * a[A_CLIP + 3];
        q[0][A_CLIP + 1] -= py * a[A_CLIP + 3];
        q[1][A_CLIP] += px * a[A_CLIP + 3];
        q[1][A_CLIP + 1] -= py * a[A_CLIP + 3];
        q[2][A_CLIP] += px * a[A_CLIP + 3];
        q[2][A_CLIP + 1] += py * a[A_CLIP + 3];
        q[3][A_CLIP] -= px * a[A_CLIP + 3];
        q[3][A_CLIP + 1] += py * a[A_CLIP + 3];
        triangle(plan, q[0], q[1], q[2], false);
        triangle(plan, q[0], q[2], q[3], false);
        return;
    }
    for (i = 0; i < 4; i++) {
        const float *v = i < 2 ? a : b;
        float side = (i == 0 || i == 3) ? 1.0f : -1.0f;

        memcpy(q[i], v, sizeof q[0]);
        q[i][A_CLIP] += side * nx * v[A_CLIP + 3];
        q[i][A_CLIP + 1] += side * ny * v[A_CLIP + 3];
    }
    triangle(plan, q[0], q[1], q[2], false);
    triangle(plan, q[0], q[2], q[3], false);
}

static void on_draw(void *user, WlGxDirectPrim prim, const WlGxDirectVertex *vertices, uint32_t count) {
    Plan plan;
    float (*a)[A_COUNT];
    uint32_t i;

    (void)user;
    if (count == 0 || g.color == NULL) {
        return;
    }
    a = malloc(sizeof *a * count);
    if (a == NULL) {
        return;
    }
    plan_make(&plan, prim, count);
    for (i = 0; i < count; i++) {
        vertex_transform(&plan, &vertices[i], a[i]);
    }
    switch (prim) {
    case WL_GX_DIRECT_TRIANGLES:
        for (i = 0; i + 2u < count; i += 3u) {
            triangle(&plan, a[i], a[i + 1u], a[i + 2u], true);
        }
        break;
    case WL_GX_DIRECT_TRIANGLE_STRIP:
        for (i = 0; i + 2u < count; i++) {
            if (i & 1u) {
                triangle(&plan, a[i + 1u], a[i], a[i + 2u], true);
            } else {
                triangle(&plan, a[i], a[i + 1u], a[i + 2u], true);
            }
        }
        break;
    case WL_GX_DIRECT_TRIANGLE_FAN:
        for (i = 1; i + 1u < count; i++) {
            triangle(&plan, a[0], a[i], a[i + 1u], true);
        }
        break;
    case WL_GX_DIRECT_QUADS:
        for (i = 0; i + 3u < count; i += 4u) {
            triangle(&plan, a[i], a[i + 1u], a[i + 2u], true);
            triangle(&plan, a[i], a[i + 2u], a[i + 3u], true);
        }
        break;
    case WL_GX_DIRECT_LINES:
        for (i = 0; i + 1u < count; i += 2u) {
            thin_quad(&plan, a[i], a[i + 1u]);
        }
        break;
    case WL_GX_DIRECT_LINE_STRIP:
        for (i = 0; i + 1u < count; i++) {
            thin_quad(&plan, a[i], a[i + 1u]);
        }
        break;
    default:
        for (i = 0; i < count; i++) {
            thin_quad(&plan, a[i], a[i]);
        }
        break;
    }
    free(a);
    g.stats.draws++;
    g.stats.triangles += plan.info.triangles;
    g.stats.culled += plan.info.culled;
    g.stats.clipped += plan.info.clipped;
    g.stats.pixels += plan.info.pixels;
    g.stats.written += plan.info.written;
    for (i = 0; i < WL_GXSOFT_ISSUES; i++) {
        if (plan.info.issues & (1u << i)) {
            g.stats.issue_draws[i]++;
        }
    }
    if (plan.info.written == 0) {
        plan.info.x0 = plan.info.y0 = plan.info.x1 = plan.info.y1 = 0.0f;
    }
    if (g.cfg.draw != NULL) {
        g.cfg.draw(g.cfg.user, &plan.info);
    }
}

// ---- copies, quads, frames ------------------------------------------------------------------------

static void efb_read(int x, int y, uint8_t rgba[4], uint32_t *z) {
    if (x < 0 || y < 0 || x >= (int)EFB_W || y >= (int)EFB_H) {
        memset(rgba, 0, 4);
        *z = 0;
        return;
    }
    memcpy(rgba, &g.color[((size_t)y * EFB_W + (size_t)x) * 4u], 4);
    *z = g.depth[(size_t)y * EFB_W + (size_t)x];
}

// the byte offset of texel (x, y) in a tiled image `w` texels wide, for blocks of bw by bh texels of
// `block` bytes
static size_t tile_offset(uint32_t x, uint32_t y, uint32_t w, uint32_t bw, uint32_t bh, uint32_t block) {
    uint32_t blocks_x = (w + bw - 1u) / bw;

    return ((size_t)(y / bh) * blocks_x + x / bw) * block;
}

static void on_copy(void *user, int x, int y, uint32_t w, uint32_t h, void *dst, unsigned format, bool clear) {
    uint8_t *out = dst;
    uint32_t tx, ty;

    (void)user;
    if (g.color == NULL || dst == NULL) {
        return;
    }
    if (format == WL_HOST_GX_COPY_Z24X8 || format == WL_GX_TEX_RGBA8) {
        memset(out, 0, wl_gx_tex_bytes(WL_GX_TEX_RGBA8, w, h));
    } else if (format < WL_GX_TEX_FORMAT_COUNT) {
        memset(out, 0, wl_gx_tex_bytes((WlGxTexFormat)format, w, h));
    }
    for (ty = 0; ty < h; ty++) {
        for (tx = 0; tx < w; tx++) {
            uint8_t px[4];
            uint32_t z;

            efb_read(x + (int)tx, y + (int)ty, px, &z);
            if (format == WL_HOST_GX_COPY_Z24X8 || format == WL_GX_TEX_RGBA8) {
                // (RGBA8 tiles: 4 by 4, the AR pairs then the GB pairs)
                uint8_t *b = out + tile_offset(tx, ty, w, 4, 4, 64);
                uint32_t k = (ty % 4u) * 4u + tx % 4u;

                if (format == WL_HOST_GX_COPY_Z24X8) {
                    px[3] = 0xFF;
                    px[0] = (uint8_t)(z >> 16);
                    px[1] = (uint8_t)(z >> 8);
                    px[2] = (uint8_t)z;
                }
                b[k * 2u] = px[3];
                b[k * 2u + 1u] = px[0];
                b[32u + k * 2u] = px[1];
                b[32u + k * 2u + 1u] = px[2];
            } else if (format == WL_GX_TEX_RGB565 || format == WL_GX_TEX_RGB5A3) {
                uint8_t *b = out + tile_offset(tx, ty, w, 4, 4, 32) + ((ty % 4u) * 4u + tx % 4u) * 2u;
                uint32_t v = format == WL_GX_TEX_RGB565
                                 ? (uint32_t)(px[0] >> 3) << 11 | (uint32_t)(px[1] >> 2) << 5 | (uint32_t)(px[2] >> 3)
                                 : wl_gxc_rgb5a3_encode(px);

                b[0] = (uint8_t)(v >> 8);
                b[1] = (uint8_t)v;
            } else if (format == WL_GX_TEX_I8) {
                out[tile_offset(tx, ty, w, 8, 4, 32) + (ty % 4u) * 8u + tx % 8u] = px[0];
            } else if (format == WL_GX_TEX_IA8) {
                uint8_t *b = out + tile_offset(tx, ty, w, 4, 4, 32) + ((ty % 4u) * 4u + tx % 4u) * 2u;

                b[0] = px[3];
                b[1] = px[0];
            } else if (format == WL_GX_TEX_I4) {
                uint8_t *b = out + tile_offset(tx, ty, w, 8, 8, 32) + ((ty % 8u) * 8u + tx % 8u) / 2u;

                *b = (uint8_t)((tx & 1u) ? ((*b & 0xF0u) | (px[0] >> 4)) : ((*b & 0x0Fu) | (px[0] & 0xF0u)));
            }
        }
    }
    if (clear) {
        efb_fill(x, y, x + (int)w, y + (int)h, true, g.clear_rgba, true, Z_FAR);
    }
    g.stats.copies++;
    textures_unchecked();
}

static void on_restore(void *user, const void *colour, const void *depth, uint32_t w, uint32_t h) {
    const uint8_t *c = colour, *d = depth;
    const WlHostGxDirect *st = wl_host_gx_direct();
    uint32_t x, y, sx1 = st->scissor[0] + st->scissor[2], sy1 = st->scissor[1] + st->scissor[3];

    (void)user;
    if (g.color == NULL) {
        return;
    }
    for (y = 0; y < h && y < EFB_H; y++) {
        for (x = 0; x < w && x < EFB_W; x++) {
            size_t i = (size_t)y * EFB_W + x, o = tile_offset(x, y, w, 4, 4, 64);
            uint32_t k = (y % 4u) * 4u + x % 4u;
            uint8_t px[4];

            if (x < st->scissor[0] || y < st->scissor[1] || x >= sx1 || y >= sy1) {
                continue;
            }
            px[3] = c[o + k * 2u];
            px[0] = c[o + k * 2u + 1u];
            px[1] = c[o + 32u + k * 2u];
            px[2] = c[o + 32u + k * 2u + 1u];
            efb_store(&g.color[i * 4u], px, true, true);
            g.depth[i] = (uint32_t)d[o + k * 2u + 1u] << 16 | (uint32_t)d[o + 32u + k * 2u] << 8 |
                         (uint32_t)d[o + 32u + k * 2u + 1u];
        }
    }
    g.stats.restores++;
}

static void on_clear(void *user, bool colour, bool depth, uint32_t rgba, float z, int x, int y, int w, int h) {
    const WlHostGxDirect *st = wl_host_gx_direct();
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;

    (void)user;
    if (g.color == NULL) {
        return;
    }
    // (the quad within the scissor)
    x0 = x0 < (int)st->scissor[0] ? (int)st->scissor[0] : x0;
    y0 = y0 < (int)st->scissor[1] ? (int)st->scissor[1] : y0;
    x1 = x1 > (int)(st->scissor[0] + st->scissor[2]) ? (int)(st->scissor[0] + st->scissor[2]) : x1;
    y1 = y1 > (int)(st->scissor[1] + st->scissor[3]) ? (int)(st->scissor[1] + st->scissor[3]) : y1;
    efb_fill(x0, y0, x1, y1, colour, rgba, depth, depth24(z));
    g.stats.clears++;
}

static void on_squeeze(void *user, uint32_t back_w, uint32_t back_h, uint32_t x, uint32_t w) {
    uint8_t *copy;
    uint32_t px, py;

    (void)user;
    if (g.color == NULL || w == 0 || back_w == 0 || back_w > EFB_W || back_h > EFB_H) {
        return;
    }
    copy = malloc((size_t)back_w * back_h * 4u);
    if (copy == NULL) {
        return;
    }
    for (py = 0; py < back_h; py++) {
        memcpy(copy + (size_t)py * back_w * 4u, g.color + (size_t)py * EFB_W * 4u, (size_t)back_w * 4u);
    }
    efb_fill(0, 0, (int)back_w, (int)back_h, true, 0x000000FFu, true, Z_FAR);
    for (py = 0; py < back_h; py++) {
        for (px = x; px < x + w && px < back_w; px++) {
            double u = ((double)(px - x) + 0.5) * back_w / w - 0.5;
            int u0 = (int)floor(u);
            double f = u - u0;
            int ua = u0 < 0 ? 0 : u0, ub = u0 + 1 >= (int)back_w ? (int)back_w - 1 : u0 + 1;
            const uint8_t *a = copy + ((size_t)py * back_w + (size_t)ua) * 4u;
            const uint8_t *b = copy + ((size_t)py * back_w + (size_t)ub) * 4u;
            uint8_t v[4];
            unsigned c;

            for (c = 0; c < 4; c++) {
                v[c] = (uint8_t)(a[c] * (1.0 - f) + b[c] * f + 0.5);
            }
            efb_store(&g.color[((size_t)py * EFB_W + px) * 4u], v, true, true);
        }
    }
    free(copy);
}

static void on_frame_begin(void *user, uint32_t clear_rgba) {
    (void)user;
    g.clear_rgba = clear_rgba;
    if (g.color != NULL) {
        efb_fill(0, 0, (int)EFB_W, (int)EFB_H, true, clear_rgba, true, Z_FAR);
    }
}

static void on_frame_end(void *user, uint32_t width, uint32_t height) {
    (void)user;
    if (g.color == NULL) {
        return;
    }
    width = width > EFB_W ? EFB_W : width;
    height = height > EFB_H ? EFB_H : height;
    g.stats.frames++;
    if (g.cfg.frame != NULL) {
        uint8_t *picture = malloc((size_t)width * height * 4u);
        uint32_t y;

        if (picture != NULL) {
            for (y = 0; y < height; y++) {
                memcpy(picture + (size_t)y * width * 4u, g.color + (size_t)y * EFB_W * 4u, (size_t)width * 4u);
            }
            g.cfg.frame(g.cfg.user, picture, width, height);
            free(picture);
        }
    }
}

static uint32_t on_peek(void *user, int x, int y) {
    uint8_t px[4];
    uint32_t z;

    (void)user;
    if (g.color == NULL) {
        return 0;
    }
    efb_read(x, y, px, &z);
    return (uint32_t)px[0] << 24 | (uint32_t)px[1] << 16 | (uint32_t)px[2] << 8 | px[3];
}

static void on_efb_alpha(void *user, bool on) {
    (void)user;
    g.efb_alpha = on;
}

static void on_texture_invalidate(void *user) {
    (void)user;
    textures_unchecked();
}

// ---- attaching ------------------------------------------------------------------------------------

WlResult wl_gxsoft_attach(const WlGxSoftConfig *cfg) {
    WlHostGxRenderer r;

    wl_gxsoft_detach();
    memset(&g, 0, sizeof g);
    g.color = malloc((size_t)EFB_W * EFB_H * 4u);
    g.depth = malloc((size_t)EFB_W * EFB_H * sizeof g.depth[0]);
    if (g.color == NULL || g.depth == NULL) {
        free(g.color);
        free(g.depth);
        g.color = NULL;
        g.depth = NULL;
        return WL_ERR_NOMEM;
    }
    if (cfg != NULL) {
        g.cfg = *cfg;
    }
    g.clear_rgba = 0x000000FFu;
    efb_fill(0, 0, (int)EFB_W, (int)EFB_H, true, g.clear_rgba, true, Z_FAR);
    // (GX's state before any program: the rasterized colour)
    wl_tev_recipe(&g.fallback, WL_GX_RECIPE_VERTEX_COLOR);
    memset(&r, 0, sizeof r);
    r.frame_begin = on_frame_begin;
    r.frame_end = on_frame_end;
    r.draw = on_draw;
    r.copy = on_copy;
    r.restore = on_restore;
    r.clear = on_clear;
    r.squeeze = on_squeeze;
    r.peek = on_peek;
    r.efb_alpha = on_efb_alpha;
    r.texture_invalidate = on_texture_invalidate;
    wl_host_gx_set_renderer(&r);
    g.attached = true;
    return WL_OK;
}

void wl_gxsoft_detach(void) {
    unsigned i;

    if (!g.attached) {
        return;
    }
    wl_host_gx_set_renderer(NULL);
    for (i = 0; i < DECODED_MAX; i++) {
        decoded_free(&g.decoded[i]);
    }
    free(g.color);
    free(g.depth);
    memset(&g, 0, sizeof g);
}

bool wl_gxsoft_attached(void) {
    return g.attached;
}

void wl_gxsoft_stats(WlGxSoftStats *out) {
    *out = g.stats;
}

const uint8_t *wl_gxsoft_efb(void) {
    return g.color;
}

uint32_t wl_gxsoft_depth(uint32_t x, uint32_t y) {
    if (g.depth == NULL || x >= EFB_W || y >= EFB_H) {
        return 0xFFFFFFFFu;
    }
    return g.depth[(size_t)y * EFB_W + x];
}

const char *wl_gxsoft_issue_name(unsigned bit) {
    static const char *const names[WL_GXSOFT_ISSUES] = {"TEV program invalid", "colour channel not sent",
                                                        "texture coordinate not generated", "texture map empty",
                                                        "TLUT not loaded"};

    return bit < WL_GXSOFT_ISSUES ? names[bit] : "?";
}
