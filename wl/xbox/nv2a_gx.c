// SPDX-License-Identifier: MIT
// The NV2A on GX (nv2a_gx.h). Derived from OpenCE-Wii's Direct3D 8 device (port/wii/src/d3d8_gx.c,
// CC0): its state mapping (states_apply, gx_compare, gx_blend_factor), the cull measure, the
// texture binding, the pixel stage (tev_setup's generic half), the vertices in screen space or
// through a fitted perspective projection (projection_fit, projection_load, vertex_store,
// clip_position, the depth scale that keeps the far plane from clipping), the vertex memo of indexed
// draws, the EFB's owner and the clears; read from the NV2A's state rather than Direct3D's, with the
// GX calls through wl/platform's direct layer.

#include "wl/xbox/nv2a_gx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "wl/base/hash.h"
#include "wl/platform/gx_direct.h"
#include "wl/platform/gx_target.h"
#include "wl/platform/platform.h"
#include "wl/platform/tev.h"
#include "wl/xbox/nv2a_pipeline.h"
#include "wl/xbox/nv2a_texture.h"
#include "wl/xbox/psh_cache.h"

#define TAG "nv2a"

enum {
    TARGETS = 8,
    // indexed draws' vertices kept by index (a direct-mapped table: a strip's or list's neighbours
    // come back soon)
    MEMO = 1024,
    // vertices in front of the eye looked at for the projection's fit, at most
    FIT_PROBES = 24,
    // ...among the draw's first ones (a strip may start behind the eye)
    FIT_SCAN = 128,
    // ...of which these, spread in depth, are enough
    FIT_ENOUGH = 6,
    // the fits kept by the transform they were made for (its signature): sets by the signature's
    // hash, ways in a set
    FIT_SETS = 128,
    FIT_WAYS = 2,
    // the inputs a draw's program is also run with for its transform's signature
    CANONICAL_POINTS = 4
};

// how far (in the depth's 0..1) a fit made with the canonical points may be from a draw's own
// depths: a few of the depth buffer's 24-bit steps, the rounding of a program's depth
#define FIT_TOLERANCE (8.0f / 16777216.0f)
// how far a fit kept for a transform may be from a draw of the same signature: only a sanity check.
// The draws of one transform must share its fit exactly whatever its rounding (a fit is exact only
// near the vertices it was made from, and a second pass may reach nearer the eye, where the depth
// runs steep: hundreds of steps there), and a + b / w belongs to the camera's projection, not to
// the object, so even two transforms the signature cannot tell apart (a position that also
// depends on what the canonical inputs set) share it rightly; a fit this far off is another
// projection
#define SIGNATURE_TOLERANCE (1.0f / 256.0f)

// the depth GX's perspective draws are scaled by: below 1 for every w while a < 1 / DEPTH_SCALE,
// so nothing is clipped at the far plane (OpenCE-Wii's b30 horizon), the same for every draw
#define DEPTH_SCALE (1.0f / (1.0f + 1.0f / 8192.0f))

typedef struct RenderTarget {
    uint32_t phys, width, height;
    uint8_t format; // WlGxTexFormat
    void *memory;
    size_t bytes;
    bool resolved;
    uint32_t used_frame;
    WlGxTarget target;
} RenderTarget;

typedef struct Projection {
    bool perspective;
    float a, b;
} Projection;

struct WlNv2aGx {
    WlNv2aGxConfig cfg;
    WlXboxPshRecipe recipe;
    WlNv2a *dev;
    WlNv2aPipeline pipeline;
    WlNv2aTexCache textures;
    WlXboxPshCache psh;
    WlNv2aGxStats stats;
    // the last draw's pixel program, and whether it was used
    const WlXboxPshProgram *last_program;
    bool last_used;

    // the frame
    bool frame_open, back_drawn;
    uint32_t back_phys;
    uint32_t displays[4];
    unsigned display_next;
    uint32_t frame;
    // the GX mode that culls triangles of positive area in the screen's coordinates (y down)
    WlGxCull cull_positive;
    bool cull_measured;
    // the EFB's owner (NULL: the back buffer) and the targets
    RenderTarget targets[TARGETS];
    RenderTarget *owner;
    // the title samples a picture it showed (a display surface as a texture): each finished back
    // buffer is copied into a target at its address
    bool keep_frames;
    uint32_t view_w, view_h;
    // the draw's
    const WlNv2aTexEntry *entries[WL_NV2A_TEXTURES];
    WlNv2aTexEntry target_entries[WL_NV2A_TEXTURES];
    WlGxTexObj stage_objects[WL_NV2A_TEXTURES], alpha_objects[WL_NV2A_TEXTURES];
    WlGxTlut tluts[WL_NV2A_TEXTURES];
    bool texture_memory_changed;
    unsigned texcoords, channels;
    Projection projection;
    bool projection_loaded;
    Projection loaded;
    float loaded_bias;
    uint32_t loaded_w, loaded_h;
    float fit_a, fit_b;
    // the fits kept by their transform's signature: the clip-space positions the program gives the
    // canonical inputs, bit for bit
    struct FitSignature {
        uint32_t clip[CANONICAL_POINTS][4];
        float a, b;
        uint32_t used;
    } signatures[FIT_SETS][FIT_WAYS];
    uint32_t signature_use;
    bool last_shared;
    struct {
        uint32_t index, stamp;
        WlGxDirectVertex vertex;
        float x, y;
    } memo[MEMO];
    uint32_t memo_stamp;
};

// ---- creation -----------------------------------------------------------------------------------

static void commit(void *user, void *memory, size_t bytes) {
    (void)user;
    wl_gpu_buffer_commit(memory, bytes);
}

WlNv2aGx *wl_nv2a_gx_create(const WlNv2aGxConfig *cfg) {
    WlNv2aGx *gx = calloc(1, sizeof *gx);
    WlNv2aTexCacheConfig textures;

    if (gx == NULL) {
        return NULL;
    }
    if (cfg != NULL) {
        gx->cfg = *cfg;
    }
    if (gx->cfg.width == 0 || gx->cfg.height == 0) {
        gx->cfg.width = 640;
        gx->cfg.height = 480;
    }
    if (gx->cfg.recipe != NULL) {
        gx->recipe = *gx->cfg.recipe;
    }
    textures = gx->cfg.textures;
    if (textures.flush == NULL) {
        textures.flush = commit;
    }
    wl_nv2a_texcache_init(&gx->textures, &textures);
    wl_nv2a_pipeline_init(&gx->pipeline, gx->cfg.natives, gx->cfg.native_count);
    wl_xbox_psh_cache_init(&gx->psh, gx->cfg.recipe != NULL ? &gx->recipe : NULL);
    wl_gx_set_efb_alpha(!gx->cfg.no_efb_alpha);
    wl_gx_target_set_back_buffer_size(gx->cfg.width, gx->cfg.height);
    gx->cull_positive = WL_GX_CULL_BACK;
    gx->fit_b = 0.0f;
    return gx;
}

