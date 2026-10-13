// SPDX-License-Identifier: MIT
// A software GX for the host (gxsoft.c): what a program asks of GX through wl/platform on the host
// (the direct layer, gx_direct.h; TEV programs, tev.h; render targets, gx_target.h; frames) drawn
// into an EFB on the processor, the way the console's GX draws it, so that a host build or a
// replayed capture shows the frames a Wii would show without a Wii or Dolphin. It attaches to the
// host backend of wl/platform as its renderer (host/gx_renderer_host.h) and reads the state each
// draw is made with from the host's recorders.
//
// What it does, as GX (and Dolphin, against which it is checked) does it:
//  - vertices: the position through the identity position matrix and the projection as GX keeps
//    it (its six numbers: an orthographic one x' = p0 x + p1, y' = p2 y + p3, z' = p4 z + p5, w 1;
//    a perspective one x' = p0 x + p1 z, y' = p2 y + p3 z, z' = p4 z + p5, w -z; the rest of the
//    matrix is not looked at), clipped where z' leaves [-w, 0] (the near and far planes) or w
//    nears 0, then the viewport (depth 0..1 as 1 + z' / w); the texture coordinates through
//    GX_TG_MTX2x4 identity generators (a coordinate the layout does not send is generated from the
//    position, as the Wii backend does), the colours unlit from the vertex;
//  - primitives: triangle lists, strips (every other one reversed), fans, quads (two triangles),
//    lines and points one pixel wide; culling by the winding on the screen (y down: a triangle
//    whose corners run clockwise is GX's front face, GX_CULL_FRONT discards it, as Dolphin
//    measures it); pixels sampled at 7/12 of a pixel, the top-left rule on edges; attributes
//    interpolated for the perspective, the depth across the screen;
//  - textures: each stage's own lookup (its texture map at its texture coordinate), decoded from
//    GX's formats with wl/codec (CMPR's 5/8 : 3/8 blend, colour-indexed through their TLUT), the
//    ramp (wl_gx_ramp_bind), clamp, repeat and mirror (clamp for a size that is not a power of
//    two, as the Wii backend forces), nearest and bilinear filtering with GX's 7-bit fractions,
//    mipmaps by a level of detail from the coordinates' derivatives;
//  - the TEV program stage by stage (wl_tev_eval_stages: konst, swap tables, colour channels),
//    its output as GX hands it on (8 bits), the alpha compare, the depth test before or after
//    texturing, blending (GX's factors, subtract), the colour and alpha updates;
//  - the EFB: 640 by 528 pixels, RGBA6_Z24 (six bits a channel) or RGB8_Z24 (alpha reads 255) as
//    wl_gx_set_efb_alpha chose, a 24-bit depth; copies out of it into textures (render targets,
//    wl_gx_copy_efb_texture) in GX's tile order, the parked back buffer put back, clear quads, and
//    the display copy of each frame.
// Not modelled: indirect texturing, fog, logic ops, dithering, anti-aliasing, the display copy's
// vertical filter and gamma, GX's texture cache (memory is read as it is at the draw; a texture
// whose memory changed is noticed by its bytes).
//
// Host and ppc-be only (it needs wl/platform's host backend). One at a time, as there is one GX.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "wl/base/base.h"

#define WL_GXSOFT_EFB_WIDTH 640u
#define WL_GXSOFT_EFB_HEIGHT 528u

// a draw's state that GX takes as it is, though it is probably not what was meant
enum {
    WL_GXSOFT_ISSUE_TEV_INVALID = 1u << 0, // the TEV program has errors (wl_tev_validate): computed anyway
    WL_GXSOFT_ISSUE_NO_CHANNEL = 1u << 1,  // a stage reads a colour channel the layout does not send (0)
    WL_GXSOFT_ISSUE_NO_TEXCOORD = 1u << 2, // a stage reads a texture coordinate no generator makes (0)
    WL_GXSOFT_ISSUE_NO_TEXTURE = 1u << 3,  // a stage reads a texture map nothing was loaded into (0)
    WL_GXSOFT_ISSUE_NO_TLUT = 1u << 4,     // a colour-indexed texture without its TLUT (0)
    WL_GXSOFT_ISSUES = 5
};

// What one primitive did.
typedef struct WlGxSoftDraw {
    uint32_t index;        // primitives since the attach
    uint8_t prim;          // WlGxDirectPrim
    uint32_t vertices;
    uint32_t triangles;    // triangles (lines, points) assembled
    uint32_t culled;       // ...culled by the winding
    uint32_t clipped;      // ...left with nothing by the near and far planes
    uint32_t pixels;       // pixels covered within the scissor
    uint32_t z_failed;     // ...that failed the depth test
    uint32_t alpha_failed; // ...that failed the alpha compare
    uint32_t written;      // ...that reached the EFB
    uint32_t issues;       // WL_GXSOFT_ISSUE_* bits
    float x0, y0, x1, y1;  // the bounds of what was covered (pixels), when anything was
} WlGxSoftDraw;

typedef struct WlGxSoftConfig {
    // each display copy (wl_gx_end_frame): the picture, `width` by `height` RGBA8 rows (NULL: none)
    void (*frame)(void *user, const uint8_t *rgba, uint32_t width, uint32_t height);
    // each primitive, once drawn (NULL: none)
    void (*draw)(void *user, const WlGxSoftDraw *draw);
    void *user;
} WlGxSoftConfig;

typedef struct WlGxSoftStats {
    uint32_t frames, draws, triangles, culled, clipped, pixels, written;
    uint32_t copies, restores, clears;
    uint32_t issue_draws[WL_GXSOFT_ISSUES]; // draws with each issue, by bit number
    uint32_t textures_decoded;              // texture images decoded (again when their bytes changed)
} WlGxSoftStats;

// Attaches the software GX as the host GX's renderer (cfg may be NULL), its EFB black with the far
// depth. WL_ERR_NOMEM without memory for the EFB.
WlResult wl_gxsoft_attach(const WlGxSoftConfig *cfg);
// Detaches it and gives its memory back.
void wl_gxsoft_detach(void);
bool wl_gxsoft_attached(void);
void wl_gxsoft_stats(WlGxSoftStats *out);
// The EFB's colour as GX holds it (six-bit channels widened as a copy widens them), RGBA8 rows of
// WL_GXSOFT_EFB_WIDTH pixels, WL_GXSOFT_EFB_HEIGHT of them; NULL when not attached.
const uint8_t *wl_gxsoft_efb(void);
// A pixel's depth (24 bits), 0xFFFFFFFF outside the EFB or when not attached.
uint32_t wl_gxsoft_depth(uint32_t x, uint32_t y);
// A short name of an issue bit's number, for logs.
const char *wl_gxsoft_issue_name(unsigned bit);
