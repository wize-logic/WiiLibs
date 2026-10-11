// SPDX-License-Identifier: MIT
// Host stub of the GX layer: records what the renderer asked for so host tests can check passes,
// materials and sampler resolution without a GPU. The Wii backend is wl/platform/wii/gx_wii.c; this stub
// keeps the same contracts (validation counters, shadow-free "what the GPU would have") but draws nothing.
#pragma once

#include "wl/platform/platform.h"

typedef struct {
    bool ready, in_frame;
    uint32_t begin_frames, end_frames;
    uint32_t clear_rgba;
    int viewport[4];
    bool ortho;
    float proj_args[6]; // ortho: left top right bottom near far; perspective: fov aspect near far
    float modelview[12];
    float slots[9][12]; // matrix slots 1..9 (wl_gx_load_matrix_slot)
    uint32_t slot_loads;
    bool texmtx_on;      // wl_gx_set_tex_matrix: a matrix is set
    float texmtx[6];     // ...and this is it
    bool texmtx_valid;
    uint32_t texmtx_loads; // matrices that differed from the one before
    WlGxCull cull;
    WlGxBlend blend;
    struct {
        bool test, write;
        WlGxCompare func;
    } z;
    WlGxCompare alpha_func;
    uint8_t alpha_ref;
    WlGxRecipe recipe;
    uint32_t konst, matcol;
    struct {
        WlGxFog mode;
        float start, end;
        uint32_t rgba;
    } fog;
    uint32_t ambient, light_count;
    WlGxDirLight lights[WL_GX_MAX_LIGHTS];
    WlGxLayout layout;
    bool tex_bound;
    uint8_t tex_min, tex_mag, tex_wrap_s, tex_wrap_t; // what the last bind resolved to
    const WlGxTexObj *tex;
    const WlGxTlut *tlut;
    uint32_t tlut_loads, tex_binds;
    WlGxFilterOverride filter_override;
    uint32_t draw_calls, vertices, dl_calls, validation_errors, sync_calls;
    int scissor[4]; // x y w h of the last wl_gx_set_scissor
    uint32_t scissor_calls;
    uint32_t efb_copies, efb_copy_clears; // wl_gx_copy_efb_texture calls, those that cleared
    int efb_copy_rect[4];                 // the last one's x y w h
} WlHostGxState;

// What one wl_gx_draw call drew (quads and triangles from immediate vertices): the bounding boxes of positions and of
// texture coordinates, the colour of the first vertex and the state it was drawn with. For tests of the UI and text layers.
typedef struct {
    uint8_t prim;
    uint32_t count;
    float x0, y0, x1, y1; // position bounds
    float u0, v0, u1, v1; // texture coordinate bounds
    uint32_t rgba;        // first vertex
    WlGxRecipe recipe;
    const WlGxTexObj *tex;
    uint32_t konst;
    int scissor[4];
} WlHostGxDraw;
// Starts recording into `buf` (capacity `cap` draws; later draws are counted but not stored); NULL stops. Returns nothing;
// wl_host_gx_recorded is the number of draws seen since it started (may exceed `cap`).
void wl_host_gx_record(WlHostGxDraw *buf, uint32_t cap);
uint32_t wl_host_gx_recorded(void);

const WlHostGxState *wl_host_gx_state(void);
// Test control: video info returned by wl_platform_video_info, and the stub's counters.
void wl_host_gx_set_widescreen(bool widescreen);
void wl_host_gx_clear_counters(void);