void wl_nv2a_gx_destroy(WlNv2aGx *gx) {
    unsigned i;

    if (gx == NULL) {
        return;
    }
    if (gx->frame_open) {
        wl_nv2a_gx_present(gx);
    }
    wl_nv2a_texcache_clear(&gx->textures);
    for (i = 0; i < TARGETS; i++) {
        if (gx->targets[i].memory != NULL) {
            gx->textures.cfg.release(gx->textures.cfg.user, gx->targets[i].memory);
        }
    }
    free(gx);
}

void wl_nv2a_gx_attach(WlNv2aGx *gx, WlNv2a *dev) {
    gx->dev = dev;
}

void wl_nv2a_gx_stats(const WlNv2aGx *gx, WlNv2aGxStats *out) {
    *out = gx->stats;
}

const WlNv2aTexCache *wl_nv2a_gx_textures(const WlNv2aGx *gx) {
    return &gx->textures;
}

bool wl_nv2a_gx_last_projection(const WlNv2aGx *gx, float *a, float *b, bool *shared) {
    if (a != NULL) {
        *a = gx->projection.a;
    }
    if (b != NULL) {
        *b = gx->projection.b;
    }
    if (shared != NULL) {
        *shared = gx->last_shared;
    }
    return gx->projection.perspective;
}

const WlXboxPshProgram *wl_nv2a_gx_last_program(const WlNv2aGx *gx, bool *used) {
    if (used != NULL) {
        *used = gx->last_used;
    }
    return gx->last_program;
}

// ---- the frame ----------------------------------------------------------------------------------

// what wl/platform's quads and the frame's start leave: everything set again before the next draw
static void device_state(WlNv2aGx *gx) {
    wl_gx_direct_begin();
    gx->projection_loaded = false;
}

static void cull_measure(WlNv2aGx *gx, uint32_t clear_rgba);

static void frame_begin(WlNv2aGx *gx, uint32_t clear_rgba) {
    if (gx->frame_open) {
        return;
    }
    wl_gx_begin_frame(clear_rgba);
    gx->frame_open = true;
    gx->back_drawn = false;
    gx->owner = NULL;
    device_state(gx);
    if (!gx->cull_measured) {
        gx->cull_measured = true;
        cull_measure(gx, clear_rgba);
    }
}

static void target_resolved(WlNv2aGx *gx, const WlGxTarget *resolved) {
    unsigned i;

    if (resolved == NULL) {
        return;
    }
    for (i = 0; i < TARGETS; i++) {
        if (&gx->targets[i] == (const RenderTarget *)resolved->id) {
            gx->targets[i].resolved = true;
        }
    }
    gx->texture_memory_changed = true;
}

static void use_back_buffer(WlNv2aGx *gx) {
    if (gx->owner != NULL) {
        target_resolved(gx, wl_gx_target_use(NULL));
        gx->owner = NULL;
        device_state(gx);
    }
}

static void frame_keep(WlNv2aGx *gx);

void wl_nv2a_gx_present(WlNv2aGx *gx) {
    if (!gx->frame_open) {
        return;
    }
    use_back_buffer(gx);
    frame_keep(gx);
    wl_gx_invalidate_shadow();
    wl_gx_end_frame();
    gx->frame_open = false;
    gx->back_drawn = false;
    gx->frame++;
    gx->stats.frames++;
    // (the GPU has drawn the frame: the textures evicted during it can go)
    wl_nv2a_texcache_frame_done(&gx->textures);
    wl_gx_direct_flush_after();
}

static bool is_display(const WlNv2aGx *gx, uint32_t phys) {
    unsigned i;

    for (i = 0; i < 4; i++) {
        if (gx->displays[i] == phys && phys != 0) {
            return true;
        }
    }
    return false;
}

// a surface the back buffer holds: one the display was flipped to, or one the size of the screen
// (not a multisampled one: a title resolves that into the surface it shows, reading it as a
// texture, so it is a render target)
static bool surface_is_screen(const WlNv2aGx *gx, const WlNv2aSurface *s) {
    uint32_t w = s->clip_width / s->samples_x, h = s->clip_height / s->samples_y;

    if (s->color_format == 0 || is_display(gx, s->color_phys)) {
        return true;
    }
    return !s->swizzled && s->samples_x == 1 && s->samples_y == 1 && w * 4u >= gx->cfg.width * 3u &&
           h * 4u >= gx->cfg.height * 3u;
}

// the target of a surface at `phys` of `w` x `h` in `format` (a WlGxTexFormat): the one there, or
// the least recently used slot made it (NULL without memory, or when that slot owns the EFB)
static RenderTarget *slot_for(WlNv2aGx *gx, uint32_t phys, uint32_t w, uint32_t h, uint8_t format) {
    RenderTarget *slot = NULL;
    unsigned i;

    for (i = 0; i < TARGETS; i++) {
        RenderTarget *t = &gx->targets[i];

        if (t->memory != NULL && t->phys == phys && t->width == w && t->height == h && t->format == format) {
            t->used_frame = gx->frame;
            return t;
        }
        if (slot == NULL || t->memory == NULL || (slot->memory != NULL && t->used_frame < slot->used_frame)) {
            slot = t;
        }
    }
    if (slot == gx->owner) {
        return NULL;
    }
    if (slot->memory != NULL) {
        gx->textures.cfg.release(gx->textures.cfg.user, slot->memory);
    }
    memset(slot, 0, sizeof *slot);
    slot->bytes = wl_gx_tex_bytes((WlGxTexFormat)format, w, h);
    slot->memory = gx->textures.cfg.alloc(gx->textures.cfg.user, slot->bytes);
    if (slot->memory == NULL) {
        return NULL;
    }
    slot->phys = phys;
    slot->width = w;
    slot->height = h;
    slot->format = format;
    slot->used_frame = gx->frame;
    slot->target.id = slot;
    slot->target.memory = slot->memory;
    slot->target.width = (uint16_t)w;
    slot->target.height = (uint16_t)h;
    slot->target.format = format;
    slot->target.user = phys;
    return slot;
}

static RenderTarget *target_for(WlNv2aGx *gx, const WlNv2aSurface *s) {
    uint32_t w = s->clip_width / s->samples_x, h = s->clip_height / s->samples_y;
    uint8_t format = s->color_bytes == 2 ? (uint8_t)WL_GX_TEX_RGB565 : (uint8_t)WL_GX_TEX_RGBA8;

    if (s->swizzled) {
        w = 1u << s->log2_width;
        h = 1u << s->log2_height;
    }
    w = (w + 3u) & ~3u;
    h = (h + 3u) & ~3u;
    if (w == 0 || h == 0 || w > 1024u || h > 1024u) {
        return NULL;
    }
    return slot_for(gx, s->color_phys, w, h, format);
}

