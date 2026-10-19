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
#include "wl/platform/gx_shadow.h"
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
    // ...and for its transform unit's matrices (xf_prepare)
    XF_PROBES = 8,
    TG_CONSTANTS = 16,
    // programs whose analyses are kept
    XF_ANALYSES = 8,
    // a stand-in texture's image: 4 x 4 RGBA8, one GX tile
    STANDIN_BYTES = 64,
    // the made vertices GX reads by index (made_begin): the ring's bytes and its chunks, each written
    // again only once GX is past the fence sent behind its last draw; a draw's vertices are at most
    // half the ring (a larger one sends its words)
    MADE_RING_BYTES = 512u << 10,
    MADE_CHUNKS = 16,
    MADE_CHUNK_BYTES = MADE_RING_BYTES / MADE_CHUNKS
};

// how far (in the depth's 0..1) a fit made with the canonical points may be from a draw's own
// depths: a few of the depth buffer's 24-bit steps, the rounding of a program's depth
#define FIT_TOLERANCE (8.0f / 16777216.0f)
// how far the camera's slope with a draw's own offset may be from its vertices' depths for a draw
// whose vertices alone make its fit (projection_fit): beyond it, the draw has a projection of its own
#define NARROW_TOLERANCE (512.0f / 16777216.0f)
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
    // the kept frame (frame_keep): the last finished back buffer, at that buffer's address
    bool kept;
    // the image's width (GX's texture): `width` but for a kept frame of a picture drawn narrow, which
    // holds the picture's columns alone (a texture reads it as `width` wide: its coordinates are fractions)
    uint32_t image_w;
    WlGxTarget target;
} RenderTarget;

typedef struct Projection {
    bool perspective;
    float a, b;
} Projection;

// a vertex as GX takes it from the FIFO in the draw's layout (wl_gx_direct_vertex_words: the
// position's three floats, the colours as 0xRRGGBBAA, the texture coordinates' floats; at most
// 3 + 2 + 4 * 2 + 1 words, the fog's coordinate last, or 3 + 9 + 2 + 3 * 2 for a lit form's normal
// beside a sphere map's direction: sphere_xf_plan), and where it lands in EFB pixels. (The words
// first: a pointer to them is one to the vertex.)
enum { PACKED_WORDS = 20 };
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
    uint64_t presented_us; // the clock at the last present (0: none yet)
    // the GX mode that culls triangles of positive area in the screen's coordinates (y down)
    WlGxCull cull_positive;
    bool cull_measured;
    // the EFB's owner (NULL: the back buffer) and the targets
    RenderTarget targets[TARGETS];
    RenderTarget *owner;
    // the title samples a picture it showed (a display surface as a texture): each finished back
    // buffer is copied into a target at its address
    bool keep_frames;
    bool park_whole; // wl_gx_target_set_park_whole: the picture being drawn read from a target
    // the targets that read it (their size and format, as Halo's blur makes the same ones in every
    // level): the back buffer is parked whole for those alone (a whole park is 1.2 MB out of the EFB,
    // and d40 parks 30 times a frame for its shadows' 128x128 targets, which draw over a corner of it)
    struct {
        uint32_t w, h;
        uint8_t format;
    } readers[8];
    unsigned reader_count;
    uint32_t view_w, view_h;
    // a 4:3 picture on a 16:9 screen drawn narrow (wl_gx_draw_narrow, in the EFB's last columns, the targets
    // drawn beside it): the back buffer's x (0 to
    // cfg.width) is the EFB's column narrow_x + x * narrow_w / cfg.width, its draws' viewport and
    // scissor, its clears, its reads (the parked picture and kept frames, narrow_w wide; the
    // occlusion queries' depth) alike; the render targets as ever
    bool narrow;
    uint32_t narrow_x, narrow_w;
    // the draw's
    const WlNv2aTexEntry *entries[WL_NV2A_TEXTURES];
    WlNv2aTexEntry target_entries[WL_NV2A_TEXTURES];
    // what is bound for a texture that could not be made (standin_bind): a 4 x 4 image of its
    // average colour a stage, in `standin_memory` 32-byte aligned for GX; a draw with one that
    // standin_hides is not drawn (`standin_hidden`, set by textures_bind)
    WlNv2aTexEntry standin_entries[WL_NV2A_TEXTURES];
    struct {
        uint32_t phys, format_word, rgba;
        bool made;
    } standins[WL_NV2A_TEXTURES];
    uint8_t standin_memory[WL_NV2A_TEXTURES * STANDIN_BYTES + 32];
    bool standin_hidden;
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
    // the camera's slope (wl_nv2a_gx_slope_keep), for a draw whose vertices alone span too little
    WlNv2aGxSlope slope;
    // the fits kept by their transform's signature: the clip-space positions the program gives the
    // canonical inputs, bit for bit
    struct FitSignature {
        uint32_t clip[CANONICAL_POINTS][4];
        float a, b;
        uint32_t used;
        bool narrow; // (made with the camera's slope: shared within NARROW_TOLERANCE only)
        uint32_t made; // (the fit's stamp: signature_made when it was written)
        // (the draw of this frame that last shared or made it: its depth range, target and viewport;
        // fit_slot_holds)
        uint32_t held_frame, held_target;
        float held_depth_max, held_viewport[2][4];
        bool held;
    } signatures[FIT_SETS][FIT_WAYS];
    uint32_t signature_use, signature_made;
    // the last draw's fit, kept for the next draws of the same program, constants, depth range and
    // target in this frame (fit_memo): its transform is theirs, so is its fit
    struct FitMemo {
        const WlNv2aKelvin *k;
        const WlXboxVshProgram *program;
        uint32_t serial, constants, target, frame, made;
        float depth_max, point_size, a, b;
        struct FitSignature *slot;
        bool valid;
    } fit_memo;
    // the program's outputs for fixed inputs (xf_prepare's probes, projection_fit's canonical points),
    // kept with what they are a function of: the program (its serial: another compiled in its place is
    // another), the constants (the tracker's generation, which moves at every change), the outputs asked
    // for and the point size. Most draws keep the draw before's program and constants (a10's 72%, d40's
    // 57%), whose probes are then not run again. device_state forgets them (draw_cuts false: never kept)
    struct ProbeKey {
        const WlNv2aKelvin *k;
        const WlXboxVshProgram *program;
        uint32_t serial, constants;
        unsigned live;
        float point_size;
        bool valid;
    } xf_probe_key, fit_probe_key, tg_probe_key;
    WlXboxVshOutput xf_probe_out[XF_PROBES], fit_probe_out[CANONICAL_POINTS], tg_probe_out[XF_PROBES];
    // (texgen_xf_plan's probe vertices, and the constants its outputs read when it ran them: kept while
    // those are the same, as a skinned draw's bones change from draw to draw and they do not)
    float tg_probe_in[XF_PROBES][WL_NV2A_ATTRIBS][4];
    struct {
        const WlXboxVshProgram *program;
        uint32_t serial;
        unsigned live;
        int count;
        unsigned char registers[TG_CONSTANTS];
        float values[TG_CONSTANTS][4];
        bool valid;
    } tg_kept;
    bool last_shared;
    // (the last draw's fit made with the camera's slope, made or kept; its depth_check)
    bool last_narrow;
    WlNv2aGxDepthCheck depth_check;
    // the EFB as GX has drawn it is readable (wl_gx_sync since the last draw, clear or frame): an
    // occlusion query's depth test (query_count) reads it
    bool query_synced;
    // (stamp: the draw; memo_batch: the batch that last used the slot, which no other index may then
    // take until it is sent; memo_vertices: the slot's vertex. Apart, the tags a lookup reads are four
    // to a 32-byte cache line, a strip's neighbouring indices in one, and each vertex starts a line:
    // on the Wii a vertex beside its tags missed the cache on most lookups, 1.8% of a30's time)
    struct WlNv2aGxMemo {
        uint32_t index, stamp;
    } memo[MEMO];
    uint32_t memo_batch[MEMO];
    PackedVertex *memo_vertices; // (memo_space's first 32-byte boundary)
    PackedVertex memo_space[MEMO + 1];
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
    // The processor's vertices GX reads by index (made_begin): a draw's made vertices in a ring GX
    // reads, a vertex in a slot of its own (its PackedVertex: each attribute's array is the slots'
    // words at the attribute's place, the stride a slot), the draw's vertices from `base` on, so that
    // each is written once and a vertex sent again costs an index an attribute.
    struct {
        PackedVertex *ring; // (MADE_RING_BYTES, 32-byte aligned; NULL: none made yet, or none to be had)
        bool tried;
        uint32_t pos;     // where the next draw's slots begin (bytes, a 32-byte boundary)
        uint32_t fence[MADE_CHUNKS]; // the fence behind each chunk's last draw (0: none to wait for)
        uint32_t entered; // chunks entered since the ring began again (their fences met, GX's cache emptied)
        uint32_t pending; // chunks written since the last fence
        // the draw's: its vertices made into the ring, sent by index; its slots and the next one
        bool ring_on, indexed;
        PackedVertex *base;
        uint32_t next, limit, batch_first, made;
        uint16_t memo[MEMO];                // (ring_on: a memo slot's vertex, the draw's slot)
        uint32_t index[WL_XBOX_VSH_BATCH];  // the batch's vertices' slots
    } made;
    // a fused batch's state (the draw's)
    WlXboxVshFused fused;
    // the draw's fetch
    WlNv2aFetchPlan fetch;
    // the draw asked for whole by draw_prefetch (its stats.draws), and the plan it was asked for by
    struct {
        uint32_t draw;
        const WlNv2aFetchPlan *plan;
    } prefetched;
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
    // the last draw's layout and its vertex's FIFO bytes (draw_bytes)
    WlGxDirectLayout bytes_layout;
    uint32_t layout_bytes;
};

// the draw path's cuts (wl_nv2a_gx_set_draw_cuts)
static bool g_draw_cuts = true;

void wl_nv2a_gx_set_draw_cuts(bool on) {
    g_draw_cuts = on;
}

// ---- creation -----------------------------------------------------------------------------------

static void commit(void *user, void *memory, size_t bytes) {
    (void)user;
    wl_gpu_buffer_commit(memory, bytes);
}

static void gpu_wait(void) {
    (void)wl_gx_sync();
}

// The draw path's switches of its second round (wl_nv2a_gx_set_switches): each turns one cut off.
enum {
    SWITCH_NO_VCACHE_WAIT = 1u << 0, // "novcwait": the vertex arrays' pending copies wait for the frame's end
    SWITCH_NO_PAIRED_COLOR = 1u << 1, // "nopscolor": the Wii's colours a component at a time (wl_nv2a_color_word)
    SWITCH_NO_FIT_MEMO = 1u << 2, // "nofitmemo": every draw's projection fitted from its vertices (fit_memo)
    SWITCH_NO_COLOR_REGISTER = 1u << 3, // "nocolorreg": an indexed draw's constant colour sent with each vertex
    SWITCH_NO_POSITION_INDEX = 1u << 4, // "noposindex": a position-alone draw's positions sent with each vertex
    SWITCH_NO_SPHERE_XF = 1u << 5, // "nospherexf": a sphere map's coordinates the processor's (sphere_xf_plan)
    SWITCH_PARK_ALL = 1u << 6, // "parkall": once a target read the picture, every park whole (not only for the readers)
    SWITCH_NO_LINE_PREFETCH = 1u << 7, // "vtxprefetch": the vertices' lines asked for vertex by vertex (draw_prefetch)
    SWITCH_NO_PSH_RECENT = 1u << 8, // "nopshrecent": each pixel program looked up by its structure (tev_setup)
    // "parklate": a target of a size that reads the picture known as one only after its first read (not already at
    // its first draw that reads it: the picture parked in part then, the first blur of each size wrong for a frame)
    SWITCH_PARK_LATE = 1u << 9,
    SWITCH_NO_WORLD_XF = 1u << 10, // "noworldxf": a skinned position the processor's (world_xf_plan)
    SWITCH_NO_MADE_INDEX = 1u << 11, // "nomadeindex": the processor's vertices sent whole, each time (made_begin)
    SWITCH_NO_PAGE_HASH = 1u << 12, // "nopagehash": a vertex array's source hashed whole at a check (nv2a_vcache.h)
    SWITCH_NO_INDEX_BOUNDS = 1u << 13, // "noindexbounds": a draw's least and greatest index by a pass at the draw
    SWITCH_NO_XF_TEXGEN = 1u << 14, // "noxftexgen": a fused form's coordinates the processor's (texgen_xf_plan)
};

static const struct {
    const char *file, *env;
    unsigned bit;
} k_switches[] = {
    {"novcwait", "WL_XB_NOVCWAIT", SWITCH_NO_VCACHE_WAIT},
    {"nopscolor", "WL_XB_NOPSCOLOR", SWITCH_NO_PAIRED_COLOR},
    {"nofitmemo", "WL_XB_NOFITMEMO", SWITCH_NO_FIT_MEMO},
    {"nocolorreg", "WL_XB_NOCOLORREG", SWITCH_NO_COLOR_REGISTER},
    {"noposindex", "WL_XB_NOPOSINDEX", SWITCH_NO_POSITION_INDEX},
    {"nospherexf", "WL_XB_NOSPHEREXF", SWITCH_NO_SPHERE_XF},
    {"parkall", "WL_XB_PARKALL", SWITCH_PARK_ALL},
    {"vtxprefetch", "WL_XB_VTXPREFETCH", SWITCH_NO_LINE_PREFETCH},
    {"nopshrecent", "WL_XB_NOPSHRECENT", SWITCH_NO_PSH_RECENT},
    {"parklate", "WL_XB_PARKLATE", SWITCH_PARK_LATE},
    {"noworldxf", "WL_XB_NOWORLDXF", SWITCH_NO_WORLD_XF},
    {"nomadeindex", "WL_XB_NOMADEINDEX", SWITCH_NO_MADE_INDEX},
    {"nopagehash", "WL_XB_NOPAGEHASH", SWITCH_NO_PAGE_HASH},
    {"noindexbounds", "WL_XB_NOINDEXBOUNDS", SWITCH_NO_INDEX_BOUNDS},
    {"noxftexgen", "WL_XB_NOXFTEXGEN", SWITCH_NO_XF_TEXGEN},
};

static unsigned g_switches_off;
// (WL_XB_FITVERIFY on the host: projection_fit's kept fits checked against the vertices' at each use)
static bool g_fit_verify;

void wl_nv2a_gx_set_switches(const char *title_dir) {
    char path[WL_FS_PATH_MAX];

    g_switches_off = 0;
    for (size_t i = 0; i < sizeof k_switches / sizeof k_switches[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", title_dir, k_switches[i].file);
        if (wl_platform_fs_exists(path)) {
            g_switches_off |= k_switches[i].bit;
            WL_LOG_I(TAG, "draw path: %s (a cut off)", k_switches[i].file);
        }
    }
}

static bool switched_on(unsigned bit) {
    return (g_switches_off & bit) == 0;
}

// A render target's image: a free piece of the textures' arena when it has one (no texture evicted
// for it: the heap, short during a level, keeps what it would have lent), else the heap's
// (targets_on_heap: always the heap's).
static void *target_alloc(WlNv2aGx *gx, size_t bytes) {
    void *p = wl_nv2a_gx_spare_alloc(gx, bytes);
    return p != NULL ? p : gx->textures.cfg.alloc(gx->textures.cfg.user, bytes);
}

static void target_release(WlNv2aGx *gx, void *memory) {
    if (!wl_nv2a_texcache_spare_free(&gx->textures, memory)) {
        gx->textures.cfg.release(gx->textures.cfg.user, memory);
    }
}

WlNv2aGx *wl_nv2a_gx_create(const WlNv2aGxConfig *cfg) {
    WlNv2aGx *gx = calloc(1, sizeof *gx);
    WlNv2aTexCacheConfig textures;
    WlNv2aVCacheConfig vertices;

#if !defined(__wii__)
    g_fit_verify = getenv("WL_XB_FITVERIFY") != NULL;
    if (getenv("WL_XB_NODRAWCUTS") != NULL) {
        g_draw_cuts = false;
    }
    for (size_t i = 0; i < sizeof k_switches / sizeof k_switches[0]; i++) {
        if (getenv(k_switches[i].env) != NULL) {
            g_switches_off |= k_switches[i].bit;
        }
    }
#endif

    if (gx == NULL) {
        return NULL;
    }
    gx->layout_bytes = wl_gx_shadow_layout_vertex_bytes(&gx->bytes_layout);
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
    gx->memo_vertices = (PackedVertex *)(void *)(((uintptr_t)gx->memo_space + 31u) & ~(uintptr_t)31u);
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
    if (vertices.gpu_wait == NULL && switched_on(SWITCH_NO_VCACHE_WAIT)) {
        vertices.gpu_wait = gpu_wait;
    }
    vertices.whole_hash = vertices.whole_hash || !switched_on(SWITCH_NO_PAGE_HASH);
    wl_nv2a_set_index_bounds(switched_on(SWITCH_NO_INDEX_BOUNDS));
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
    wl_gx_target_set_park_whole(false);
    // (a 16:9 screen's pillarbox drawn narrow, not narrowed by a copy of each finished frame; the
    // switch "pillarbox", WlGxBisect.pillarbox, the copy)
    if (wl_gx_draw_narrow(true) && wl_gx_target_picture(&gx->narrow_x, &gx->narrow_w)) {
        gx->narrow = true;
        WL_LOG_I(TAG, "16:9: the picture drawn into columns %u..%u", (unsigned)gx->narrow_x,
                 (unsigned)(gx->narrow_x + gx->narrow_w));
    }
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
    if (gx->narrow) {
        (void)wl_gx_draw_narrow(false);
    }
    wl_nv2a_texcache_clear(&gx->textures);
    wl_nv2a_vcache_clear(&gx->vertices);
    if (gx->made.ring != NULL) {
        gx->textures.cfg.release(gx->textures.cfg.user, gx->made.ring);
    }
    for (i = 0; i < TARGETS; i++) {
        if (gx->targets[i].memory != NULL) {
            target_release(gx, gx->targets[i].memory);
        }
    }
    free(gx);
}

void wl_nv2a_gx_host_bytes(const WlNv2aGx *gx, size_t *record, size_t *targets, unsigned *target_count) {
    unsigned i;

    *record = sizeof *gx + (gx->made.ring != NULL ? MADE_RING_BYTES : 0u);
    *targets = 0;
    *target_count = 0;
    for (i = 0; i < TARGETS; i++) {
        if (gx->targets[i].memory != NULL) {
            if (!wl_nv2a_texcache_spare_owns(&gx->textures, gx->targets[i].memory)) {
                *targets += gx->targets[i].bytes;
            }
            ++*target_count;
        }
    }
}

void *wl_nv2a_gx_spare_alloc(WlNv2aGx *gx, size_t bytes) {
    return gx->cfg.targets_on_heap ? NULL : wl_nv2a_texcache_spare_alloc(&gx->textures, bytes);
}

bool wl_nv2a_gx_spare_free(WlNv2aGx *gx, void *memory) {
    return wl_nv2a_texcache_spare_free(&gx->textures, memory);
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

bool wl_nv2a_gx_last_depth_check(const WlNv2aGx *gx, WlNv2aGxDepthCheck *out) {
    if (out != NULL) {
        *out = gx->depth_check;
    }
    return gx->depth_check.checked != 0;
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
    gx->xf_probe_key.valid = gx->fit_probe_key.valid = gx->tg_probe_key.valid = false;
    gx->fit_memo.valid = false;
}

// whether the program's outputs kept under `key` are those it gives now for `live` (and, false, the key
// set for the outputs about to be made)
static bool probes_kept(WlNv2aGx *gx, struct ProbeKey *key, const WlNv2aKelvin *k, unsigned live) {
    const WlXboxVshProgram *program = gx->pipeline.program;

    if (key->valid && key->k == k && key->program == program && key->serial == program->serial &&
        key->constants == k->constants_generation && key->live == live &&
        key->point_size == gx->pipeline.point_size) {
        gx->stats.probes_kept++;
        return true;
    }
    key->k = k;
    key->program = program;
    key->serial = program->serial;
    key->constants = k->constants_generation;
    key->live = live;
    key->point_size = gx->pipeline.point_size;
    key->valid = g_draw_cuts;
    return false;
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
            target_release(gx, r->memory);
            memset(r, 0, sizeof *r);
        }
    }
}

