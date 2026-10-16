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
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wl/base/hash.h"
#include "wl/platform/gx_direct.h"
#include "wl/platform/gx_target.h"
#include "wl/platform/platform.h"
#include "wl/platform/tev.h"
#include "wl/xbox/nv2a_color.h"
#include "wl/xbox/nv2a_pipeline.h"
#include "wl/xbox/nv2a_texture.h"
#include "wl/xbox/psh_bias.h"
#include "wl/xbox/psh_cache.h"

#define TAG "nv2a"

enum {
    TARGETS = 8,
    // a render target neither drawn into nor read for this many frames gives its image back (the
    // host's heap: a 640x480 one is 1.2 MB, and one a cutscene made would be kept for good)
    TARGET_IDLE_FRAMES = 1800,
    // indexed draws' vertices kept by index (a direct-mapped table: a strip's or list's neighbours
    // come back soon)
    MEMO = 1024,
    // vertices in front of the eye looked at for the projection's fit, at most
    FIT_PROBES = 24,
    // ...among the draw's first ones (a strip may start behind the eye)
    FIT_SCAN = 128,
    // ...of which these, spread in depth, are enough
    FIT_ENOUGH = 6,
    // ...run through the program this many at a time
    FIT_BATCH = 8,
    // the fits kept by the transform they were made for (its signature): sets by the signature's
    // hash, ways in a set
    FIT_SETS = 128,
    FIT_WAYS = 2,
    // the inputs a draw's program is also run with for its transform's signature
    CANONICAL_POINTS = 4,
    // programs whose analyses are kept
    XF_ANALYSES = 8
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
    // the guest memory the surface takes, and the write watch's stamp over it since the image was
    // resolved (0: none): a store there (the memory used again for a texture the processor fills)
    // makes the guest's texels the texture's again
    uint32_t guest_bytes, stamp;
    WlGxTarget target;
} RenderTarget;

typedef struct Projection {
    bool perspective;
    float a, b;
} Projection;

// a vertex as GX takes it from the FIFO in the draw's layout (wl_gx_direct_vertex_words: the
// position's three floats, the colours as 0xRRGGBBAA, the texture coordinates' floats; at most
// 3 + 2 + 4 * 2 + 1 words, the fog's coordinate last), and where it lands in EFB pixels. (The words
// first: a pointer to them is one to the vertex.)
enum { PACKED_WORDS = 14 };
typedef struct PackedVertex {
    uint32_t w[PACKED_WORDS];
    float x, y;
} PackedVertex;

struct WlNv2aGx {
    WlNv2aGxConfig cfg;
    WlXboxPshRecipe recipe;
    WlNv2a *dev;
    WlNv2aPipeline pipeline;
    WlNv2aTexCache textures;
    WlNv2aVCache vertices;
    WlXboxPshCache psh;
    WlNv2aGxStats stats;
    // the last draw's pixel program, and whether it was used
    const WlXboxPshProgram *last_program;
    bool last_used;

    // the frame
    bool frame_open, back_drawn;
    uint32_t back_phys;
    // the depth buffer the back buffer was last drawn with: a colour surface there is a target
    // (Halo's blur of the picture draws into it, the picture read back as a texture)
    uint32_t back_zeta;
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
    // what each object was made from (bit n of objects_known: stage n's, bit n + 4: its alpha's)
    WlGxTexDesc stage_descs[WL_NV2A_TEXTURES], alpha_descs[WL_NV2A_TEXTURES];
    uint8_t objects_known;
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
    // (stamp: the draw; batch: the batch that last used the slot, which no other index may then
    // take until it is sent)
    struct WlNv2aGxMemo {
        uint32_t index, stamp, batch;
        PackedVertex vertex;
    } memo[MEMO];
    uint32_t memo_stamp, batch_stamp;
    // a batch of a draw's vertices (WL_XBOX_VSH_BATCH): each one's vertex (in the memo, or made
    // here), the program run for those not kept (their elements as the fetch takes them, where their
    // vertex goes, and its words, a fused batch's way), the runs' inputs and outputs
    struct {
        const uint32_t *words[WL_XBOX_VSH_BATCH]; // (their words: batch_vertex's vertex)
        uint32_t elements[WL_XBOX_VSH_BATCH];
        PackedVertex *run_vertices[WL_XBOX_VSH_BATCH];
        uint32_t *words_made[WL_XBOX_VSH_BATCH];
        PackedVertex made[WL_XBOX_VSH_BATCH];
        float inputs[WL_XBOX_VSH_BATCH][WL_NV2A_ATTRIBS][4];
        WlXboxVshOutput outputs[WL_XBOX_VSH_BATCH];
        WlXboxVshWork work;
    } batch;
    // a fused batch's state (the draw's)
    WlXboxVshFused fused;
    // the draw's fetch
    WlNv2aFetchPlan fetch;
    // the transform unit's state this backend left (the position matrix set; the coordinates with a
    // generator of their own), and the last program's verdict (by its compile count and outputs)
    bool xf_position;
    uint8_t xf_texgens;
    const WlXboxVshProgram *xf_program;
    uint32_t xf_compiled;
    unsigned xf_live;
    bool xf_eligible, xf_full;
    // why the last program's vertices are not all on the transform unit (xf_full false), and the
    // last draw's way (WL_NV2A_GX_XF_WHYS: none, a draw left before its transform was chosen)
    uint8_t xf_partial, xf_why;
    WlXboxVshDependencies xf_dependencies;
    // the last programs' analyses (wl_xbox_vsh_analyze), by program and its serial (another compiled
    // in its place is another): a frame's draws take turns among a few programs, and an analysis
    // costs thousands of instructions
    struct XfAnalysis {
        const WlXboxVshProgram *program;
        uint32_t serial;
        uint32_t used;
        WlXboxVshDependencies dependencies;
    } xf_analyses[XF_ANALYSES];
    uint32_t xf_analysis_use;
    // the state the last applied draw left in GX (state_valid; device_state ends it), what has
    // changed since (the tracker's WL_NV2A_DIRTY_* of every draw since, applied or not), whether
    // its raster state and pixel program were exact, and whether its textures must be bound again
    // anyway (one found changed within a frame, checked at every use, or one not bound)
    bool state_valid;
    uint32_t dirty;
    bool states_exact, tev_exact, textures_volatile;
    // what of the bound textures the last pixel program was made from (texture_pixel_inputs), so
    // that a draw whose combiners, fog and these are as they were keeps it without a lookup
    uint32_t pixel_textures[WL_NV2A_TEXTURES];
};

// ---- creation -----------------------------------------------------------------------------------

static void commit(void *user, void *memory, size_t bytes) {
    (void)user;
    wl_gpu_buffer_commit(memory, bytes);
}

static void gpu_wait(void) {
    (void)wl_gx_sync();
}

WlNv2aGx *wl_nv2a_gx_create(const WlNv2aGxConfig *cfg) {
    WlNv2aGx *gx = calloc(1, sizeof *gx);
    WlNv2aTexCacheConfig textures;
    WlNv2aVCacheConfig vertices;

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
    // (a light's falloff cube as its lit face, wl_nv2a_spot_coordinates, not a sphere map)
    textures.spots = true;
    textures.gpu_lag = gx->cfg.overlap ? 1u : 0u;
    if (textures.flush == NULL) {
        textures.flush = commit;
    }
    if (textures.gpu_wait == NULL) {
        textures.gpu_wait = gpu_wait;
    }
    wl_nv2a_texcache_init(&gx->textures, &textures);
    vertices = gx->cfg.vertices;
    vertices.gpu_lag = textures.gpu_lag;
    if (vertices.flush == NULL) {
        vertices.flush = commit;
    }
    wl_nv2a_vcache_init(&gx->vertices, &vertices);
    wl_nv2a_pipeline_init(&gx->pipeline, gx->cfg.natives, gx->cfg.native_count);
    gx->pipeline.decoded = gx->cfg.program_decoded;
    gx->pipeline.decoded_user = gx->cfg.program_user;
    wl_xbox_psh_cache_init(&gx->psh, gx->cfg.recipe != NULL ? &gx->recipe : NULL);
    wl_gx_set_efb_alpha(!gx->cfg.no_efb_alpha);
    wl_gx_target_set_back_buffer_size(gx->cfg.width, gx->cfg.height);
    // (the back buffer's depth parked only for a target drawn with depth writes: states_apply says
    // so; Halo's shadows and their blur have no depth buffer, and cost one EFB copy fewer each)
    wl_gx_target_set_lazy_depth(true);
    wl_gx_set_frame_overlap(gx->cfg.overlap);
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
    // (GX done with what it reads before it is given back)
    (void)wl_gx_sync();
    wl_gx_set_frame_overlap(false);
    wl_nv2a_texcache_clear(&gx->textures);
    wl_nv2a_vcache_clear(&gx->vertices);
    for (i = 0; i < TARGETS; i++) {
        if (gx->targets[i].memory != NULL) {
            gx->textures.cfg.release(gx->textures.cfg.user, gx->targets[i].memory);
        }
    }
    free(gx);
}

void wl_nv2a_gx_host_bytes(const WlNv2aGx *gx, size_t *record, size_t *targets, unsigned *target_count) {
    unsigned i;

    *record = sizeof *gx;
    *targets = 0;
    *target_count = 0;
    for (i = 0; i < TARGETS; i++) {
        if (gx->targets[i].memory != NULL) {
            *targets += gx->targets[i].bytes;
            ++*target_count;
        }
    }
}

void wl_nv2a_gx_attach(WlNv2aGx *gx, WlNv2a *dev) {
    gx->dev = dev;
}

void wl_nv2a_gx_stats(const WlNv2aGx *gx, WlNv2aGxStats *out) {
    *out = gx->stats;
}

void wl_nv2a_gx_psh_counts(const WlNv2aGx *gx, WlXboxPshCacheCounts *out) {
    wl_xbox_psh_cache_counts(&gx->psh, out);
}

const WlNv2aTexCache *wl_nv2a_gx_textures(const WlNv2aGx *gx) {
    return &gx->textures;
}

const WlNv2aVCache *wl_nv2a_gx_vertex_cache(const WlNv2aGx *gx) {
    return &gx->vertices;
}