// The finished back buffer copied into a target at its address, for a title that samples a picture
// it showed (Halo's sun glare reads the last frame's alpha, where it drew the sun's visibility):
// on the NV2A that surface's pixels are in memory, on GX only in the EFB until the display copy.
static void frame_keep(WlNv2aGx *gx) {
    uint32_t w = gx->cfg.width & ~3u, h = gx->cfg.height & ~3u;
    RenderTarget *t;

    if (!gx->keep_frames || !gx->back_drawn || gx->back_phys == 0 || gx->owner != NULL) {
        return;
    }
    t = slot_for(gx, gx->back_phys, w, h, (uint8_t)WL_GX_TEX_RGBA8);
    if (t == NULL) {
        return;
    }
    wl_gx_copy_efb_texture(0, 0, (int)w, (int)h, t->memory, WL_GX_TEX_RGBA8, false);
    t->resolved = true;
    gx->texture_memory_changed = true;
}

static bool is_display(const WlNv2aGx *gx, uint32_t phys);

// The EFB for the surface the next draw or clear is into: the frame begun, the back buffer or a
// render target its owner, the viewport and scissor. false when the surface cannot be drawn.
static bool surface_prepare(WlNv2aGx *gx, const WlNv2aSurface *s) {
    uint32_t sx = s->samples_x, sy = s->samples_y, x0, y0, x1, y1;

    if (surface_is_screen(gx, s)) {
        if (gx->frame_open && gx->back_drawn && gx->back_phys != s->color_phys && s->color_format != 0 &&
            gx->back_phys != 0) {
            // (another screen-sized surface: the one before is finished)
            wl_nv2a_gx_present(gx);
        }
        frame_begin(gx, 0x000000FFu);
        use_back_buffer(gx);
        if (s->color_format != 0) {
            gx->back_phys = s->color_phys;
        }
        gx->back_drawn = true;
        wl_gx_target_drawn();
        gx->view_w = gx->cfg.width;
        gx->view_h = gx->cfg.height;
    } else {
        RenderTarget *t;

        frame_begin(gx, 0x000000FFu);
        t = target_for(gx, s);
        if (t == NULL) {
            gx->stats.target_skipped++;
            return false;
        }
        if (gx->owner != t) {
            target_resolved(gx, wl_gx_target_use(&t->target));
            gx->owner = t;
            device_state(gx);
        }
        t->resolved = false;
        gx->stats.target_draws++;
        gx->view_w = t->width < gx->cfg.width ? t->width : gx->cfg.width;
        gx->view_h = t->height < gx->cfg.height ? t->height : gx->cfg.height;
    }
    x0 = s->clip_x / sx;
    y0 = s->clip_y / sy;
    x1 = (s->clip_x + s->clip_width) / sx;
    y1 = (s->clip_y + s->clip_height) / sy;
    x1 = x1 > gx->view_w || x1 == 0 ? gx->view_w : x1;
    y1 = y1 > gx->view_h || y1 == 0 ? gx->view_h : y1;
    if (x0 >= x1 || y0 >= y1) {
        x0 = y0 = 0;
        x1 = gx->view_w;
        y1 = gx->view_h;
    }
    wl_gx_direct_viewport(0.0f, 0.0f, (float)gx->view_w, (float)gx->view_h);
    wl_gx_direct_scissor(x0, y0, x1 - x0, y1 - y0);
    return true;
}

// ---- the raster state -----------------------------------------------------------------------------

// whether this draw's raster state was mapped exactly (blending's approximations counted)
static bool states_apply(WlNv2aGx *gx, const WlNv2aKelvin *k, const WlNv2aSurface *s) {
    uint32_t mask = wl_nv2a_k(k, WL_NV2A_K_SET_COLOR_MASK);
    bool alpha_test = wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_TEST_ENABLE) != 0, depth = s->zeta_format != 0, exact;
    bool depth_test = depth && wl_nv2a_k(k, WL_NV2A_K_SET_DEPTH_TEST_ENABLE) != 0;
    WlGxBlendMode mode;
    WlGxBlendFactor source, destination;

    exact = wl_nv2a_gx_blend(k, &mode, &source, &destination);
    wl_gx_direct_blend(mode, source, destination);
    wl_gx_direct_color_update((mask & 0x00010101u) != 0, (mask & 0x01000000u) != 0);
    // (no depth buffer: no depth test and no writes; no writes without the test, as the NV2A)
    wl_gx_direct_zmode(depth_test, wl_nv2a_gx_compare(wl_nv2a_k(k, WL_NV2A_K_SET_DEPTH_FUNC)),
                       depth_test && wl_nv2a_k(k, WL_NV2A_K_SET_DEPTH_MASK) != 0);
    if (alpha_test) {
        wl_gx_direct_alpha_compare(wl_nv2a_gx_compare(wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_FUNC)),
                                   (uint8_t)wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_REF));
    } else {
        wl_gx_direct_alpha_compare(WL_GX_ALWAYS, 0);
    }
    // (the depth test after texturing when the alpha test may discard, so a discarded texel writes
    // no depth)
    wl_gx_direct_zcompare_early(!alpha_test);
    wl_gx_direct_cull(wl_nv2a_gx_cull(wl_nv2a_cull(k), gx->cull_positive));
    return exact;
}

// The GX mode that culls a triangle of positive area in the screen's coordinates (x right, y down,
// as the vertices are sent): a small one drawn with GX_CULL_FRONT and read back (OpenCE-Wii's
// cull_measure: GX's names for its windings are not the screen's).
static void cull_measure(WlNv2aGx *gx, uint32_t clear_rgba) {
    static const float corners[3][2] = {{0.0f, 0.0f}, {16.0f, 0.0f}, {0.0f, 16.0f}};
    WlGxDirectLayout layout = {1, 0, 0};
    WlTevProgram tev;
    float m[4][4];
    unsigned pass, v;
    uint32_t read;

    memset(m, 0, sizeof m);
    m[0][0] = 2.0f / (float)gx->cfg.width;
    m[0][3] = -1.0f;
    m[1][1] = -2.0f / (float)gx->cfg.height;
    m[1][3] = 1.0f;
    m[2][2] = 1.0f;
    m[3][3] = 1.0f;
    wl_gx_direct_projection((const float (*)[4])m, true);
    wl_gx_direct_viewport(0.0f, 0.0f, (float)gx->cfg.width, (float)gx->cfg.height);
    wl_gx_direct_scissor(0, 0, gx->cfg.width, gx->cfg.height);
    wl_gx_direct_blend(WL_GX_BM_NONE, WL_GX_BL_ONE, WL_GX_BL_ZERO);
    wl_gx_direct_alpha_compare(WL_GX_ALWAYS, 0);
    wl_gx_direct_zmode(false, WL_GX_ALWAYS, false);
    wl_gx_direct_color_update(true, true);
    wl_tev_recipe(&tev, WL_GX_RECIPE_VERTEX_COLOR);
    wl_tev_apply_unchecked(&tev);
    wl_gx_direct_layout(&layout);
    // black, unculled; then white, with GX_CULL_FRONT
    for (pass = 0; pass < 2; pass++) {
        WlGxDirectVertex vertices[3];

        memset(vertices, 0, sizeof vertices);
        for (v = 0; v < 3; v++) {
            uint8_t value = pass ? 255 : 0;

            vertices[v].position[0] = corners[v][0];
            vertices[v].position[1] = corners[v][1];
            vertices[v].position[2] = -0.5f;
            vertices[v].color[0][0] = vertices[v].color[0][1] = vertices[v].color[0][2] = value;
            vertices[v].color[0][3] = 255;
        }
        wl_gx_direct_cull(pass ? WL_GX_CULL_FRONT : WL_GX_CULL_NONE);
        wl_gx_direct_draw(WL_GX_DIRECT_TRIANGLES, vertices, 3);
    }
    wl_gx_direct_cull(WL_GX_CULL_NONE);
    read = wl_gx_peek_rgba(2, 2);
    // (the corner as the frame's clear left it)
    wl_gx_target_clear(true, true, clear_rgba, 1.0f, 0, 0, 16, 16);
    device_state(gx);
    // drawn: GX_CULL_FRONT keeps it, so GX_CULL_BACK culls it
    gx->cull_positive = (read >> 24) > 128u ? WL_GX_CULL_BACK : WL_GX_CULL_FRONT;
    WL_LOG_I(TAG, "GX_CULL_FRONT %s a triangle of positive area (read %08x): culled by %s",
             (read >> 24) > 128u ? "keeps" : "discards", (unsigned)read,
             gx->cull_positive == WL_GX_CULL_BACK ? "GX_CULL_BACK" : "GX_CULL_FRONT");
    gx->projection_loaded = false;
}