// While the processor waits for the GPU at the frame's end: a texture the frame budget put off made
// (wl_nv2a_texcache_work), so that the next frame finds it.
static bool textures_work(void *user) {
    WlNv2aGx *gx = user;
    bool made, more = wl_nv2a_texcache_work(&gx->textures, gx->dev, &made);

    if (made) {
        gx->texture_memory_changed = true;
    }
    return more;
}

// the frame's time, from the last present to this one
static void frame_time(WlNv2aGx *gx) {
    static const uint32_t bounds[5] = {34000u, 67000u, 100000u, 150000u, 250000u};
    uint64_t now = wl_platform_time_us();
    uint32_t us;
    unsigned i;

    if (gx->presented_us != 0) {
        us = (uint32_t)(now - gx->presented_us);
        for (i = 0; i < 5 && us >= bounds[i]; i++) {
        }
        gx->stats.frames_by_time[i]++;
        if (us > gx->stats.frame_us_worst) {
            gx->stats.frame_us_worst = us;
        }
    }
    gx->presented_us = now;
}

void wl_nv2a_gx_present(WlNv2aGx *gx) {
    if (!gx->frame_open) {
        return;
    }
    use_back_buffer(gx);
    frame_keep(gx);
    wl_gx_invalidate_shadow();
    if (gx->textures.deferred_count != 0) {
        wl_gx_set_wait_work(textures_work, gx);
    }
    wl_gx_end_frame();
    wl_gx_set_wait_work(NULL, NULL);
    frame_time(gx);
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
// the least recently used slot made it (NULL without memory, or when that slot owns the EFB); its
// image `image_w` wide (RenderTarget.image_w: `w` but for a kept frame drawn narrow)
static RenderTarget *slot_for_image(WlNv2aGx *gx, uint32_t phys, uint32_t w, uint32_t h, uint32_t image_w,
                                    uint8_t format) {
    RenderTarget *slot = NULL;
    unsigned i;

    for (i = 0; i < TARGETS; i++) {
        RenderTarget *t = &gx->targets[i];

        if (t->memory != NULL && t->phys == phys && t->width == w && t->height == h && t->format == format &&
            t->image_w == image_w) {
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
        target_release(gx, slot->memory);
    }
    memset(slot, 0, sizeof *slot);
    slot->bytes = wl_gx_tex_bytes((WlGxTexFormat)format, image_w, h);
    slot->memory = target_alloc(gx, slot->bytes);
    if (slot->memory == NULL) {
        return NULL;
    }
    slot->phys = phys;
    slot->width = w;
    slot->height = h;
    slot->image_w = image_w;
    slot->format = format;
    slot->used_frame = gx->frame;
    slot->target.id = slot;
    slot->target.memory = slot->memory;
    slot->target.width = (uint16_t)image_w;
    slot->target.height = (uint16_t)h;
    slot->target.format = format;
    slot->target.user = phys;
    return slot;
}

static RenderTarget *slot_for(WlNv2aGx *gx, uint32_t phys, uint32_t w, uint32_t h, uint8_t format) {
    return slot_for_image(gx, phys, w, h, w, format);
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
    // (the picture's columns alone when it is drawn narrow: a quarter fewer bytes, the texture's
    // coordinates fractions of its width as ever)
    uint32_t x = gx->narrow ? gx->narrow_x : 0u, image_w = gx->narrow ? gx->narrow_w : w;
    RenderTarget *t;
    unsigned i;

    if (!gx->keep_frames || !gx->back_drawn || gx->back_phys == 0 || gx->owner != NULL) {
        return;
    }
    // (one kept frame, the last shown, at the address of the buffer that held it: a title reads the
    // picture before the one it draws, and with two buffers that is the other one's; one per buffer
    // would be a second megabyte that only a read of an older picture uses)
    for (t = NULL, i = 0; i < TARGETS && t == NULL; i++) {
        if (gx->targets[i].kept && gx->targets[i].memory != NULL && gx->targets[i].width == w &&
            gx->targets[i].height == h && gx->targets[i].image_w == image_w && &gx->targets[i] != gx->owner) {
            t = &gx->targets[i];
        }
    }
    if (t != NULL) {
        t->phys = gx->back_phys;
        t->target.user = gx->back_phys;
        t->used_frame = gx->frame;
    } else {
        t = slot_for_image(gx, gx->back_phys, w, h, image_w, (uint8_t)WL_GX_TEX_RGBA8);
        if (t == NULL) {
            return;
        }
        t->kept = true;
    }
    if (t->target.resident) {
        wl_gx_copy_efb_texture_again((int)x, 0, (int)image_w, (int)h, t->memory, WL_GX_TEX_RGBA8, false);
    } else {
        wl_gx_copy_efb_texture((int)x, 0, (int)image_w, (int)h, t->memory, WL_GX_TEX_RGBA8, false);
    }
    t->guest_bytes = w * h * 4u;
    target_image_made(gx, t);
    gx->stats.frames_kept++;
    gx->texture_memory_changed = true;
}

static bool is_display(const WlNv2aGx *gx, uint32_t phys);

// whether a target is one of those that read the picture being drawn (gx->readers: its size and format)
static bool target_reads_picture(const WlNv2aGx *gx, const RenderTarget *t) {
    unsigned i;

    for (i = 0; i < gx->reader_count; i++) {
        if (gx->readers[i].w == t->width && gx->readers[i].h == t->height && gx->readers[i].format == t->format) {
            return true;
        }
    }
    return false;
}

// the EFB's column of the back buffer's column `x` (0 to cfg.width) of a picture drawn narrow
static uint32_t back_column(const WlNv2aGx *gx, uint32_t x) {
    return gx->narrow_x + (uint32_t)(((uint64_t)x * gx->narrow_w + gx->cfg.width / 2u) / gx->cfg.width);
}

// The viewport over `vw` by `vh` pixels of the EFB's owner and the scissor [x0, x1) by [y0, y1) of
// them: on the back buffer of a picture drawn narrow, within its columns
static void view_set(const WlNv2aGx *gx, float vw, float vh, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
    if (gx->narrow && gx->owner == NULL) {
        wl_gx_direct_viewport((float)gx->narrow_x, 0.0f, vw * (float)gx->narrow_w / (float)gx->cfg.width, vh);
        x0 = back_column(gx, x0);
        x1 = back_column(gx, x1);
    } else {
        wl_gx_direct_viewport(0.0f, 0.0f, vw, vh);
    }
    wl_gx_direct_scissor(x0, y0, x1 - x0, y1 - y0);
}

// A clear quad of the EFB's owner (wl_gx_target_clear), the rectangle [x0, x1) by [y0, y1) of it:
// within the picture's columns on the back buffer of one drawn narrow
static void clear_rect(const WlNv2aGx *gx, bool colour, bool depth, uint32_t rgba, float z, uint32_t x0, uint32_t y0,
                       uint32_t x1, uint32_t y1) {
    if (gx->narrow && gx->owner == NULL) {
        x0 = back_column(gx, x0);
        x1 = back_column(gx, x1);
    }
    wl_gx_target_clear(colour, depth, rgba, z, (int)x0, (int)y0, (int)(x1 - x0), (int)(y1 - y0));
}

// The EFB for the surface the next draw or clear is into: the frame begun, the back buffer or a
// render target its owner, the viewport and scissor. false when the surface cannot be drawn.
// whether a draw's stages sample the picture being drawn (a texture at the back buffer's address: Halo's blur
// of the picture, into a target)
static bool draw_reads_picture(const WlNv2aGx *gx, const WlNv2aKelvin *k) {
    unsigned stage;

    if (k == NULL || !gx->frame_open || !gx->back_drawn || gx->back_phys == 0) {
        return false;
    }
    for (stage = 0; stage < WL_NV2A_TEXTURES; stage++) {
        WlNv2aTexture t;

        if (wl_nv2a_stage_program(k, stage) != WL_NV2A_STAGE_NONE && wl_nv2a_texture(k, stage, &t) &&
            t.phys == gx->back_phys) {
            return true;
        }
    }
    return false;
}

// a target of `t`'s size and format known to read the picture being drawn (gx->readers): the back buffer parked
// whole for it from now on
static void reader_add(WlNv2aGx *gx, const RenderTarget *t, uint32_t parked_w, uint32_t parked_h) {
    if (target_reads_picture(gx, t) || gx->reader_count >= sizeof gx->readers / sizeof gx->readers[0]) {
        return;
    }
    gx->readers[gx->reader_count].w = t->width;
    gx->readers[gx->reader_count].h = t->height;
    gx->readers[gx->reader_count].format = t->format;
    gx->reader_count++;
    gx->park_whole = true;
    wl_gx_target_set_park_whole(true);
    WL_LOG_I(TAG, "the picture being drawn (%08x) is read while a target of %ux%u has the EFB (%ux%u of it parked): "
                  "the back buffer parked whole for targets of that size from now on",
             (unsigned)gx->back_phys, (unsigned)t->width, (unsigned)t->height, (unsigned)parked_w, (unsigned)parked_h);
}

// `k`: the draw's state (NULL for a clear)
static bool surface_prepare(WlNv2aGx *gx, const WlNv2aSurface *s, const WlNv2aKelvin *k) {
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
            // (a target whose first draw reads the picture: one of its readers from now on, and the picture
            // parked whole for it at once; with the switch parklate, from its read on: target_texture)
            if (switched_on(SWITCH_PARK_LATE) && !target_reads_picture(gx, t) && draw_reads_picture(gx, k)) {
                reader_add(gx, t, 0, 0);
            }
            if (gx->park_whole && switched_on(SWITCH_PARK_ALL)) {
                // (the back buffer parked whole for a target that reads it: target_texture)
                wl_gx_target_set_park_whole(target_reads_picture(gx, t));
            }
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
    view_set(gx, (float)gx->view_w, (float)gx->view_h, x0, y0, x1, y1);
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
    WlGxDirectLayout layout = {1, 0, 0, 0, 0, 0, 0, 0, 0};
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
    WlGxDirectLayout layout = {1, 0, 0, 0, 0, 0, 0, 0, 0};
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
    view_set(gx, (float)gx->cfg.width, (float)gx->cfg.height, 0, 0, gx->view_w, gx->view_h);
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

// whether a texture at `phys` is the picture being drawn, read while a target has the EFB: its
// colour comes from the back buffer's park (target_texture), never from a frame kept before
static bool reads_picture_drawn(const WlNv2aGx *gx, uint32_t phys) {
    return gx->owner != NULL && gx->frame_open && gx->back_drawn && phys == gx->back_phys && phys != 0;
}

// a picture the title showed, read as a texture (not the one being drawn, which parks serve):
// each finished back buffer kept as one from the next frame on
static void display_read(WlNv2aGx *gx, uint32_t phys) {
    if (!gx->keep_frames && is_display(gx, phys) && !reads_picture_drawn(gx, phys)) {
        gx->keep_frames = true;
        WL_LOG_I(TAG, "a displayed surface (%08x) is read as a texture: each frame is kept as one", (unsigned)phys);
    }
}

// a resolved render target at a texture's address, as an entry the stages read like any texture;
// or the back buffer of the frame being drawn, while a target has the EFB: its colour as it was
// parked, the picture so far (Halo's blur reads it into a target of its size, and the next pass
// back), where a kept frame would be one shown before
static const WlNv2aTexEntry *target_texture(WlNv2aGx *gx, unsigned stage, const WlNv2aTexture *t) {
    unsigned i;

    if (reads_picture_drawn(gx, t->phys)) {
        uint32_t w = 0, h = 0;
        const void *parked = wl_gx_target_parked_colour(&w, &h);
        // (a picture drawn narrow is parked narrow, its columns alone: read as the whole picture, the
        // texture's coordinates being fractions of its width)
        uint32_t whole_w = gx->narrow ? gx->narrow_w : gx->cfg.width & ~3u;

        if (parked != NULL && w >= whole_w && h >= (gx->cfg.height & ~3u)) {
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
        if (gx->owner != NULL) {
            // (only the part a smaller target draws over was parked, the target not known to read it when it
            // took the EFB: from the next park for a target of its size on, all of it, else the texture is the
            // guest's memory, which holds no picture, and a megabyte; with the switch parkall, for every target)
            reader_add(gx, gx->owner, w, h);
        }
    }
    for (i = 0; i < TARGETS; i++) {
        RenderTarget *r = &gx->targets[i];

        if (r->memory != NULL && r->resolved && r->phys == t->phys && r != gx->owner &&
            target_fits(r, t) && !target_written(gx, r)) {
            WlNv2aTexEntry *e = &gx->target_entries[stage];

            r->used_frame = gx->frame; // (read: in use, targets_idle)
            gx->stats.kept_frame_reads += r->kept ? 1u : 0u;
            memset(e, 0, sizeof *e);
            e->phys = r->phys;
            e->image = r->memory;
            e->layout.format = (WlGxcFormat)r->format;
            e->layout.width = r->image_w; // (a kept frame drawn narrow: the picture's columns)
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

// The texture stages the pixel program reads: a stage whose register (t0..t3) a combiner stage in
// use or the final combiner takes as an input, and every stage before one whose texture shader
// program reads the stages before it (a dependent lookup, a dot product: modes 6 and up). Stage 0
// counts as read (the stand-in for combiners beyond GX is texture 0 times the colour).
static unsigned stages_read(const WlNv2aKelvin *k) {
    uint32_t words[WL_XBOX_PSH_WORDS];
    unsigned read = 1u, count, stage, shift;

    wl_nv2a_psh_words(k, words);
    count = words[WL_XBOX_PSH_COMBINER_COUNT] & 0xFFu;
    if (count == 0 || count > 8) {
        count = 8;
    }
    for (stage = 0; stage < count; stage++) {
        uint32_t inputs = words[WL_XBOX_PSH_RGB_INPUTS0 + stage], alpha = words[WL_XBOX_PSH_ALPHA_INPUTS0 + stage];

        for (shift = 0; shift < 32; shift += 8) {
            unsigned a = (inputs >> shift) & 0xFu, b = (alpha >> shift) & 0xFu;

            read |= (a >= 8u && a <= 11u ? 1u << (a - 8u) : 0u) | (b >= 8u && b <= 11u ? 1u << (b - 8u) : 0u);
        }
    }
    for (shift = 0; shift < 32; shift += 8) {
        unsigned a = (words[WL_XBOX_PSH_FINAL_INPUTS_ABCD] >> shift) & 0xFu;
        unsigned b = shift != 0 ? (words[WL_XBOX_PSH_FINAL_INPUTS_EFG] >> shift) & 0xFu : 0u;

        read |= (a >= 8u && a <= 11u ? 1u << (a - 8u) : 0u) | (b >= 8u && b <= 11u ? 1u << (b - 8u) : 0u);
    }
    for (stage = 1; stage < WL_NV2A_TEXTURES; stage++) {
        if (wl_nv2a_stage_program(k, stage) >= WL_NV2A_STAGE_BUMPENVMAP) {
            read |= (1u << stage) - 1u;
        }
    }
    return read;
}

// whether a stage's texture is looked up: its program samples it (not pass-through, clip plane,
// DOT_ZW or dot product, which read none of their own), D3D bound one (CONTROL0's enable: D3D's
// SetTexture(stage, NULL) leaves the format and offset of the texture before) and the pixel
// program reads it (`read`, stages_read)
static bool stage_sampled(WlNv2aGx *gx, const WlNv2aKelvin *k, unsigned stage, const WlNv2aTexture *t,
                          unsigned read) {
    switch (wl_nv2a_stage_program(k, stage)) {
    case WL_NV2A_STAGE_PASS_THROUGH:
    case WL_NV2A_STAGE_CLIP_PLANE:
    case WL_NV2A_STAGE_DOT_ZW:
    case WL_NV2A_STAGE_DOT_PRODUCT:
        gx->stats.textures_unsampled++;
        return false;
    default:
        break;
    }
    if (!t->enabled) {
        gx->stats.textures_disabled++;
        return false;
    }
    if (!(read & (1u << stage))) {
        gx->stats.textures_unread++;
        return false;
    }
    return true;
}

// Whether a draw whose texture could not be made is left undrawn: one that blends onto what is
// drawn (the destination's factor one, one minus the source's colour or alpha, or the source's
// alpha: an effect, a decal, the HUD), where any one colour shows as a square (the HUD's meters,
// whose colour is their constants' and whose texture gives only the alpha, drew a solid bar), as
// Halo, which never draws without its texture, would not have drawn it
static bool standin_hides(const WlNv2aKelvin *k) {
    uint32_t destination = wl_nv2a_k(k, WL_NV2A_K_SET_BLEND_FUNC_DFACTOR);

    return wl_nv2a_k(k, WL_NV2A_K_SET_BLEND_ENABLE) != 0 &&
           (destination == WL_NV2A_BLEND_ONE || destination == WL_NV2A_BLEND_ONE_MINUS_SRC_COLOR ||
            destination == WL_NV2A_BLEND_SRC_ALPHA || destination == WL_NV2A_BLEND_ONE_MINUS_SRC_ALPHA);
}

// A stand-in's colour made neutral for whatever its texture holds (standin_bind): the average of
// a picture tints a whole model or wall when the texture is not one (a mask, a ramp, a lookup
// table, a normalization cube: a combiner reads its channels as weights or vectors; a red and
// yellow ramp stood in as yellow, Halo's environments yellow and a Flood or an Elite in one
// colour), so its light alone is kept, as grey; a normal map's average (blue, its red and green
// about the middle) stands for its flat normal (0.5, 0.5, 1), and a signed map's (V16U16, the
// bump offsets) for its zero
static void standin_neutral(const WlNv2aTexture *t, uint8_t rgba[4]) {
    uint32_t light;

    if (t->signs != 0) {
        rgba[0] = rgba[1] = 0;
        rgba[2] = rgba[3] = 255;
        return;
    }
    if (rgba[0] >= 104u && rgba[0] <= 152u && rgba[1] >= 104u && rgba[1] <= 152u && rgba[2] >= 208u) {
        rgba[0] = rgba[1] = 128;
        rgba[2] = 255;
        return;
    }
    light = (77u * rgba[0] + 150u * rgba[1] + 29u * rgba[2] + 128u) >> 8;
    rgba[0] = rgba[1] = rgba[2] = (uint8_t)light;
}

// What a stage binds for a texture the cache could not make (no memory for it, a format GX cannot
// take, texels outside memory) in a draw drawn (standin_hides) so that it is not drawn untextured:
// a 4 x 4 image of its average colour made neutral (standin_neutral; grey when its texels cannot be
// read), made again when the stage's texture changes (a multiplying pass, a detail map's, takes
// the average as well). The entry is dynamic: the texture is looked up again at the next draw.
static const WlNv2aTexEntry *standin_bind(WlNv2aGx *gx, unsigned stage, const WlNv2aTexture *t) {
    WlNv2aTexEntry *e = &gx->standin_entries[stage];
    uint8_t *image = (uint8_t *)(((uintptr_t)gx->standin_memory + 31u) & ~(uintptr_t)31u) + stage * STANDIN_BYTES;

    gx->stats.textures_stood_in++;
    if ((!gx->standins[stage].made || gx->standins[stage].phys != t->phys ||
                   gx->standins[stage].format_word != t->format_word)) {
        uint8_t rgba[4] = {128, 128, 128, 255}, texels[16 * 4];
        const uint8_t *src = t->known ? wl_nv2a_phys(gx->dev, t->phys, t->total_bytes) : NULL;
        const uint8_t *palette = src != NULL && t->palette_entries != 0
                                     ? wl_nv2a_phys(gx->dev, t->palette_phys, 4u * t->palette_entries)
                                     : NULL;
        uint32_t value;
        unsigned i;

        if (src != NULL && !wl_nv2a_texture_average(t, src, palette, rgba)) {
            rgba[0] = rgba[1] = rgba[2] = 128, rgba[3] = 255;
        }
        standin_neutral(t, rgba);
        value = (uint32_t)rgba[0] << 24 | (uint32_t)rgba[1] << 16 | (uint32_t)rgba[2] << 8 | rgba[3];
        if (!gx->standins[stage].made || value != gx->standins[stage].rgba) {
            for (i = 0; i < 16; i++) {
                memcpy(texels + 4u * i, rgba, 4);
            }
            wl_gxc_encode(WL_GXC_RGBA8, texels, 4, 4, image, STANDIN_BYTES);
            commit(NULL, image, STANDIN_BYTES);
            gx->texture_memory_changed = true;
        }
        gx->standins[stage].phys = t->phys;
        gx->standins[stage].format_word = t->format_word;
        gx->standins[stage].rgba = value;
        gx->standins[stage].made = true;
        WL_LOG_D(TAG, "stage %u: texture %08x (format %02x) stood in by %08x", stage, (unsigned)t->phys,
                 (unsigned)t->color, (unsigned)value);
    }
    memset(e, 0, sizeof *e);
    e->phys = t->phys;
    e->format_word = t->format_word;
    e->image = image;
    e->layout.format = WL_GXC_RGBA8;
    e->layout.width = e->layout.height = 4;
    e->layout.levels = 1;
    e->layout.image_bytes = e->layout.total_bytes = STANDIN_BYTES;
    e->layout.sphere = t->cube != 0;
    e->sphere = t->cube;
    e->contents = t->contents;
    e->linear = t->linear;
    e->width = (uint16_t)t->width;
    e->height = (uint16_t)t->height;
    e->dynamic = 1;
    return e;
}

static void textures_bind(WlNv2aGx *gx, const WlNv2aKelvin *k) {
    unsigned stage, wanted = 0, read = stages_read(k);

    gx->standin_hidden = false;
    for (stage = 0; stage < WL_NV2A_TEXTURES; stage++) {
        WlNv2aTexture t;
        const WlNv2aTexEntry *e;
        WlGxTexDesc desc;
        bool made = false, mipmapped;

        gx->entries[stage] = NULL;
        if (wl_nv2a_stage_program(k, stage) == WL_NV2A_STAGE_NONE || !wl_nv2a_texture(k, stage, &t) ||
            !stage_sampled(gx, k, stage, &t, read)) {
            continue;
        }
        wanted |= 1u << stage;
        if (t.wrap_u == WL_NV2A_WRAP_BORDER || t.wrap_v == WL_NV2A_WRAP_BORDER) {
            gx->stats.border_clamped++;
        }
        e = target_texture(gx, stage, &t);
        if (e == NULL) {
            bool mipmaps = t.levels > 1 && t.min_filter >= WL_NV2A_FILTER_NEAREST_MIP_NEAREST &&
                           t.min_filter <= WL_NV2A_FILTER_LINEAR_MIP_LINEAR;

            display_read(gx, t.phys);
            if (gx->stats.frames_kept != 0 && is_display(gx, t.phys) && !reads_picture_drawn(gx, t.phys) &&
                gx->stats.kept_frame_misses++ == 0) {
                // (a picture older than the kept frame, the last shown: its guest memory holds none)
                WL_LOG_W(TAG, "a displayed surface (%08x) is read that the kept frame (the last shown) is not",
                         (unsigned)t.phys);
            }
            if (reads_picture_drawn(gx, t.phys)) {
                // (the picture being drawn is in the EFB, its guest memory holds none: not
                // converted into the arena, a megabyte the frame's textures need, before the park
                // is whole and serves it)
                gx->stats.screen_reads_unserved++;
                continue;
            }
            e = wl_nv2a_texcache_get(&gx->textures, gx->dev, &t, mipmaps, &made);
            if (e == NULL) {
                if (standin_hides(k)) {
                    gx->standin_hidden = true;
                    continue;
                }
                e = standin_bind(gx, stage, &t);
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
    const uint32_t extra = wl_nv2a_k(k, WL_NV2A_K_SET_SHADER_STAGE_PROGRAM);
    const WlXboxPshProgram *program = NULL;
    const bool recent = switched_on(SWITCH_NO_PSH_RECENT);
    bool usable, translated = false;

    wl_nv2a_psh_textures(k, gx->entries, &textures);
    if (textures.fog) {
        wl_gx_ramp_bind(WL_XBOX_PSH_FOG_TEXMAP);
    }
    wl_nv2a_psh_words(k, words);
    // (a program of the recent draws' by the combiners' own words, not unbiased nor looked up by its
    // structure again: wl_xbox_psh_cache_recent)
    if (recent) {
        program = wl_xbox_psh_cache_recent(&gx->psh, words, extra, &textures, &usable);
    }
    if (program == NULL) {
        // (signed sums that come back to [0, 1] made without registers below 0, which GX's lose)
        bool unbias = wl_xbox_psh_unbias(words, unbiased);

        if (unbias) {
            gx->stats.programs_unbiased++;
        }
        program = wl_xbox_psh_cache_program(&gx->psh, unbias ? unbiased : words, extra, &textures, &usable,
                                            &translated);
        if (recent) {
            wl_xbox_psh_cache_remember(&gx->psh, words, extra, &textures);
        }
    }
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

// the depth a + b / w with the slope b given, its a halfway between the vertices' least and greatest
// depth - b / w; returns the fit's worst error over them
static float offset_fit(float b, const float *inverse_w, const float *depth, unsigned n, float *a) {
    float low = 0.0f, high = 0.0f, r;
    unsigned i;

    for (i = 0; i < n; i++) {
        r = depth[i] - b * inverse_w[i];
        low = i == 0 || r < low ? r : low;
        high = i == 0 || r > high ? r : high;
    }
    *a = 0.5f * (low + high);
    return 0.5f * (high - low);
}

bool wl_nv2a_gx_slope_keep(WlNv2aGxSlope *s, float a, float b, const float *inverse_w, const float *depth, unsigned n,
                           float depth_max, uint32_t frame, uint32_t target) {
    float near_w = 0.0f, far_w = 0.0f;
    unsigned i;

    for (i = 0; i < n; i++) {
        near_w = i == 0 || inverse_w[i] < near_w ? inverse_w[i] : near_w;
        far_w = i == 0 || inverse_w[i] > far_w ? inverse_w[i] : far_w;
    }
    // (a few vertices spread in depth, as projection_fit's enough, and the fit holding for them: a fit
    // through the canonical points alone, or over vertices at one depth, is no camera's)
    if (n < FIT_ENOUGH || !(far_w - near_w > 1.0e-3f * far_w) ||
        !(fit_error(a, b, inverse_w, depth, n) <= FIT_TOLERANCE)) {
        return false;
    }
    s->b = b;
    s->depth_max = depth_max;
    s->frame = frame;
    s->target = target;
    s->known = true;
    return true;
}

bool wl_nv2a_gx_slope_offset(const WlNv2aGxSlope *s, const float *inverse_w, const float *depth, unsigned n,
                             float depth_max, uint32_t frame, uint32_t target, float *a, float *b) {
    float offset;

    // (kept in this frame or the last: a frame's first draws, its characters among them, come before
    // any fit of the camera's in it)
    if (!s->known || n == 0 || frame - s->frame > 1u || s->target != target || s->depth_max != depth_max ||
        !(offset_fit(s->b, inverse_w, depth, n, &offset) <= NARROW_TOLERANCE)) {
        return false;
    }
    *a = offset;
    *b = s->b;
    return true;
}

// the bytes of the prefetch areas over elements lo .. hi
static uint32_t prefetch_span_bytes(const WlNv2aFetchPlan *fetch, uint32_t lo, uint32_t hi) {
    uint32_t bytes = 0;
    unsigned a;

    for (a = 0; a < fetch->prefetch_count; a++) {
        bytes += (hi - lo) * fetch->prefetch[a].stride + fetch->prefetch[a].last + 1u;
    }
    return bytes;
}

// the lines of the prefetch areas' bytes of elements lo .. hi asked for, each once
static void prefetch_lines(const WlNv2aFetchPlan *fetch, uint32_t lo, uint32_t hi) {
    unsigned a;

    for (a = 0; a < fetch->prefetch_count; a++) {
        uintptr_t line = (uintptr_t)(fetch->prefetch[a].data + (size_t)lo * fetch->prefetch[a].stride) &
                         ~(uintptr_t)(WL_NV2A_CACHE_LINE - 1);
        const uintptr_t last =
            (uintptr_t)(fetch->prefetch[a].data + (size_t)hi * fetch->prefetch[a].stride + fetch->prefetch[a].last);

        for (; line <= last; line += WL_NV2A_CACHE_LINE) {
            WL_NV2A_PREFETCH(line);
        }
    }
}

// The guest's bytes of the draw's vertices first .. first + count - 1 (those in the draw) asked for
// ahead of their fetch (wl_nv2a_fetch_prefetch): a batch ahead, a miss each line otherwise. Line by
// line over the elements' span, each line once, where the span's lines are few for its vertices
// (PREFETCH_LINES a vertex at most: a line costs a third of what a vertex's own asking does):
// - the whole draw at its first call, when its bytes are few too (PREFETCH_WHOLE: L1 keeps them);
//   its later calls then ask nothing (for the same areas: projection_fit's own plan, of the
//   positions alone, does not stand for the draw's);
// - else this call's elements (indices: their least to their greatest), or vertex by vertex when
//   they are spread wider.
// ("vtxprefetch": every vertex's two lines for each area, as before, a line asked for again by
// each vertex that shares it)
// (a plan is the draw's for one draw: projection_fit's of its own, backend_draw's gx->fetch)
enum { PREFETCH_WHOLE = 16384, PREFETCH_LINES = 4 };
static void draw_prefetch_some(WlNv2aGx *gx, const WlNv2aFetchPlan *fetch, const WlNv2aDraw *draw, uint32_t first,
                               uint32_t count) {
    uint32_t v, end, lo, hi, bytes;
    bool indexed;

    if (fetch->prefetch_count == 0 || first >= draw->count) {
        return;
    }
    end = draw->count - first < count ? draw->count : first + count;
    if (!switched_on(SWITCH_NO_LINE_PREFETCH)) {
        for (v = first; v < end; v++) {
            wl_nv2a_fetch_prefetch(fetch, wl_nv2a_fetch_element(fetch, v));
        }
        return;
    }
    indexed = fetch->source == WL_NV2A_SOURCE_ARRAYS && fetch->indices != NULL;
    hi = indexed ? draw->index_last - draw->index_base : draw->count - 1u;
    if (hi < PREFETCH_WHOLE) {
        bytes = prefetch_span_bytes(fetch, 0, hi);
        if (bytes <= PREFETCH_WHOLE && bytes / WL_NV2A_CACHE_LINE <= PREFETCH_LINES * draw->count) {
            prefetch_lines(fetch, 0, hi);
            gx->prefetched.draw = gx->stats.draws;
            gx->prefetched.plan = fetch;
            return;
        }
    }
    if (indexed) {
        const uint32_t *indices = fetch->indices;

        lo = hi = indices[first];
        for (v = first + 1u; v < end; v++) {
            uint32_t index = indices[v];

            lo = index < lo ? index : lo;
            hi = index > hi ? index : hi;
        }
        lo -= fetch->index_base;
        hi -= fetch->index_base;
        if (hi - lo >= PREFETCH_WHOLE ||
            prefetch_span_bytes(fetch, lo, hi) / WL_NV2A_CACHE_LINE > PREFETCH_LINES * (end - first)) {
            for (v = first; v < end; v++) {
                wl_nv2a_fetch_prefetch(fetch, indices[v] - fetch->index_base);
            }
            return;
        }
    } else {
        lo = first;
        hi = end - 1u;
    }
    prefetch_lines(fetch, lo, hi);
}

static inline void draw_prefetch(WlNv2aGx *gx, const WlNv2aFetchPlan *fetch, const WlNv2aDraw *draw, uint32_t first,
                                 uint32_t count) {
    if (wl_nv2a_prefetching() && (gx->prefetched.draw != gx->stats.draws || gx->prefetched.plan != fetch)) {
        draw_prefetch_some(gx, fetch, draw, first, count);
    }
}

// the target a draw's fit is kept for (wl_nv2a_gx_slope_keep): 0 for the screen, whichever back
// buffer it is drawn into (two take turns, frame by frame), else the render target's colour surface
static uint32_t fit_target(const WlNv2aGx *gx, const WlNv2aSurface *s) {
    return surface_is_screen(gx, s) ? 0u : s->color_phys;
}

// the set of a signature: wl_fnv1a64's of its bytes modulo FIT_SETS, which only its low bits make, the
// same in 32 bits (the 64-bit prime's low word 0x1B3; the 2^40 term never reaches them)
static uint32_t signature_set(const uint32_t clip[CANONICAL_POINTS][4]) {
    const uint8_t *p = (const uint8_t *)clip;
    uint32_t hash = (uint32_t)WL_FNV1A64_INIT;
    unsigned i;

    _Static_assert((FIT_SETS & (FIT_SETS - 1)) == 0, "FIT_SETS a power of two");
    for (i = 0; i < CANONICAL_POINTS * 4u * 4u; i++) {
        hash = (hash ^ p[i]) * 0x1B3u;
    }
    return hash % FIT_SETS;
}

// (the fits kept, fit_memo_keep, and their use, fit_memo_holds and fit_slot_holds: g_fit_unkept while
// projection_fit checks one)
static bool g_fit_unkept;
static bool fit_memo_on(void) {
    return g_draw_cuts && switched_on(SWITCH_NO_FIT_MEMO);
}

// The fit kept from the last draw (projection_fit) for this one: the same program and constants, so the
// same transform and signature (the canonical points' outputs are a function of them alone), its
// fit still the one in the signature's slot, the same depth range and target, in the same frame, and
// the camera's slope already what this draw would keep (the slope of that fit, this frame, this
// target and range: wl_nv2a_gx_slope_keep would write it as it is). A fit made with the camera's
// slope is never kept (it holds only as near as its own vertices), nor one that saw no vertex in
// front of the eye. The draw's vertices are then not run: they would find its fit shared (their
// depths a + b / w of the same transform).
static bool fit_memo_holds(WlNv2aGx *gx, const WlNv2aKelvin *k, float depth_max, uint32_t target) {
    const struct FitMemo *m = &gx->fit_memo;
    const WlXboxVshProgram *program = gx->pipeline.program;
    const WlNv2aGxSlope *s = &gx->slope;

    return m->valid && !g_fit_unkept && m->k == k && m->program == program && m->serial == program->serial &&
           m->constants == k->constants_generation && m->point_size == gx->pipeline.point_size &&
           m->depth_max == depth_max && m->target == target && m->frame == gx->frame && m->slot->made == m->made &&
           s->known && s->frame == gx->frame && s->target == target && s->depth_max == depth_max && s->b == m->b;
}

// The same for a draw of another program or constants whose transform has the signature of a fit
// shared or made by a draw of this frame (the signature is what tells a transform, the draws that
// share it share its fit): that draw's depth range, target and viewport (the program's mapping to
// the screen, which the signature's clip-space positions leave out), and the camera's slope already
// what this draw would keep.
static bool fit_slot_holds(const WlNv2aGx *gx, const WlNv2aKelvin *k, const struct FitSignature *slot,
                           float depth_max, uint32_t target) {
    const WlNv2aGxSlope *s = &gx->slope;

    return slot->held && !g_fit_unkept && slot->held_frame == gx->frame && slot->held_target == target &&
           slot->held_depth_max == depth_max && !slot->narrow &&
           memcmp(slot->held_viewport, k->constants[WL_NV2A_CONSTANT_VIEWPORT_SCALE], sizeof slot->held_viewport) == 0 &&
           s->known && s->frame == gx->frame && s->target == target && s->depth_max == depth_max && s->b == slot->b;
}

// the fit p of this draw, from its signature's slot, kept for the next draws (fit_memo_holds,
// fit_slot_holds)
static void fit_memo_keep(WlNv2aGx *gx, const WlNv2aKelvin *k, float depth_max, uint32_t target,
                          struct FitSignature *slot) {
    struct FitMemo *m = &gx->fit_memo;

    _Static_assert(WL_NV2A_CONSTANT_VIEWPORT_OFFSET == WL_NV2A_CONSTANT_VIEWPORT_SCALE + 1, "viewport constants");
    slot->held_frame = gx->frame;
    slot->held_target = target;
    slot->held_depth_max = depth_max;
    memcpy(slot->held_viewport, k->constants[WL_NV2A_CONSTANT_VIEWPORT_SCALE], sizeof slot->held_viewport);
    slot->held = true;

    m->k = k;
    m->program = gx->pipeline.program;
    m->serial = gx->pipeline.program->serial;
    m->constants = k->constants_generation;
    m->point_size = gx->pipeline.point_size;
    m->depth_max = depth_max;
    m->target = target;
    m->frame = gx->frame;
    m->slot = slot;
    m->made = slot->made;
    m->a = slot->a;
    m->b = slot->b;
    m->valid = fit_memo_on();
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
//
// The fit a draw of this frame shared or made (not with the camera's slope) is kept for the next
// draws of its transform, whose vertices are then not run (fit_memo_holds: the last draw's program and
// constants; fit_slot_holds: its signature, from the canonical points): a10's draws 2.2 thousand
// instructions each (ppc-be), d40's skinned ones more. A cut ("nofitmemo"); WL_XB_FITVERIFY checks it.
static Projection projection_fit_full(WlNv2aGx *gx, const WlNv2aKelvin *k, const WlNv2aDraw *draw, float depth_max,
                                      uint32_t target);

static Projection projection_fit_memo(WlNv2aGx *gx, const WlNv2aKelvin *k, const WlNv2aDraw *draw,
                                      float depth_max, uint32_t target);

// WL_XB_FITVERIFY: the draw's projection with the fits kept, then again from the backend's state before
// it without them (the one used), any difference told
static Projection projection_fit_verify(WlNv2aGx *gx, const WlNv2aKelvin *k, const WlNv2aDraw *draw,
                                        float depth_max, uint32_t target) {
    static struct FitSignature signatures[FIT_SETS][FIT_WAYS];
    const WlNv2aGxSlope slope = gx->slope;
    const struct FitMemo memo = gx->fit_memo;
    const struct ProbeKey key = gx->fit_probe_key;
    const WlNv2aGxStats stats = gx->stats;
    const uint32_t use = gx->signature_use, made = gx->signature_made;
    const float fit_a = gx->fit_a, fit_b = gx->fit_b;
    WlNv2aGxSlope kept_slope;
    Projection kept, p;
    uint32_t used;
    bool shared, narrow;

    memcpy(signatures, gx->signatures, sizeof signatures);
    kept = projection_fit_memo(gx, k, draw, depth_max, target);
    shared = gx->last_shared;
    narrow = gx->last_narrow;
    kept_slope = gx->slope;
    used = gx->stats.fits_memo - stats.fits_memo;
    memcpy(gx->signatures, signatures, sizeof signatures);
    gx->slope = slope;
    gx->fit_memo = memo;
    gx->fit_probe_key = key;
    gx->stats = stats;
    gx->signature_use = use;
    gx->signature_made = made;
    gx->fit_a = fit_a;
    gx->fit_b = fit_b;
    g_fit_unkept = true;
    p = projection_fit_memo(gx, k, draw, depth_max, target);
    g_fit_unkept = false;
    gx->stats.fits_memo += used;
    if (kept.perspective != p.perspective || (p.perspective && (kept.a != p.a || kept.b != p.b)) ||
        shared != gx->last_shared || narrow != gx->last_narrow || memcmp(&kept_slope, &gx->slope, sizeof slope) != 0) {
        // (to stderr: the log has no sink under qemu-ppc)
        fprintf(stderr, "fit check: draw %u (%s) gave %d %.9g %.9g shared %d narrow %d, its vertices %d %.9g %.9g "
                        "shared %d narrow %d\n",
                (unsigned)gx->stats.draws, used ? "kept" : "made", kept.perspective, (double)kept.a,
                (double)kept.b, shared, narrow, p.perspective, (double)p.a, (double)p.b, gx->last_shared,
                gx->last_narrow);
    }
    return p;
}

static Projection projection_fit(WlNv2aGx *gx, const WlNv2aKelvin *k, const WlNv2aDraw *draw, float depth_max,
                                 uint32_t target) {
    return g_fit_verify ? projection_fit_verify(gx, k, draw, depth_max, target)
                        : projection_fit_memo(gx, k, draw, depth_max, target);
}

static Projection projection_fit_memo(WlNv2aGx *gx, const WlNv2aKelvin *k, const WlNv2aDraw *draw,
                                      float depth_max, uint32_t target) {
    Projection p = {false, gx->fit_a, gx->fit_b};

    gx->last_shared = false;
    gx->last_narrow = false;
    // (a program that never converts its position to the screen leaves no clip-space position)
    if (gx->pipeline.program == NULL || !gx->pipeline.program->captures_clip) {
        gx->fit_memo.valid = false;
        return p;
    }
    // (the last draw's fit, when it is this one's)
    if (draw->count != 0 && fit_memo_holds(gx, k, depth_max, target)) {
        gx->signature_use++;
        gx->fit_memo.slot->used = gx->signature_use;
        p.perspective = true;
        p.a = gx->fit_a = gx->fit_memo.a;
        p.b = gx->fit_b = gx->fit_memo.b;
        gx->last_shared = true;
        gx->stats.probes_kept++;
        gx->stats.fits_shared++;
        gx->stats.fits_memo++;
        return p;
    }
    gx->fit_memo.valid = false;
    return projection_fit_full(gx, k, draw, depth_max, target);
}

static Projection projection_fit_full(WlNv2aGx *gx, const WlNv2aKelvin *k, const WlNv2aDraw *draw, float depth_max,
                                      uint32_t target) {
    Projection p = {false, gx->fit_a, gx->fit_b};
    float inverse_w[FIT_PROBES + CANONICAL_POINTS], depth[FIT_PROBES + CANONICAL_POINTS];
    uint32_t clip[CANONICAL_POINTS][4];
    uint32_t probes = draw->count < FIT_SCAN ? draw->count : FIT_SCAN, v, n = 0, c, i, slot = 0, attributes, run;
    struct FitSignature *set;
    float near_w = 0.0f, far_w = 0.0f, a, b, d;
    bool all_w1 = true, in_front = false, enough = false, narrow = false;
    WlXboxVshOutput *outputs = gx->batch.outputs;
    WlNv2aFetchPlan fetch;
    uint32_t numbers[FIT_BATCH];

    attributes = wl_nv2a_pipeline_inputs(&gx->pipeline, WL_XBOX_VSH_LIVE_POSITION);
    // the transform's signature first (each canonical point in the input registers the position
    // reads, as the draw's fetch fills only those; kept from the last draw of the same program and
    // constants: probes_kept)
    if (!probes_kept(gx, &gx->fit_probe_key, k, WL_XBOX_VSH_LIVE_POSITION)) {
        for (i = 0; i < CANONICAL_POINTS; i++) {
            for (v = 0; v < WL_NV2A_ATTRIBS; v++) {
                if (attributes & (1u << v)) {
                    memcpy(gx->batch.inputs[i][v], canonical_points[i], sizeof gx->batch.inputs[i][v]);
                }
            }
        }
        wl_nv2a_pipeline_vertices(&gx->pipeline, k, (const float (*)[WL_NV2A_ATTRIBS][4])gx->batch.inputs,
                                  CANONICAL_POINTS, WL_XBOX_VSH_LIVE_POSITION, gx->fit_probe_out, &gx->batch.work);
    }
    for (i = 0; i < CANONICAL_POINTS; i++) {
        memcpy(clip[i], gx->fit_probe_out[i].clip, sizeof clip[i]);
    }
    set = gx->signatures[signature_set(clip)];
    // (a transform whose fit a draw of this frame shared or made: its vertices are not run, as for the
    // last draw's fit, fit_memo_holds)
    for (i = 0; i < FIT_WAYS && draw->count != 0 && fit_memo_on() && !g_fit_unkept; i++) {
        if (set[i].used != 0 && memcmp(set[i].clip, clip, sizeof clip) == 0) {
            if (fit_slot_holds(gx, k, &set[i], depth_max, target)) {
                gx->signature_use++;
                set[i].used = gx->signature_use;
                p.perspective = true;
                p.a = gx->fit_a = set[i].a;
                p.b = gx->fit_b = set[i].b;
                gx->last_shared = true;
                gx->stats.fits_shared++;
                gx->stats.fits_memo++;
                fit_memo_keep(gx, k, depth_max, target, &set[i]);
                return p;
            }
            break;
        }
    }
    // the vertices' positions, FIT_BATCH at a time (the program run for them together; the first run
    // FIT_ENOUGH, which are enough when they spread in depth)
    // (the fetch decided once for the draw, as the draw's own: wl_nv2a_draw_fetch_some's values)
    wl_nv2a_fetch_plan(draw, attributes, &fetch);
    wl_nv2a_fetch_constants(&fetch, k, gx->batch.inputs, FIT_BATCH);
    draw_prefetch(gx, &fetch, draw, 0, probes < FIT_ENOUGH ? probes : FIT_ENOUGH);
    for (v = 0; v < probes && n < FIT_PROBES && !enough; v += run) {
        run = v == 0 ? FIT_ENOUGH : FIT_BATCH;
        run = probes - v < run ? probes - v : run;
        for (i = 0; i < run; i++) {
            numbers[i] = v + i;
        }
        // (the next run's positions asked for while this one's are fetched and run)
        draw_prefetch(gx, &fetch, draw, v + run, probes - v - run < FIT_BATCH ? probes - v - run : FIT_BATCH);
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
    // the canonical points' w and depth
    c = n;
    for (i = 0; i < CANONICAL_POINTS; i++) {
        const WlXboxVshOutput *out = &gx->fit_probe_out[i];

        d = out->position[2] / depth_max;
        if (out->clip[3] > 1.0e-4f && d >= 0.0f && d <= 1.0f) {
            inverse_w[c] = 1.0f / out->clip[3];
            depth[c] = d;
            c++;
        }
    }
    gx->signature_use++;
    for (i = 0; i < FIT_WAYS; i++) {
        if (set[i].used != 0 && memcmp(set[i].clip, clip, sizeof clip) == 0) {
            // (no vertex in front of the eye among those looked at: the transform's fit as it is)
            // (a fit made with the camera's slope, from vertices spanning little depth, only as near as it
            // was made: the loose tolerance would let it stand for a draw it does not fit)
            if (n == 0 || fit_error(set[i].a, set[i].b, inverse_w, depth, n) <=
                              (set[i].narrow ? NARROW_TOLERANCE : SIGNATURE_TOLERANCE)) {
                set[i].used = gx->signature_use;
                // (the camera's slope kept for this frame and target by a shared fit too, as most of a
                // frame's draws share theirs: when it holds for this draw's vertices spread in depth)
                if (enough && !set[i].narrow) {
                    wl_nv2a_gx_slope_keep(&gx->slope, set[i].a, set[i].b, inverse_w, depth, n, depth_max, gx->frame,
                                          target);
                }
                p.a = gx->fit_a = set[i].a;
                p.b = gx->fit_b = set[i].b;
                gx->last_shared = true;
                gx->last_narrow = set[i].narrow;
                gx->stats.fits_shared++;
                if (n != 0 && !set[i].narrow) {
                    fit_memo_keep(gx, k, depth_max, target, &set[i]);
                }
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
    // canonical points alone; with neither, the last fit). Through the vertices alone, the slope of
    // the camera's slope first, for vertices spanning too little depth, when it holds for them with an
    // offset of their own: a small or distant model (Halo's skinned characters, whose canonical
    // points stray) spans too little depth to set a slope (a10's captain: 1 / w over
    // 0.0916..0.0916), and one of its own, kept for its transform, put him in front of the bridge's
    // console in one frame and behind it in the next (its error over his body 4e-5, 700 of the
    // depth's steps). The camera's slope is that of the last fit here that held for vertices spread
    // in depth (never the canonical points' alone), in this frame or the last, target and depth range
    if (fit_two(inverse_w, depth, c, &a, &b) && fit_error(a, b, inverse_w, depth, n) <= FIT_TOLERANCE) {
        wl_nv2a_gx_slope_keep(&gx->slope, a, b, inverse_w, depth, n, depth_max, gx->frame, target);
    } else if (wl_nv2a_gx_slope_offset(&gx->slope, inverse_w, depth, n, depth_max, gx->frame, target, &a, &b)) {
        narrow = true;
    } else if (!fit_two(inverse_w, depth, n, &a, &b)) {
        return p;
    }
    gx->stats.fits_made++;
    memcpy(set[slot].clip, clip, sizeof clip);
    set[slot].a = a;
    set[slot].b = b;
    set[slot].used = gx->signature_use;
    set[slot].narrow = narrow;
    set[slot].made = ++gx->signature_made;
    set[slot].held = false;
    gx->last_narrow = narrow;
    if (n != 0 && !narrow) {
        fit_memo_keep(gx, k, depth_max, target, &set[slot]);
    }
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

// WlNv2aGxConfig.depth_check: the draw's fit p against its program's depth at each of its vertices
// in front of the eye and within the depth range (the program run again for them, FIT_BATCH at a
// time, as projection_fit runs it)
static void projection_check(WlNv2aGx *gx, const WlNv2aKelvin *k, const WlNv2aDraw *draw, const Projection *p,
                             float depth_max) {
    WlNv2aGxDepthCheck *c = &gx->depth_check;
    WlXboxVshOutput *outputs = gx->batch.outputs;
    WlNv2aFetchPlan fetch;
    uint32_t numbers[FIT_BATCH], v, i, run;

    memset(c, 0, sizeof *c);
    if (gx->pipeline.program == NULL || (p->perspective && !gx->pipeline.program->captures_clip)) {
        return;
    }
    c->perspective = p->perspective;
    c->made = !gx->last_shared;
    c->narrow = gx->last_narrow;
    wl_nv2a_fetch_plan(draw, wl_nv2a_pipeline_inputs(&gx->pipeline, WL_XBOX_VSH_LIVE_POSITION), &fetch);
    wl_nv2a_fetch_constants(&fetch, k, gx->batch.inputs, FIT_BATCH);
    for (v = 0; v < draw->count; v += run) {
        run = draw->count - v < FIT_BATCH ? draw->count - v : FIT_BATCH;
        for (i = 0; i < run; i++) {
            numbers[i] = v + i;
        }
        wl_nv2a_fetch_vertices(&fetch, draw, numbers, run, gx->batch.inputs);
        wl_nv2a_pipeline_vertices(&gx->pipeline, k, (const float (*)[WL_NV2A_ATTRIBS][4])gx->batch.inputs, run,
                                  WL_XBOX_VSH_LIVE_POSITION, outputs, &gx->batch.work);
        for (i = 0; i < run; i++) {
            const WlXboxVshOutput *out = &outputs[i];
            float d = out->position[2] / depth_max, error, moved;

            if (!p->perspective || !out->clip_captured) {
                // (a screen-space vertex: its depth as_depth's, clamped to the range)
                c->clamped += !(d >= 0.0f && d <= 1.0f);
                c->checked++;
                continue;
            }
            if (!(out->clip[3] > 1.0e-4f) || !(d >= 0.0f && d <= 1.0f)) {
                continue;
            }
            error = p->a + p->b / out->clip[3] - d;
            // (in w: the depth's error over its slope there, d depth / d w = -b / w^2)
            moved = p->b != 0.0f ? error * out->clip[3] * out->clip[3] / -p->b : 0.0f;
            if (fabsf(moved) > (c->nearer_w > c->farther_w ? c->nearer_w : c->farther_w)) {
                c->worst_w = out->clip[3];
            }
            c->nearer = -error > c->nearer ? -error : c->nearer;
            c->farther = error > c->farther ? error : c->farther;
            c->nearer_w = -moved > c->nearer_w ? -moved : c->nearer_w;
            c->farther_w = moved > c->farther_w ? moved : c->farther_w;
            c->checked++;
        }
    }
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
    // a sphere map's made by GX from the direction sent (sphere_xf_plan)
    bool sphere_xf;
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
    // the stages (bit n: stage n) whose sphere map GX makes from the direction sent (sphere_xf_plan),
    // never beside projected ones
    unsigned sphere_xf;
    // the stages (bit n: stage n) GX's texgen makes from an input register the fused form sends once
    // (texgen_xf_plan; WL_XBOX_VSH_FUSED_STAGE_INPUT), and each one's register
    unsigned texgen_xf;
    unsigned char texgen_from[WL_NV2A_TEXTURES];
    // the transform unit's draw (NULL: the processor's)
    const struct XfPlan *xf;
    // the program's fused form for the draw (fused_known: decided; NULL: none)
    bool fused_known;
    const WlXboxVshFusedForm *fused;
    // d0's r, g and b lit by GX's transform unit from the normal the program's lit form sends
    // (skin_lit), and whether the draw could be (lit_wanted: the form the forms list asks for)
    bool lit, lit_wanted;
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

enum { XF_MIN_VERTICES = 1 };

typedef struct XfPlan {
    // every live output the transform unit's (the program not run), or the position alone (the
    // program run for the others, which are sent as the processor makes them); world: the position
    // made from the world position the program's "w" form sends (world_xf_plan), not from v0
    bool full, world;
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
        } else if (g_draw_cuts) {
            wl_nv2a_sphere_coordinates_inline(t, made);
        } else {
            wl_nv2a_sphere_coordinates_double(t, made);
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
// a sphere map's direction for GX (sphere_xf_plan): d / |d| as the fused forms make it (one root of two
// steps, single precision), 0 for a direction too short or too long (GX's disc centre, as
// wl_nv2a_sphere_coordinates')
static inline void sphere_direction(const float t[4], float d[3]) {
    float across = t[0] * t[0] + t[1] * t[1], length2 = wl_nv2a_madd(t[2], t[2], across), root;

    if (!(length2 > 1.0e-24f && length2 <= 3.4028235e38f)) {
        d[0] = d[1] = d[2] = 0.0f;
        return;
    }
    root = wl_nv2a_sphere_root(length2, length2 * 0.5f);
    d[0] = t[0] * root;
    d[1] = t[1] * root;
    d[2] = t[2] * root;
}

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
    } else if (plan->sphere_xf != 0) {
        // (the sphere maps GX makes: each one's direction of unit length, in the same places)
        unsigned sent = 0;

        for (stage = 0; stage < stage_count; stage++) {
            unsigned n = plan->stage_list[stage];

            if (!(plan->sphere_xf & (1u << n))) {
                continue;
            }
            for (r = 0; r < runs; r++) {
                float d[3];

                sphere_direction(o[r].t[n], d);
                put_float(&made[r]->w[i], d[0]);
                put_float(&made[r]->w[i + 1], d[1]);
                put_float(&made[r]->w[i + 2], d[2]);
            }
            i += 3;
            sent++;
        }
        for (; sent > 1u && sent < WL_GX_DIRECT_PROJECTED; sent++, i += 3) {
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

#if WL_NV2A_COLOR_PAIRED
                if (switched_on(SWITCH_NO_PAIRED_COLOR)) {
                    wl_nv2a_color_words_paired((const char *)o + from, (uint32_t)sizeof o[0], (void *const *)made,
                                               (uint32_t)(offsetof(PackedVertex, w) + i * sizeof(uint32_t)), runs);
                    continue;
                }
#endif
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

        if (sp->sphere_xf) {
            i -= 2;
        } else if (sp->sphere && !sp->spot && g_draw_cuts) {
            // (stage_texcoord's, inline: its constants kept in registers from vertex to vertex)
            for (r = 0; r < runs; r++) {
                float st[2];

                wl_nv2a_sphere_coordinates_inline(o[r].t[n], st);
                put_float(&made[r]->w[i], st[0]);
                put_float(&made[r]->w[i + 1], st[1]);
            }
        } else if (sp->sphere) {
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
    // (the transform unit's position, the program run for the rest: WL_XBOX_VSH_FUSED_XF, v0 sent as fetched)
    if ((plan->xf != NULL && (plan->xf->full || gx->cfg.no_fused_xf)) ||
        (plan->xf == NULL && (plan->landing || plan->to_sx == 0.0f || plan->to_sy == 0.0f)) ||
        (plan->texcoords & (1u << WL_XBOX_PSH_FOG_TEXCOORD)) || fetch->source == WL_NV2A_SOURCE_IMMEDIATE ||
        plan->stage_count + plan->projected_count > 4u || plan->colors > 2u) {
        return false;
    }
    key->live = (unsigned short)(plan->xf != NULL ? plan->live & ~(unsigned)WL_XBOX_VSH_LIVE_POSITION : plan->live);
    // (the coordinates GX makes from an input: not the program's)
    key->live &= (unsigned short)~((plan->texgen_xf & 0xFu) * (unsigned)WL_XBOX_VSH_LIVE_T0);
    key->lit = (plan->xf == NULL || plan->xf->world) && plan->lit ? 1u : 0u;
    key->perspective = plan->xf == NULL   ? gx->projection.perspective
                       : plan->xf->world ? WL_XBOX_VSH_FUSED_WORLD
                                         : WL_XBOX_VSH_FUSED_XF;
    key->colors = (unsigned char)plan->colors;
    key->channels = (unsigned char)(plan->channels & ((1u << plan->colors) - 1u));
    // (the projected coordinates first, as vertices_pack writes them: WL_XBOX_VSH_FUSED_STAGE_PROJECTED)
    for (n = 0; n < plan->projected_count; n++) {
        const StagePlan *sp = &plan->stages[plan->projected_list[n]];

        if (sp->spot || plan->projected_list[n] > 3u) {
            return false;
        }
        key->stages[key->stage_count++] =
            (unsigned char)(plan->projected_list[n] | (sp->linear ? (unsigned)WL_XBOX_VSH_FUSED_STAGE_PROJECTED_LINEAR
                                                                  : (unsigned)WL_XBOX_VSH_FUSED_STAGE_PROJECTED)
                                                          << 2);
    }
    for (n = 0; n < plan->stage_count; n++) {
        const StagePlan *sp = &plan->stages[plan->stage_list[n]];
        unsigned kind = (plan->texgen_xf & (1u << plan->stage_list[n])) ? WL_XBOX_VSH_FUSED_STAGE_INPUT
                        : sp->sphere_xf  ? WL_XBOX_VSH_FUSED_STAGE_SPHERE_XF
                        : sp->sphere     ? WL_XBOX_VSH_FUSED_STAGE_SPHERE
                        : sp->projective ? (sp->linear ? WL_XBOX_VSH_FUSED_STAGE_PROJECTIVE_LINEAR
                                                       : WL_XBOX_VSH_FUSED_STAGE_PROJECTIVE)
                        : sp->linear     ? WL_XBOX_VSH_FUSED_STAGE_LINEAR
                                         : WL_XBOX_VSH_FUSED_STAGE_PLAIN;

        if (sp->spot || plan->stage_list[n] > 3u) {
            return false;
        }
        key->stages[key->stage_count++] = (unsigned char)(plan->stage_list[n] | kind << 2);
    }
    for (n = 0; n < fetch->count; n++) {
        unsigned format = wl_nv2a_fetch_fused_format(fetch, n);

        if (format == WL_XBOX_VSH_FUSED_FORMATS || fetch->streamed[n].attribute >= 16u) {
            return false;
        }
        key->format[fetch->streamed[n].attribute] = (unsigned char)format;
    }
    // (one stride for every array: fused_find takes a form without it too)
    key->interleaved = fetch->count > 1u ? 1u : 0u;
    for (n = 1; n < fetch->count; n++) {
        if (fetch->streamed[n].stride != fetch->streamed[0].stride) {
            key->interleaved = 0;
        }
    }
    return true;
}

// the program's fused form for the key (NULL: none); an interleaved draw's, else one that is not
static const WlXboxVshFusedForm *fused_find(const WlXboxVshProgram *program, WlXboxVshFusedKey *key) {
    unsigned n;

    for (;;) {
        for (n = 0; n < program->native_fused_count; n++) {
            if (memcmp(&program->native_fused[n].key, key, sizeof *key) == 0) {
                return &program->native_fused[n];
            }
        }
        if (!key->interleaved) {
            return NULL;
        }
        key->interleaved = 0;
    }
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
    form = fused_find(program, &key);
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
    // (the viewport's scale and offset in samples: to_sx and to_sy are powers of two, the products exact)
    f->pack[WL_XBOX_VSH_FUSED_SCALE] = plan->scale[0] * plan->to_sx;
    f->pack[WL_XBOX_VSH_FUSED_SCALE + 1] = plan->scale[1] * plan->to_sy;
    f->pack[WL_XBOX_VSH_FUSED_OFFSET] = plan->offset[0] * plan->to_sx;
    f->pack[WL_XBOX_VSH_FUSED_OFFSET + 1] = plan->offset[1] * plan->to_sy;
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
            key.lit = plan->lit_wanted ? 1u : 0u;
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
           : format >= WL_XBOX_VSH_FUSED_UB && format < WL_XBOX_VSH_FUSED_S32K    ? format - WL_XBOX_VSH_FUSED_UB + 1u
           : format >= WL_XBOX_VSH_FUSED_S32K && format < WL_XBOX_VSH_FUSED_FORMATS ? 2u * (format - WL_XBOX_VSH_FUSED_S32K + 1u)
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

    // (a lit form's words are not the general path's: its normal, d0's alpha alone; nor a "w" form's)
    if (plan->lit || (plan->xf != NULL && plan->xf->world) || plan->texgen_xf != 0) {
        return;
    }
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

// ---- made vertices read by index ---------------------------------------------------------------
// An indexed draw sends each vertex once for each time its index comes: a strip's or a list's
// vertices about 1.7 times each, 16 to 20 words each time. Its vertices made into the ring
// (gx->made) are read by GX at an index of their own instead (wl_gx_direct_array_words: each
// attribute's array the slots' words at its place), written once, each repeat two bytes an
// attribute. A draw takes slots from `pos` on for at most its vertices and one a batch (a batch's
// first vertex starts a 32-byte line: GX may have read the line before it, written back with its
// batch, into its vertex cache); its slots stay its own until GX is past it. The ring's chunks are
// written again only once GX is past the fence (wl_gx_fence) sent when the writing left them, and GX's
// vertex cache is emptied as each is entered. The switch "nomadeindex" (WL_XB_NOMADEINDEX) sends
// every vertex's words.

// whether a draw of `count` vertices may have its vertices made into the ring
static bool made_possible(const WlNv2aGx *gx, uint32_t count) {
    uint32_t slots = count + (count + WL_XBOX_VSH_BATCH - 1u) / WL_XBOX_VSH_BATCH + 1u;

    return switched_on(SWITCH_NO_MADE_INDEX) && count != 0 && (gx->made.ring != NULL || !gx->made.tried) &&
           (uint64_t)slots * sizeof(PackedVertex) + 32u <= MADE_RING_BYTES / 2u;
}

// a fence behind what was sent, for the chunks given (and the next draw flushed first: a token)
static void made_fence(WlNv2aGx *gx, uint32_t chunks) {
    uint32_t t;
    unsigned c;

    if (chunks == 0) {
        return;
    }
    t = wl_gx_fence();
    wl_gx_direct_flush_after();
    for (c = 0; c < MADE_CHUNKS; c++) {
        if (chunks & (1u << c)) {
            gx->made.fence[c] = t;
        }
    }
    gx->made.pending &= ~chunks;
    gx->stats.made_fences++;
}

// the draw's slots taken (false: its vertices made the usual way, into the memo)
static bool made_begin(WlNv2aGx *gx, uint32_t count) {
    uint32_t slots = count + (count + WL_XBOX_VSH_BATCH - 1u) / WL_XBOX_VSH_BATCH + 1u, bytes, c, last, first;
    bool emptied = false;

    gx->made.ring_on = gx->made.indexed = false;
    if (!made_possible(gx, count)) {
        gx->stats.made_refused += switched_on(SWITCH_NO_MADE_INDEX) && count != 0 ? 1u : 0u;
        return false;
    }
    if (gx->made.ring == NULL) {
        gx->made.tried = true;
        gx->made.ring = gx->textures.cfg.alloc(gx->textures.cfg.user, MADE_RING_BYTES);
        if (gx->made.ring == NULL) {
            WL_LOG_W(TAG, "made vertices: no %u KB for their ring (their words sent)", MADE_RING_BYTES >> 10);
            return false;
        }
        WL_LOG_I(TAG, "made vertices: a ring of %u KB for GX to read by index", MADE_RING_BYTES >> 10);
    }
    // (the draw's slots from the first that starts at pos or after: the slots are numbered from the
    // ring's start, which every array has as its base, so that a layout's arrays stay as GX holds them)
    first = (gx->made.pos + (uint32_t)sizeof(PackedVertex) - 1u) / (uint32_t)sizeof(PackedVertex);
    if ((first + slots) * (uint32_t)sizeof(PackedVertex) > MADE_RING_BYTES) {
        // (from the start again: what was written since the last fence goes behind one)
        made_fence(gx, gx->made.pending);
        gx->made.pos = 0;
        gx->made.entered = 0;
        gx->stats.made_laps++;
        first = 0;
    }
    bytes = (first + slots) * (uint32_t)sizeof(PackedVertex) - gx->made.pos;
    last = (gx->made.pos + bytes - 1u) / MADE_CHUNK_BYTES;
    for (c = gx->made.pos / MADE_CHUNK_BYTES; c <= last; c++) {
        if (!(gx->made.entered & (1u << c))) {
            if (gx->made.fence[c] != 0 && !wl_gx_fence_passed(gx->made.fence[c])) {
                gx->stats.made_waits++;
                (void)wl_gx_fence_wait(gx->made.fence[c]);
            }
            gx->made.fence[c] = 0;
            gx->made.entered |= 1u << c;
            emptied = true;
        }
    }
    if (emptied) {
        // (GX's vertex cache may hold the chunk's lines as their last lap had them)
        wl_gx_direct_arrays_changed();
    }
    gx->made.base = gx->made.ring;
    gx->made.made = 0;
    gx->made.next = first;
    gx->made.limit = first + slots;
    gx->made.ring_on = true;
    return true;
}

// The batch's slots: an indexed draw's vertex kept by its index (the memo) is the slot made for it
// before, the others each a new slot (a function of its index: the same whatever the memo held).
// The vertices to make (their elements, their slots) as vertices_make's memo has them; their count.
static unsigned made_slots(WlNv2aGx *gx, const WlNv2aDraw *draw, uint32_t first, uint32_t count, bool indexed) {
    PackedVertex *const base = gx->made.base;
    uint32_t next = gx->made.next, *const slot_of = gx->made.index;
    const uint32_t **const words = gx->batch.words;
    unsigned runs = 0, n;

    gx->made.batch_first = next;
    if (indexed) {
        const uint32_t *indices = draw->indices + first;
        const uint32_t stamp = gx->memo_stamp, index_base = draw->index_base;
        struct WlNv2aGxMemo *const memo = gx->memo;
        uint16_t *const made_memo = gx->made.memo;

        for (n = 0; n < count; n++) {
            uint32_t index = indices[n], slot = index & (MEMO - 1u), at;
            struct WlNv2aGxMemo *m = &memo[slot];

            if (m->stamp == stamp && m->index == index) {
                at = made_memo[slot];
            } else {
                // (a slot's vertex stays where it is when another index takes the memo's place)
                m->stamp = stamp;
                m->index = index;
                at = next++;
                made_memo[slot] = (uint16_t)at;
                gx->batch.elements[runs] = index - index_base;
                gx->batch.words_made[runs] = base[at].w;
                gx->batch.run_vertices[runs] = &base[at];
                runs++;
            }
            slot_of[n] = at;
            words[n] = base[at].w;
        }
    } else {
        for (n = 0; n < count; n++) {
            uint32_t at = next++;

            slot_of[n] = at;
            words[n] = base[at].w;
            gx->batch.elements[n] = first + n;
            gx->batch.words_made[n] = base[at].w;
            gx->batch.run_vertices[n] = &base[at];
        }
        runs = count;
    }
    gx->made.made += next - gx->made.batch_first;
    gx->made.next = next;
    if (next != gx->made.batch_first) {
        // (the new slots' lines, which no one has read since their chunk was entered, zeroed before
        // the vertices are written: on the Wii the wrapped memset's dcbz, a line had without its
        // fill from memory; made_flush writes the same lines back)
        uintptr_t from = (uintptr_t)&base[gx->made.batch_first] & ~(uintptr_t)31u;
        uintptr_t to = ((uintptr_t)&base[next] + 31u) & ~(uintptr_t)31u;

        memset((void *)from, 0, to - from);
    }
    return runs;
}

// the batch's new slots written back for GX, the next batch's from the next line on
static void made_flush(WlNv2aGx *gx) {
    uintptr_t base = (uintptr_t)gx->made.base, from, to;

    if (gx->made.next == gx->made.batch_first) {
        return;
    }
    from = (uintptr_t)&gx->made.base[gx->made.batch_first] & ~(uintptr_t)31u;
    to = ((uintptr_t)&gx->made.base[gx->made.next] + 31u) & ~(uintptr_t)31u;
    commit(NULL, (void *)from, to - from);
    gx->made.next = (uint32_t)((to - base + sizeof(PackedVertex) - 1u) / sizeof(PackedVertex));
}

// The attributes a layout sends, each read from the slots (WlGxDirectLayout.indexed), and the word
// each starts at (place, NULL: not wanted); the words a vertex has
static unsigned made_places(const WlGxDirectLayout *layout, uint16_t *indexed_out, uint8_t *place) {
    uint8_t unused[WL_GX_DIRECT_ARRAY_NORMAL + 1];
    unsigned at = 3, n, groups = (unsigned)__builtin_popcount(layout->texcoords_projected) + (layout->lit ? 1u : 0u);
    uint16_t indexed = 1u << WL_GX_DIRECT_ARRAY_POSITION;

    if (place == NULL) {
        place = unused;
    }
    // (the words as the Wii's layout has GX read them: the position, the normal or the normal,
    // binormal and tangent, the colours not the register's, the coordinates sent)
    place[WL_GX_DIRECT_ARRAY_POSITION] = 0;
    if (groups != 0) {
        place[WL_GX_DIRECT_ARRAY_NORMAL] = (uint8_t)at;
        indexed |= 1u << WL_GX_DIRECT_ARRAY_NORMAL;
        at += groups == 1u ? 3u : 9u;
    }
    for (n = 0; n < layout->colors && n < 2u; n++) {
        if (!(layout->colors_register & (1u << n))) {
            place[WL_GX_DIRECT_ARRAY_COLOR0 + n] = (uint8_t)at++;
            indexed |= (uint16_t)(1u << (WL_GX_DIRECT_ARRAY_COLOR0 + n));
        }
    }
    for (n = 0; n < 8u; n++) {
        if (layout->texcoords & (1u << n)) {
            place[WL_GX_DIRECT_ARRAY_TEXCOORD0 + n] = (uint8_t)at;
            at += (layout->texcoord_s & (1u << n)) ? 1u : 2u;
            indexed |= (uint16_t)(1u << (WL_GX_DIRECT_ARRAY_TEXCOORD0 + n));
        }
    }
    *indexed_out = indexed;
    return at;
}

// a layout as a draw whose vertices are made into slots sets it before its first batch: every
// attribute indexed (made_arrays finds the same unless the first batch changed the layout)
static void made_layout(const WlNv2aGx *gx, WlGxDirectLayout *layout) {
    if (gx->made.ring_on) {
        (void)made_places(layout, &layout->indexed, NULL);
    }
}

// The draw's layout with every attribute it sends read from its slots (true), set with the arrays;
// false when the layout's words are not the plan's (then sent as they are)
static bool made_arrays(WlNv2aGx *gx, const DrawPlan *plan, WlGxDirectLayout *layout) {
    uint8_t place[WL_GX_DIRECT_ARRAY_NORMAL + 1];
    uint16_t indexed;
    unsigned n, at = made_places(layout, &indexed, place);
    const uint8_t *words = (const uint8_t *)gx->made.ring->w;

    if (at != plan->words || at > PACKED_WORDS) {
        gx->stats.made_unmatched++;
        if (layout->indexed != 0) {
            layout->indexed = 0;
            wl_gx_direct_layout(layout);
        }
        return false;
    }
    layout->indexed = indexed;
    wl_gx_direct_layout(layout);
    for (n = 0; n <= WL_GX_DIRECT_ARRAY_NORMAL; n++) {
        if (indexed & (1u << n)) {
            wl_gx_direct_array_words(n, words + 4u * place[n], (uint32_t)sizeof(PackedVertex));
        }
    }
    return true;
}

// the draw sent: its slots kept until GX is past it, the chunks the writing left fenced
static void made_end(WlNv2aGx *gx, uint32_t count) {
    uint32_t end, c, last, next_chunk;

    if (!gx->made.ring_on) {
        return;
    }
    if (gx->made.indexed) {
        gx->stats.made_draws++;
        gx->stats.made_sent += count;
    }
    gx->stats.made_vertices += gx->made.made;
    end = ((gx->made.next * (uint32_t)sizeof(PackedVertex)) + 31u) & ~31u;
    if (end > gx->made.pos) {
        last = (end - 1u) / MADE_CHUNK_BYTES;
        for (c = gx->made.pos / MADE_CHUNK_BYTES; c <= last; c++) {
            gx->made.pending |= 1u << c;
        }
        gx->made.pos = end;
    }
    next_chunk = gx->made.pos / MADE_CHUNK_BYTES;
    made_fence(gx, gx->made.pending & ~(next_chunk < MADE_CHUNKS ? 1u << next_chunk : 0u));
    gx->made.ring_on = gx->made.indexed = false;
}

static bool vertices_make(WlNv2aGx *gx, const WlNv2aKelvin *k, const WlNv2aDraw *draw, uint32_t first,
                          uint32_t count, DrawPlan *plan, WlGxDirectLayout *layout) {
    bool indexed = draw->source == WL_NV2A_SOURCE_ARRAYS && draw->indices != NULL;
    uint32_t batch = ++gx->batch_stamp;
    unsigned runs = 0, n, r;
    bool changed = false;

    // (each vertex made: its element as the fetch takes it, wl_nv2a_fetch_element's, and where its
    // words go; the draw's and the tables' addresses in locals, which the stores would otherwise
    // have the compiler load again for each vertex; or into the draw's slots)
    if (gx->made.ring_on) {
        runs = made_slots(gx, draw, first, count, indexed);
    } else if (indexed) {
        const uint32_t *indices = draw->indices + first;
        const uint32_t stamp = gx->memo_stamp, index_base = draw->index_base;
        struct WlNv2aGxMemo *const memo = gx->memo;
        uint32_t *const memo_batch = gx->memo_batch;
        PackedVertex *const memo_vertices = gx->memo_vertices, *const batch_made = gx->batch.made;
        const uint32_t **const words = gx->batch.words;

        for (n = 0; n < count; n++) {
            uint32_t index = indices[n], slot = index & (MEMO - 1u);
            struct WlNv2aGxMemo *m = &memo[slot];
            PackedVertex *made;

            if (m->stamp == stamp && m->index == index) {
                memo_batch[slot] = batch;
                words[n] = memo_vertices[slot].w;
                continue;
            }
            if (m->stamp != stamp || memo_batch[slot] != batch) {
                m->stamp = stamp;
                m->index = index;
                memo_batch[slot] = batch;
                made = &memo_vertices[slot];
            } else {
                made = &batch_made[runs];
            }
            words[n] = made->w;
            gx->batch.elements[runs] = index - index_base;
            gx->batch.words_made[runs] = made->w;
            gx->batch.run_vertices[runs] = made;
            runs++;
        }
    } else {
        for (n = 0; n < count; n++) {
            PackedVertex *made = &gx->batch.made[n];

            gx->batch.words[n] = made->w;
            gx->batch.elements[n] = first + n;
            gx->batch.words_made[n] = made->w;
            gx->batch.run_vertices[n] = made;
        }
        runs = count;
    }
    gx->stats.vertices_reused += count - runs;
    if ((plan->xf == NULL || !plan->xf->full) && runs != 0) {
        const WlXboxVshFusedForm *form = fused_for(gx, plan, layout, runs);

        if (form != NULL) {
            gx->fused.elements = gx->batch.elements;
            gx->fused.words = gx->batch.words_made;
            form->run((const float (*)[4])k->constants, &gx->fused, runs, plan->live);
            gx->stats.vertices_fused += runs;
#ifdef WL_XBOX_FUSED_CHECK
            fused_check(gx, k, draw, plan, runs, form);
#endif
            goto made;
        }
    }
    wl_nv2a_fetch_elements(&gx->fetch, draw, gx->batch.elements, runs, gx->batch.inputs);
    if (plan->xf != NULL && plan->xf->full) {
        for (r = 0; r < runs; r++) {
            vertex_pack_xf(plan, (const float (*)[4])gx->batch.inputs[r], gx->batch.run_vertices[r]);
        }
        goto made;
    }
    // (the transform unit's position: the program run for the other outputs alone)
    wl_nv2a_pipeline_vertices(&gx->pipeline, k, (const float (*)[WL_NV2A_ATTRIBS][4])gx->batch.inputs, runs,
                              plan->xf != NULL ? plan->live & ~(unsigned)WL_XBOX_VSH_LIVE_POSITION : plan->live,
                              gx->batch.outputs, &gx->batch.work);
    changed = layout != NULL && projected_plan(gx, plan, layout, runs);
    vertices_pack(gx, k, plan, runs);
made:
    // (the slots' new vertices written back for GX)
    if (gx->made.ring_on) {
        made_flush(gx);
    }
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
    const WlXboxVshOutput *o = gx->xf_probe_out;
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
    // (kept from the last draw of the same program and constants: probes_kept)
    if (!probes_kept(gx, &gx->xf_probe_key, k, plan->live)) {
        reads = wl_nv2a_pipeline_inputs(&gx->pipeline, plan->live);
        for (n = 0; n < XF_PROBES; n++) {
            for (a = 0; a < WL_NV2A_ATTRIBS; a++) {
                if (reads & (1u << a)) {
                    memcpy(gx->batch.inputs[n][a], probe[n], sizeof probe[n]);
                }
            }
        }
        wl_nv2a_pipeline_vertices(&gx->pipeline, k, (const float (*)[WL_NV2A_ATTRIBS][4])gx->batch.inputs,
                                  XF_PROBES, plan->live, gx->xf_probe_out, &gx->batch.work);
    }
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
        if (!(draw->streamed & source) && g_draw_cuts) {
            // (passed as it is from an input the draw does not stream: the attribute's current
            // value at every vertex, wl_nv2a_fetch_constants', a constant word. As an array, the
            // vertex cache has none to keep for it, and the whole draw was sent: d40's 141 draws a
            // frame)
            xf->color_source[c] = -1;
            xf->color_word[c] = wl_nv2a_color_word(k->current[bit_index(source)]);
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

// a vertex as vertices_pack made it (landing x, y; its position words) as the EFB's x, y and GX's
// depth 0..1, the projection_load's mapping (the far plane's scale; the polygon offset left out)
static void query_vertex(const WlNv2aGx *gx, const PackedVertex *vertex, float out[3]) {
    float z, d;

    memcpy(&z, &vertex->w[2], sizeof z);
    out[0] = vertex->x;
    out[1] = vertex->y;
    if (gx->projection.perspective) {
        // (z is -w)
        float scale = gx->projection.a * DEPTH_SCALE > 1.0f ? 1.0f / gx->projection.a : DEPTH_SCALE;

        d = z < 0.0f ? (gx->projection.a + gx->projection.b / -z) * scale : 0.0f;
    } else {
        // (z is the depth less 1)
        d = (z + 1.0f) * DEPTH_SCALE;
    }
    out[2] = d;
}

// an occlusion query's vertices tested against the EFB's depth (query_count), at most
#define QUERY_VERTICES 16u
// and at most this many of its pixels read (a grid over a larger query, each read standing for its cell)
#define QUERY_READS 64u

// whether depth `z` (0..0xFFFFFF) passes the draw's depth test against `stored`
static bool query_passes(uint32_t func, uint32_t z, uint32_t stored) {
    switch (func) {
    case WL_NV2A_FUNC_NEVER:
        return false;
    case WL_NV2A_FUNC_LESS:
        return z < stored;
    case WL_NV2A_FUNC_EQUAL:
        return z == stored;
    case WL_NV2A_FUNC_LEQUAL:
        return z <= stored;
    case WL_NV2A_FUNC_GREATER:
        return z > stored;
    case WL_NV2A_FUNC_NOTEQUAL:
        return z != stored;
    case WL_NV2A_FUNC_GEQUAL:
        return z >= stored;
    default:
        return true;
    }
}

// The pixels of an occlusion query's triangle (a, b, c: EFB x, y and GX's depth 0..1) that pass the
// draw's depth test against the EFB's depth, read at pixel centres (a grid of cells over a large
// one, QUERY_READS reads at most, each counting for its cell). -1 when the EFB cannot be read.
static float query_triangle(const WlNv2aGx *gx, const float a[3], const float b[3], const float c[3],
                            uint32_t func) {
    float area = (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]), passed = 0.0f, step = 1.0f;
    float x0 = fminf(a[0], fminf(b[0], c[0])), x1 = fmaxf(a[0], fmaxf(b[0], c[0]));
    float y0 = fminf(a[1], fminf(b[1], c[1])), y1 = fmaxf(a[1], fmaxf(b[1], c[1]));
    float x, y, ex;
    bool narrow = gx->narrow && gx->owner == NULL;

    if (!(fabsf(area) > 0.0f)) {
        return 0.0f;
    }
    // (cells of step by step pixels: no more than QUERY_READS over the triangle's bounds)
    while ((x1 - x0) * (y1 - y0) / (step * step) > (float)QUERY_READS) {
        step *= 2.0f;
    }
    x0 = fmaxf(x0, 0.0f);
    y0 = fmaxf(y0, 0.0f);
    for (y = floorf(y0 / step) * step + 0.5f * step; y < y1; y += step) {
        for (x = floorf(x0 / step) * step + 0.5f * step; x < x1; x += step) {
            // (barycentric weights of the cell's centre, nudged off the edges two triangles of a quad
            // share, so that a centre on one counts once: inside when all three are >= 0)
            float sx = x + 1.0f / 64.0f, sy = y + 1.0f / 128.0f;
            float wa = ((b[0] - sx) * (c[1] - sy) - (c[0] - sx) * (b[1] - sy)) / area;
            float wb = ((c[0] - sx) * (a[1] - sy) - (a[0] - sx) * (c[1] - sy)) / area;
            float wc = 1.0f - wa - wb, depth;
            uint32_t z, stored;

            if (wa < 0.0f || wb < 0.0f || wc < 0.0f) {
                continue;
            }
            depth = wa * a[2] + wb * b[2] + wc * c[2];
            depth = depth < 0.0f ? 0.0f : depth > 1.0f ? 1.0f : depth;
            z = (uint32_t)(depth * 16777215.0f + 0.5f);
            // (the back buffer of a picture drawn narrow: its depth read at the EFB's column of the cell)
            ex = narrow ? (float)gx->narrow_x + x * (float)gx->narrow_w / (float)gx->cfg.width : x;
            stored = wl_gx_peek_z((int)ex, (int)y);
            if (stored > 0xFFFFFFu) {
                return -1.0f;
            }
            if (query_passes(func, z, stored)) {
                passed += step * step;
            }
        }
    }
    return passed;
}

// An occlusion query's pixel count with the depth test, as the NV2A counts it: its triangles' pixels
// tested against the EFB's depth (GX synced once for a run of queries, the draws before them drawn).
// Halo asks one for each lens flare and glow and scales the flare by it; counted by area alone, a
// flare behind a wall or a person showed through them (a10's opening: rings over the captain, the
// engines' glare). -1 when the query has too many vertices or the EFB cannot be read (the host with
// no renderer): the caller counts its area instead.
static float query_count(WlNv2aGx *gx, const WlNv2aKelvin *k, uint32_t primitive, const float (*v)[3], uint32_t n) {
    uint32_t func = wl_nv2a_k(k, WL_NV2A_K_SET_DEPTH_FUNC), i;
    float total = 0.0f, part;

    if (!wl_nv2a_k(k, WL_NV2A_K_SET_DEPTH_TEST_ENABLE)) {
        return -1.0f;
    }
    if (!gx->query_synced) {
        (void)wl_gx_sync();
        gx->query_synced = true;
    }
    for (i = 2; i < n; i++) {
        const float *a, *b, *c = v[i];

        switch (primitive) {
        case WL_NV2A_PRIM_TRIANGLES:
            if (i % 3u != 2u) {
                continue;
            }
            a = v[i - 2];
            b = v[i - 1];
            break;
        case WL_NV2A_PRIM_TRIANGLE_STRIP:
        case WL_NV2A_PRIM_QUAD_STRIP:
            a = v[i - 2];
            b = v[i - 1];
            break;
        case WL_NV2A_PRIM_TRIANGLE_FAN:
        case WL_NV2A_PRIM_POLYGON:
            a = v[0];
            b = v[i - 1];
            break;
        case WL_NV2A_PRIM_QUADS:
            if (i % 4u < 2u) {
                continue;
            }
            a = v[i - i % 4u];
            b = v[i - 1];
            break;
        default:
            return -1.0f;
        }
        part = query_triangle(gx, a, b, c, func);
        if (part < 0.0f) {
            return -1.0f;
        }
        total += part;
    }
    return total;
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
        if (target_texture(gx, stage, &t) == NULL) {
            display_read(gx, t.phys);
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
    float corners[3][2], area = 0.0f, query[QUERY_VERTICES][3], counted;
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
    gx->projection = projection_fit(gx, k, draw, plan.depth_max, fit_target(gx, surface));
    gx->memo_stamp++;
    wl_nv2a_fetch_plan(draw, wl_nv2a_pipeline_inputs(&gx->pipeline, plan.live), &gx->fetch);
    wl_nv2a_fetch_constants(&gx->fetch, k, gx->batch.inputs, WL_XBOX_VSH_BATCH);
    for (v = 0; v < draw->count; v++) {
        unsigned n = (v - first) % WL_XBOX_VSH_BATCH;

        if (n == 0) {
            first = v;
            // (this batch's vertices when it is the first, and the next batch's, asked for ahead)
            draw_prefetch(gx, &gx->fetch, draw, v == 0 ? 0 : v + WL_XBOX_VSH_BATCH,
                          v == 0 ? 2u * WL_XBOX_VSH_BATCH : WL_XBOX_VSH_BATCH);
            vertices_make(gx, k, draw, v, draw->count - v < WL_XBOX_VSH_BATCH ? draw->count - v : WL_XBOX_VSH_BATCH,
                          &plan, NULL);
        }
        landing_area(draw->primitive, batch_vertex(gx, n), &seen, corners, &area);
        if (v < QUERY_VERTICES) {
            query_vertex(gx, batch_vertex(gx, n), query[v]);
        }
    }
    gx->stats.unwritten_counted++;
    counted = !gx->cfg.query_by_area && draw->count <= QUERY_VERTICES ? query_count(gx, k, draw->primitive, (const float (*)[3])query, draw->count)
                                            : -1.0f;
    if (counted >= 0.0f) {
        gx->stats.queries_tested++;
        return (uint32_t)(counted + 0.5f);
    }
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

// ---- skinned models lit by the transform unit -----------------------------------------------------
// A program whose position stays on the processor (a skinned model's, its normal blended by bones)
// whose d0 has the light shape (WlXboxVshLightShape: an ambient plus directional lights' clamped dot
// products with a temporary as the normal, one with a back term) leaves d0's r, g and b to GX's
// lighting where the program has the draw's lit form (WlXboxVshFusedKey.lit): the form sends the
// normal after the position, and COLOR0 is lit by up to five lights (light 0 and the extra ones),
// the dot products clamped at 0. A back term max(d, q d, 0), q <= 0, is a light of its own the other
// way, of colour -q times the light's; 0 <= q <= 1 is none. The directions made of unit length (their
// length into the colour), the colours times 1 / s and the normal matrix s (s the largest colour
// component, as lit_finish: the colours' bytes their most precise). Not to the bit: the colours and
// the ambient are bytes; no_xf_skin (and no_xf) leave d0 to the processor.

// the number of components an array of the fused format gives (0: none)
static unsigned fused_components(unsigned format) {
    if (format >= WL_XBOX_VSH_FUSED_F1 && format <= WL_XBOX_VSH_FUSED_F4) {
        return format - WL_XBOX_VSH_FUSED_F1 + 1u;
    }
    if (format == WL_XBOX_VSH_FUSED_D3DCOLOR) {
        return 4u;
    }
    if (format == WL_XBOX_VSH_FUSED_CMP) {
        return 3u;
    }
    if (format >= WL_XBOX_VSH_FUSED_S1 && format < WL_XBOX_VSH_FUSED_FORMATS) {
        return (format - WL_XBOX_VSH_FUSED_S1) % 4u + 1u;
    }
    return 0u;
}

// the draw's lighting and its lit form (plan->lit, plan->fused), or false (plan->lit_wanted when only
// the form is missing)
static bool skin_lit(WlNv2aGx *gx, const WlNv2aKelvin *k, DrawPlan *plan, WlGxDirectLighting *lighting) {
    const WlXboxVshProgram *program = gx->pipeline.program;
    const WlXboxVshLightShape *shape = program != NULL ? &program->light : NULL;
    const float(*c)[4] = (const float(*)[4])k->constants;
    WlXboxVshFusedKey key;
    float directions[5][3], colors[5][3], most = 0.0f;
    unsigned count = 0, n, i;
    const WlXboxVshFusedForm *form = NULL;

    if (shape == NULL || shape->count == 0 || gx->cfg.no_xf_skin || gx->cfg.no_xf || gx->cfg.no_fused ||
        plan->colors < 1u || !(plan->channels & 1u) || projected_possible(gx, plan) || !fused_key(gx, plan, &key)) {
        return false;
    }
    // (the maximum's 0: the zero input's component no array gives, or a current value of 0)
    if (key.format[shape->zero] != WL_XBOX_VSH_FUSED_NONE
            ? fused_components(key.format[shape->zero]) > shape->zero_component
            : gx->batch.inputs[0][shape->zero][shape->zero_component] != 0.0f) {
        return false;
    }
    for (n = 0; n < shape->count; n++) {
        const WlXboxVshLight *light = &shape->lights[n];
        float sign = light->direction_negate ? -1.0f : 1.0f, length = 0.0f, q = 0.0f;

        for (i = 0; i < 3; i++) {
            length += c[light->direction][i] * c[light->direction][i];
        }
        length = sqrtf(length);
        if (light->has_back) {
            q = c[light->back][light->back_component] * (light->back_negate ? -1.0f : 1.0f);
            if (!(q <= 1.0f)) {
                return false;
            }
        }
        if (!(length > 0.0f)) {
            // (a dot product of 0: no term)
            continue;
        }
        for (i = 0; i < 3; i++) {
            directions[count][i] = sign * c[light->direction][i] / length;
            colors[count][i] = c[light->color][i] * length;
            if (!(colors[count][i] >= 0.0f) || colors[count][i] > 1.0e6f) {
                return false;
            }
        }
        count++;
        if (q < 0.0f) {
            if (count >= 5u) {
                return false;
            }
            for (i = 0; i < 3; i++) {
                directions[count][i] = -directions[count - 1u][i];
                colors[count][i] = -q * colors[count - 1u][i];
            }
            count++;
        }
        if (count > 5u) {
            return false;
        }
    }
    for (i = 0; i < 3; i++) {
        if (!(c[shape->ambient][i] >= 0.0f)) {
            return false;
        }
    }
    plan->lit_wanted = true;
    key.lit = 1u;
    form = fused_find(program, &key);
    if (form == NULL) {
        return false;
    }
    plan->lit = true;
    plan->fused_known = true;
    plan->fused = fused_form(gx, plan);
    if (plan->fused != form) {
        plan->lit = false;
        plan->fused_known = false;
        plan->fused = NULL;
        return false;
    }
    memset(lighting, 0, sizeof *lighting);
    for (n = 0; n < count; n++) {
        for (i = 0; i < 3; i++) {
            if (colors[n][i] > most) {
                most = colors[n][i];
            }
        }
    }
    if (!(most > 0.0f)) {
        most = 1.0f;
    }
    for (i = 0; i < 3; i++) {
        lighting->normal_matrix[i][i] = most;
        lighting->ambient[0][i] =
            (uint8_t)(wl_nv2a_color_word((const float[4]){c[shape->ambient][i], 0.0f, 0.0f, 0.0f}) >> 24);
    }
    for (n = 0; n < count; n++) {
        float *direction = n == 0 ? lighting->direction[0] : lighting->extra_direction[n - 1u];
        uint8_t *color = n == 0 ? lighting->color[0] : lighting->extra_color[n - 1u];

        for (i = 0; i < 3; i++) {
            float v = colors[n][i] / most * 255.0f + 0.5f;

            direction[i] = directions[n][i];
            color[i] = (uint8_t)(v > 255.0f ? 255.0f : v);
        }
        if (n != 0) {
            lighting->extra[0] |= (uint8_t)(1u << (n - 1u));
        }
    }
    lighting->clamped = 1u;
    return true;
}

// a draw's vertex bytes as the console's FIFO takes them (gx_shadow.h's model), counted by its way
static void draw_bytes(WlNv2aGx *gx, const WlGxDirectLayout *layout, uint32_t count) {
    if (memcmp(&gx->bytes_layout, layout, sizeof *layout) != 0) {
        gx->bytes_layout = *layout;
        gx->layout_bytes = wl_gx_shadow_layout_vertex_bytes(layout);
    }
    gx->stats.xf_why_bytes[gx->xf_why] += (uint64_t)count * gx->layout_bytes;
}

// ---- sphere maps made by the transform unit ------------------------------------------------------
// A sphere map's coordinates (wl_nv2a_sphere_coordinates: two roots, a division's worth of work a
// vertex) are GX's: the processor sends the direction of unit length (one root) as a projected
// coordinate's three numbers, after a lit form's normal, and GX's generator makes (x, y, z + 1),
// normalizes it and halves it about 1/2 (WlGxDirectLayout.texcoords_sphere). Not beside projected
// coordinates, two at most (a lit form's normal the third), and only for a program with no fused forms
// or with one of them (a "U" stage): an older forms list keeps its "S" forms. The switch "nospherexf"
// (WL_XB_NOSPHEREXF) leaves them to the processor.

// a lit form's normal's words beside `count` such directions (the normal, binormal and tangent's)
static unsigned sphere_lit_words(unsigned count) {
    return count == 0u ? 3u : count == 1u ? 6u : 0u;
}

static void sphere_xf_plan(WlNv2aGx *gx, DrawPlan *plan, WlGxDirectLayout *layout) {
    const WlXboxVshProgram *program = gx->pipeline.program;
    unsigned n, count = 0, words;
    bool forms = program != NULL && program->native_fused_count == 0;

    plan->sphere_xf = 0;
    if (!switched_on(SWITCH_NO_SPHERE_XF) || program == NULL || plan->stage_count == 0) {
        return;
    }
    for (n = 0; !forms && n < program->native_fused_count; n++) {
        const WlXboxVshFusedKey *key = &program->native_fused[n].key;
        unsigned i;

        for (i = 0; i < key->stage_count; i++) {
            forms = forms || (key->stages[i] >> 2) == WL_XBOX_VSH_FUSED_STAGE_SPHERE_XF;
        }
    }
    for (n = 0; forms && n < plan->stage_count; n++) {
        count += plan->stages[plan->stage_list[n]].sphere && !plan->stages[plan->stage_list[n]].spot ? 1u : 0u;
    }
    if (count == 0 || count > 2u || projected_possible(gx, plan)) {
        return;
    }
    // (a vertex's words within PACKED_WORDS, a lit form's normal counted)
    words = plan->words - 2u * count + (count == 1u ? 3u : 9u) + sphere_lit_words(count);
    if (words > PACKED_WORDS) {
        return;
    }
    for (n = 0; n < plan->stage_count; n++) {
        unsigned stage = plan->stage_list[n];
        StagePlan *sp = &plan->stages[stage];

        if (sp->sphere && !sp->spot) {
            sp->sphere_xf = true;
            plan->sphere_xf |= 1u << stage;
        }
    }
    layout->texcoords &= (uint8_t)~plan->sphere_xf;
    layout->texcoords_projected = (uint8_t)plan->sphere_xf;
    layout->texcoords_sphere = (uint8_t)plan->sphere_xf;
    plan->words += (count == 1u ? 3u : 9u) - 2u * count;
}

// ---- the world position to the transform unit ------------------------------------------------------
// A program whose clip-space position is four dph's of one temporary and four constants
// (WlXboxVshWorldPosition: a skinned program's blended position) leaves its x and y to GX: its fused
// "w" form sends the temporary's x, y and z with the clip-space w the program makes in place of one of
// them, k (the one c[w] weighs most: world_slot), and GX's position matrix recovers component k from w
// and makes (x w, y w, -w) in pixels with the viewport and the samples folded in (xf_prepare's
// perspective rows), loaded for each draw. The depth is the processor's w to the bit, as for the
// other passes over the same geometry (another program's, or the general way's: their depths meet
// for the depth test's equal); x and y are not (the transform unit's arithmetic is its own: an edge's
// pixel may move). Every vertex, one bone or two, no run split. Perspective draws only, every batch
// fused. The switch "noworldxf" (WL_XB_NOWORLDXF) leaves the position to the processor.
// ---- texture coordinates by GX's texgen from one input ----------------------------------------------
// A fused draw's stage ("w", "p" or "o" forms) whose coordinate the program makes affinely from one
// input register of one or two components (a skinned model's T0 = c13, c14 . v4, T1 = T0 * c12, T2 =
// T0: wl_xbox_vsh_texgen_source) is GX's: its form ("M", WL_XBOX_VSH_FUSED_STAGE_INPUT) sends the register's x and y once, in the first
// such stage's coordinate, and GX's texgen makes each stage's coordinate from them through a 2x4 matrix
// (the program run for four probe vertices, as xf_prepare's, a linear texture's scale folded in), the
// stages after the first from the first one's input (WL_GX_DIRECT_TG_INPUT0): two words a stage fewer,
// and the program's instructions for them not run. Not to the bit (GX's arithmetic is its own). A
// projective stage's q must be 1. The switch "noxftexgen" (WL_XB_NOXFTEXGEN) leaves them to the processor.
// whether texgen_xf_plan's probe outputs for `live` are those of the program and constants now: the same
// program, and the constants its instructions for those outputs read the same (none by a0); else the key
// set for the probes about to run (false)
static bool texgen_probes_kept(WlNv2aGx *gx, const WlNv2aKelvin *k, unsigned live) {
    const WlXboxVshProgram *program = gx->pipeline.program;
    const float(*c)[4] = (const float(*)[4])k->constants;
    int n;

    if (!g_draw_cuts) {
        return false;
    }
    if (gx->tg_kept.valid && gx->tg_kept.program == program && gx->tg_kept.serial == program->serial &&
        gx->tg_kept.live == live && gx->tg_kept.count >= 0) {
        for (n = 0; n < gx->tg_kept.count; n++) {
            if (memcmp(gx->tg_kept.values[n], c[gx->tg_kept.registers[n]], sizeof gx->tg_kept.values[n]) != 0) {
                break;
            }
        }
        if (n == gx->tg_kept.count) {
            gx->stats.probes_kept++;
            return true;
        }
    } else {
        gx->tg_kept.program = program;
        gx->tg_kept.serial = program->serial;
        gx->tg_kept.live = live;
        gx->tg_kept.count = wl_xbox_vsh_live_constants(program, live, gx->tg_kept.registers, TG_CONSTANTS);
    }
    gx->tg_kept.valid = gx->tg_kept.count >= 0;
    for (n = 0; n < gx->tg_kept.count; n++) {
        memcpy(gx->tg_kept.values[n], c[gx->tg_kept.registers[n]], sizeof gx->tg_kept.values[n]);
    }
    return false;
}

static void texgen_xf_plan(WlNv2aGx *gx, const WlNv2aKelvin *k, const WlNv2aDraw *draw, DrawPlan *plan,
                           XfPlan *xf) {
    enum { T_ZERO, T_X, T_Y, T_W, T_PROBES };
    static const float probe[T_PROBES][4] = {
        {0.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}};
    const WlXboxVshDependencies *d = &gx->xf_dependencies;
    const WlXboxVshOutput *o = gx->tg_probe_out;
    unsigned n, stage, live = 0, candidates = 0, row, a;
    bool full;

    plan->texgen_xf = 0;
    if (!switched_on(SWITCH_NO_XF_TEXGEN) || gx->pipeline.program == NULL) {
        return;
    }
    // (a program with a form of such stages: the probes not run for the others)
    for (n = 0; n < gx->pipeline.program->native_fused_count; n++) {
        const WlXboxVshFusedKey *key = &gx->pipeline.program->native_fused[n].key;

        for (a = 0; a < key->stage_count && (key->stages[a] >> 2) != WL_XBOX_VSH_FUSED_STAGE_INPUT; a++) {
        }
        if (a < key->stage_count) {
            break;
        }
    }
    if (n == gx->pipeline.program->native_fused_count) {
        return;
    }
    // (the program's analysis, xf_structure's: its verdict for the position is not this one's)
    (void)xf_structure(gx, plan->live, &full);
    for (n = 0; n < plan->stage_count; n++) {
        const StagePlan *sp;
        unsigned source;

        stage = plan->stage_list[n];
        sp = &plan->stages[stage];
        if (stage > 3u || sp->sphere || sp->sphere_xf || sp->spot) {
            continue;
        }
        source = xf_source(d->t[stage], 2);
        // (a q that reads no input, 1 at the probes; the register streamed, of two components at most)
        if (source == 0u || source == 1u || source == ~0u || (sp->projective && xf_source(&d->t[stage][3], 1) != 0u) ||
            !(draw->streamed & source) || draw->attribs[bit_index(source)].size > 2u) {
            continue;
        }
        plan->texgen_from[stage] = (unsigned char)bit_index(source);
        candidates |= 1u << stage;
        live |= (unsigned)WL_XBOX_VSH_LIVE_T0 << stage;
    }
    if (candidates == 0) {
        return;
    }
    if (!texgen_probes_kept(gx, k, live) && !probes_kept(gx, &gx->tg_probe_key, k, live)) {
        uint32_t reads = wl_nv2a_pipeline_inputs(&gx->pipeline, live);

        for (n = 0; n < T_PROBES; n++) {
            for (a = 0; a < WL_NV2A_ATTRIBS; a++) {
                if (reads & (1u << a)) {
                    memcpy(gx->tg_probe_in[n][a], probe[n], sizeof probe[n]);
                }
            }
        }
        wl_nv2a_pipeline_vertices(&gx->pipeline, k, (const float (*)[WL_NV2A_ATTRIBS][4])gx->tg_probe_in, T_PROBES,
                                  live, gx->tg_probe_out, &gx->batch.work);
    }
    for (stage = 0; stage < 4u; stage++) {
        const StagePlan *sp = &plan->stages[stage];

        if (!(candidates & (1u << stage))) {
            continue;
        }
        if (sp->projective) {
            for (n = 0; n < T_PROBES && o[n].t[stage][3] == 1.0f; n++) {
            }
            if (n < T_PROBES) {
                continue;
            }
        }
        for (row = 0; row < 2; row++) {
            float scale = !sp->linear ? 1.0f : row == 0 ? sp->to_s : sp->to_t;
            float base = o[T_ZERO].t[stage][row];

            xf->stage_matrix[stage][row][0] = (o[T_X].t[stage][row] - base) * scale;
            xf->stage_matrix[stage][row][1] = (o[T_Y].t[stage][row] - base) * scale;
            xf->stage_matrix[stage][row][2] = 0.0f;
            xf->stage_matrix[stage][row][3] = o[T_W].t[stage][row] * scale;
        }
        xf->stage_source[stage] = plan->texgen_from[stage];
        plan->texgen_xf |= 1u << stage;
    }
}

// the stage before `stage` (in the draw's order) whose input GX's texgen reads for it, or stage itself
// when it is the first made from its register (its coordinate the one sent)
static unsigned texgen_xf_first(const DrawPlan *plan, unsigned stage) {
    unsigned n;

    for (n = 0; n < plan->stage_count && plan->stage_list[n] < stage; n++) {
        if ((plan->texgen_xf & (1u << plan->stage_list[n])) &&
            plan->texgen_from[plan->stage_list[n]] == plan->texgen_from[stage]) {
            return plan->stage_list[n];
        }
    }
    return stage;
}

// the layout and the words for the stages GX's texgen makes (the first of each register sent)
static void texgen_xf_layout(DrawPlan *plan, WlGxDirectLayout *layout) {
    unsigned stage;

    for (stage = 0; stage < 4u; stage++) {
        if ((plan->texgen_xf & (1u << stage)) && texgen_xf_first(plan, stage) != stage) {
            layout->texcoords &= (uint8_t)~(1u << stage);
            layout->texcoords_generated |= (uint8_t)(1u << stage);
            plan->words -= 2u;
        }
    }
}

// ... and its generators, after the layout
static void texgen_xf_load(WlNv2aGx *gx, const DrawPlan *plan, const XfPlan *xf) {
    unsigned stage;

    for (stage = 0; plan->texgen_xf != 0 && stage < 4u; stage++) {
        if (plan->texgen_xf & (1u << stage)) {
            unsigned first = texgen_xf_first(plan, stage);

            wl_gx_direct_texgen(stage,
                                first == stage ? WL_GX_DIRECT_TG_TEXCOORD
                                               : (WlGxDirectTexSource)(WL_GX_DIRECT_TG_INPUT0 + first),
                                (const float (*)[4])xf->stage_matrix[stage]);
            gx->xf_texgens |= (uint8_t)(1u << stage);
        }
    }
}

// A draw the processor transforms ("p" and "o" forms, no world position) whose fused form has stages GX's texgen
// makes: the form decided before the layout (fused_for's lazy choice made here), the matrices into xf (its
// position unused). false: none, the draw's form found at its first batch as before
static bool texgen_fused_plan(WlNv2aGx *gx, const WlNv2aKelvin *k, const WlNv2aDraw *draw, DrawPlan *plan,
                              XfPlan *xf) {
    const WlXboxVshProgram *program = gx->pipeline.program;
    WlXboxVshFusedKey key;
    unsigned n;

    if (!switched_on(SWITCH_NO_XF_TEXGEN) || gx->cfg.no_xf || gx->cfg.no_fused || program == NULL ||
        program->native_fused_count == 0 || plan->xf != NULL || plan->fused_known || plan->stage_count == 0 ||
        draw->source == WL_NV2A_SOURCE_IMMEDIATE || projected_possible(gx, plan)) {
        return false;
    }
    memset(xf, 0, sizeof *xf);
    texgen_xf_plan(gx, k, draw, plan, xf);
    if (plan->texgen_xf == 0 || !fused_key(gx, plan, &key)) {
        plan->texgen_xf = 0;
        return false;
    }
    for (n = 0; n < 4u; n++) {
        if ((plan->texgen_xf & (1u << n)) &&
            wl_xbox_vsh_texgen_source(&gx->xf_dependencies, &key, n) != (int)plan->texgen_from[n]) {
            plan->texgen_xf = 0;
            return false;
        }
    }
    if (fused_find(program, &key) == NULL) {
        plan->texgen_xf = 0;
        return false;
    }
    plan->fused_known = true;
    plan->fused = fused_form(gx, plan);
    if (plan->fused == NULL) {
        plan->fused_known = false;
        plan->texgen_xf = 0;
        return false;
    }
    return true;
}

static bool world_xf_plan(WlNv2aGx *gx, const WlNv2aKelvin *k, const WlNv2aDraw *draw, DrawPlan *plan, XfPlan *xf) {
    const WlXboxVshProgram *program = gx->pipeline.program;
    const float(*c)[4] = (const float(*)[4])k->constants;
    const float *cw = c[program != NULL ? program->world.constant[3] : 0];
    WlXboxVshFusedKey key;
    float to_x, to_y, w_x, w_y;
    double rows[2][4];
    unsigned n, j, most = 0;

    if (!switched_on(SWITCH_NO_WORLD_XF) || gx->cfg.no_xf || gx->cfg.no_fused || gx->cfg.no_fused_xf ||
        program == NULL || !program->world.found || program->native_fused_count == 0 ||
        !gx->projection.perspective || plan->landing || plan->xf != NULL || draw->source == WL_NV2A_SOURCE_IMMEDIATE ||
        (!plan->fused_known && projected_possible(gx, plan))) {
        return false;
    }
    // (component k: the one w weighs most, recovered as (w - the rest of c[w] . (x, y, z, 1)) / c[w][k])
    for (n = 1; n < 3; n++) {
        if (fabsf(cw[n]) > fabsf(cw[most])) {
            most = n;
        }
    }
    if (!(fabsf(cw[most]) > 1.0e-6f * (fabsf(cw[0]) + fabsf(cw[1]) + fabsf(cw[2]) + fabsf(cw[3])))) {
        return false;
    }
    memset(xf, 0, sizeof *xf);
    xf->world = true;
    xf->words = plan->words;
    xf->attributes = 1u;
    // ((x w, y w, -w) in pixels from the clip-space position c[n] . (x, y, z, 1), as the "p" forms make
    // it, for the vertex (x, y, z) with w at k)
    to_x = plan->to_sx != 0.0f ? plan->scale[0] * plan->to_sx : plan->scale[0] / plan->sx;
    to_y = plan->to_sy != 0.0f ? plan->scale[1] * plan->to_sy : plan->scale[1] / plan->sy;
    w_x = (plan->to_sx != 0.0f ? plan->offset[0] * plan->to_sx : plan->offset[0] / plan->sx) + 0.5f;
    w_y = (plan->to_sy != 0.0f ? plan->offset[1] * plan->to_sy : plan->offset[1] / plan->sy) + 0.5f;
    for (j = 0; j < 2; j++) {
        const float *r = c[program->world.constant[j]];
        double by = (double)r[most] / (double)cw[most];

        for (n = 0; n < 4; n++) {
            rows[j][n] = n == most ? by : (double)r[n] - by * (double)cw[n];
        }
    }
    for (n = 0; n < 4; n++) {
        double on_w = n == most ? 1.0 : 0.0;

        xf->position[0][n] = (float)(rows[0][n] * (double)to_x + on_w * (double)w_x);
        xf->position[1][n] = (float)(rows[1][n] * (double)to_y + on_w * (double)w_y);
        xf->position[2][n] = n == most ? -1.0f : 0.0f;
    }
    // (the draw's form with the world position: the lit one where skin_lit lights the draw; with the
    // coordinates GX makes from an input where it has one, else without)
    plan->xf = xf;
    texgen_xf_plan(gx, k, draw, plan, xf);
    if (plan->texgen_xf != 0 && fused_key(gx, plan, &key)) {
        for (n = 0; n < 4u; n++) {
            if ((plan->texgen_xf & (1u << n)) &&
                wl_xbox_vsh_texgen_source(&gx->xf_dependencies, &key, n) != (int)plan->texgen_from[n]) {
                break;
            }
        }
        if (n < 4u || fused_find(program, &key) == NULL) {
            plan->texgen_xf = 0;
        }
    } else {
        plan->texgen_xf = 0;
    }
    if (!fused_key(gx, plan, &key) || fused_find(program, &key) == NULL) {
        plan->xf = NULL;
        plan->texgen_xf = 0;
        return false;
    }
    plan->fused_known = true;
    plan->fused = fused_form(gx, plan);
    if (plan->fused == NULL) {
        plan->xf = NULL;
        plan->fused_known = false;
        plan->texgen_xf = 0;
        return false;
    }
    gx->fused.world_slot = 4u * most;
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
    if (!surface_prepare(gx, &surface, k)) {
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
    if (gx->standin_hidden) {
        gx->stats.standin_hidden++;
        return 0;
    }
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
    plan.texgen_xf = 0;
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

    sphere_xf_plan(gx, &plan, &layout);
    gx->projection = projection_fit(gx, k, draw, plan.depth_max, fit_target(gx, &surface));
    if (gx->cfg.depth_check) {
        projection_check(gx, k, draw, &gx->projection, plan.depth_max);
    }
    if (gx->projection.perspective) {
        gx->stats.perspective_draws++;
    }
    projection_load(gx, k, &gx->projection);

    // the transform unit's draw, or the processor's (the transform unit's matrices then the identity)
    xf_ok = xf_prepare(gx, k, draw, &plan, count_pixels, &xf);
    if (xf_ok && !xf.full) {
        // (the position alone: the layout and the coordinates as the processor makes them. The
        // position GX reads from the vertex cache's array of it, the same floats, where it has one:
        // an index of two bytes a vertex for its twelve; the other passes of the same geometry have
        // it already)
        const void *positions = NULL;

        plan.xf = &xf;
        // (not for a draw whose vertices GX reads from their slots: made_begin)
        if (switched_on(SWITCH_NO_POSITION_INDEX) && plan.sphere_xf == 0 && plan.words > 3u && draw->source == WL_NV2A_SOURCE_ARRAYS &&
            (draw->streamed & 1u) && wl_nv2a_vcache_on(&gx->vertices) && !made_possible(gx, draw->count) &&
            draw->index_last - draw->index_base < WL_NV2A_VCACHE_SPAN) {
            positions = wl_nv2a_vcache_get(&gx->vertices, gx->dev, &draw->attribs[0], draw->index_base,
                                           draw->index_last, WL_NV2A_VCACHE_POSITION);
            layout.indexed = positions != NULL ? (uint16_t)(1u << WL_GX_DIRECT_ARRAY_POSITION) : 0u;
        }
        // (the vertices made into slots GX reads them from, where they fit: made_begin)
        (void)made_begin(gx, draw->count);
        made_layout(gx, &layout);
        wl_gx_direct_layout(&layout);
        if (positions != NULL) {
            wl_gx_direct_array(WL_GX_DIRECT_ARRAY_POSITION, positions,
                               wl_nv2a_vcache_element_bytes(WL_NV2A_VCACHE_POSITION));
            if (wl_nv2a_vcache_made(&gx->vertices)) {
                wl_gx_direct_arrays_changed();
            }
            gx->stats.position_indexed_draws++;
            gx->stats.position_indexed_vertices += draw->count;
        }
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
            if (switched_on(SWITCH_NO_COLOR_REGISTER)) {
                // (a constant colour, which every vertex would send as the same word, GX's channel
                // register instead: four bytes a vertex fewer)
                unsigned c;

                for (c = 0; c < plan.colors && c < 2u; c++) {
                    if (xf.color_source[c] < 0) {
                        layout.colors_register |= (uint8_t)(1u << c);
                    }
                }
            }
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
        if (layout.colors_register != 0) {
            // (the register is also a lit channel's material, white: a lit colour channel keeps its
            // red, green and blue at 255, a lit alpha channel its alpha, and the unlit one of the
            // pair reads the vertex's constant colour)
            unsigned c;

            for (c = 0; c < 2u; c++) {
                if (layout.colors_register & (1u << c)) {
                    uint32_t lit = (xf.lit >> (2u * c)) & 3u;

                    wl_gx_direct_material(c, xf.color_word[c] | ((lit & 1u) ? 0xFFFFFF00u : 0u) | ((lit & 2u) ? 0xFFu : 0u));
                }
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
        // (the layout set once the fetch is planned: skin_lit may light d0)
        memset(&xf, 0, sizeof xf);
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
            if (!(layout.indexed & (1u << (WL_GX_DIRECT_ARRAY_COLOR0 + c))) && !(layout.colors_register & (1u << c))) {
                words[colors++] = xf.color_word[c];
            }
        }
        draw_bytes(gx, &layout, draw->count);
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
    plan.lit = plan.lit_wanted = false;
    if (plan.xf == NULL) {
        WlGxDirectLighting lighting;
        bool lit, world;

        (void)made_begin(gx, draw->count);
        lit = skin_lit(gx, k, &plan, &lighting);
        if (lit) {
            layout.lit = 1u;
            plan.words += sphere_lit_words((unsigned)__builtin_popcount(plan.sphere_xf));
        }
        // (the world position's form decided before the layout: GX's texgen may make coordinates)
        world = world_xf_plan(gx, k, draw, &plan, &xf);
        if (world || texgen_fused_plan(gx, k, draw, &plan, &xf)) {
            texgen_xf_layout(&plan, &layout);
        }
        made_layout(gx, &layout);
        wl_gx_direct_layout(&layout);
        if (lit) {
            wl_gx_direct_lighting(&lighting);
            gx->stats.skin_lit_draws++;
            gx->stats.skin_lit_vertices += draw->count;
        }
        if (world) {
            wl_gx_direct_position_matrix((const float (*)[4])xf.position);
            gx->xf_position = true;
            gx->stats.xf_position_draws++;
        }
        if (plan.texgen_xf != 0) {
            texgen_xf_load(gx, &plan, &xf);
            gx->stats.xf_texgen_draws++;
            gx->stats.xf_texgen_vertices += draw->count;
        }
    }
    // (the first two batches' vertices asked for, then each batch the next one's: wl_nv2a_fetch_prefetch)
    draw_prefetch(gx, &gx->fetch, draw, 0, 2u * WL_XBOX_VSH_BATCH);
    // (the first batch, which finds the projective coordinates GX is to divide at each pixel)
    if (draw->count != 0 &&
        vertices_make(gx, k, draw, 0, draw->count < WL_XBOX_VSH_BATCH ? draw->count : WL_XBOX_VSH_BATCH, &plan,
                      &layout)) {
        // (projected coordinates: the position sent, as the other attributes are; or all read from
        // the slots)
        if (gx->made.ring_on) {
            made_layout(gx, &layout);
        } else {
            layout.indexed = layout.texcoords_projected != 0 ? 0u : layout.indexed;
        }
        wl_gx_direct_layout(&layout);
    }
    if (gx->made.ring_on) {
        gx->made.indexed = made_arrays(gx, &plan, &layout);
    }
    draw_bytes(gx, &layout, draw->count);
    wl_gx_direct_primitive_begin(gx_primitive(draw->primitive), draw->count);
    for (first = 0; first < draw->count; first += run) {
        run = draw->count - first < WL_XBOX_VSH_BATCH ? draw->count - first : WL_XBOX_VSH_BATCH;
        if (first != 0) {
            draw_prefetch(gx, &gx->fetch, draw, first + WL_XBOX_VSH_BATCH, WL_XBOX_VSH_BATCH);
            vertices_make(gx, k, draw, first, run, &plan, NULL);
        }
        // (the batch's vertices into the FIFO in one call: their slots' indices, or their words)
        if (gx->made.indexed) {
            wl_gx_direct_vertices_index(gx->made.index, 0, run, NULL, 0);
        } else if (layout.indexed != 0) {
            wl_gx_direct_vertices_position_index(draw->indices, draw->index_base, first, run, gx->batch.words, 3u,
                                                 plan.words - 3u);
        } else {
            wl_gx_direct_vertices_words(gx->batch.words, run, plan.words);
        }
        for (v = 0; count_pixels && v < run; v++) {
            landing_area(draw->primitive, batch_vertex(gx, v), &seen, corners, &area);
        }
    }
    wl_gx_direct_primitive_end();
    made_end(gx, draw->count);
    {
        float most = (float)gx->view_w * (float)gx->view_h, pixels = area * 0.5f;

        return (uint32_t)(pixels > most ? most : pixels);
    }
}

// ---- clears ---------------------------------------------------------------------------------------

static void backend_clear(void *user, const WlNv2aKelvin *k, uint32_t flags) {
    ((WlNv2aGx *)user)->query_synced = false;
    WlNv2aGx *gx = user;
    WlNv2aSurface s;
    uint32_t h = wl_nv2a_k(k, WL_NV2A_K_SET_CLEAR_RECT_HORIZONTAL);
    uint32_t v = wl_nv2a_k(k, WL_NV2A_K_SET_CLEAR_RECT_VERTICAL);
    bool colour = (flags & WL_NV2A_CLEAR_COLOR) != 0, depth = (flags & WL_NV2A_CLEAR_Z) != 0;
    // (only some of the colour's channels: red, green and blue together, or alpha alone, on GX)
    bool rgb = (flags & (WL_NV2A_CLEAR_R | WL_NV2A_CLEAR_G | WL_NV2A_CLEAR_B)) != 0;
    bool part = colour && rgb != ((flags & WL_NV2A_CLEAR_A) != 0);
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
    if (surface_is_screen(gx, &s) && !gx->frame_open && colour && !part && depth && z >= 1.0f && x0 == 0 && y0 == 0 &&
        x1 >= gx->cfg.width && y1 >= gx->cfg.height) {
        // the frame's first clear, of all of it: GX's own (the copy that ends a frame clears the EFB)
        frame_begin(gx, rgba);
        gx->back_phys = s.color_phys;
        gx->back_drawn = true;
        wl_gx_target_drawn();
        return;
    }
    if (!surface_prepare(gx, &s, NULL)) {
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
    if (part) {
        if (depth) {
            clear_rect(gx, false, true, rgba, z, x0, y0, x1, y1);
        }
        clear_channels(gx, flags, rgba, x0, y0, x1, y1);
        return;
    } else {
        clear_rect(gx, colour, depth, rgba, z, x0, y0, x1, y1);
    }
    gx->stats.quad_clears++;
    device_state(gx);
}

// ---- frames and flips -------------------------------------------------------------------------------

static void backend_frame_done(void *user, const WlNv2aKelvin *k) {
    ((WlNv2aGx *)user)->query_synced = false;
    WlNv2aGx *gx = user;

    (void)k;
    gx->state_valid = false;
    if (gx->frame_open && gx->back_drawn) {
        wl_nv2a_gx_present(gx);
    }
}

static void backend_flip(void *user, const WlNv2aKelvin *k, uint32_t phys) {
    ((WlNv2aGx *)user)->query_synced = false;
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
