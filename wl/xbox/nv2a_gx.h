// SPDX-License-Identifier: MIT
// The NV2A's draws on GX (nv2a_gx.c): the backend (nv2a.h) that draws what a title's push buffers
// ask for. Target wl_xbox_gx; it calls only wl/platform (the direct GX layer, render targets, TEV
// programs), so it builds and runs everywhere, over libogc on the Wii and over the host's recorder
// elsewhere (host/gx_direct_host.h), where tests see what it would have drawn.
//
// What it does, OpenCE-Wii's Direct3D device on GX (d3d8_gx.c) reorganized around the NV2A's state:
//  - vertices on the processor: each draw's vertices fetched, run through the title's vertex
//    program (wl/xbox/vsh), and sent to GX in screen space through an orthographic projection with
//    the depth range, or, for a program that divides by its clip-space w, as (x w, y w, -w) through
//    a perspective projection whose depth a + b / w is fitted from the draw's vertices (so GX clips
//    at the eye and interpolates for the perspective); the far plane moved so nothing is clipped
//    there;
//  - the pixel stage: the combiners translated to TEV (wl/xbox/psh through its cache, with a title's
//    recipe hook), the fog a ramp texture the translation reads; a program beyond GX falls back to
//    texture 0 modulated by the diffuse colour;
//  - textures converted into GX's formats and kept (nv2a_texcache.h);
//  - the raster state: blending (GX's factors; constant colours, MIN, MAX and src - dst
//    approximated), depth, alpha test, culling by the NV2A's own rule on the screen winding (GX's
//    sense measured once), colour masks; draws with the stencil test, which GX has not, skipped;
//  - surfaces: the EFB is the back buffer for the frame (A8R8G8B8 640x480 with a depth); a draw into
//    another surface takes the EFB through wl/platform's render targets, and its pixels are copied
//    into an image a texture of the same address then samples;
//  - clears: the frame's first full clear is GX's own copy clear; the rest are quads;
//  - presenting: a frame is copied to the display when the title says it finished its back buffer
//    (FLIP_INCREMENT_WRITE), when it starts drawing another screen-sized surface, or when the
//    display is flipped to the surface still in the EFB.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "wl/xbox/nv2a.h"
#include "wl/xbox/nv2a_texcache.h"
#include "wl/xbox/nv2a_vcache.h"
#include "wl/xbox/psh.h"
#include "wl/xbox/vsh.h"

typedef struct WlNv2aGx WlNv2aGx;

typedef struct WlNv2aGxConfig {
    // vertex programs compiled ahead of time (tools/xbox_vsh_codegen.c), or NULL
    const WlXboxVshNative *natives;
    unsigned long native_count;
    // called for each vertex program decoded (its instructions, four words each, and whether it was
    // among the natives), to list a title's programs for compiling ahead of time (NULL: none)
    void (*program_decoded)(void *user, const uint32_t *words, unsigned long count, bool native);
    void *program_user;
    // a title's pixel programs made by hand (NULL: none), copied
    const WlXboxPshRecipe *recipe;
    // the converted textures' budget and memory (zero: 4 MiB from aligned_alloc; flush NULL: the
    // images committed for the GPU, wl_gpu_buffer_commit)
    WlNv2aTexCacheConfig textures;
    // the vertex arrays kept for GX to read by index (nv2a_vcache.h: budget 0, none; flush NULL: the
    // arrays committed for the GPU): a draw through the transform unit whose attributes are all
    // kept sends its indices alone
    WlNv2aVCacheConfig vertices;
    // the back buffer's size in EFB pixels (0: 640 x 480)
    uint32_t width, height;
    // keep the EFB without a destination alpha (RGB8_Z24; default: RGBA6_Z24, 6 bits a channel, as
    // a title that blends by the destination's alpha needs)
    bool no_efb_alpha;
    // every vertex through the program on the processor, never GX's transform unit (the draws whose
    // live outputs are affine in their inputs otherwise go to the transform unit: not to the bit)
    bool no_xf;
} WlNv2aGxConfig;

typedef struct WlNv2aGxStats {
    uint32_t draws, vertices, clears, quad_clears, frames;
    uint32_t stencil_skipped;     // draws with the stencil test that write only the stencil (skipped)
    uint32_t stencil_ignored;     // draws with the stencil test drawn as if it passed
    uint32_t target_draws;        // draws into a surface other than the back buffer
    uint32_t target_skipped;      // ... that had no render target (no memory, a size or format GX has not)
    uint32_t programs_unusable;   // draws whose combiners GX cannot hold (texture 0 times colour)
    uint32_t approximations;      // draws whose combiners or blending were approximated
    uint32_t vertices_reused;     // vertices of indexed draws taken from the ones already run
    uint32_t perspective_draws;   // draws through the perspective projection
    uint32_t fits_made;           // ...whose depth a + b / w was fitted for a transform not seen
    uint32_t fits_shared;         // ...that used the fit kept for their transform
    uint32_t xf_draws;            // draws whose vertices GX's transform unit transformed
    uint32_t xf_vertices;         // ...and their vertices
    uint32_t xf_position_draws;   // draws whose positions alone it transformed (the rest the program's)
    uint32_t unwritten_counted;   // draws that write nothing (no colour, no depth) counting pixels: counted, not drawn
    uint32_t unwritten_skipped;   // ...and not counting: skipped
    uint32_t indexed_draws;       // draws through the transform unit that sent their indices alone
    uint32_t indexed_vertices;    // ...and their vertices
    uint32_t raster_kept;         // draws that left GX's raster state as the draw before (unchanged)
    uint32_t textures_kept;       // ...its textures
    uint32_t combiners_kept;      // ...its pixel program
} WlNv2aGxStats;

// A backend over GX, which must be up (wl_gx_init). NULL when there is no memory. The device it
// reads guest memory through is attached afterwards (the device is made with the backend).
WlNv2aGx *wl_nv2a_gx_create(const WlNv2aGxConfig *cfg);
void wl_nv2a_gx_destroy(WlNv2aGx *gx);
void wl_nv2a_gx_attach(WlNv2aGx *gx, WlNv2a *dev);
void wl_nv2a_gx_backend(WlNv2aGx *gx, WlNv2aBackend *out);
// A frame begun and not yet shown is shown now (the runtime's own end of a frame, if the title
// never says one finished).
void wl_nv2a_gx_present(WlNv2aGx *gx);
void wl_nv2a_gx_stats(const WlNv2aGx *gx, WlNv2aGxStats *out);
const WlNv2aTexCache *wl_nv2a_gx_textures(const WlNv2aGx *gx);
const WlNv2aVCache *wl_nv2a_gx_vertex_cache(const WlNv2aGx *gx);
// The host memory the backend holds besides its textures' (wl_nv2a_texcache_memory): its record
// (the caches of pixel programs, fits and vertices are in it) and its render targets' images.
void wl_nv2a_gx_host_bytes(const WlNv2aGx *gx, size_t *record, size_t *targets, unsigned *target_count);
// The pixel program the last draw was made with (psh's: its reason when it is beyond GX, what it
// approximated), and whether it was used (false: texture 0 times the colour instead); NULL before
// the first draw. Valid until the next draw. For logs and tools.
const WlXboxPshProgram *wl_nv2a_gx_last_program(const WlNv2aGx *gx, bool *used);
// Whether the last draw went through the perspective projection, its depth a + b / w (0..1 of the
// depth range, before the far plane's scale), and whether that was the fit kept for its transform
// (shared with the draws of the same transform). For logs and tools.
bool wl_nv2a_gx_last_projection(const WlNv2aGx *gx, float *a, float *b, bool *shared);