// ---- textures -------------------------------------------------------------------------------------

static uint8_t gx_wrap(uint8_t wrap) {
    switch (wrap) {
    case WL_NV2A_WRAP_REPEAT:
        return WL_GX_WRAP_REPEAT;
    case WL_NV2A_WRAP_MIRROR:
        return WL_GX_WRAP_MIRROR;
    default:
        return WL_GX_WRAP_CLAMP;
    }
}

static uint8_t gx_min_filter(uint8_t filter, bool mipmapped) {
    switch (filter) {
    case WL_NV2A_FILTER_NEAREST:
        return WL_GX_FILTER_NEAREST;
    case WL_NV2A_FILTER_NEAREST_MIP_NEAREST:
        return mipmapped ? (uint8_t)WL_GX_FILTER_NEAREST_MIP_NEAREST : (uint8_t)WL_GX_FILTER_NEAREST;
    case WL_NV2A_FILTER_LINEAR_MIP_NEAREST:
        return mipmapped ? (uint8_t)WL_GX_FILTER_LINEAR_MIP_NEAREST : (uint8_t)WL_GX_FILTER_LINEAR;
    case WL_NV2A_FILTER_NEAREST_MIP_LINEAR:
        return mipmapped ? (uint8_t)WL_GX_FILTER_NEAREST_MIP_LINEAR : (uint8_t)WL_GX_FILTER_NEAREST;
    case WL_NV2A_FILTER_LINEAR_MIP_LINEAR:
        return mipmapped ? (uint8_t)WL_GX_FILTER_LINEAR_MIP_LINEAR : (uint8_t)WL_GX_FILTER_LINEAR;
    default:
        return WL_GX_FILTER_LINEAR;
    }
}

static bool power_of_two(uint32_t v) {
    return v != 0 && (v & (v - 1u)) == 0;
}

// a resolved render target at a texture's address, as an entry the stages read like any texture
static const WlNv2aTexEntry *target_texture(WlNv2aGx *gx, unsigned stage, const WlNv2aTexture *t) {
    unsigned i;

    for (i = 0; i < TARGETS; i++) {
        RenderTarget *r = &gx->targets[i];

        if (r->memory != NULL && r->resolved && r->phys == t->phys && r != gx->owner) {
            WlNv2aTexEntry *e = &gx->target_entries[stage];

            memset(e, 0, sizeof *e);
            e->phys = r->phys;
            e->image = r->memory;
            e->layout.format = (WlGxcFormat)r->format;
            e->layout.width = r->width;
            e->layout.height = r->height;
            e->layout.levels = 1;
            e->layout.image_bytes = r->bytes;
            e->layout.total_bytes = r->bytes;
            e->contents = WL_XBOX_PSH_TEXTURE_ALL;
            e->linear = t->linear;
            e->width = (uint16_t)t->width;
            e->height = (uint16_t)t->height;
            e->serial = 0x80000000u | i;
            return e;
        }
    }
    return NULL;
}

static void textures_bind(WlNv2aGx *gx, const WlNv2aKelvin *k) {
    unsigned stage;

    for (stage = 0; stage < WL_NV2A_TEXTURES; stage++) {
        WlNv2aTexture t;
        const WlNv2aTexEntry *e;
        WlGxTexDesc desc;
        bool made = false, mipmapped;

        gx->entries[stage] = NULL;
        if (wl_nv2a_stage_program(k, stage) == WL_NV2A_STAGE_NONE || !wl_nv2a_texture(k, stage, &t)) {
            continue;
        }
        e = target_texture(gx, stage, &t);
        if (e == NULL && !gx->keep_frames && is_display(gx, t.phys)) {
            // (a picture the title showed, as a texture: kept from the next frame on)
            gx->keep_frames = true;
            WL_LOG_I(TAG, "a displayed surface (%08x) is read as a texture: each frame is kept as one",
                     (unsigned)t.phys);
        }
        if (e == NULL) {
            bool mipmaps = t.levels > 1 && t.min_filter >= WL_NV2A_FILTER_NEAREST_MIP_NEAREST &&
                           t.min_filter <= WL_NV2A_FILTER_LINEAR_MIP_LINEAR;

            e = wl_nv2a_texcache_get(&gx->textures, gx->dev, &t, mipmaps, &made);
            if (e == NULL) {
                continue;
            }
        }
        if (made) {
            gx->texture_memory_changed = true;
        }
        mipmapped = e->layout.levels > 1;
        memset(&desc, 0, sizeof desc);
        desc.image = e->image;
        desc.width = (uint16_t)e->layout.width;
        desc.height = (uint16_t)e->layout.height;
        desc.format = (uint8_t)e->layout.format;
        desc.mips = (uint8_t)e->layout.levels;
        desc.tlut = (uint8_t)stage;
        if (power_of_two(desc.width) && power_of_two(desc.height) && !e->sphere) {
            desc.sampler.wrap_s = gx_wrap(t.wrap_u);
            desc.sampler.wrap_t = gx_wrap(t.wrap_v);
        }
        desc.sampler.min_filter = gx_min_filter(t.min_filter, mipmapped);
        desc.sampler.mag_filter = t.mag_filter == WL_NV2A_FILTER_NEAREST ? (uint8_t)WL_GX_FILTER_NEAREST
                                                                         : (uint8_t)WL_GX_FILTER_LINEAR;
        desc.sampler.pinned = true;
        if (wl_gx_tex_init_ex(&gx->stage_objects[stage], &desc) != WL_OK) {
            continue;
        }
        if (e->layout.format == WL_GXC_C8) {
            if (wl_gx_tlut_init(&gx->tluts[stage], (const uint8_t *)e->image + e->layout.tlut_offset, WL_GX_TLUT_RGB5A3,
                                (uint16_t)e->layout.tlut_entries) != WL_OK) {
                continue;
            }
            wl_gx_direct_texture(stage, &gx->stage_objects[stage], NULL, &gx->tluts[stage]);
        } else {
            wl_gx_direct_texture(stage, &gx->stage_objects[stage], NULL, NULL);
        }
        if (e->layout.separate_alpha) {
            desc.image = (const uint8_t *)e->image + ((e->layout.image_bytes + 31u) & ~(size_t)31u);
            desc.format = WL_GX_TEX_I4;
            if (wl_gx_tex_init_ex(&gx->alpha_objects[stage], &desc) == WL_OK) {
                wl_gx_direct_texture(stage + 4u, &gx->alpha_objects[stage], NULL, NULL);
            }
        }
        gx->entries[stage] = e;
    }
    if (gx->texture_memory_changed) {
        wl_gx_direct_texture_invalidate();
        gx->texture_memory_changed = false;
    }
}

