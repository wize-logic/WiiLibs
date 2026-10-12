// SPDX-License-Identifier: MIT
// GX driven directly, for a renderer whose state is beyond wl_gx's recipes and its one vertex layout:
// an emulated GPU's (wl/xbox's NV2A backend), which brings its own TEV programs (wl_tev_apply), up to
// two colours and eight texture coordinates a vertex, any blend factors, its own projection matrix
// and texture objects on any texture map. These calls bypass wl_gx's state shadow:
// wl_gx_direct_begin marks it unknown, and the caller then sets everything it relies on.
//
// The transform unit's flush: GX's transform unit can hang (GX_DrawDone, or a frame's token, never
// arrives) when a primitive it clips follows a raster state change such as an EFB copy, a draw-sync
// token or a draw-done. libogc names the remedy, GX_SetMisc(GX_MT_XF_FLUSH, GX_XF_FLUSH_SAFE), but its
// GX_Begin sends only the primitive (libogc.a's gx.o, disassembled: OpenCE-Wii's finding). So the
// first wl_gx_direct_draw after wl_gx_direct_begin or wl_gx_direct_flush_after is preceded by a strip
// of eight vertices of zeros in its own format, which are one point and draw nothing.
//
// The Wii backend is wii/gx_direct_wii.c; the host's (host/gx_direct_host.c) records what was asked
// (host/gx_direct_host.h).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "wl/platform/platform.h"

// blend modes and factors, in GX's numbering (GX_BM_*, GX_BL_*: the colour factors are the other
// side's, a source's factor of the destination colour and a destination's of the source colour)
typedef enum {
    WL_GX_BM_NONE = 0,
    WL_GX_BM_BLEND,
    WL_GX_BM_LOGIC,
    WL_GX_BM_SUBTRACT // destination minus source
} WlGxBlendMode;
typedef enum {
    WL_GX_BL_ZERO = 0,
    WL_GX_BL_ONE,
    WL_GX_BL_OTHERCLR,    // GX_BL_SRCCLR / GX_BL_DSTCLR
    WL_GX_BL_INVOTHERCLR, // GX_BL_INVSRCCLR / GX_BL_INVDSTCLR
    WL_GX_BL_SRCALPHA,
    WL_GX_BL_INVSRCALPHA,
    WL_GX_BL_DSTALPHA,
    WL_GX_BL_INVDSTALPHA
} WlGxBlendFactor;

typedef enum {
    WL_GX_DIRECT_POINTS = 0,
    WL_GX_DIRECT_LINES,
    WL_GX_DIRECT_LINE_STRIP,
    WL_GX_DIRECT_TRIANGLES,
    WL_GX_DIRECT_TRIANGLE_STRIP,
    WL_GX_DIRECT_TRIANGLE_FAN,
    WL_GX_DIRECT_QUADS
} WlGxDirectPrim;

// A vertex: its position (three floats, through the identity position matrix), its colours (bytes
// r, g, b, a) and texture coordinates; the layout says which of them are sent.
typedef struct WlGxDirectVertex {
    float position[3];
    uint8_t color[2][4];
    float texcoord[8][2];
} WlGxDirectVertex;

typedef struct WlGxDirectLayout {
    uint8_t colors;     // 0, 1 or 2: COLOR0A0, COLOR1A1 (the vertex's colours, unlit)
    uint8_t texcoords;  // bit n: texture coordinate n is sent (GX_VA_TEXn -> GX_TEXCOORDn, identity)
    uint8_t texcoord_s; // of them, those of one component (s); the others two (s, t)
} WlGxDirectLayout;

// Starts direct drawing: wl_gx's shadow and this layer's are marked unknown, the position matrix 0
// is the identity, no indirect stages, and the next draw is flushed first.
void wl_gx_direct_begin(void);
// The vertices' layout (vertex format 7), the texture coordinate generators and colour channels.
void wl_gx_direct_layout(const WlGxDirectLayout *layout);
// Primitives of `count` vertices (at most 65535) in the layout set: all at once, or each vertex
// written into the FIFO as it is made (begin, `count` vertices, end).
void wl_gx_direct_draw(WlGxDirectPrim prim, const WlGxDirectVertex *vertices, uint32_t count);
void wl_gx_direct_primitive_begin(WlGxDirectPrim prim, uint32_t count);
void wl_gx_direct_vertex(const WlGxDirectVertex *vertex);
void wl_gx_direct_primitive_end(void);
// A raster state change (an EFB copy, a token, a draw-done): the next draw is flushed first.
void wl_gx_direct_flush_after(void);
// Whether the flush is sent (on by default).
void wl_gx_direct_set_xf_flush(bool on);

// The projection, a 4x4 row-major matrix (libogc's Mtx44), orthographic or perspective.
void wl_gx_direct_projection(const float m[4][4], bool orthographic);
// The viewport (depth 0..1) and scissor, in EFB pixels.
void wl_gx_direct_viewport(float x, float y, float w, float h);
void wl_gx_direct_scissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void wl_gx_direct_blend(WlGxBlendMode mode, WlGxBlendFactor source, WlGxBlendFactor destination);
void wl_gx_direct_zmode(bool test, WlGxCompare func, bool write);
// the depth test before texturing (true) or after it, so that an alpha test's discarded pixels
// write no depth (false)
void wl_gx_direct_zcompare_early(bool early);
// one compare of the TEV's alpha with a reference (always: no test)
void wl_gx_direct_alpha_compare(WlGxCompare func, uint8_t ref);
void wl_gx_direct_color_update(bool color, bool alpha);
void wl_gx_direct_cull(WlGxCull cull);
// A texture object (wl_gx_tex_init_ex) on texture map `map` (0..7), with a sampler (NULL: the
// object's) and, for C4/C8, its TLUT (loaded into the object's slot).
void wl_gx_direct_texture(uint32_t map, const WlGxTexObj *tex, const WlGxSampler *sampler, const WlGxTlut *tlut);
// GX's texture cache emptied: memory a texture was read from has changed.
void wl_gx_direct_texture_invalidate(void);

typedef struct WlGxDirectStats {
    uint32_t draws, vertices, flushes, layouts, textures;
} WlGxDirectStats;
void wl_gx_direct_stats(WlGxDirectStats *out);