unsigned wl_nv2a_gx_last_xf(const WlNv2aGx *gx) {
    return gx->xf_why;
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
    gx->state_valid = false;
    gx->projection_loaded = false;
    gx->xf_position = false;
    gx->xf_texgens = 0;
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

// a target's image made (resolved): the write watch armed over its guest memory
static void target_image_made(WlNv2aGx *gx, RenderTarget *r) {
    const WlNv2aTexCacheConfig *c = &gx->textures.cfg;

    r->resolved = true;
    // (its memory has been copied into: the processor's cache holds none of it, the copies leave it alone)
    r->target.resident = true;
    r->stamp = c->watch != NULL && r->guest_bytes != 0 ? c->watch(c->user, r->phys, r->guest_bytes)
                                                        : 0u;
}

static void target_resolved(WlNv2aGx *gx, const WlGxTarget *resolved) {
    unsigned i;

    if (resolved == NULL) {
        return;
    }
    for (i = 0; i < TARGETS; i++) {
        if (&gx->targets[i] == (const RenderTarget *)resolved->id) {
            target_image_made(gx, &gx->targets[i]);
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

// the render targets not used for TARGET_IDLE_FRAMES give their images back (the GPU has finished
// with them: they were last drawn or read frames ago)
static void targets_idle(WlNv2aGx *gx) {
    unsigned i;

    for (i = 0; i < TARGETS; i++) {
        RenderTarget *r = &gx->targets[i];

        if (r->memory != NULL && r != gx->owner && gx->frame - r->used_frame > TARGET_IDLE_FRAMES) {
            gx->textures.cfg.release(gx->textures.cfg.user, r->memory);
            memset(r, 0, sizeof *r);
        }
    }
}

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
    targets_idle(gx);
    // (the GPU has drawn the frame: the textures evicted during it can go)
    wl_nv2a_texcache_frame_done(&gx->textures);
    wl_nv2a_vcache_frame_done(&gx->vertices);
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
// texture, so it is a render target; nor one at the back buffer's depth buffer, whose memory a
// title borrows for a picture of the screen's size between two passes of its own)
static bool surface_is_screen(const WlNv2aGx *gx, const WlNv2aSurface *s) {
    uint32_t w = s->clip_width / s->samples_x, h = s->clip_height / s->samples_y;

    if (s->color_format == 0 || is_display(gx, s->color_phys)) {
        return true;
    }
    if (s->color_phys != 0 && s->color_phys == gx->back_zeta) {
        return false;
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
        if (slot->used_frame + gx->textures.cfg.gpu_lag >= gx->frame) {
            // (GX may still copy into it or read it: done first, for the memory may be another
            // image's at once)
            (void)wl_gx_sync();
        }
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
    RenderTarget *t;

    if (s->swizzled) {
        w = 1u << s->log2_width;
        h = 1u << s->log2_height;
    }
    w = (w + 3u) & ~3u;
    h = (h + 3u) & ~3u;
    if (w == 0 || h == 0 || w > 1024u || h > 1024u) {
        return NULL;
    }
    t = slot_for(gx, s->color_phys, w, h, format);
    if (t != NULL) {
        // (its rows in guest memory: a swizzled surface's texels, a linear one's pitch)
        t->guest_bytes = s->swizzled ? (1u << s->log2_width) * (1u << s->log2_height) * s->color_bytes
                                     : s->color_pitch * s->clip_height;
    }
    return t;
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
    if (t->target.resident) {
        wl_gx_copy_efb_texture_again(0, 0, (int)w, (int)h, t->memory, WL_GX_TEX_RGBA8, false);
    } else {
        wl_gx_copy_efb_texture(0, 0, (int)w, (int)h, t->memory, WL_GX_TEX_RGBA8, false);
    }
    t->guest_bytes = w * h * 4u;
    target_image_made(gx, t);
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
        if (s->zeta_format != 0 && s->zeta_phys != 0 && s->zeta_phys != s->color_phys) {
            gx->back_zeta = s->zeta_phys;
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
    if (depth_test && wl_nv2a_k(k, WL_NV2A_K_SET_DEPTH_MASK) != 0 && gx->owner != NULL) {
        wl_gx_target_depth_write();
    }
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
    WlGxDirectLayout layout = {1, 0, 0, 0, 0, 0, 0};
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

// A clear of some of the colour's channels (WL_NV2A_CLEAR_R..A in `flags`), the rectangle [x0, x1) by
// [y0, y1) of the EFB's owner: a quad of the clear's colour with GX's colour and alpha updates as the
// channels have them (wl_gx_target_clear writes all four). Halo clears the alpha alone before its
// fog pass writes the fog's amount there; a clear of the colour as well left the picture black and
// the fog's colour over all of it. GX updates red, green and blue together: any of them, all three.
static void clear_channels(WlNv2aGx *gx, uint32_t flags, uint32_t rgba, uint32_t x0, uint32_t y0, uint32_t x1,
                           uint32_t y1) {
    const float corners[4][2] = {{(float)x0, (float)y0}, {(float)x1, (float)y0}, {(float)x0, (float)y1},
                                 {(float)x1, (float)y1}};
    static const uint8_t order[6] = {0, 1, 2, 2, 1, 3};
    WlGxDirectLayout layout = {1, 0, 0, 0, 0, 0, 0};
    WlGxDirectVertex vertices[6];
    WlTevProgram tev;
    float m[4][4];
    unsigned v;

    device_state(gx);
    memset(m, 0, sizeof m);
    m[0][0] = 2.0f / (float)gx->cfg.width;
    m[0][3] = -1.0f;
    m[1][1] = -2.0f / (float)gx->cfg.height;
    m[1][3] = 1.0f;
    m[2][2] = 1.0f;
    m[3][3] = 1.0f;
    wl_gx_direct_projection((const float (*)[4])m, true);
    wl_gx_direct_viewport(0.0f, 0.0f, (float)gx->cfg.width, (float)gx->cfg.height);
    wl_gx_direct_scissor(0, 0, gx->view_w, gx->view_h);
    wl_gx_direct_blend(WL_GX_BM_NONE, WL_GX_BL_ONE, WL_GX_BL_ZERO);
    wl_gx_direct_alpha_compare(WL_GX_ALWAYS, 0);
    wl_gx_direct_zmode(false, WL_GX_ALWAYS, false);
    wl_gx_direct_cull(WL_GX_CULL_NONE);
    wl_gx_direct_color_update((flags & (WL_NV2A_CLEAR_R | WL_NV2A_CLEAR_G | WL_NV2A_CLEAR_B)) != 0,
                              (flags & WL_NV2A_CLEAR_A) != 0);
    wl_tev_recipe(&tev, WL_GX_RECIPE_VERTEX_COLOR);
    wl_tev_apply_unchecked(&tev);
    wl_gx_direct_layout(&layout);
    memset(vertices, 0, sizeof vertices);
    for (v = 0; v < 6; v++) {
        vertices[v].position[0] = corners[order[v]][0];
        vertices[v].position[1] = corners[order[v]][1];
        vertices[v].position[2] = -0.5f;
        vertices[v].color[0][0] = (uint8_t)(rgba >> 24);
        vertices[v].color[0][1] = (uint8_t)(rgba >> 16);
        vertices[v].color[0][2] = (uint8_t)(rgba >> 8);
        vertices[v].color[0][3] = (uint8_t)rgba;
    }
    wl_gx_direct_draw(WL_GX_DIRECT_TRIANGLES, vertices, 6);
    gx->stats.quad_clears++;
    device_state(gx);
}

// ---- textures -------------------------------------------------------------------------------------

static uint8_t gx_wrap(uint8_t wrap) {
    switch (wrap) {
    case WL_NV2A_WRAP_REPEAT:
        return WL_GX_WRAP_REPEAT;
    case WL_NV2A_WRAP_MIRROR:
        return WL_GX_WRAP_MIRROR;
    // (border too: GX has no border colour, and the textures Halo addresses so have edges at or near
    // theirs already, its object shadows' targets cleared to it among them)
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

// What a texture read from a render target's image (in `format`, a WlGxTexFormat) holds: its alpha
// where the texture's format has one and the image kept it (the EFB's), else its colour alone (an
// X8R8G8B8 read gives 1 as its alpha on the NV2A, not what the EFB holds)
static uint8_t target_contents(const WlNv2aTexture *t, uint8_t format) {
    switch (t->color) {
    case WL_NV2A_TEX_SZ_A8R8G8B8:
    case WL_NV2A_TEX_LU_A8R8G8B8:
    case WL_NV2A_TEX_SZ_A8B8G8R8:
    case WL_NV2A_TEX_LU_A8B8G8R8:
    case WL_NV2A_TEX_SZ_B8G8R8A8:
    case WL_NV2A_TEX_LU_B8G8R8A8:
    case WL_NV2A_TEX_SZ_R8G8B8A8:
    case WL_NV2A_TEX_LU_R8G8B8A8:
    case WL_NV2A_TEX_SZ_A1R5G5B5:
    case WL_NV2A_TEX_LU_A1R5G5B5:
    case WL_NV2A_TEX_SZ_A4R4G4B4:
    case WL_NV2A_TEX_LU_A4R4G4B4:
        return format == WL_GX_TEX_RGBA8 ? (uint8_t)WL_XBOX_PSH_TEXTURE_ALL
                                         : (uint8_t)WL_XBOX_PSH_TEXTURE_COLOR;
    default:
        return WL_XBOX_PSH_TEXTURE_COLOR;
    }
}

// whether a texture reads a target's image as it is: texels of the image's size (4 bytes for its
// RGBA8, 2 for its RGB565), within its width and height
static bool target_fits(const RenderTarget *r, const WlNv2aTexture *t) {
    uint32_t bytes = r->format == WL_GX_TEX_RGB565 ? 2u : 4u;

    return t->texel_bytes == bytes && t->width <= r->width && t->height <= r->height;
}

// whether the guest stored to a target's memory since its image was made (the write watch's; with
// no watch, never): its image is then not the texture's (the memory holds a texture the processor
// filled), until the target is drawn again
static bool target_written(WlNv2aGx *gx, RenderTarget *r) {
    const WlNv2aTexCacheConfig *c = &gx->textures.cfg;

    if (r->stamp == 0 || c->written == NULL ||
        !c->written(c->user, r->phys, r->guest_bytes, r->stamp)) {
        return false;
    }
    r->resolved = false;
    r->stamp = 0;
    gx->stats.targets_written++;
    return true;
}

// a resolved render target at a texture's address, as an entry the stages read like any texture;
// or the back buffer of the frame being drawn, while a target has the EFB: its colour as it was
// parked, the picture so far (Halo's blur reads it into a target of its size, and the next pass
// back), where a kept frame would be one shown before
static const WlNv2aTexEntry *target_texture(WlNv2aGx *gx, unsigned stage, const WlNv2aTexture *t) {
    unsigned i;

    if (gx->owner != NULL && gx->frame_open && gx->back_drawn && t->phys == gx->back_phys &&
        t->phys != 0) {
        uint32_t w = 0, h = 0;
        const void *parked = wl_gx_target_parked_colour(&w, &h);

        if (parked != NULL && w >= (gx->cfg.width & ~3u) && h >= (gx->cfg.height & ~3u)) {
            WlNv2aTexEntry *e = &gx->target_entries[stage];

            memset(e, 0, sizeof *e);
            e->phys = t->phys;
            e->image = (void *)(uintptr_t)parked;
            e->layout.format = (WlGxcFormat)WL_GX_TEX_RGBA8;
            e->layout.width = w;
            e->layout.height = h;
            e->layout.levels = 1;
            e->layout.image_bytes = wl_gx_tex_bytes(WL_GX_TEX_RGBA8, w, h);
            e->layout.total_bytes = e->layout.image_bytes;
            e->contents = target_contents(t, WL_GX_TEX_RGBA8);
            e->linear = t->linear;
            e->width = (uint16_t)t->width;
            e->height = (uint16_t)t->height;
            e->serial = 0x80000000u | TARGETS;
            // (parked anew at each change of the EFB's owner: GX's texture cache may hold the
            // last park)
            gx->texture_memory_changed = true;
            return e;
        }
    }
    for (i = 0; i < TARGETS; i++) {
        RenderTarget *r = &gx->targets[i];

        if (r->memory != NULL && r->resolved && r->phys == t->phys && r != gx->owner &&
            target_fits(r, t) && !target_written(gx, r)) {
            WlNv2aTexEntry *e = &gx->target_entries[stage];

            r->used_frame = gx->frame; // (read: in use, targets_idle)
            memset(e, 0, sizeof *e);
            e->phys = r->phys;
            e->image = r->memory;
            e->layout.format = (WlGxcFormat)r->format;
            e->layout.width = r->width;
            e->layout.height = r->height;
            e->layout.levels = 1;
            e->layout.image_bytes = r->bytes;
            e->layout.total_bytes = r->bytes;
            e->contents = target_contents(t, r->format);
            e->linear = t->linear;
            e->width = (uint16_t)t->width;
            e->height = (uint16_t)t->height;
            e->serial = 0x80000000u | i;
            return e;
        }
    }
    return NULL;
}

// The texture object a description makes, in `obj`: the one made from the same description before
// kept (bit `known` of objects_known), else made again. false: it cannot be made.
static bool texture_object(WlNv2aGx *gx, WlGxTexObj *obj, WlGxTexDesc *made_from, unsigned known,
                           const WlGxTexDesc *desc) {
    if ((gx->objects_known & (1u << known)) != 0 && memcmp(made_from, desc, sizeof *desc) == 0) {
        gx->stats.texture_objects_kept++;
        return true;
    }
    gx->objects_known &= (uint8_t)~(1u << known);
    if (wl_gx_tex_init_ex(obj, desc) != WL_OK) {
        return false;
    }
    memcpy(made_from, desc, sizeof *desc); // (with its padding, which the compare reads)
    gx->objects_known |= (uint8_t)(1u << known);
    return true;
}

static void textures_bind(WlNv2aGx *gx, const WlNv2aKelvin *k) {
    unsigned stage, wanted = 0;

    for (stage = 0; stage < WL_NV2A_TEXTURES; stage++) {
        WlNv2aTexture t;
        const WlNv2aTexEntry *e;
        WlGxTexDesc desc;
        bool made = false, mipmapped;

        gx->entries[stage] = NULL;
        if (wl_nv2a_stage_program(k, stage) == WL_NV2A_STAGE_NONE || !wl_nv2a_texture(k, stage, &t)) {
            continue;
        }
        wanted |= 1u << stage;
        if (t.wrap_u == WL_NV2A_WRAP_BORDER || t.wrap_v == WL_NV2A_WRAP_BORDER) {
            gx->stats.border_clamped++;
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
        gx->stats.texture_stages++;
        if (!texture_object(gx, &gx->stage_objects[stage], &gx->stage_descs[stage], stage, &desc)) {
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
            if (texture_object(gx, &gx->alpha_objects[stage], &gx->alpha_descs[stage], stage + 4u, &desc)) {
                wl_gx_direct_texture(stage + 4u, &gx->alpha_objects[stage], NULL, NULL);
            }
        }
        gx->entries[stage] = e;
    }
    gx->textures_volatile = false;
    for (stage = 0; stage < WL_NV2A_TEXTURES; stage++) {
        if ((wanted & (1u << stage)) && (gx->entries[stage] == NULL || gx->entries[stage]->dynamic)) {
            gx->textures_volatile = true;
        }
    }
    if (gx->texture_memory_changed) {
        wl_gx_direct_texture_invalidate();
        gx->texture_memory_changed = false;
    }
}

// ---- the pixel stage ------------------------------------------------------------------------------

// what of a bound texture the pixel program reads (wl_nv2a_psh_textures: there or not, a sphere's,
// its contents, its alpha apart; tev_setup's fallback: there or not), as one word
static uint32_t texture_pixel_inputs(const WlNv2aTexEntry *e) {
    if (e == NULL) {
        return 0;
    }
    return 1u | (uint32_t)e->contents << 1 | (e->sphere ? 1u << 9 : 0u) |
           (e->layout.separate_alpha ? 1u << 10 : 0u);
}

// whether the bound textures read as the last pixel program's did; they are then its
static bool pixel_textures_same(WlNv2aGx *gx) {
    bool same = true;
    unsigned stage;

    for (stage = 0; stage < WL_NV2A_TEXTURES; stage++) {
        uint32_t inputs = texture_pixel_inputs(gx->entries[stage]);

        same = same && inputs == gx->pixel_textures[stage];
        gx->pixel_textures[stage] = inputs;
    }
    return same;
}

static bool tev_setup(WlNv2aGx *gx, const WlNv2aKelvin *k) {
    WlXboxPshTextures textures;
    uint32_t words[WL_XBOX_PSH_WORDS], unbiased[WL_XBOX_PSH_WORDS];
    const WlXboxPshProgram *program;
    bool usable, translated;

    wl_nv2a_psh_textures(k, gx->entries, &textures);
    if (textures.fog) {
        wl_gx_ramp_bind(WL_XBOX_PSH_FOG_TEXMAP);
    }
    wl_nv2a_psh_words(k, words);
    // (signed sums that come back to [0, 1] made without registers below 0, which GX's lose)
    if (wl_xbox_psh_unbias(words, unbiased)) {
        memcpy(words, unbiased, sizeof words);
        gx->stats.programs_unbiased++;
    }
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
    float inverse_w[FIT_PROBES + CANONICAL_POINTS], depth[FIT_PROBES + CANONICAL_POINTS];
    uint32_t clip[CANONICAL_POINTS][4];
    uint32_t probes = draw->count < FIT_SCAN ? draw->count : FIT_SCAN, v, n = 0, c, i, slot = 0, attributes, run;
    struct FitSignature *set;
    float near_w = 0.0f, far_w = 0.0f, a, b, d;
    bool all_w1 = true, in_front = false, enough = false;
    WlXboxVshOutput *outputs = gx->batch.outputs;
    WlNv2aFetchPlan fetch;
    uint32_t numbers[FIT_BATCH];

    gx->last_shared = false;
    // (a program that never converts its position to the screen leaves no clip-space position)
    if (gx->pipeline.program == NULL || !gx->pipeline.program->captures_clip) {
        return p;
    }
    // the vertices' positions, FIT_BATCH at a time (the program run for them together)
    attributes = wl_nv2a_pipeline_inputs(&gx->pipeline, WL_XBOX_VSH_LIVE_POSITION);
    // (the fetch decided once for the draw, as the draw's own: wl_nv2a_draw_fetch_some's values)
    wl_nv2a_fetch_plan(draw, attributes, &fetch);
    wl_nv2a_fetch_constants(&fetch, k, gx->batch.inputs, FIT_BATCH);
    for (v = 0; v < probes && n < FIT_PROBES && !enough; v += run) {
        run = probes - v < FIT_BATCH ? probes - v : FIT_BATCH;
        for (i = 0; i < run; i++) {
            numbers[i] = v + i;
        }
        wl_nv2a_fetch_vertices(&fetch, draw, numbers, run, gx->batch.inputs);
        wl_nv2a_pipeline_vertices(&gx->pipeline, k, (const float (*)[WL_NV2A_ATTRIBS][4])gx->batch.inputs, run,
                                  WL_XBOX_VSH_LIVE_POSITION, outputs, &gx->batch.work);
        for (i = 0; i < run && n < FIT_PROBES; i++) {
            const WlXboxVshOutput *out = &outputs[i];

            if (!out->clip_captured) {
                return p;
            }
            p.perspective = true;
            if (!(out->clip[3] > 1.0e-4f)) {
                continue;
            }
            in_front = true;
            all_w1 = all_w1 && fabsf(out->clip[3] - 1.0f) < 1.0e-4f;
            d = out->position[2] / depth_max;
            // (a vertex beyond the near or the far plane: GX clips there, its depth is never drawn, and
            // near the eye the depth runs steep, so it is no measure of a fit)
            if (!(d >= 0.0f && d <= 1.0f)) {
                continue;
            }
            inverse_w[n] = 1.0f / out->clip[3];
            depth[n] = d;
            near_w = n == 0 || inverse_w[n] < near_w ? inverse_w[n] : near_w;
            far_w = n == 0 || inverse_w[n] > far_w ? inverse_w[n] : far_w;
            n++;
            // (enough to tell a projection and fit one: a few vertices spread in depth)
            if (n >= FIT_ENOUGH && far_w - near_w > 1.0e-3f * far_w) {
                enough = true;
                break;
            }
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
    // the transform's signature, and the canonical points' w and depth (each point in the input
    // registers the position reads, as the draw's fetch fills only those)
    for (i = 0; i < CANONICAL_POINTS; i++) {
        for (v = 0; v < WL_NV2A_ATTRIBS; v++) {
            if (attributes & (1u << v)) {
                memcpy(gx->batch.inputs[i][v], canonical_points[i], sizeof gx->batch.inputs[i][v]);
            }
        }
    }
    wl_nv2a_pipeline_vertices(&gx->pipeline, k, (const float (*)[WL_NV2A_ATTRIBS][4])gx->batch.inputs,
                              CANONICAL_POINTS, WL_XBOX_VSH_LIVE_POSITION, outputs, &gx->batch.work);
    c = n;
    for (i = 0; i < CANONICAL_POINTS; i++) {
        const WlXboxVshOutput *out = &outputs[i];

        memcpy(clip[i], out->clip, sizeof clip[i]);
        d = out->position[2] / depth_max;
        if (out->clip[3] > 1.0e-4f && d >= 0.0f && d <= 1.0f) {
            inverse_w[c] = 1.0f / out->clip[3];
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

// a stage's texture coordinate as wl_nv2a_texcoord makes it, its texture's facts read once a draw
typedef struct StagePlan {
    const WlNv2aTexEntry *entry;
    bool sphere, projective, linear;
    // a cube map made a light's spot (entry->layout.spot: its face), not a sphere map
    uint8_t spot;
    float to_s, to_t; // (a linear texture's texels to a fraction: 1 / its width, height)
} StagePlan;

typedef struct DrawPlan {
    unsigned live;
    // (to_sx, to_sy: 1 / sx and 1 / sy when sx and sy are powers of two, where a product is the
    // quotient to the bit; 0: divided)
    float sx, sy, to_sx, to_sy, depth_max;
    // where the vertices land is wanted (the pixel count's areas)
    bool landing;
    const float *scale, *offset;
    // the layout's colours (0..2), the colours the program gives (gx->channels), the texture
    // coordinates sent (bit n: stage n; the fog's), and a vertex's words
    unsigned colors, channels, texcoords, words;
    StagePlan stages[WL_NV2A_TEXTURES];
    // the stages whose coordinates are sent, in order; of them, the projective ones whose q varies
    // from vertex to vertex, sent as (s, t, q) for GX to divide at each pixel (after the position:
    // three words for one, nine for two or three), the others divided here
    unsigned stage_count, stage_list[WL_NV2A_TEXTURES];
    unsigned projected_count, projected_list[WL_GX_DIRECT_PROJECTED];
    // the transform unit's draw (NULL: the processor's)
    const struct XfPlan *xf;
    // the program's fused form for the draw (fused_known: decided; NULL: none)
    bool fused_known;
    const WlXboxVshFusedForm *fused;
} DrawPlan;

// ---- the transform unit's draws ----------------------------------------------------------------------
// A program whose outputs the draw reads are, for the draw's constants, affine functions of their
// inputs leaves its work to GX's transform unit: the position through a matrix (with the viewport and
// the samples folded in, so the eye vector is what vertices_pack makes, for the same projection), each
// texture coordinate through a generator from its one input (the vertex's coordinate sent as fetched,
// or its position), the colours a vertex input passed as it is, or a constant. The matrices come from
// the program run for five probe vertices (every input register (0, 0, 0, 0), (0, 0, 0, 1), (1, 0, 0,
// 1), (0, 1, 0, 1), (0, 0, 1, 1)): an output affine in its register r is then known everywhere r's w
// is 1. Not to the bit: the transform unit's arithmetic is its own.

enum { XF_PROBES = 8, XF_MIN_VERTICES = 1 };

typedef struct XfPlan {
    // every live output the transform unit's (the program not run), or the position alone (the
    // program run for the others, which are sent as the processor makes them)
    bool full;
    float position[3][4];
    unsigned attributes; // the input registers fetched (bit n: v[n])
    // each colour: the input register passed as it is (-1: a constant, word)
    int color_source[2];
    uint32_t color_word[2];
    // each stage: its source register (WL_XBOX_VSH_INPUT_COUNT: the position, generated) and matrix
    unsigned stage_source[WL_NV2A_TEXTURES];
    float stage_matrix[WL_NV2A_TEXTURES][2][4];
    unsigned words;
    // the channels lit by GX from a normal (WlGxDirectLayout.lit: a colour affine in one input but
    // not passed as it is), the input register the normal is, and the light
    unsigned lit;
    unsigned normal_source;
    WlGxDirectLighting lighting;
} XfPlan;

static void stage_plan(StagePlan *s, const WlNv2aTexEntry *entry, unsigned stage_program) {
    memset(s, 0, sizeof *s);
    s->entry = entry;
    s->sphere = entry != NULL && entry->sphere;
    s->spot = s->sphere ? entry->layout.spot : 0;
    s->projective = stage_program == WL_NV2A_STAGE_2D_PROJECTIVE || stage_program == WL_NV2A_STAGE_3D_PROJECTIVE;
    s->linear = entry != NULL && entry->linear && entry->width != 0 && entry->height != 0;
    if (s->linear) {
        s->to_s = 1.0f / (float)entry->width;
        s->to_t = 1.0f / (float)entry->height;
    }
}

// a projective stage's coordinate for GX to divide at each pixel: (s, t, q) undivided, a linear
// texture's s and t to a fraction (the scale and the division commute)
static inline void stage_texcoord_projected(const StagePlan *s, const float t[4], float stq[3]) {
    if (s->spot) {
        // a spot's face as wl_nv2a_spot_coordinates reads it, (sc / ma + 1) / 2, undivided: ((sc + ma) /
        // 2, (tc + ma) / 2, ma). Off the face's side of the light, -1 (its dark edge) at a small q, so
        // a primitive that reaches behind darkens toward it
        float x = t[0], y = t[1], z = t[2], ma, sc, tc;

        switch (s->spot) {
        case 1: ma = x, sc = -z, tc = -y; break;
        case 2: ma = -x, sc = z, tc = -y; break;
        case 3: ma = y, sc = x, tc = z; break;
        case 4: ma = -y, sc = x, tc = -z; break;
        case 5: ma = z, sc = x, tc = -y; break;
        default: ma = -z, sc = -x, tc = -y; break;
        }
        if (!(ma > 1.0e-6f)) {
            float q = 1.0e-3f * (fabsf(x) + fabsf(y) + fabsf(z)) + 1.0e-12f;

            stq[0] = stq[1] = -q;
            stq[2] = q;
            return;
        }
        stq[0] = (sc + ma) * 0.5f;
        stq[1] = (tc + ma) * 0.5f;
        stq[2] = ma;
        return;
    }
    stq[0] = s->linear ? t[0] * s->to_s : t[0];
    stq[1] = s->linear ? t[1] * s->to_t : t[1];
    stq[2] = t[3];
}

static inline void stage_texcoord(const StagePlan *s, const float t[4], float st[2]) {
    if (s->sphere) {
        // (through a copy: `st` itself never goes to a call, so the caller keeps it in registers)
        float made[2];

        if (s->spot) {
            wl_nv2a_spot_coordinates(t, s->spot, made);
        } else {
            wl_nv2a_sphere_coordinates(t, made);
        }
        st[0] = made[0];
        st[1] = made[1];
        return;
    }
    st[0] = t[0];
    st[1] = t[1];
    if (s->projective && t[3] != 1.0f && fabsf(t[3]) > 1.0e-12f) {
        st[0] /= t[3];
        st[1] /= t[3];
    }
    if (s->linear) {
        st[0] *= s->to_s;
        st[1] *= s->to_t;
    }
}

// a float into a word's place, its bits: a float store (may_alias: the word is read as an integer
// after), not a trip through an integer register, which GCC makes of a memcpy (a store, a load and a
// store on PowerPC)
typedef float WordFloat __attribute__((may_alias));

static inline void put_float(uint32_t *word, float f) {
    *(WordFloat *)word = f;
}

// a texture coordinate's word sent as the program left it (stage_texcoord's st for a stage that
// is not a sphere map, projective or linear), its bits copied
static inline void put_bits(uint32_t *word, const float *f) {
    uint32_t bits;

    memcpy(&bits, f, sizeof bits);
    *word = bits;
}

// vertices_pack's positions: (x w, y w, -w) in pixels for a perspective vertex, else (x, y, depth);
// made inline for constant choices (mul_x, mul_y: the samples' sizes powers of two, to_sx and to_sy
// multiplied by; landing: where the vertices land kept)
static inline __attribute__((always_inline)) void positions_pack(const WlNv2aGx *gx, const DrawPlan *plan,
                                                                 unsigned runs, bool mul_x, bool mul_y,
                                                                 bool landing) {
    PackedVertex *const *made = gx->batch.run_vertices;
    const WlXboxVshOutput *o = gx->batch.outputs;
    const bool perspective = gx->projection.perspective;
    const float scale_x = plan->scale[0], scale_y = plan->scale[1], offset_x = plan->offset[0],
                offset_y = plan->offset[1], sx = plan->sx, sy = plan->sy, to_sx = plan->to_sx, to_sy = plan->to_sy,
                depth_max = plan->depth_max;
    unsigned r;

    for (r = 0; r < runs; r++) {
        PackedVertex *out = made[r];
        const WlXboxVshOutput *v = &o[r];

        if (perspective && v->clip_captured) {
            // (x w, y w, -w) in pixels, the pixel centres on integers as Direct3D puts them
            float cw = v->clip[3];
            float x = v->clip[0] * scale_x + offset_x * cw, y = v->clip[1] * scale_y + offset_y * cw;
            float px = (mul_x ? x * to_sx : x / sx) + 0.5f * cw;
            float py = (mul_y ? y * to_sy : y / sy) + 0.5f * cw;

            put_float(&out->w[0], px);
            put_float(&out->w[1], py);
            put_float(&out->w[2], -cw);
            if (landing) {
                out->x = cw != 0.0f ? px / cw : 0.0f;
                out->y = cw != 0.0f ? py / cw : 0.0f;
            }
        } else {
            float px = (mul_x ? v->position[0] * to_sx : v->position[0] / sx) + 0.5f;
            float py = (mul_y ? v->position[1] * to_sy : v->position[1] / sy) + 0.5f;

            put_float(&out->w[0], px);
            put_float(&out->w[1], py);
            put_float(&out->w[2], as_depth(v->position[2], depth_max) - 1.0f);
            out->x = px;
            out->y = py;
        }
    }
}

// the batch's vertices the program made (gx->batch.outputs[r] into gx->batch.run_vertices[r]) as GX
// takes them (OpenCE-Wii's vertex_store), in the draw's layout, and where they land in EFB pixels.
// A word at a time for all of them (as the fetch takes an attribute): a word's place and the
// draw's choices for it are the same for every vertex, so each loop is made for its choices and
// keeps them in registers (a float store may alias the plan, which would otherwise be read again
// after each).
static void vertices_pack(const WlNv2aGx *gx, const WlNv2aKelvin *k, const DrawPlan *plan, unsigned runs) {
    PackedVertex *const *made = gx->batch.run_vertices;
    const WlXboxVshOutput *o = gx->batch.outputs;
    const unsigned colors = plan->colors, channels = plan->channels, stage_count = plan->stage_count,
                   projected_count = plan->projected_count;
    unsigned i = 3, stage, r;

    if (plan->xf != NULL) {
        // (the transform unit's: the position as the vertex has it)
        for (r = 0; r < runs; r++) {
            uint32_t *w = made[r]->w;
            const float *in = gx->batch.inputs[r][0];

            put_bits(&w[0], &in[0]);
            put_bits(&w[1], &in[1]);
            put_bits(&w[2], &in[2]);
        }
    } else {
        // (a loop made for the samples' usual power of two, each way of landing, so the choices are
        // not tested again at each vertex)
        if (plan->to_sx != 0.0f && plan->to_sy != 0.0f) {
            if (plan->landing) {
                positions_pack(gx, plan, runs, true, true, true);
            } else {
                positions_pack(gx, plan, runs, true, true, false);
            }
        } else {
            positions_pack(gx, plan, runs, plan->to_sx != 0.0f, plan->to_sy != 0.0f, plan->landing);
        }
    }
    if (projected_count != 0) {
        // (the projected coordinates first, as GX's normal, binormal and tangent)
        for (stage = 0; stage < projected_count; stage++, i += 3) {
            unsigned n = plan->projected_list[stage];
            const StagePlan *sp = &plan->stages[n];

            for (r = 0; r < runs; r++) {
                float stq[3];

                stage_texcoord_projected(sp, o[r].t[n], stq);
                put_float(&made[r]->w[i], stq[0]);
                put_float(&made[r]->w[i + 1], stq[1]);
                put_float(&made[r]->w[i + 2], stq[2]);
            }
        }
        for (; projected_count > 1u && stage < WL_GX_DIRECT_PROJECTED; stage++, i += 3) {
            for (r = 0; r < runs; r++) {
                made[r]->w[i] = made[r]->w[i + 1] = made[r]->w[i + 2] = 0;
            }
        }
    }
    if (colors != 0) {
        const WlNv2aColorConstants color_k = wl_nv2a_color_constants();
        unsigned c;

        for (c = 0; c < colors; c++, i++) {
            if (channels & (1u << c)) {
                size_t from = c == 0 ? offsetof(WlXboxVshOutput, d0) : offsetof(WlXboxVshOutput, d1);

                for (r = 0; r < runs; r++) {
                    made[r]->w[i] = wl_nv2a_color_word_with((const float *)((const char *)&o[r] + from), &color_k);
                }
            } else {
                for (r = 0; r < runs; r++) {
                    made[r]->w[i] = 0u;
                }
            }
        }
    }
    for (stage = 0; stage < stage_count; stage++, i += 2) {
        unsigned n = plan->stage_list[stage];
        const StagePlan *sp = &plan->stages[n];

        if (sp->sphere) {
            for (r = 0; r < runs; r++) {
                float st[2];

                stage_texcoord(sp, o[r].t[n], st);
                put_float(&made[r]->w[i], st[0]);
                put_float(&made[r]->w[i + 1], st[1]);
            }
        } else if (sp->projective) {
            // (stage_texcoord's, its choices read once)
            const bool linear = sp->linear;
            const float to_s = sp->to_s, to_t = sp->to_t;

            for (r = 0; r < runs; r++) {
                const float *t = o[r].t[n];
                float s0 = t[0], s1 = t[1], q = t[3];

                if (q != 1.0f && fabsf(q) > 1.0e-12f) {
                    s0 /= q;
                    s1 /= q;
                }
                if (linear) {
                    s0 *= to_s;
                    s1 *= to_t;
                }
                put_float(&made[r]->w[i], s0);
                put_float(&made[r]->w[i + 1], s1);
            }
        } else if (sp->linear) {
            const float to_s = sp->to_s, to_t = sp->to_t;

            for (r = 0; r < runs; r++) {
                put_float(&made[r]->w[i], o[r].t[n][0] * to_s);
                put_float(&made[r]->w[i + 1], o[r].t[n][1] * to_t);
            }
        } else {
            // (as the program left them)
            for (r = 0; r < runs; r++) {
                put_bits(&made[r]->w[i], &o[r].t[n][0]);
                put_bits(&made[r]->w[i + 1], &o[r].t[n][1]);
            }
        }
    }
    if (plan->texcoords & (1u << WL_XBOX_PSH_FOG_TEXCOORD)) {
        for (r = 0; r < runs; r++) {
            put_float(&made[r]->w[i], wl_gx_ramp_coordinate(wl_nv2a_fog_factor(k, o[r].fog)));
        }
    }
}

// a vertex for the transform unit: its position and the stages' sources as fetched, the colours
static void vertex_pack_xf(const DrawPlan *plan, const float (*inputs)[4], PackedVertex *out) {
    const XfPlan *xf = plan->xf;
    unsigned stage, i = 3, c;

    put_float(&out->w[0], inputs[0][0]);
    put_float(&out->w[1], inputs[0][1]);
    put_float(&out->w[2], inputs[0][2]);
    if (xf->lit != 0) {
        put_float(&out->w[i++], inputs[xf->normal_source][0]);
        put_float(&out->w[i++], inputs[xf->normal_source][1]);
        put_float(&out->w[i++], inputs[xf->normal_source][2]);
    }
    for (c = 0; c < plan->colors; c++) {
        out->w[i++] = xf->color_source[c] < 0 ? xf->color_word[c] : wl_nv2a_color_word(inputs[xf->color_source[c]]);
    }
    for (stage = 0; stage < plan->stage_count; stage++) {
        unsigned source = xf->stage_source[plan->stage_list[stage]];

        if (source < WL_XBOX_VSH_INPUT_COUNT) {
            put_float(&out->w[i++], inputs[source][0]);
            put_float(&out->w[i++], inputs[source][1]);
        }
    }
    out->x = out->y = 0.0f;
}

// the batch's vertex n (its words' first member)
static inline const PackedVertex *batch_vertex(const WlNv2aGx *gx, unsigned n) {
    return (const PackedVertex *)(const void *)gx->batch.words[n];
}

// vertices first .. first + count - 1 of a draw (count at most WL_XBOX_VSH_BATCH) as GX takes them:
// gx->batch.words[n] (batch_vertex). An indexed draw's vertex kept by its index (the memo) is not made again; the
// others are fetched (the inputs the program reads alone) and run through the program together, and
// their vertices made into the memo (or, where another index of the batch holds the slot, here).
// A vertex is a function of its index, so each is the same whatever the memo held. The draw's first
// batch (`layout` not NULL) first decides its projected coordinates (projected_plan): true when the
// plan and the layout changed, which is then to be set again.
static bool projected_plan(WlNv2aGx *gx, DrawPlan *plan, WlGxDirectLayout *layout, unsigned runs);
static bool projected_possible(WlNv2aGx *gx, const DrawPlan *plan);

// ---- fused batches -------------------------------------------------------------------------------
// A program compiled ahead of time for the Wii may have fused forms (WlXboxVshFusedProc): its batch
// reads the vertex arrays, runs the program and writes the vertices' words in one loop, for one form
// of the arrays and of the layout (WlXboxVshFusedKey), which vertices_pack's words to the same bits
// (the colours to a byte as the Gekko's quantized store makes it: below 0 or a NaN 0, else the value
// cut down, which is what wl_nv2a_color_byte does). The draw's form is found once, at its first batch
// that may run fused (not the first when a coordinate may be projected: projected_plan reads the
// program's outputs).

// the draw's form as the key the forms are found by; false when it has none (a way vertices_pack takes
// that no fused batch writes)
static bool fused_key(const WlNv2aGx *gx, const DrawPlan *plan, WlXboxVshFusedKey *key) {
    const WlNv2aFetchPlan *fetch = &gx->fetch;
    unsigned n;

    memset(key, 0, sizeof *key);
    if (plan->xf != NULL || plan->landing || plan->projected_count != 0 || plan->to_sx == 0.0f ||
        plan->to_sy == 0.0f || (plan->texcoords & (1u << WL_XBOX_PSH_FOG_TEXCOORD)) ||
        fetch->source == WL_NV2A_SOURCE_IMMEDIATE || plan->stage_count > 4u || plan->colors > 2u) {
        return false;
    }
    key->live = (unsigned short)plan->live;
    key->perspective = gx->projection.perspective;
    key->colors = (unsigned char)plan->colors;
    key->channels = (unsigned char)(plan->channels & ((1u << plan->colors) - 1u));
    key->stage_count = (unsigned char)plan->stage_count;
    for (n = 0; n < plan->stage_count; n++) {
        const StagePlan *sp = &plan->stages[plan->stage_list[n]];
        unsigned kind = sp->sphere       ? WL_XBOX_VSH_FUSED_STAGE_SPHERE
                        : sp->projective ? (sp->linear ? WL_XBOX_VSH_FUSED_STAGE_PROJECTIVE_LINEAR
                                                       : WL_XBOX_VSH_FUSED_STAGE_PROJECTIVE)
                        : sp->linear     ? WL_XBOX_VSH_FUSED_STAGE_LINEAR
                                         : WL_XBOX_VSH_FUSED_STAGE_PLAIN;

        if (sp->spot || plan->stage_list[n] > 3u) {
            return false;
        }
        key->stages[n] = (unsigned char)(plan->stage_list[n] | kind << 2);
    }
    for (n = 0; n < fetch->count; n++) {
        unsigned format = wl_nv2a_fetch_fused_format(fetch, n);

        if (format == WL_XBOX_VSH_FUSED_FORMATS || fetch->streamed[n].attribute >= 16u) {
            return false;
        }
        key->format[fetch->streamed[n].attribute] = (unsigned char)format;
    }
    return true;
}

// the draw's fused form (NULL: none), and the fused batches' state for it
static const WlXboxVshFusedForm *fused_form(WlNv2aGx *gx, const DrawPlan *plan) {
    const WlXboxVshProgram *program = gx->pipeline.program;
    const WlNv2aFetchPlan *fetch = &gx->fetch;
    WlXboxVshFused *f = &gx->fused;
    WlXboxVshFusedKey key;
    const WlXboxVshFusedForm *form = NULL;
    unsigned n, c;

    if (gx->cfg.no_fused || program == NULL || (program->native_fused_count == 0 && gx->cfg.fused_seen == NULL) ||
        !fused_key(gx, plan, &key)) {
        return NULL;
    }
    for (n = 0; n < program->native_fused_count; n++) {
        if (memcmp(&program->native_fused[n].key, &key, sizeof key) == 0) {
            form = &program->native_fused[n];
            break;
        }
    }
    if (form == NULL) {
        return NULL;
    }
    {
        static const float pack[WL_XBOX_VSH_FUSED_PACK] = {
            [WL_XBOX_VSH_FUSED_HALF] = 0.5f,          [WL_XBOX_VSH_FUSED_HALF + 1] = 0.5f,
            [WL_XBOX_VSH_FUSED_255] = 255.0f,         [WL_XBOX_VSH_FUSED_255 + 1] = 255.0f,
            [WL_XBOX_VSH_FUSED_DEPTH + 1] = 1.0f,     [WL_XBOX_VSH_FUSED_TINY] = 1.0e-12f,
            [WL_XBOX_VSH_FUSED_SHORT] = 1.0f / 32767.0f, [WL_XBOX_VSH_FUSED_SHORT + 1] = 1.0f / 32767.0f,
            [WL_XBOX_VSH_FUSED_NEGATIVE] = -1.0f,     [WL_XBOX_VSH_FUSED_NEGATIVE + 1] = -1.0f,
            [WL_XBOX_VSH_FUSED_BYTE] = 1.0f / 255.0f, [WL_XBOX_VSH_FUSED_BYTE + 1] = 1.0f / 255.0f,
            [WL_XBOX_VSH_FUSED_EDGE] = 0.5f,          [WL_XBOX_VSH_FUSED_EDGE + 1] = 1.0f,
            [WL_XBOX_VSH_FUSED_ONES] = 1.0f,          [WL_XBOX_VSH_FUSED_ONES + 1] = 1.0f};
        static const double sphere[WL_XBOX_VSH_FUSED_SPHERE] = {1.0e-24, 1.0e76, 1.5, 0.5, 2.5e-13, 2.0};

        memcpy(f->pack, pack, sizeof pack);
        memcpy(f->sphere, sphere, sizeof sphere);
    }
    f->pack[WL_XBOX_VSH_FUSED_SCALE] = plan->scale[0];
    f->pack[WL_XBOX_VSH_FUSED_SCALE + 1] = plan->scale[1];
    f->pack[WL_XBOX_VSH_FUSED_OFFSET] = plan->offset[0];
    f->pack[WL_XBOX_VSH_FUSED_OFFSET + 1] = plan->offset[1];
    f->pack[WL_XBOX_VSH_FUSED_TO_SAMPLES] = plan->to_sx;
    f->pack[WL_XBOX_VSH_FUSED_TO_SAMPLES + 1] = plan->to_sy;
    // (as_depth's z / depth_max, or z itself: z / 1)
    f->pack[WL_XBOX_VSH_FUSED_DEPTH] = plan->depth_max > 0.0f ? plan->depth_max : 1.0f;
    for (n = 0; n < 4; n++) {
        const StagePlan *sp = &plan->stages[n];

        f->pack[WL_XBOX_VSH_FUSED_TO_TEXELS + 2 * n] = sp->linear ? sp->to_s : 1.0f;
        f->pack[WL_XBOX_VSH_FUSED_TO_TEXELS + 2 * n + 1] = sp->linear ? sp->to_t : 1.0f;
    }
    // (the inputs' start values: the current values the fetch wrote into every run's, and (0, 0, 0, 1)
    // under an array's components)
    for (n = 0; n < 16; n++) {
        for (c = 0; c < 4; c++) {
            f->inputs[0][n][c] = f->inputs[1][n][c] = key.format[n] != WL_XBOX_VSH_FUSED_NONE ? (c == 3 ? 1.0f : 0.0f)
                                                                                              : gx->batch.inputs[0][n][c];
        }
        f->data[n] = NULL;
        f->stride[n] = 0;
    }
    for (n = 0; n < fetch->count; n++) {
        f->data[fetch->streamed[n].attribute] = fetch->streamed[n].data;
        f->stride[fetch->streamed[n].attribute] = fetch->streamed[n].stride;
    }
    wl_nv2a_fetch_tables(&f->byte_float, &f->cmp11, &f->cmp10);
    return form;
}

// the draw's fused form, found at its first batch that may run fused (NULL: the general way)
static const WlXboxVshFusedForm *fused_for(WlNv2aGx *gx, DrawPlan *plan, const WlGxDirectLayout *layout,
                                           unsigned runs) {
    if (!plan->fused_known) {
        if (layout != NULL && projected_possible(gx, plan)) {
            return NULL;
        }
        plan->fused_known = true;
        plan->fused = fused_form(gx, plan);
    }
    if (gx->cfg.fused_seen != NULL && gx->pipeline.program != NULL) {
        WlXboxVshFusedKey key;

        if (plan->fused != NULL) {
            gx->cfg.fused_seen(gx->cfg.fused_user, gx->pipeline.program->hash, &plan->fused->key, runs, true);
        } else if (!gx->cfg.no_fused && fused_key(gx, plan, &key)) {
            gx->cfg.fused_seen(gx->cfg.fused_user, gx->pipeline.program->hash, &key, runs, false);
        }
    }
    return plan->fused;
}

#ifdef WL_XBOX_FUSED_CHECK
// (a build with WL_XBOX_FUSED_CHECK: each fused batch run the general way too, and the first word
// that differs in a draw logged with the vertex's inputs, FUSED_CHECK_LINES lines a run at most; the
// fused words are kept, so the picture is the fused build's)
enum { FUSED_CHECK_LINES = 48 };

// an array's element size as the fused form reads it (0: none)
static unsigned fused_check_bytes(unsigned format) {
    return format >= WL_XBOX_VSH_FUSED_F1 && format <= WL_XBOX_VSH_FUSED_F4 ? 4u * (format - WL_XBOX_VSH_FUSED_F1 + 1u)
           : format == WL_XBOX_VSH_FUSED_D3DCOLOR || format == WL_XBOX_VSH_FUSED_CMP ? 4u
           : format >= WL_XBOX_VSH_FUSED_S1 && format < WL_XBOX_VSH_FUSED_UB       ? 2u * (format - WL_XBOX_VSH_FUSED_S1 + 1u)
           : format >= WL_XBOX_VSH_FUSED_UB && format < WL_XBOX_VSH_FUSED_FORMATS  ? format - WL_XBOX_VSH_FUSED_UB + 1u
                                                                                   : 0u;
}

static void fused_check(WlNv2aGx *gx, const WlNv2aKelvin *k, const WlNv2aDraw *draw, DrawPlan *plan, unsigned runs,
                        const WlXboxVshFusedForm *form) {
    static PackedVertex fused[WL_XBOX_VSH_BATCH];
    static unsigned lines, checked, differing;
    static uint32_t logged_stamp;
    const WlXboxVshProgram *program = gx->pipeline.program;
    const WlNv2aFetchPlan *fetch = &gx->fetch;
    const unsigned words = 3u + plan->colors + 2u * plan->stage_count;
    unsigned r, n, w;

    for (r = 0; r < runs; r++) {
        fused[r] = *gx->batch.run_vertices[r];
    }
    wl_nv2a_fetch_elements(&gx->fetch, draw, gx->batch.elements, runs, gx->batch.inputs);
    wl_nv2a_pipeline_vertices(&gx->pipeline, k, (const float (*)[WL_NV2A_ATTRIBS][4])gx->batch.inputs, runs,
                              plan->live, gx->batch.outputs, &gx->batch.work);
    vertices_pack(gx, k, plan, runs);
    checked += runs;
    for (r = 0; r < runs; r++) {
        uint32_t *general = gx->batch.run_vertices[r]->w;
        unsigned first = words, count = 0;

        for (w = 0; w < words; w++) {
            if (fused[r].w[w] != general[w]) {
                first = first < w ? first : w;
                count++;
            }
        }
        if (count != 0) {
            differing++;
        }
        if (count != 0 && logged_stamp != gx->memo_stamp && lines + 3u <= FUSED_CHECK_LINES) {
            char text[400];
            size_t at = 0;

            logged_stamp = gx->memo_stamp;
            lines += 3u;
            for (w = first; w < words; w++) {
                if (fused[r].w[w] != general[w]) {
                    at += (size_t)snprintf(text + at, sizeof text - at, " w%u %08x/%08x", w, fused[r].w[w], general[w]);
                }
            }
            WL_LOG_W(TAG,
                     "fused check: program %08lx form %u live %03x element %u (%u of %u vertices differ so "
                     "far); words fused/general:%s",
                     program->hash, (unsigned)(form - program->native_fused), plan->live,
                     (unsigned)gx->batch.elements[r], differing, checked, text);
            at = 0;
            for (n = 0; n < fetch->count && at < sizeof text - 48u; n++) {
                unsigned bytes = fused_check_bytes(wl_nv2a_fetch_fused_format(fetch, n)), b;
                const uint8_t *p = fetch->streamed[n].data + (size_t)fetch->streamed[n].stride * gx->batch.elements[r];

                at += (size_t)snprintf(text + at, sizeof text - at, " v%u", (unsigned)fetch->streamed[n].attribute);
                for (b = 0; b < bytes && at < sizeof text - 4u; b++) {
                    at += (size_t)snprintf(text + at, sizeof text - at, "%s%02x", b % 4u ? "" : " ", p[b]);
                }
            }
            WL_LOG_W(TAG, "fused check:  arrays%s", text);
            at = 0;
            for (n = 0; n < fetch->count && at < sizeof text - 64u; n++) {
                const float *v = gx->batch.inputs[r][fetch->streamed[n].attribute];

                at += (size_t)snprintf(text + at, sizeof text - at, " v%u %g %g %g %g",
                                       (unsigned)fetch->streamed[n].attribute, (double)v[0], (double)v[1],
                                       (double)v[2], (double)v[3]);
            }
            WL_LOG_W(TAG, "fused check:  inputs%s", text);
        }
        memcpy(gx->batch.run_vertices[r]->w, fused[r].w, sizeof fused[r].w);
    }
}
#endif

static bool vertices_make(WlNv2aGx *gx, const WlNv2aKelvin *k, const WlNv2aDraw *draw, uint32_t first,
                          uint32_t count, DrawPlan *plan, WlGxDirectLayout *layout) {
    bool indexed = draw->source == WL_NV2A_SOURCE_ARRAYS && draw->indices != NULL;
    uint32_t batch = ++gx->batch_stamp;
    unsigned runs = 0, n, r;
    bool changed;

    // (each vertex made: its element as the fetch takes it, wl_nv2a_fetch_element's, and where its
    // words go)
    for (n = 0; n < count; n++) {
        uint32_t v = first + n, element = v;
        PackedVertex *made = &gx->batch.made[runs];

        if (indexed) {
            uint32_t index = draw->indices[v];
            struct WlNv2aGxMemo *m = &gx->memo[index & (MEMO - 1u)];

            if (m->stamp == gx->memo_stamp && m->index == index) {
                m->batch = batch;
                gx->batch.words[n] = m->vertex.w;
                continue;
            }
            if (m->stamp != gx->memo_stamp || m->batch != batch) {
                m->stamp = gx->memo_stamp;
                m->index = index;
                m->batch = batch;
                made = &m->vertex;
            }
            element = index - draw->index_base;
        }
        gx->batch.words[n] = made->w;
        gx->batch.elements[runs] = element;
        gx->batch.words_made[runs] = made->w;
        gx->batch.run_vertices[runs] = made;
        runs++;
    }
    gx->stats.vertices_reused += count - runs;
    if (plan->xf == NULL && runs != 0) {
        const WlXboxVshFusedForm *form = fused_for(gx, plan, layout, runs);

        if (form != NULL) {
            gx->fused.elements = gx->batch.elements;
            gx->fused.words = gx->batch.words_made;
            form->run((const float (*)[4])k->constants, &gx->fused, runs, plan->live);
            gx->stats.vertices_fused += runs;
#ifdef WL_XBOX_FUSED_CHECK
            fused_check(gx, k, draw, plan, runs, form);
#endif
            return false;
        }
    }
    wl_nv2a_fetch_elements(&gx->fetch, draw, gx->batch.elements, runs, gx->batch.inputs);
    if (plan->xf != NULL && plan->xf->full) {
        for (r = 0; r < runs; r++) {
            vertex_pack_xf(plan, (const float (*)[4])gx->batch.inputs[r], gx->batch.run_vertices[r]);
        }
        return false;
    }
    // (the transform unit's position: the program run for the other outputs alone)
    wl_nv2a_pipeline_vertices(&gx->pipeline, k, (const float (*)[WL_NV2A_ATTRIBS][4])gx->batch.inputs, runs,
                              plan->xf != NULL ? plan->live & ~(unsigned)WL_XBOX_VSH_LIVE_POSITION : plan->live,
                              gx->batch.outputs, &gx->batch.work);
    changed = layout != NULL && projected_plan(gx, plan, layout, runs);
    vertices_pack(gx, k, plan, runs);
    return changed;
}

// the one input register (bit) an output's components read, 0 for none, ~0u for more or not affine
static unsigned xf_source(const unsigned long *components, unsigned count) {
    unsigned long all = 0;
    unsigned c;

    for (c = 0; c < count; c++) {
        all |= components[c];
    }
    if (all & WL_XBOX_VSH_NOT_AFFINE) {
        return ~0u;
    }
    all &= 0xFFFFu;
    return (all & (all - 1u)) != 0 ? ~0u : (unsigned)all;
}

static unsigned bit_index(unsigned bit) {
    unsigned n = 0;

    while (bit > 1u) {
        bit >>= 1;
        n++;
    }
    return n;
}

// the program's verdict for the outputs a draw reads: whether its position may go to the transform
// unit (an affine function of v0), and whether all of them may (*full)
// the program's analysis into gx->xf_dependencies: kept, or made and kept in place of the least used
static void xf_analysis(WlNv2aGx *gx, const WlXboxVshProgram *program) {
    struct XfAnalysis *slot = &gx->xf_analyses[0];
    unsigned i;

    gx->xf_analysis_use++;
    for (i = 0; i < XF_ANALYSES; i++) {
        struct XfAnalysis *a = &gx->xf_analyses[i];

        if (a->used != 0 && a->program == program && a->serial == program->serial) {
            a->used = gx->xf_analysis_use;
            gx->xf_dependencies = a->dependencies;
            return;
        }
        if (a->used < slot->used) {
            slot = a;
        }
    }
    wl_xbox_vsh_analyze(program, &gx->xf_dependencies);
    slot->program = program;
    slot->serial = program->serial;
    slot->used = gx->xf_analysis_use;
    slot->dependencies = gx->xf_dependencies;
}

static bool xf_structure(WlNv2aGx *gx, unsigned live, bool *full) {
    const WlXboxVshProgram *program = gx->pipeline.program;
    const WlXboxVshDependencies *d = &gx->xf_dependencies;
    unsigned stage;

    if (program == gx->xf_program && gx->pipeline.compiled == gx->xf_compiled && live == gx->xf_live) {
        *full = gx->xf_full;
        return gx->xf_eligible;
    }
    if (program != gx->xf_program || gx->pipeline.compiled != gx->xf_compiled) {
        xf_analysis(gx, program);
    }
    gx->xf_program = program;
    gx->xf_compiled = gx->pipeline.compiled;
    gx->xf_live = live;
    gx->xf_eligible = gx->xf_full = *full = false;
    // (the clip-space position, or a screen-space one, from v0 alone)
    if (xf_source(d->clip_captured ? d->clip : d->position, 4) != 1u) {
        return false;
    }
    gx->xf_eligible = true;
    gx->xf_partial = WL_NV2A_GX_XF_INDEXED;
    if (live & (WL_XBOX_VSH_LIVE_FOG | WL_XBOX_VSH_LIVE_POINT_SIZE | WL_XBOX_VSH_LIVE_BACK)) {
        gx->xf_partial = WL_NV2A_GX_XF_POS_OUTPUTS;
    } else if (((live & WL_XBOX_VSH_LIVE_D0) && xf_source(d->d0, 4) == ~0u) ||
               ((live & WL_XBOX_VSH_LIVE_D1) && xf_source(d->d1, 4) == ~0u)) {
        gx->xf_partial = WL_NV2A_GX_XF_POS_COLOR;
    }
    for (stage = 0; gx->xf_partial == WL_NV2A_GX_XF_INDEXED && stage < WL_NV2A_TEXTURES; stage++) {
        if ((live & ((unsigned)WL_XBOX_VSH_LIVE_T0 << stage)) && xf_source(d->t[stage], 4) == ~0u) {
            gx->xf_partial = WL_NV2A_GX_XF_POS_TEXCOORD;
        }
    }
    gx->xf_full = gx->xf_partial == WL_NV2A_GX_XF_INDEXED;
    *full = gx->xf_full;
    return true;
}

static bool same4(const float *a, float x, float y, float z, float w) {
    return a[0] == x && a[1] == y && a[2] == z && a[3] == w;
}

// A colour affine in one input register, value = k + M v (from the probes: k at the zero vector, M's
// columns at the basis), as GX's lighting makes it (wl_gx_direct_lighting): each of its parts (r, g, b
// and a) either the same at every vertex or the ambient plus the signed dot product of the input as a
// normal and one direction, times a colour, the r, g, b scales of one sign. Into xf's lit channels
// (c's two: 2c the colour, 2c + 1 the alpha), their directions and colours (the scale of each kept in
// lit_scale for lit_finish), and k into the constant word; false when it is not of that form, or its
// input not a normal GX can take (more than three numbers: the w of four, or of a two floats and w).
static bool lit_plan(XfPlan *xf, const WlNv2aDraw *draw, unsigned c, unsigned source, const float *zero,
                     const float *const basis[4], float lit_scale[4][4]) {
    unsigned r = bit_index(source), part, i, j;
    const WlNv2aAttrib *attrib = &draw->attribs[r];
    bool w_one = !(attrib->size >= 4u || (attrib->type == WL_NV2A_VTX_F && attrib->size == 7u));
    float k[4], rows[4][3], color[4];

    if (!(draw->streamed & source) || (xf->lit != 0 && xf->normal_source != r)) {
        return false;
    }
    for (i = 0; i < 4; i++) {
        float w = basis[3][i] - zero[i];

        // (a w of 1 at every vertex adds its column to k; one read from the vertex is beyond a normal)
        if (w != 0.0f && !w_one) {
            return false;
        }
        k[i] = zero[i] + (w_one ? w : 0.0f);
        for (j = 0; j < 3; j++) {
            rows[i][j] = basis[j][i] - zero[i];
        }
    }
    for (part = 0; part < 2; part++) {
        unsigned first = part == 0 ? 0u : 3u, last = part == 0 ? 3u : 4u, best = first;
        float best_length = 0.0f, length;

        for (i = first; i < last; i++) {
            length = rows[i][0] * rows[i][0] + rows[i][1] * rows[i][1] + rows[i][2] * rows[i][2];
            if (length > best_length) {
                best_length = length;
                best = i;
            }
        }
        if (best_length == 0.0f) {
            // (the same at every vertex: the constant word's)
            continue;
        }
        length = sqrtf(best_length);
        for (i = first; i < last; i++) {
            // (row i a multiple t of the longest, t >= 0)
            float t = (rows[i][0] * rows[best][0] + rows[i][1] * rows[best][1] + rows[i][2] * rows[best][2]) /
                      best_length;

            if (t < 0.0f) {
                return false;
            }
            for (j = 0; j < 3; j++) {
                if (fabsf(rows[i][j] - t * rows[best][j]) > 1.0e-5f * length) {
                    return false;
                }
            }
            color[i] = t * length;
        }
        for (i = first; i < last; i++) {
            // (the ambient a byte: none below 0)
            if (k[i] < -1.0e-6f) {
                return false;
            }
        }
        xf->lit |= 1u << (2u * c + part);
        for (j = 0; j < 3; j++) {
            xf->lighting.direction[2u * c + part][j] = rows[best][j] / length;
        }
        for (i = first; i < last; i++) {
            lit_scale[2u * c + part][i] = color[i];
        }
    }
    xf->normal_source = r;
    xf->color_word[c] = wl_nv2a_color_word(k);
    for (i = 0; i < 4; i++) {
        xf->lighting.ambient[c][i] = (uint8_t)(wl_nv2a_color_word((const float[4]){k[i], 0.0f, 0.0f, 0.0f}) >> 24);
    }
    return true;
}

// The lit channels' normal matrix (the largest scale, the normal taken as it is times it) and colours
// (each scale over the largest)
static void lit_finish(XfPlan *xf, const float lit_scale[4][4]) {
    float most = 0.0f;
    unsigned n, i;

    for (n = 0; n < 4; n++) {
        for (i = 0; i < 4; i++) {
            if ((xf->lit & (1u << n)) && lit_scale[n][i] > most) {
                most = lit_scale[n][i];
            }
        }
    }
    memset(xf->lighting.normal_matrix, 0, sizeof xf->lighting.normal_matrix);
    for (i = 0; i < 3; i++) {
        xf->lighting.normal_matrix[i][i] = most;
    }
    for (n = 0; n < 4; n++) {
        for (i = 0; i < 4; i++) {
            float v = (xf->lit & (1u << n)) && most > 0.0f ? lit_scale[n][i] / most * 255.0f + 0.5f : 0.0f;

            xf->lighting.color[n][i] = (uint8_t)(v > 255.0f ? 255.0f : v);
        }
    }
}

// The draw's transform unit plan, or false for the processor's: the structure, then the probes (the
// position's w 1 where the draw is in screen space; each colour passed as it is or constant; no
// sphere map; a projected stage's q 1; a stage's input the position or a coordinate of two
// components). The fetch's registers are written by the probes: called before the draw's fetch.
static bool xf_prepare(WlNv2aGx *gx, const WlNv2aKelvin *k, const WlNv2aDraw *draw, const DrawPlan *plan,
                       bool count_pixels, XfPlan *xf) {
    // (the probes: every input register the same; Z the zero vector, X Y Z W the basis, then the
    // basis's x, y and z with w 1. A linear form of v0 gives each column exactly at X, Y, Z, W, so two
    // programs that compute the same transform give the same matrix: their passes over the same
    // geometry keep the same depths)
    enum { P_ZERO, P_X, P_Y, P_Z, P_W, P_X1, P_Y1, P_Z1 };
    static const float probe[XF_PROBES][4] = {
        {0.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f, 1.0f}};
    const WlXboxVshDependencies *d = &gx->xf_dependencies;
    const WlXboxVshOutput *o = gx->batch.outputs;
    unsigned n, a, stage, c, row;
    uint32_t reads;
    float rows[4][4], lit_scale[4][4] = {{0.0f}};
    bool full;

    if (gx->cfg.no_xf || gx->pipeline.program == NULL || count_pixels || draw->count < XF_MIN_VERTICES ||
        draw->source == WL_NV2A_SOURCE_IMMEDIATE) {
        gx->xf_why = gx->cfg.no_xf                         ? WL_NV2A_GX_XF_OFF
                     : gx->pipeline.program == NULL        ? WL_NV2A_GX_XF_FIXED
                     : count_pixels                        ? WL_NV2A_GX_XF_COUNTING
                     : draw->count < XF_MIN_VERTICES       ? WL_NV2A_GX_XF_EMPTY
                                                           : WL_NV2A_GX_XF_IMMEDIATE;
        return false;
    }
    if (!xf_structure(gx, plan->live, &full)) {
        gx->xf_why = WL_NV2A_GX_XF_STRUCTURE;
        return false;
    }
    gx->xf_why = gx->xf_partial;
    // (the position an F3 array: w 1 for every vertex)
    if (!(draw->streamed & 1u) || draw->attribs[0].type != WL_NV2A_VTX_F || draw->attribs[0].size != 3) {
        gx->xf_why = WL_NV2A_GX_XF_POSITION;
        return false;
    }
    // (each probe in the input registers the draw's outputs read, as the draw's fetch fills only those)
    reads = wl_nv2a_pipeline_inputs(&gx->pipeline, plan->live);
    for (n = 0; n < XF_PROBES; n++) {
        for (a = 0; a < WL_NV2A_ATTRIBS; a++) {
            if (reads & (1u << a)) {
                memcpy(gx->batch.inputs[n][a], probe[n], sizeof probe[n]);
            }
        }
    }
    wl_nv2a_pipeline_vertices(&gx->pipeline, k, (const float (*)[WL_NV2A_ATTRIBS][4])gx->batch.inputs, XF_PROBES,
                              plan->live, gx->batch.outputs, &gx->batch.work);
    memset(xf, 0, sizeof *xf);
    xf->attributes = 1u;
    if (gx->projection.perspective) {
        // (x w, y w, -w) in pixels, as vertices_pack makes them from the clip-space position
        float to_x = plan->to_sx != 0.0f ? plan->scale[0] * plan->to_sx : plan->scale[0] / plan->sx;
        float to_y = plan->to_sy != 0.0f ? plan->scale[1] * plan->to_sy : plan->scale[1] / plan->sy;
        float w_x = (plan->to_sx != 0.0f ? plan->offset[0] * plan->to_sx : plan->offset[0] / plan->sx) + 0.5f;
        float w_y = (plan->to_sy != 0.0f ? plan->offset[1] * plan->to_sy : plan->offset[1] / plan->sy) + 0.5f;

        if (!o[P_W].clip_captured) {
            gx->xf_why = WL_NV2A_GX_XF_CLIP_W;
            return false;
        }
        for (row = 0; row < 4; row++) {
            for (c = 0; c < 3; c++) {
                rows[row][c] = o[P_X + c].clip[row] - o[P_ZERO].clip[row];
            }
            rows[row][3] = o[P_W].clip[row];
        }
        for (c = 0; c < 4; c++) {
            xf->position[0][c] = rows[0][c] * to_x + rows[3][c] * w_x;
            xf->position[1][c] = rows[1][c] * to_y + rows[3][c] * w_y;
            xf->position[2][c] = -rows[3][c];
        }
    } else {
        // (screen space: w 1 for the vertices, each a point with w 1)
        for (row = 0; row < 4; row++) {
            for (c = 0; c < 3; c++) {
                rows[row][c] = o[P_X1 + c].position[row] - o[P_W].position[row];
            }
            rows[row][3] = o[P_W].position[row];
        }
        if (d->clip_captured) {
            for (n = P_W; n < XF_PROBES; n++) {
                if (o[n].clip[3] != 1.0f) {
                    gx->xf_why = WL_NV2A_GX_XF_SCREEN;
                    return false;
                }
            }
        }
        for (c = 0; c < 4; c++) {
            xf->position[0][c] = rows[0][c] / plan->sx;
            xf->position[1][c] = rows[1][c] / plan->sy;
            xf->position[2][c] = rows[2][c] / plan->depth_max;
        }
        xf->position[0][3] += 0.5f;
        xf->position[1][3] += 0.5f;
        xf->position[2][3] -= 1.0f;
        // (the depth as the processor's draw clamps it to [0, 1] (as_depth), where GX clips: one the
        // same at every vertex clamped here, and one that varies with the position, which might leave
        // the range at a vertex the transform unit cannot clamp, left to the processor)
        if (xf->position[2][0] != 0.0f || xf->position[2][1] != 0.0f || xf->position[2][2] != 0.0f) {
            gx->xf_why = WL_NV2A_GX_XF_SCREEN;
            return false;
        }
        if (!(xf->position[2][3] >= -1.0f)) {
            xf->position[2][3] = -1.0f;
        } else if (xf->position[2][3] > 0.0f) {
            xf->position[2][3] = 0.0f;
        }
    }
    xf->full = full;
    xf->words = plan->words;
    if (!full) {
        return true;
    }
    // the colours: an input passed as it is, or the same at every probe
    for (c = 0; c < plan->colors; c++) {
        const unsigned long *dc = c == 0 ? d->d0 : d->d1;
        unsigned source = xf_source(dc, 4);

        if (c == 0 ? !(plan->channels & 1u) : !(plan->channels & 2u)) {
            xf->color_source[c] = -1;
            xf->color_word[c] = 0;
            continue;
        }
        if (source == 0) {
            xf->color_source[c] = -1;
            xf->color_word[c] = wl_nv2a_color_word(c == 0 ? o[0].d0 : o[0].d1);
            continue;
        }
        for (n = 0; n < XF_PROBES; n++) {
            const float *value = c == 0 ? o[n].d0 : o[n].d1;

            if (!same4(value, probe[n][0], probe[n][1], probe[n][2], probe[n][3])) {
                break;
            }
        }
        if (n < XF_PROBES) {
            // (not passed as it is: lit by GX from the input as a normal, or the processor's)
            const float *basis[4] = {c == 0 ? o[P_X].d0 : o[P_X].d1, c == 0 ? o[P_Y].d0 : o[P_Y].d1,
                                     c == 0 ? o[P_Z].d0 : o[P_Z].d1, c == 0 ? o[P_W].d0 : o[P_W].d1};

            if (gx->cfg.no_xf_light || !lit_plan(xf, draw, c, source, c == 0 ? o[P_ZERO].d0 : o[P_ZERO].d1, basis,
                                                 lit_scale)) {
                xf->full = false;
                gx->xf_why = WL_NV2A_GX_XF_POS_COLOR_MIX;
                return true;
            }
            xf->color_source[c] = -1;
            xf->attributes |= source;
            continue;
        }
        xf->color_source[c] = (int)bit_index(source);
        xf->attributes |= source;
    }
    if (xf->lit != 0) {
        lit_finish(xf, (const float (*)[4])lit_scale);
    }
    // the stages
    xf->words = 3u + (xf->lit != 0 ? 3u : 0u) + plan->colors;
    for (stage = 0; stage < WL_NV2A_TEXTURES; stage++) {
        const StagePlan *sp = &plan->stages[stage];
        unsigned source, r;

        if (!(plan->texcoords & (1u << stage))) {
            continue;
        }
        source = xf_source(d->t[stage], 4);
        if (sp->sphere || source == ~0u) {
            xf->full = false;
            gx->xf_why = sp->sphere ? WL_NV2A_GX_XF_POS_SPHERE : WL_NV2A_GX_XF_POS_TEXCOORD;
            break;
        }
        if (sp->projective) {
            for (n = P_W; n < XF_PROBES && o[n].t[stage][3] == 1.0f; n++) {
            }
            if (n < XF_PROBES) {
                xf->full = false;
                gx->xf_why = WL_NV2A_GX_XF_POS_PROJECTIVE;
                break;
            }
        }
        if (source == 1u || source == 0u) {
            // (from the position, or a constant: a generator from the position)
            xf->stage_source[stage] = WL_XBOX_VSH_INPUT_COUNT;
        } else {
            r = bit_index(source);
            // (a coordinate of more than two components is beyond the vertex's s, t)
            if (!(draw->streamed & source) || draw->attribs[r].size > 2u) {
                xf->full = false;
                gx->xf_why = WL_NV2A_GX_XF_POS_COORD;
                break;
            }
            xf->stage_source[stage] = r;
            xf->attributes |= source;
            xf->words += 2u;
        }
        for (row = 0; row < 2; row++) {
            float scale = !sp->linear ? 1.0f : row == 0 ? sp->to_s : sp->to_t;
            float base = o[P_ZERO].t[stage][row];

            xf->stage_matrix[stage][row][0] = (o[P_X].t[stage][row] - base) * scale;
            xf->stage_matrix[stage][row][1] = (o[P_Y].t[stage][row] - base) * scale;
            xf->stage_matrix[stage][row][2] =
                xf->stage_source[stage] == WL_XBOX_VSH_INPUT_COUNT ? (o[P_Z].t[stage][row] - base) * scale : 0.0f;
            xf->stage_matrix[stage][row][3] = o[P_W].t[stage][row] * scale;
        }
    }
    if (xf->full && xf->words > PACKED_WORDS) {
        // (a lit draw's normal beyond a sent vertex's words)
        xf->full = false;
        gx->xf_why = WL_NV2A_GX_XF_POS_COLOR_MIX;
    }
    if (!xf->full) {
        xf->words = plan->words;
        xf->attributes = 1u;
        xf->lit = 0;
    }
    return true;
}

// The arrays of a draw through the transform unit alone, from the vertex cache: its position, each
// colour passed from the vertices and each stage's coordinate read by GX at the draw's indices less
// its least (their arrays from that element on), into arrays[WL_GX_DIRECT_ARRAY_*], the layout's
// indexed attributes set (a constant colour is still sent). false (the vertices sent as before) when
// the cache is off, the draw is not from arrays or its indices are beyond 16 bits of its least, or an
// array cannot be kept.
static bool xf_arrays(WlNv2aGx *gx, const WlNv2aDraw *draw, const DrawPlan *plan, const XfPlan *xf,
                      WlGxDirectLayout *layout, const void *arrays[WL_GX_DIRECT_ARRAY_NORMAL + 1]) {
    unsigned c, stage;
    uint16_t indexed = 1u << WL_GX_DIRECT_ARRAY_POSITION;

    if (!wl_nv2a_vcache_on(&gx->vertices) || draw->source != WL_NV2A_SOURCE_ARRAYS ||
        draw->index_last - draw->index_base >= WL_NV2A_VCACHE_SPAN) {
        gx->xf_why = draw->source == WL_NV2A_SOURCE_ARRAYS && wl_nv2a_vcache_on(&gx->vertices) ? WL_NV2A_GX_XF_SENT_SPAN
                                                                                               : WL_NV2A_GX_XF_SENT_SOURCE;
        return false;
    }
    gx->xf_why = WL_NV2A_GX_XF_SENT_ARRAY;
    for (c = 0; c < plan->colors && c < 2u; c++) {
        if (xf->color_source[c] < 0) {
            continue;
        }
        arrays[WL_GX_DIRECT_ARRAY_COLOR0 + c] =
            wl_nv2a_vcache_get(&gx->vertices, gx->dev, &draw->attribs[xf->color_source[c]], draw->index_base,
                               draw->index_last, WL_NV2A_VCACHE_COLOR);
        if (arrays[WL_GX_DIRECT_ARRAY_COLOR0 + c] == NULL) {
            return false;
        }
        indexed |= (uint16_t)(1u << (WL_GX_DIRECT_ARRAY_COLOR0 + c));
    }
    if (xf->lit != 0) {
        arrays[WL_GX_DIRECT_ARRAY_NORMAL] = wl_nv2a_vcache_get(&gx->vertices, gx->dev, &draw->attribs[xf->normal_source],
                                                               draw->index_base, draw->index_last, WL_NV2A_VCACHE_NORMAL);
        if (arrays[WL_GX_DIRECT_ARRAY_NORMAL] == NULL) {
            return false;
        }
        indexed |= (uint16_t)(1u << WL_GX_DIRECT_ARRAY_NORMAL);
    }
    arrays[WL_GX_DIRECT_ARRAY_POSITION] = wl_nv2a_vcache_get(&gx->vertices, gx->dev, &draw->attribs[0],
                                                             draw->index_base, draw->index_last, WL_NV2A_VCACHE_POSITION);
    if (arrays[WL_GX_DIRECT_ARRAY_POSITION] == NULL) {
        return false;
    }
    for (stage = 0; stage < WL_NV2A_TEXTURES; stage++) {
        if (!(layout->texcoords & (1u << stage))) {
            continue;
        }
        arrays[WL_GX_DIRECT_ARRAY_TEXCOORD0 + stage] =
            wl_nv2a_vcache_get(&gx->vertices, gx->dev, &draw->attribs[xf->stage_source[stage]], draw->index_base,
                               draw->index_last, WL_NV2A_VCACHE_TEXCOORD);
        if (arrays[WL_GX_DIRECT_ARRAY_TEXCOORD0 + stage] == NULL) {
            return false;
        }
        indexed |= (uint16_t)(1u << (WL_GX_DIRECT_ARRAY_TEXCOORD0 + stage));
    }
    layout->indexed = indexed;
    return true;
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

// a vertex's landing (vertices_pack's x, y) into the draw's triangles' areas: what a pixel count without
// a depth test would say
static void landing_area(uint32_t primitive, const PackedVertex *vertex, uint32_t *seen_count, float corners[3][2],
                         float *area) {
    float here[2] = {vertex->x, vertex->y};
    uint32_t seen = *seen_count;

    switch (primitive) {
    case WL_NV2A_PRIM_TRIANGLES:
        memcpy(corners[seen % 3u], here, sizeof here);
        if (seen % 3u == 2u) {
            *area += fabsf(area2(corners[0], corners[1], corners[2]));
        }
        break;
    case WL_NV2A_PRIM_TRIANGLE_STRIP:
    case WL_NV2A_PRIM_QUAD_STRIP:
        memcpy(corners[seen % 3u], here, sizeof here);
        if (seen >= 2u) {
            *area += fabsf(area2(corners[0], corners[1], corners[2]));
        }
        break;
    case WL_NV2A_PRIM_TRIANGLE_FAN:
    case WL_NV2A_PRIM_POLYGON:
    case WL_NV2A_PRIM_QUADS:
        if (primitive == WL_NV2A_PRIM_QUADS && seen % 4u == 0u) {
            memcpy(corners[0], here, sizeof here);
        } else if (seen == 0u) {
            memcpy(corners[0], here, sizeof here);
        } else {
            if ((primitive == WL_NV2A_PRIM_QUADS ? seen % 4u : seen) >= 2u) {
                *area += fabsf(area2(corners[0], corners[1], here));
            }
            memcpy(corners[1], here, sizeof here);
        }
        break;
    default:
        break;
    }
    *seen_count = seen + 1u;
}

// what textures_bind notes of a draw's textures for later frames, for a draw that binds none: a
// picture the title showed, read as a texture (each frame then kept as one)
static void display_textures_note(WlNv2aGx *gx, const WlNv2aKelvin *k) {
    unsigned stage;

    for (stage = 0; stage < WL_NV2A_TEXTURES && !gx->keep_frames; stage++) {
        WlNv2aTexture t;

        if (wl_nv2a_stage_program(k, stage) == WL_NV2A_STAGE_NONE || !wl_nv2a_texture(k, stage, &t)) {
            continue;
        }
        if (target_texture(gx, stage, &t) == NULL && is_display(gx, t.phys)) {
            gx->keep_frames = true;
            WL_LOG_I(TAG, "a displayed surface (%08x) is read as a texture: each frame is kept as one",
                     (unsigned)t.phys);
        }
    }
}

// A draw that writes nothing GX keeps (no colour channel, no depth write: an occlusion query's
// quad), counting pixels: only the count is made, the program run for the positions and their
// landings as backend_draw makes them; no state, texture, program or primitive goes to GX (GX keeps
// the last drawn draw's, and what changed is kept for the next). The projection is fitted as for a
// drawn draw (its fits are kept for later draws). The pixels counted, the same as when it was drawn.
static uint32_t unwritten_count(WlNv2aGx *gx, const WlNv2aKelvin *k, const WlNv2aDraw *draw,
                                const WlNv2aSurface *surface) {
    DrawPlan plan;
    float corners[3][2], area = 0.0f;
    uint32_t v, seen = 0, first = 0;

    memset(&plan, 0, sizeof plan);
    plan.live = WL_XBOX_VSH_LIVE_POSITION;
    plan.sx = (float)surface->samples_x;
    plan.sy = (float)surface->samples_y;
    plan.to_sx = power_of_two(surface->samples_x) ? 1.0f / plan.sx : 0.0f;
    plan.to_sy = power_of_two(surface->samples_y) ? 1.0f / plan.sy : 0.0f;
    plan.landing = true;
    plan.depth_max = surface->depth_max > 0.0f ? surface->depth_max : 16777215.0f;
    plan.scale = k->constants[WL_NV2A_CONSTANT_VIEWPORT_SCALE];
    plan.offset = k->constants[WL_NV2A_CONSTANT_VIEWPORT_OFFSET];
    gx->projection = projection_fit(gx, k, draw, plan.depth_max);
    gx->memo_stamp++;
    wl_nv2a_fetch_plan(draw, wl_nv2a_pipeline_inputs(&gx->pipeline, plan.live), &gx->fetch);
    wl_nv2a_fetch_constants(&gx->fetch, k, gx->batch.inputs, WL_XBOX_VSH_BATCH);
    for (v = 0; v < draw->count; v++) {
        unsigned n = (v - first) % WL_XBOX_VSH_BATCH;

        if (n == 0) {
            first = v;
            vertices_make(gx, k, draw, v, draw->count - v < WL_XBOX_VSH_BATCH ? draw->count - v : WL_XBOX_VSH_BATCH,
                          &plan, NULL);
        }
        landing_area(draw->primitive, batch_vertex(gx, n), &seen, corners, &area);
    }
    gx->stats.unwritten_counted++;
    {
        float most = (float)gx->view_w * (float)gx->view_h, pixels = area * 0.5f;

        return (uint32_t)(pixels > most ? most : pixels);
    }
}

// The draw's projective stages whose q varies from vertex to vertex, up to WL_GX_DIRECT_PROJECTED of
// them, found in the draw's first batch (its `runs` vertices just made: the program's t[n].w reads an
// input, as its analysis finds, and differs among them): sent as (s, t, q) for GX to divide at each
// pixel, as the NV2A does, out of the stages divided per vertex. A constant q is divided here: per
// vertex or per pixel, the same (so is a q that varies only after the first batch: divided here, as
// before). True when the plan changed (the layout to be set again).
// whether projected_plan may find a coordinate to project (the outputs it reads not looked at)
static bool projected_possible(WlNv2aGx *gx, const DrawPlan *plan) {
    unsigned n;
    bool full;

    if (gx->pipeline.program == NULL || (plan->xf != NULL && plan->xf->full)) {
        return false;
    }
    xf_structure(gx, plan->live, &full);
    for (n = 0; n < plan->stage_count; n++) {
        unsigned stage = plan->stage_list[n];
        const StagePlan *sp = &plan->stages[stage];

        if (sp->spot ||
            (sp->projective && !sp->sphere && (gx->xf_dependencies.t[stage][3] & 0xFFFFu) != 0)) {
            return true;
        }
    }
    return false;
}

static bool projected_plan(WlNv2aGx *gx, DrawPlan *plan, WlGxDirectLayout *layout, unsigned runs) {
    const WlXboxVshOutput *o = gx->batch.outputs;
    unsigned n, r, kept = 0, count = 0, words, chosen = 0;
    bool full;

    if (gx->pipeline.program == NULL || (plan->xf != NULL && plan->xf->full) || runs < 2u) {
        return false;
    }
    xf_structure(gx, plan->live, &full);
    for (n = 0; n < plan->stage_count && count < WL_GX_DIRECT_PROJECTED; n++) {
        unsigned stage = plan->stage_list[n];
        const StagePlan *sp = &plan->stages[stage];

        if (sp->spot) {
            // (a spot: its face's projection, divided by GX where the direction varies)
            for (r = 1; r < runs && same4(o[r].t[stage], o[0].t[stage][0], o[0].t[stage][1], o[0].t[stage][2],
                                          o[r].t[stage][3]);
                 r++) {
            }
        } else if (!sp->projective || sp->sphere || (gx->xf_dependencies.t[stage][3] & 0xFFFFu) == 0) {
            continue;
        } else {
            for (r = 1; r < runs && o[r].t[stage][3] == o[0].t[stage][3]; r++) {
            }
        }
        if (r < runs) {
            // (the vertex's words within PACKED_WORDS: two projected cost nine words for four)
            words = plan->words - 2u * (count + 1u) + (count == 0 ? 3u : 3u * WL_GX_DIRECT_PROJECTED);
            if (words > PACKED_WORDS) {
                break;
            }
            chosen |= 1u << stage;
            count++;
        }
    }
    if (count == 0) {
        return false;
    }
    for (n = 0; n < plan->stage_count; n++) {
        unsigned stage = plan->stage_list[n];

        if (chosen & (1u << stage)) {
            plan->projected_list[plan->projected_count++] = stage;
            continue;
        }
        plan->stage_list[kept++] = stage;
    }
    plan->stage_count = kept;
    layout->texcoords &= (uint8_t)~chosen;
    layout->texcoords_projected = (uint8_t)chosen;
    plan->words += (count == 1u ? 3u : 3u * WL_GX_DIRECT_PROJECTED) - 2u * count;
    gx->stats.projected_draws++;
    return true;
}

static uint32_t backend_draw(void *user, const WlNv2aKelvin *k, const WlNv2aDraw *draw) {
    WlNv2aGx *gx = user;
    WlNv2aSurface surface;
    WlGxDirectLayout layout;
    DrawPlan plan;
    XfPlan xf;
    bool exact, xf_ok, indexed = false, count_pixels = wl_nv2a_k(k, WL_NV2A_K_SET_ZPASS_PIXEL_COUNT_ENABLE) != 0;
    const void *arrays[WL_GX_DIRECT_ARRAY_NORMAL + 1] = {NULL};
    float corners[3][2], area = 0.0f;
    uint32_t v, seen = 0, first = 0, run, attributes;
    unsigned stage;
    bool rebind, pixel;

    // (what changed since the last draw, kept until a draw applies it)
    gx->dirty |= k->dirty;
    gx->xf_why = WL_NV2A_GX_XF_WHYS;
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
    wl_gx_hang_where(0, "nv2a draws", gx->stats.draws);
    if ((wl_nv2a_k(k, WL_NV2A_K_SET_COLOR_MASK) & 0x01010101u) == 0 && wl_nv2a_k(k, WL_NV2A_K_SET_DEPTH_MASK) == 0) {
        // (nothing written: its pixel count alone, or nothing)
        display_textures_note(gx, k);
        if (count_pixels && gx->pipeline.program != NULL) {
            return unwritten_count(gx, k, draw, &surface);
        }
        if (!count_pixels) {
            gx->stats.unwritten_skipped++;
            return 0;
        }
    }
    // the state GX keeps from the last draw that applied it, applied again where it changed: the
    // raster state (its methods, the surface's depth buffer), the textures (theirs, the combiners'
    // stage programs; one changing within frames always), the pixel program (the combiners, the
    // textures bound, the fog)
    if (!gx->state_valid || (gx->dirty & (WL_NV2A_DIRTY_RASTER | WL_NV2A_DIRTY_SURFACE))) {
        gx->states_exact = states_apply(gx, k, &surface);
    } else {
        gx->stats.raster_kept++;
    }
    rebind = !gx->state_valid || gx->textures_volatile || (gx->dirty & (WL_NV2A_DIRTY_TEXTURES | WL_NV2A_DIRTY_COMBINERS));
    if (rebind) {
        textures_bind(gx, k);
    } else {
        gx->stats.textures_kept++;
    }
    // (the program is a function of the combiners, the fog and the textures as pixel_textures_same
    // reads them, which change only where they are bound: kept, with the TEV as it left it, unless
    // one of them changed)
    pixel = !gx->state_valid || (gx->dirty & WL_NV2A_DIRTY_PIXEL) != 0;
    if (rebind && !pixel_textures_same(gx)) {
        pixel = true;
    }
    if (pixel) {
        gx->tev_exact = tev_setup(gx, k);
    } else {
        gx->stats.combiners_kept++;
        if (!gx->last_used) {
            gx->stats.programs_unusable++;
        }
    }
    gx->state_valid = true;
    gx->dirty = 0;
    exact = gx->states_exact && gx->tev_exact;
    if (!exact) {
        gx->stats.approximations++;
    }

    memset(&layout, 0, sizeof layout);
    layout.colors = (gx->channels & 2u) ? 2u : (gx->channels & 1u) ? 1u : 0u;
    layout.texcoords = (uint8_t)(gx->texcoords & 0x1Fu);
    layout.texcoord_s = (uint8_t)(1u << WL_XBOX_PSH_FOG_TEXCOORD);

    plan.live = WL_XBOX_VSH_LIVE_POSITION;
    plan.xf = NULL;
    plan.stage_count = 0;
    plan.projected_count = 0;
    plan.colors = layout.colors;
    plan.channels = gx->channels;
    plan.texcoords = layout.texcoords;
    plan.words = 3u + layout.colors;
    if (layout.colors >= 1) {
        plan.live |= WL_XBOX_VSH_LIVE_D0;
    }
    if (layout.colors >= 2) {
        plan.live |= WL_XBOX_VSH_LIVE_D1;
    }
    for (stage = 0; stage < WL_NV2A_TEXTURES; stage++) {
        if (gx->texcoords & (1u << stage)) {
            stage_plan(&plan.stages[stage], gx->entries[stage], wl_nv2a_stage_program(k, stage));
            plan.stage_list[plan.stage_count++] = stage;
            plan.live |= (unsigned)WL_XBOX_VSH_LIVE_T0 << stage;
            plan.words += 2u;
        }
    }
    if (gx->texcoords & (1u << WL_XBOX_PSH_FOG_TEXCOORD)) {
        plan.live |= WL_XBOX_VSH_LIVE_FOG;
        plan.words += 1u;
    }
    plan.sx = (float)surface.samples_x;
    plan.sy = (float)surface.samples_y;
    plan.to_sx = power_of_two(surface.samples_x) ? 1.0f / plan.sx : 0.0f;
    plan.to_sy = power_of_two(surface.samples_y) ? 1.0f / plan.sy : 0.0f;
    plan.landing = count_pixels;
    plan.fused_known = false;
    plan.fused = NULL;
    plan.depth_max = surface.depth_max > 0.0f ? surface.depth_max : 16777215.0f;
    plan.scale = k->constants[WL_NV2A_CONSTANT_VIEWPORT_SCALE];
    plan.offset = k->constants[WL_NV2A_CONSTANT_VIEWPORT_OFFSET];

    gx->projection = projection_fit(gx, k, draw, plan.depth_max);
    if (gx->projection.perspective) {
        gx->stats.perspective_draws++;
    }
    projection_load(gx, k, &gx->projection);

    // the transform unit's draw, or the processor's (the transform unit's matrices then the identity)
    xf_ok = xf_prepare(gx, k, draw, &plan, count_pixels, &xf);
    if (xf_ok && !xf.full) {
        // (the position alone: the layout and the coordinates as the processor makes them)
        plan.xf = &xf;
        wl_gx_direct_layout(&layout);
        wl_gx_direct_position_matrix((const float (*)[4])xf.position);
        gx->xf_position = true;
        for (stage = 0; gx->xf_texgens != 0 && stage < 8; stage++) {
            if (gx->xf_texgens & (1u << stage)) {
                wl_gx_direct_texgen(stage, WL_GX_DIRECT_TG_TEXCOORD, NULL);
            }
        }
        gx->xf_texgens = 0;
        gx->stats.xf_position_draws++;
    } else if (xf_ok) {
        plan.xf = &xf;
        plan.words = xf.words;
        layout.texcoords = 0;
        for (stage = 0; stage < WL_NV2A_TEXTURES; stage++) {
            if (plan.texcoords & (1u << stage)) {
                if (xf.stage_source[stage] < WL_XBOX_VSH_INPUT_COUNT) {
                    layout.texcoords |= (uint8_t)(1u << stage);
                } else {
                    layout.texcoords_generated |= (uint8_t)(1u << stage);
                }
            }
        }
        layout.lit = (uint8_t)xf.lit;
        indexed = xf_arrays(gx, draw, &plan, &xf, &layout, arrays);
        if (indexed) {
            gx->xf_why = WL_NV2A_GX_XF_INDEXED;
        }
        wl_gx_direct_layout(&layout);
        if (xf.lit != 0) {
            wl_gx_direct_lighting(&xf.lighting);
            gx->stats.xf_lit_draws++;
            if (indexed) {
                wl_gx_direct_array(WL_GX_DIRECT_ARRAY_NORMAL, arrays[WL_GX_DIRECT_ARRAY_NORMAL],
                                   wl_nv2a_vcache_element_bytes(WL_NV2A_VCACHE_NORMAL));
            }
        }
        for (stage = 0; indexed && stage < WL_GX_DIRECT_ARRAY_TEXCOORD0 + WL_NV2A_TEXTURES; stage++) {
            if (layout.indexed & (1u << stage)) {
                wl_gx_direct_array(stage, arrays[stage],
                                   wl_nv2a_vcache_element_bytes(stage == WL_GX_DIRECT_ARRAY_POSITION ? WL_NV2A_VCACHE_POSITION
                                                                : stage < WL_GX_DIRECT_ARRAY_TEXCOORD0 ? WL_NV2A_VCACHE_COLOR
                                                                                                       : WL_NV2A_VCACHE_TEXCOORD));
            }
        }
        if (indexed && wl_nv2a_vcache_made(&gx->vertices)) {
            wl_gx_direct_arrays_changed();
        }
        wl_gx_direct_position_matrix((const float (*)[4])xf.position);
        gx->xf_position = true;
        for (stage = 0; stage < WL_NV2A_TEXTURES; stage++) {
            if (plan.texcoords & (1u << stage)) {
                wl_gx_direct_texgen(stage,
                                    xf.stage_source[stage] < WL_XBOX_VSH_INPUT_COUNT ? WL_GX_DIRECT_TG_TEXCOORD
                                                                                      : WL_GX_DIRECT_TG_POSITION,
                                    (const float (*)[4])xf.stage_matrix[stage]);
                gx->xf_texgens |= (uint8_t)(1u << stage);
            }
        }
        gx->stats.xf_draws++;
        gx->stats.xf_vertices += draw->count;
    } else {
        memset(&xf, 0, sizeof xf);
        wl_gx_direct_layout(&layout);
        if (gx->xf_position) {
            wl_gx_direct_position_matrix(NULL);
            gx->xf_position = false;
        }
        for (stage = 0; gx->xf_texgens != 0 && stage < 8; stage++) {
            if (gx->xf_texgens & (1u << stage)) {
                wl_gx_direct_texgen(stage, WL_GX_DIRECT_TG_TEXCOORD, NULL);
            }
        }
        gx->xf_texgens = 0;
    }
    gx->stats.xf_why_draws[gx->xf_why]++;
    gx->stats.xf_why_vertices[gx->xf_why] += draw->count;

    if (indexed) {
        // (the vertices' indices alone, and the colours' words the transform unit's draw sends)
        uint32_t words[2];
        unsigned colors = 0, c;

        for (c = 0; c < plan.colors && c < 2u; c++) {
            if (!(layout.indexed & (1u << (WL_GX_DIRECT_ARRAY_COLOR0 + c)))) {
                words[colors++] = xf.color_word[c];
            }
        }
        wl_gx_direct_primitive_begin(gx_primitive(draw->primitive), draw->count);
        wl_gx_direct_vertices_index(draw->indices, draw->index_base, draw->count, words, colors);
        wl_gx_direct_primitive_end();
        gx->stats.indexed_draws++;
        gx->stats.indexed_vertices += draw->count;
        return 0;
    }
    gx->memo_stamp++;
    // (the fetch decided once for the draw: the attributes the program reads, the current values'
    // written once into every run's inputs)
    attributes = plan.xf == NULL ? wl_nv2a_pipeline_inputs(&gx->pipeline, plan.live)
                 : xf.full         ? xf.attributes
                                   : wl_nv2a_pipeline_inputs(&gx->pipeline, plan.live) | 1u;
    wl_nv2a_fetch_plan(draw, attributes, &gx->fetch);
    wl_nv2a_fetch_constants(&gx->fetch, k, gx->batch.inputs, WL_XBOX_VSH_BATCH);
    // (the first batch, which finds the projective coordinates GX is to divide at each pixel)
    if (draw->count != 0 &&
        vertices_make(gx, k, draw, 0, draw->count < WL_XBOX_VSH_BATCH ? draw->count : WL_XBOX_VSH_BATCH, &plan,
                      &layout)) {
        wl_gx_direct_layout(&layout);
    }
    wl_gx_direct_primitive_begin(gx_primitive(draw->primitive), draw->count);
    for (first = 0; first < draw->count; first += run) {
        run = draw->count - first < WL_XBOX_VSH_BATCH ? draw->count - first : WL_XBOX_VSH_BATCH;
        if (first != 0) {
            vertices_make(gx, k, draw, first, run, &plan, NULL);
        }
        // (the batch's vertices into the FIFO in one call)
        wl_gx_direct_vertices_words(gx->batch.words, run, plan.words);
        for (v = 0; count_pixels && v < run; v++) {
            landing_area(draw->primitive, batch_vertex(gx, v), &seen, corners, &area);
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
    if (colour && (flags & WL_NV2A_CLEAR_COLOR) != WL_NV2A_CLEAR_COLOR) {
        clear_channels(gx, flags, rgba, x0, y0, x1, y1);
        if (!depth) {
            return;
        }
        colour = false;
    }
    wl_gx_target_clear(colour, depth, rgba, z, (int)x0, (int)y0, (int)(x1 - x0), (int)(y1 - y0));
    gx->stats.quad_clears++;
    device_state(gx);
}

// ---- frames and flips -------------------------------------------------------------------------------

static void backend_frame_done(void *user, const WlNv2aKelvin *k) {
    WlNv2aGx *gx = user;

    (void)k;
    gx->state_valid = false;
    if (gx->frame_open && gx->back_drawn) {
        wl_nv2a_gx_present(gx);
    }
}

static void backend_flip(void *user, const WlNv2aKelvin *k, uint32_t phys) {
    WlNv2aGx *gx = user;

    (void)k;
    // (a surface shown: textures at its address are looked at again)
    gx->state_valid = false;
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