// ---- the pixel stage ------------------------------------------------------------------------------

static bool tev_setup(WlNv2aGx *gx, const WlNv2aKelvin *k) {
    WlXboxPshTextures textures;
    uint32_t words[WL_XBOX_PSH_WORDS];
    const WlXboxPshProgram *program;
    bool usable, translated;

    wl_nv2a_psh_textures(k, gx->entries, &textures);
    if (textures.fog) {
        wl_gx_ramp_bind(WL_XBOX_PSH_FOG_TEXMAP);
    }
    wl_nv2a_psh_words(k, words);
    program = wl_xbox_psh_cache_program(&gx->psh, words, wl_nv2a_k(k, WL_NV2A_K_SET_SHADER_STAGE_PROGRAM), &textures,
                                        &usable, &translated);
    gx->last_program = program;
    gx->last_used = usable;
    if (usable) {
        wl_xbox_psh_apply(program);
        gx->texcoords = program->texcoords;
        gx->channels = program->channels;
        return !program->approximate;
    }
    if (translated) {
        WL_LOG_W(TAG, "combiners beyond GX (%s): texture 0 times the diffuse colour",
                 program->reason != NULL ? program->reason : "?");
    }
    gx->stats.programs_unusable++;
    {
        WlTevProgram tev;

        wl_tev_recipe(&tev, gx->entries[0] != NULL ? WL_GX_RECIPE_TEX_MODULATE : WL_GX_RECIPE_VERTEX_COLOR);
        wl_tev_apply_unchecked(&tev);
    }
    gx->texcoords = gx->entries[0] != NULL ? 1u : 0u;
    gx->channels = 1u;
    return false;
}

// ---- vertices ------------------------------------------------------------------------------------

static uint8_t color_byte(float value) {
    return (uint8_t)(value <= 0.0f ? 0 : value >= 1.0f ? 255 : (int)(value * 255.0f + 0.5f));
}

static float as_depth(float z, float depth_max) {
    float depth = depth_max > 0.0f ? z / depth_max : z;

    if (!(depth >= 0.0f)) {
        return 0.0f;
    }
    return depth > 1.0f ? 1.0f : depth;
}

// the inputs a draw's program is run with for its transform's signature (every input register
// one of these), and whose w and depth widen its fit: points spread far through model space
static const float canonical_points[CANONICAL_POINTS][4] = {
    {0.0f, 0.0f, 0.0f, 1.0f}, {1024.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 1024.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1024.0f, 1.0f}};

// the depth a + b / w that goes through the points with the least and the greatest 1 / w
static bool fit_two(const float *inverse_w, const float *depth, unsigned n, float *a, float *b) {
    unsigned i, near = 0, far = 0;

    for (i = 1; i < n; i++) {
        near = inverse_w[i] < inverse_w[near] ? i : near;
        far = inverse_w[i] > inverse_w[far] ? i : far;
    }
    if (n < 2 || !(inverse_w[far] - inverse_w[near] > 1.0e-6f * inverse_w[far])) {
        return false;
    }
    *b = (depth[far] - depth[near]) / (inverse_w[far] - inverse_w[near]);
    *a = depth[near] - *b * inverse_w[near];
    return true;
}

static float fit_error(float a, float b, const float *inverse_w, const float *depth, unsigned n) {
    float worst = 0.0f;
    unsigned i;

    for (i = 0; i < n; i++) {
        float error = fabsf(a + b * inverse_w[i] - depth[i]);

        worst = error > worst || error != error ? error : worst;
    }
    return worst;
}

// The projection of a draw: a few of its vertices, for their w and depth (OpenCE-Wii's
// projection_fit). A program that captures its clip-space position (divides by w) is drawn through
// the perspective projection, its depth a + b / w; vertices all at w 1 are screen space.
//
// a and b come from vertices, and two draws of one camera give the same ones but for the rounding
// of their depths: enough for a pass drawn again over the same geometry with a depth test of
// LEQUAL or EQUAL and no depth write (a shadow or a light over a base pass) to fail on GX where it
// passes on the NV2A, in some frames and not others. So the fit is kept by the transform it was
// made for, and every draw of that transform uses it, the same numbers: the transform's signature
// is the clip-space positions the program gives four canonical inputs (every input register set
// to one point), compared bit for bit, which two draws share whatever their vertices when their
// programs compute the position alike from the same constants (two programs, Halo's ring and its
// shadow pass, among them). A fit is made, for a transform not seen, from its
// draw's vertices and the canonical points (the two farthest apart in 1 / w, for the steadiest
// fit), or from the vertices alone where the points stray from them (a program whose depth is not
// a + b / w everywhere). The points that count are those within the depth range (between the near
// and far planes: what GX draws), the draw's first 24 of them among its first 128 vertices; a draw
// with none uses its transform's fit as it is.
static Projection projection_fit(WlNv2aGx *gx, const WlNv2aKelvin *k, const WlNv2aDraw *draw, float depth_max) {
    Projection p = {false, gx->fit_a, gx->fit_b};
    float inputs[WL_NV2A_ATTRIBS][4], inverse_w[FIT_PROBES + CANONICAL_POINTS], depth[FIT_PROBES + CANONICAL_POINTS];
    uint32_t clip[CANONICAL_POINTS][4];
    uint32_t probes = draw->count < FIT_SCAN ? draw->count : FIT_SCAN, v, n = 0, c, i, slot = 0;
    struct FitSignature *set;
    float near_w = 0.0f, far_w = 0.0f, a, b, d;
    bool all_w1 = true, in_front = false;
    WlXboxVshOutput out;

    gx->last_shared = false;
    if (gx->pipeline.program == NULL) {
        return p;
    }
    for (v = 0; v < probes && n < FIT_PROBES; v++) {
        wl_nv2a_draw_fetch(draw, k, v, inputs);
        wl_nv2a_pipeline_vertex(&gx->pipeline, k, (const float (*)[4])inputs, WL_XBOX_VSH_LIVE_POSITION, &out);
        if (!out.clip_captured) {
            return p;
        }
        p.perspective = true;
        if (!(out.clip[3] > 1.0e-4f)) {
            continue;
        }
        in_front = true;
        all_w1 = all_w1 && fabsf(out.clip[3] - 1.0f) < 1.0e-4f;
        d = out.position[2] / depth_max;
        // (a vertex beyond the near or the far plane: GX clips there, its depth is never drawn, and
        // near the eye the depth runs steep, so it is no measure of a fit)
        if (!(d >= 0.0f && d <= 1.0f)) {
            continue;
        }
        inverse_w[n] = 1.0f / out.clip[3];
        depth[n] = d;
        near_w = n == 0 || inverse_w[n] < near_w ? inverse_w[n] : near_w;
        far_w = n == 0 || inverse_w[n] > far_w ? inverse_w[n] : far_w;
        n++;
        // (enough to tell a projection and fit one: a few vertices spread in depth)
        if (n >= FIT_ENOUGH && far_w - near_w > 1.0e-3f * far_w) {
            break;
        }
    }
    if (!p.perspective) {
        return p;
    }
    if (in_front && all_w1 && !(far_w - near_w > 1.0e-6f * far_w)) {
        // (screen space: every vertex at w 1)
        p.perspective = false;
        return p;
    }
    // the transform's signature, and the canonical points' w and depth
    c = n;
    for (i = 0; i < CANONICAL_POINTS; i++) {
        for (v = 0; v < WL_NV2A_ATTRIBS; v++) {
            memcpy(inputs[v], canonical_points[i], sizeof inputs[v]);
        }
        wl_nv2a_pipeline_vertex(&gx->pipeline, k, (const float (*)[4])inputs, WL_XBOX_VSH_LIVE_POSITION, &out);
        memcpy(clip[i], out.clip, sizeof clip[i]);
        d = out.position[2] / depth_max;
        if (out.clip[3] > 1.0e-4f && d >= 0.0f && d <= 1.0f) {
            inverse_w[c] = 1.0f / out.clip[3];
            depth[c] = d;
            c++;
        }
    }
    gx->signature_use++;
    set = gx->signatures[wl_fnv1a64(clip, sizeof clip) % FIT_SETS];
    for (i = 0; i < FIT_WAYS; i++) {
        if (set[i].used != 0 && memcmp(set[i].clip, clip, sizeof clip) == 0) {
            // (no vertex in front of the eye among those looked at: the transform's fit as it is)
            if (n == 0 || fit_error(set[i].a, set[i].b, inverse_w, depth, n) <= SIGNATURE_TOLERANCE) {
                set[i].used = gx->signature_use;
                p.a = gx->fit_a = set[i].a;
                p.b = gx->fit_b = set[i].b;
                gx->last_shared = true;
                gx->stats.fits_shared++;
                return p;
            }
            // (the same signature, another projection: made again)
            slot = i;
            break;
        }
        if (set[i].used < set[slot].used) {
            slot = i;
        }
    }
    // a fit for a transform not seen: through the vertices and the canonical points, unless they
    // stray from the vertices; else through the vertices alone (with none in front of the eye, the
    // canonical points alone; with neither, the last fit)
    if (!(fit_two(inverse_w, depth, c, &a, &b) && fit_error(a, b, inverse_w, depth, n) <= FIT_TOLERANCE) &&
        !fit_two(inverse_w, depth, n, &a, &b)) {
        return p;
    }
    gx->stats.fits_made++;
    memcpy(set[slot].clip, clip, sizeof clip);
    set[slot].a = a;
    set[slot].b = b;
    set[slot].used = gx->signature_use;
    p.a = gx->fit_a = a;
    p.b = gx->fit_b = b;
    return p;
}

// the polygon offset as OpenCE-Wii's projection_bias reads Direct3D's z bias: a negative bias in
// depth units pulls the draw toward the eye
static float projection_bias(const WlNv2aKelvin *k) {
    float offset;

    if (!wl_nv2a_k(k, WL_NV2A_K_SET_POLY_OFFSET_FILL_ENABLE)) {
        return 0.0f;
    }
    offset = wl_nv2a_kf(k, WL_NV2A_K_SET_POLYGON_OFFSET_BIAS);
    return offset < 0.0f ? -offset : 0.0f;
}

static void projection_load(WlNv2aGx *gx, const WlNv2aKelvin *k, const Projection *p) {
    float bias = projection_bias(k), m[4][4], w = (float)gx->view_w, h = (float)gx->view_h;

    if (gx->projection_loaded && gx->loaded.perspective == p->perspective && gx->loaded_bias == bias &&
        gx->loaded_w == gx->view_w && gx->loaded_h == gx->view_h &&
        (!p->perspective || (gx->loaded.a == p->a && gx->loaded.b == p->b))) {
        return;
    }
    memset(m, 0, sizeof m);
    if (p->perspective) {
        // x' = 2/W X + Z, y' = -2/H Y - Z, z' = (1 - a) Z + b, w' = -Z: (X, Y) = pixels times w, Z = -w,
        // with a and b scaled so the far plane clips nothing
        float scale = p->a * DEPTH_SCALE > 1.0f ? 1.0f / p->a : DEPTH_SCALE, a = p->a * scale, b = p->b * scale;

        m[0][0] = 2.0f / w;
        m[0][2] = 1.0f;
        m[1][1] = -2.0f / h;
        m[1][2] = -1.0f;
        m[2][2] = 1.0f - a;
        m[2][3] = b - fabsf(b) * bias * (1.0f / 16384.0f);
        m[3][2] = -1.0f;
    } else {
        m[0][0] = 2.0f / w;
        m[0][3] = -1.0f;
        m[1][1] = -2.0f / h;
        m[1][3] = 1.0f;
        // (the depth, z + 1, scaled as the perspective draws' are)
        m[2][2] = DEPTH_SCALE;
        m[2][3] = DEPTH_SCALE - 1.0f - bias * (1.0f / 16777216.0f);
        m[3][3] = 1.0f;
    }
    wl_gx_direct_projection((const float (*)[4])m, !p->perspective);
    gx->loaded = *p;
    gx->loaded_bias = bias;
    gx->loaded_w = gx->view_w;
    gx->loaded_h = gx->view_h;
    gx->projection_loaded = true;
}

typedef struct DrawPlan {
    unsigned live;
    unsigned stage_programs[WL_NV2A_TEXTURES];
    float sx, sy, depth_max;
    const float *scale, *offset;
} DrawPlan;

// a vertex as GX takes it (OpenCE-Wii's vertex_store), and where it lands in EFB pixels
static void vertex_make(WlNv2aGx *gx, const WlNv2aKelvin *k, const WlNv2aDraw *draw, uint32_t v, const DrawPlan *plan,
                        WlGxDirectVertex *out, float *px, float *py) {
    float inputs[WL_NV2A_ATTRIBS][4];
    WlXboxVshOutput o;
    unsigned stage;

    wl_nv2a_draw_fetch(draw, k, v, inputs);
    wl_nv2a_pipeline_vertex(&gx->pipeline, k, (const float (*)[4])inputs, plan->live, &o);
    if (gx->projection.perspective && o.clip_captured) {
        // (x w, y w, -w) in pixels, the pixel centres on integers as Direct3D puts them
        float w = o.clip[3];

        out->position[0] = (o.clip[0] * plan->scale[0] + plan->offset[0] * w) / plan->sx + 0.5f * w;
        out->position[1] = (o.clip[1] * plan->scale[1] + plan->offset[1] * w) / plan->sy + 0.5f * w;
        out->position[2] = -w;
        *px = w != 0.0f ? out->position[0] / w : 0.0f;
        *py = w != 0.0f ? out->position[1] / w : 0.0f;
    } else {
        out->position[0] = o.position[0] / plan->sx + 0.5f;
        out->position[1] = o.position[1] / plan->sy + 0.5f;
        out->position[2] = as_depth(o.position[2], plan->depth_max) - 1.0f;
        *px = out->position[0];
        *py = out->position[1];
    }
    if (gx->channels & 1u) {
        out->color[0][0] = color_byte(o.d0[0]);
        out->color[0][1] = color_byte(o.d0[1]);
        out->color[0][2] = color_byte(o.d0[2]);
        out->color[0][3] = color_byte(o.d0[3]);
    }
    if (gx->channels & 2u) {
        out->color[1][0] = color_byte(o.d1[0]);
        out->color[1][1] = color_byte(o.d1[1]);
        out->color[1][2] = color_byte(o.d1[2]);
        out->color[1][3] = color_byte(o.d1[3]);
    }
    for (stage = 0; stage < WL_NV2A_TEXTURES; stage++) {
        if (gx->texcoords & (1u << stage)) {
            wl_nv2a_texcoord(gx->entries[stage], plan->stage_programs[stage], o.t[stage], out->texcoord[stage]);
        }
    }
    if (gx->texcoords & (1u << WL_XBOX_PSH_FOG_TEXCOORD)) {
        out->texcoord[WL_XBOX_PSH_FOG_TEXCOORD][0] = wl_gx_ramp_coordinate(wl_nv2a_fog_factor(k, o.fog));
    }
}

static WlGxDirectPrim gx_primitive(uint32_t primitive) {
    switch (primitive) {
    case WL_NV2A_PRIM_POINTS:
        return WL_GX_DIRECT_POINTS;
    case WL_NV2A_PRIM_LINES:
        return WL_GX_DIRECT_LINES;
    case WL_NV2A_PRIM_LINE_LOOP:
    case WL_NV2A_PRIM_LINE_STRIP:
        return WL_GX_DIRECT_LINE_STRIP;
    case WL_NV2A_PRIM_TRIANGLE_STRIP:
    case WL_NV2A_PRIM_QUAD_STRIP:
        return WL_GX_DIRECT_TRIANGLE_STRIP;
    case WL_NV2A_PRIM_TRIANGLE_FAN:
    case WL_NV2A_PRIM_POLYGON:
        return WL_GX_DIRECT_TRIANGLE_FAN;
    case WL_NV2A_PRIM_QUADS:
        return WL_GX_DIRECT_QUADS;
    default:
        return WL_GX_DIRECT_TRIANGLES;
    }
}

static float area2(const float a[2], const float b[2], const float c[2]) {
    return (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);
}

static uint32_t backend_draw(void *user, const WlNv2aKelvin *k, const WlNv2aDraw *draw) {
    WlNv2aGx *gx = user;
    WlNv2aSurface surface;
    WlGxDirectLayout layout;
    DrawPlan plan;
    bool exact, count_pixels = wl_nv2a_k(k, WL_NV2A_K_SET_ZPASS_PIXEL_COUNT_ENABLE) != 0;
    float corners[3][2], area = 0.0f;
    uint32_t v, seen = 0;
    unsigned stage;

    if (wl_nv2a_k(k, WL_NV2A_K_SET_STENCIL_TEST_ENABLE)) {
        // GX has no stencil. A pass that writes only the stencil (no colour, no depth) draws
        // nothing GX can keep, and is skipped; any other is drawn as if its test passed, as
        // OpenCE-Wii's Direct3D device on GX does for the same game: skipping them lost about half
        // of Halo's a10 (its level geometry is drawn with the stencil test on)
        if (wl_nv2a_k(k, WL_NV2A_K_SET_COLOR_MASK) == 0 && wl_nv2a_k(k, WL_NV2A_K_SET_DEPTH_MASK) == 0) {
            gx->stats.stencil_skipped++;
            return 0;
        }
        gx->stats.stencil_ignored++;
    }
    if (gx->dev == NULL || !wl_nv2a_pipeline_begin(&gx->pipeline, k)) {
        return 0;
    }
    wl_nv2a_surface(k, &surface);
    if (!surface_prepare(gx, &surface)) {
        return 0;
    }
    gx->stats.draws++;
    gx->stats.vertices += draw->count;
    exact = states_apply(gx, k, &surface);
    textures_bind(gx, k);
    exact = tev_setup(gx, k) && exact;
    if (!exact) {
        gx->stats.approximations++;
    }

    memset(&layout, 0, sizeof layout);
    layout.colors = (gx->channels & 2u) ? 2u : (gx->channels & 1u) ? 1u : 0u;
    layout.texcoords = (uint8_t)(gx->texcoords & 0x1Fu);
    layout.texcoord_s = (uint8_t)(1u << WL_XBOX_PSH_FOG_TEXCOORD);
    wl_gx_direct_layout(&layout);

    memset(&plan, 0, sizeof plan);
    plan.live = WL_XBOX_VSH_LIVE_POSITION;
    if (layout.colors >= 1) {
        plan.live |= WL_XBOX_VSH_LIVE_D0;
    }
    if (layout.colors >= 2) {
        plan.live |= WL_XBOX_VSH_LIVE_D1;
    }
    for (stage = 0; stage < WL_NV2A_TEXTURES; stage++) {
        plan.stage_programs[stage] = wl_nv2a_stage_program(k, stage);
        if (gx->texcoords & (1u << stage)) {
            plan.live |= (unsigned)WL_XBOX_VSH_LIVE_T0 << stage;
        }
    }
    if (gx->texcoords & (1u << WL_XBOX_PSH_FOG_TEXCOORD)) {
        plan.live |= WL_XBOX_VSH_LIVE_FOG;
    }
    plan.sx = (float)surface.samples_x;
    plan.sy = (float)surface.samples_y;
    plan.depth_max = surface.depth_max > 0.0f ? surface.depth_max : 16777215.0f;
    plan.scale = k->constants[WL_NV2A_CONSTANT_VIEWPORT_SCALE];
    plan.offset = k->constants[WL_NV2A_CONSTANT_VIEWPORT_OFFSET];

    gx->projection = projection_fit(gx, k, draw, plan.depth_max);
    if (gx->projection.perspective) {
        gx->stats.perspective_draws++;
    }
    projection_load(gx, k, &gx->projection);

    wl_gx_direct_primitive_begin(gx_primitive(draw->primitive), draw->count);
    gx->memo_stamp++;
    for (v = 0; v < draw->count; v++) {
        WlGxDirectVertex vertex;
        float px, py;

        if (draw->source == WL_NV2A_SOURCE_ARRAYS && draw->indices != NULL) {
            uint32_t index = draw->indices[v], slot = index & (MEMO - 1u);

            if (gx->memo[slot].stamp == gx->memo_stamp && gx->memo[slot].index == index) {
                vertex = gx->memo[slot].vertex;
                px = gx->memo[slot].x;
                py = gx->memo[slot].y;
                gx->stats.vertices_reused++;
            } else {
                memset(&vertex, 0, sizeof vertex);
                vertex_make(gx, k, draw, v, &plan, &vertex, &px, &py);
                gx->memo[slot].stamp = gx->memo_stamp;
                gx->memo[slot].index = index;
                gx->memo[slot].vertex = vertex;
                gx->memo[slot].x = px;
                gx->memo[slot].y = py;
            }
        } else {
            memset(&vertex, 0, sizeof vertex);
            vertex_make(gx, k, draw, v, &plan, &vertex, &px, &py);
        }
        wl_gx_direct_vertex(&vertex);
        if (count_pixels) {
            // (the triangles' areas: what a pixel count without a depth test would say)
            float here[2] = {px, py};

            switch (draw->primitive) {
            case WL_NV2A_PRIM_TRIANGLES:
                memcpy(corners[seen % 3u], here, sizeof here);
                if (seen % 3u == 2u) {
                    area += fabsf(area2(corners[0], corners[1], corners[2]));
                }
                break;
            case WL_NV2A_PRIM_TRIANGLE_STRIP:
            case WL_NV2A_PRIM_QUAD_STRIP:
                memcpy(corners[seen % 3u], here, sizeof here);
                if (seen >= 2u) {
                    area += fabsf(area2(corners[0], corners[1], corners[2]));
                }
                break;
            case WL_NV2A_PRIM_TRIANGLE_FAN:
            case WL_NV2A_PRIM_POLYGON:
            case WL_NV2A_PRIM_QUADS:
                if (draw->primitive == WL_NV2A_PRIM_QUADS && seen % 4u == 0u) {
                    memcpy(corners[0], here, sizeof here);
                } else if (seen == 0u) {
                    memcpy(corners[0], here, sizeof here);
                } else {
                    if ((draw->primitive == WL_NV2A_PRIM_QUADS ? seen % 4u : seen) >= 2u) {
                        area += fabsf(area2(corners[0], corners[1], here));
                    }
                    memcpy(corners[1], here, sizeof here);
                }
                break;
            default:
                break;
            }
            seen++;
        }
    }
    wl_gx_direct_primitive_end();
    {
        float most = (float)gx->view_w * (float)gx->view_h, pixels = area * 0.5f;

        return (uint32_t)(pixels > most ? most : pixels);
    }
}

// ---- clears ---------------------------------------------------------------------------------------

static void backend_clear(void *user, const WlNv2aKelvin *k, uint32_t flags) {
    WlNv2aGx *gx = user;
    WlNv2aSurface s;
    uint32_t h = wl_nv2a_k(k, WL_NV2A_K_SET_CLEAR_RECT_HORIZONTAL);
    uint32_t v = wl_nv2a_k(k, WL_NV2A_K_SET_CLEAR_RECT_VERTICAL);
    bool colour = (flags & WL_NV2A_CLEAR_COLOR) != 0, depth = (flags & WL_NV2A_CLEAR_Z) != 0;
    uint32_t x0, x1, y0, y1, rgba;
    float z;

    wl_nv2a_surface(k, &s);
    depth = depth && s.zeta_format != 0;
    if (!colour && !depth) {
        return;
    }
    gx->stats.clears++;
    // the rectangle: inclusive edges, in samples
    x0 = (h & 0xFFFFu) / s.samples_x;
    x1 = ((h >> 16) + 1u) / s.samples_x;
    y0 = (v & 0xFFFFu) / s.samples_y;
    y1 = ((v >> 16) + 1u) / s.samples_y;
    rgba = wl_nv2a_clear_rgba(k, &s);
    z = wl_nv2a_clear_depth(k, &s);
    if (surface_is_screen(gx, &s) && !gx->frame_open && colour && depth && z >= 1.0f && x0 == 0 && y0 == 0 &&
        x1 >= gx->cfg.width && y1 >= gx->cfg.height) {
        // the frame's first clear, of all of it: GX's own (the copy that ends a frame clears the EFB)
        frame_begin(gx, rgba);
        gx->back_phys = s.color_phys;
        gx->back_drawn = true;
        wl_gx_target_drawn();
        return;
    }
    if (!surface_prepare(gx, &s)) {
        return;
    }
    if (x1 > gx->view_w) {
        x1 = gx->view_w;
    }
    if (y1 > gx->view_h) {
        y1 = gx->view_h;
    }
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    wl_gx_target_clear(colour, depth, rgba, z, (int)x0, (int)y0, (int)(x1 - x0), (int)(y1 - y0));
    gx->stats.quad_clears++;
    device_state(gx);
}

// ---- frames and flips -------------------------------------------------------------------------------

static void backend_frame_done(void *user, const WlNv2aKelvin *k) {
    WlNv2aGx *gx = user;

    (void)k;
    if (gx->frame_open && gx->back_drawn) {
        wl_nv2a_gx_present(gx);
    }
}

static void backend_flip(void *user, const WlNv2aKelvin *k, uint32_t phys) {
    WlNv2aGx *gx = user;

    (void)k;
    if (!is_display(gx, phys)) {
        gx->displays[gx->display_next] = phys;
        gx->display_next = (gx->display_next + 1u) % 4u;
    }
    if (gx->frame_open && gx->back_drawn && gx->back_phys == phys) {
        wl_nv2a_gx_present(gx);
    }
}

void wl_nv2a_gx_backend(WlNv2aGx *gx, WlNv2aBackend *out) {
    memset(out, 0, sizeof *out);
    out->user = gx;
    out->clear = backend_clear;
    out->draw = backend_draw;
    out->frame_done = backend_frame_done;
    out->flip = backend_flip;
}
