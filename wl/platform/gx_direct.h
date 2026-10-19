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
// Indexed attributes. An attribute the layout marks indexed is not sent with each vertex but read by
// GX from an array (wl_gx_direct_array) at a 16-bit index the vertex sends in its place
// (wl_gx_direct_vertex_index): geometry converted once and kept (wl/xbox's vertex cache) costs the
// processor two bytes an attribute a vertex.
//
// The Wii backend is wii/gx_direct_wii.c; the host's (host/gx_direct_host.c) records what was asked
// (host/gx_direct_host.h).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "wl/platform/platform.h"
#include "wl/platform/tev.h"

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
    // a projected coordinate's q (WlGxDirectLayout.texcoords_projected), which GX divides s and t by
    float texcoord_q[8];
    // the normal (WlGxDirectLayout.lit), which the lit channels read
    float normal[3];
} WlGxDirectVertex;

typedef struct WlGxDirectLayout {
    uint8_t colors;     // 0, 1 or 2: COLOR0A0, COLOR1A1 (the vertex's colours, unlit)
    uint8_t texcoords;  // bit n: texture coordinate n is sent (GX_VA_TEXn -> GX_TEXCOORDn, identity)
    uint8_t texcoord_s; // of them, those of one component (s); the others two (s, t)
    // bit n: texture coordinate n is not sent but made by the transform unit from the position
    // (wl_gx_direct_texgen with WL_GX_DIRECT_TG_POSITION) or from another coordinate's input
    // (WL_GX_DIRECT_TG_INPUT0 + k)
    uint8_t texcoords_generated;
    // the attributes read from arrays at the vertex's index (bit WL_GX_DIRECT_ARRAY_*: the position,
    // a colour, a texture coordinate the layout has), the others sent
    uint16_t indexed;
    // bit n: texture coordinate n is projective, (s, t, q) with q divided per pixel (not per vertex):
    // at most WL_GX_DIRECT_PROJECTED of them, not in texcoords. GX takes them as the vertex's normal
    // (one) or its normal, binormal and tangent (two or three: nine numbers, the unused zero), sent
    // after the position, and makes each through a 3x4 generator
    uint8_t texcoords_projected;
    // bit n: channel n lit by the transform unit from the vertex's normal (0 COLOR0, 1 ALPHA0,
    // 2 COLOR1, 3 ALPHA1; wl_gx_direct_lighting), its vertex colour unread. The normal (three
    // floats) is then sent after the position, and a layout's projected coordinates after it, in the
    // binormal and tangent (the normal's and theirs at most WL_GX_DIRECT_PROJECTED in all).
    uint8_t lit;
    // bit c: colour c (one of the layout's colours) is the same for every vertex and not sent: its
    // unlit channels' material is the register wl_gx_direct_material set (GX_SRC_REG, the value GX
    // would interpolate from equal vertex colours). A lit channel's material is the same register:
    // its components there white.
    uint8_t colors_register;
    // of texcoords_projected, bit n: a sphere map's coordinate, its three numbers not (s, t, q) but a
    // direction d of unit length (0 for none), from which the transform unit makes (s, t) =
    // normalize(d + (0, 0, 1)).xy / 2 + 1/2 (its normalize and second matrix), q 1: the processor's
    // wl_nv2a_sphere_coordinates for d's direction, in the GX's arithmetic
    uint8_t texcoords_sphere;
} WlGxDirectLayout;

// projected coordinates a layout may have
enum { WL_GX_DIRECT_PROJECTED = 3 };

// an indexed attribute (WlGxDirectLayout.indexed's bit, wl_gx_direct_array's attribute): the
// position, colour c (WL_GX_DIRECT_ARRAY_COLOR0 + c), texture coordinate n (..._TEXCOORD0 + n), the
// normal of a lit layout (three big-endian floats), or what GX takes as the normal of a layout with
// projected coordinates (the normal, or the normal, binormal and tangent: three or nine floats at one
// index, a lit layout's normal first)
enum {
    WL_GX_DIRECT_ARRAY_POSITION = 0,
    WL_GX_DIRECT_ARRAY_COLOR0 = 1,
    WL_GX_DIRECT_ARRAY_TEXCOORD0 = 3,
    WL_GX_DIRECT_ARRAY_NORMAL = 11
};

// The lit channels' light (WlGxDirectLayout.lit): channel n's value is
// clamp(ambient + dot(normal_matrix * normal, direction[n]) * color[n], 0, 1) for each component it
// has (a colour channel r, g, b, an alpha channel a; ambient that of COLOR0A0 or COLOR1A1), the dot
// product signed (GX_DF_SIGNED) and the direction of unit length (a light at infinity). A channel
// may add more lights to its own, each a term of the same sum (extra[n]'s bit k: light 4 + k,
// extra_direction[k] and extra_color[k]), and take each dot product's maximum with 0 rather than
// the signed one (bit n of clamped: GX_DF_CLAMP), as a model lit by several lights is.
typedef struct WlGxDirectLighting {
    float normal_matrix[3][3];
    float direction[4][3];
    uint8_t color[4][4];
    uint8_t ambient[2][4];
    float extra_direction[4][3];
    uint8_t extra_color[4][4];
    uint8_t extra[4];
    uint8_t clamped;
} WlGxDirectLighting;
// The light of the lit channels, for the draws that follow (set after the layout).
void wl_gx_direct_lighting(const WlGxDirectLighting *lighting);
// The colour (0xRRGGBBAA) of colour c of a layout that takes it from the register
// (WlGxDirectLayout.colors_register), for the draws that follow (set after the layout and the lighting).
void wl_gx_direct_material(unsigned c, uint32_t rgba);

// what a texture coordinate is generated from (wl_gx_direct_texgen)
typedef enum {
    WL_GX_DIRECT_TG_TEXCOORD = 0, // the vertex's own coordinate n, as (s, t, 1, 1)
    WL_GX_DIRECT_TG_POSITION,     // its position as sent (before the position matrix), (x, y, z, 1)
    // the numbers the vertex sends for coordinate k (WL_GX_DIRECT_TG_INPUT0 + k, one the layout sends), as
    // (s, t, 1, 1): a generated coordinate (WlGxDirectLayout.texcoords_generated) made from another one's
    // input, several coordinates from one pair of numbers sent
    WL_GX_DIRECT_TG_INPUT0
} WlGxDirectTexSource;

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
// The same vertex as the FIFO takes it in the layout set: `count` words, the position's three floats,
// a lit layout's normal (three floats), the projected coordinates' (s, t, q) in order (three words for one, nine for two or three, the
// unused zero), each colour as 0xRRGGBBAA, each texture coordinate's one or two floats (a float as its
// bits), in that order. On the Wii the words go into the FIFO as they are, without the layout's tests.
void wl_gx_direct_vertex_words(const uint32_t *words, uint32_t count);
// `n` such vertices, `count` words each, one call (a renderer's batch: no call a vertex)
void wl_gx_direct_vertices_words(const uint32_t *const *vertices, uint32_t n, uint32_t count);
void wl_gx_direct_primitive_end(void);
// An indexed attribute's array (WL_GX_DIRECT_ARRAY_*), for the vertices that follow: element i at
// data + i * stride, in memory the GPU reads (32-byte aligned, written back from the cache). Its
// elements as the FIFO would take the attribute: the position three big-endian floats, a colour
// its bytes r, g, b, a, a texture coordinate its one or two big-endian floats. Set it after the
// layout.
void wl_gx_direct_array(unsigned attribute, const void *data, uint32_t stride);
// The same for an array whose elements are words as wl_gx_direct_vertex_words takes them, each a
// uint32_t in the processor's own order (a renderer's vertices made once and sent again by index:
// the attribute's words at data + i * stride). On the Wii these are the bytes wl_gx_direct_array
// describes; the host reads them as the processor wrote them.
void wl_gx_direct_array_words(unsigned attribute, const void *data, uint32_t stride);
// A vertex of the layout set with indexed attributes: `index` (into every array) for each of them,
// `count` words for the others, as wl_gx_direct_vertex_words takes them (the attributes the layout
// sends, in its order, the indexed ones left out).
void wl_gx_direct_vertex_index(uint16_t index, const uint32_t *words, uint32_t count);
// `n` such vertices, one call: vertex v's index indices[v] - base (indices NULL: v), the same `count`
// words for each
void wl_gx_direct_vertices_index(const uint32_t *indices, uint32_t base, uint32_t n, const uint32_t *words,
                                 uint32_t count);
// `n` vertices of a layout whose position alone is indexed: vertex v's position index
// indices[first + v] - base (indices NULL: first + v), then `count` words of its own from
// vertices[v] + skip, as wl_gx_direct_vertices_words takes them (the position's words left out).
void wl_gx_direct_vertices_position_index(const uint32_t *indices, uint32_t base, uint32_t first, uint32_t n,
                                          const uint32_t *const *vertices, uint32_t skip, uint32_t count);
// Memory an array was read from has changed: GX's vertex cache emptied.
void wl_gx_direct_arrays_changed(void);
// A raster state change (an EFB copy, a token, a draw-done): the next draw is flushed first.
void wl_gx_direct_flush_after(void);
// Whether the flush is sent (on by default).
void wl_gx_direct_set_xf_flush(bool on);

// Work left out of what GX gets, to find on a console what hangs its GPU by halving (each a switch
// of the title's folder; all off by default, and off they cost a branch). The host takes and
// ignores them.
typedef struct {
    bool no_textures;  // every map's texture one fixed 4x4 RGBA8 texture in MEM1 (no TLUT loads)
    bool no_tlut;      // C4 and C8 textures that fixed texture (no TLUT loads); the others as they are
    bool tlut_mem1;    // each TLUT copied into MEM1 before it is loaded (the entries are in MEM2)
    uint32_t max_draws; // only the first max_draws primitives of a frame reach GX (0: all)
    bool no_quads;     // no quad of the targets (the back buffer's restore, the clears, the squeeze)
    bool no_z_restore; // the back buffer's restore puts back the colour only (no Z texture)
    bool no_copies;    // no texture copy out of the EFB (the display's copy stays)
    // the texture coordinates in the vertex descriptor's slots of their own names, gaps and all
    // (TEX0, TEX2, TEX3), as before they were sent in its first slots: the hang of such draws back
    bool texcoord_gaps;
    // the depth on a console (a10's light beams and characters through walls, the Wii only):
    bool late_z;    // the depth test always after texturing (GX_SetZCompLoc(GX_FALSE): no early Z)
    bool vtx_inval; // GX's vertex cache invalidated before every primitive (stale indexed vertices)
    // the old ways, for an A/B on the console (the same bytes reach GX either way):
    bool pipe32;      // a vertex's words put in the FIFO a word a store (not two a move: lfd, stfd)
    bool copy_filter; // the copy filter written around each texture copy out of the EFB (not only when it changes)
    bool restore_whole; // the back buffer's restore puts back all it parked (not only what targets drew over)
    bool tlut_always;   // a colour-indexed texture's TLUT loaded at each bind (not only when its slot holds other bytes)
    bool regions_libogc; // libogc's texture cache regions (32K + 32K a map; not 128K + 128K for maps 0-2)
    bool tex_whole;      // a bind that changes anything is GX_LoadTexObj's whole load (not only its registers that differ)
    bool arrays_always;  // GX_SetArray at each call (not only when its base or stride differs)
    bool pillarbox;      // a 16:9 pillarbox narrowed by a copy of the finished picture (wl_gx_draw_narrow refused)
    // a picture drawn narrow in the framebuffer's middle columns, as shown, and copied to the screen whole (not in
    // its last columns, the display copy taking those alone into the screen's middle: wl_gx_draw_narrow)
    bool narrow_mid;
} WlGxBisect;
void wl_gx_bisect_set(const WlGxBisect *bisect);
// The switches set (all off before wl_gx_bisect_set).
const WlGxBisect *wl_gx_bisect(void);

// The transform unit's position matrix for the draws that follow (row-major 3x4: (x', y', z') =
// m (x, y, z, 1), before the projection), NULL the identity (as wl_gx_direct_begin leaves it).
void wl_gx_direct_position_matrix(const float m[3][4]);
// Texture coordinate n (0..7) made by the transform unit: (s, t) = m (the source's four numbers), m
// row-major 2x4; NULL: the identity from the vertex's coordinate n (as the layout makes it). Set it
// after the layout, which puts every coordinate back to the identity when it changes.
void wl_gx_direct_texgen(unsigned n, WlGxDirectTexSource source, const float m[2][4]);
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
// object's) and, for C4/C8, its TLUT (loaded into the object's slot). An object the map already
// holds as it would be loaded is not loaded again (the TLUT is: its entries may have changed);
// wl/platform's own loads (its quads, wl_gx_tex_bind, the ramp) and wl_gx_invalidate_shadow make
// the maps they touch unknown.
void wl_gx_direct_texture(uint32_t map, const WlGxTexObj *tex, const WlGxSampler *sampler, const WlGxTlut *tlut);
// GX's texture cache emptied: memory a texture was read from has changed.
void wl_gx_direct_texture_invalidate(void);

typedef struct WlGxDirectStats {
    uint32_t draws, vertices, flushes, layouts, textures;
    uint32_t textures_kept; // of the textures, the ones their map held already (the Wii's; not loaded)
    // GX calls the state setters and wl_tev_apply asked for, and of those the ones sent (the rest
    // GX held already: gx_shadow.h; counted alike on the host)
    uint32_t state_calls, state_sent;
    // TLUT (palette) loads the colour-indexed textures asked for, and of those the ones sent (the rest
    // found their slot holding the same bytes: gx_shadow.h; counted alike on the host)
    uint32_t tlut_calls, tlut_sent;
    // texture binds asked for, the ones that wrote something, of those the whole loads (GX_LoadTexObj),
    // and the BP writes they made (gx_shadow.h; counted alike on the host)
    uint32_t tex_binds, tex_changed, tex_loads, tex_writes;
    // GX_SetArray calls asked for and sent
    uint32_t array_calls, array_sent;
    // the FIFO's bytes by kind (WlGxFifoKind: a model, gx_shadow.h)
    uint64_t fifo[8];
} WlGxDirectStats;
void wl_gx_direct_stats(WlGxDirectStats *out);

// ---- the flush for a renderer's own vertex formats ---------------------------------------------
// A renderer that sets GX's vertex descriptors and formats itself (indexed arrays, matrix indices,
// normals, any vertex format: a Direct3D device's, OpenCE-Wii's d3d8_gx.c) keeps the transform
// unit's flush through these. Each attribute format it sends directly is set through
// wl_gx_direct_vertex_format, which remembers its size; before a primitive,
// wl_gx_direct_flush_pending sends the flush if one is due (wl_gx_direct_flush_after, on unless
// wl_gx_direct_set_xf_flush turned it off): a strip of eight vertices of zeros in the primitive's
// own format, whose size is the vertex descriptor as GX_Begin will send it (GX_GetVtxDescv) and
// the sizes remembered. Where an attribute's size is not known, no flush is sent. The flushes count
// in wl_gx_direct_stats' and wl_gx_direct_flushes.

// GX's numbering of the vertex attributes, their inputs and formats (GX_VA_*, GX_NONE ..
// GX_INDEX16, GX_U8 .. GX_F32, GX_RGB565 .. GX_RGBA8, GX_POS_XY, GX_NRM_XYZ, GX_TEX_S)
enum {
    WL_GX_VA_PNMTXIDX = 0,
    WL_GX_VA_TEX7MTXIDX = 8,
    WL_GX_VA_POS = 9,
    WL_GX_VA_NRM = 10,
    WL_GX_VA_CLR0 = 11,
    WL_GX_VA_CLR1 = 12,
    WL_GX_VA_TEX0 = 13,
    WL_GX_VA_TEX7 = 20,
    WL_GX_VA_ATTRIBUTES = 26, // GX_VA_MAXATTR
    WL_GX_VA_END = 0xFF       // GX_VA_NULL: a descriptor list's end
};
enum { WL_GX_IN_NONE = 0, WL_GX_IN_DIRECT, WL_GX_IN_INDEX8, WL_GX_IN_INDEX16 };
enum { WL_GX_TYPE_U8 = 0, WL_GX_TYPE_S8, WL_GX_TYPE_U16, WL_GX_TYPE_S16, WL_GX_TYPE_F32 };
enum { WL_GX_COUNT_POS_XY = 0, WL_GX_COUNT_POS_XYZ, WL_GX_COUNT_NRM_XYZ = 0, WL_GX_COUNT_TEX_S = 0 };

// one entry of a vertex descriptor list (libogc's GXVtxDesc)
typedef struct WlGxVtxDesc {
    uint8_t attr, type;
} WlGxVtxDesc;

// The bytes an attribute sent directly takes in a vertex, by its format (GX_SetVtxAttrFmt's count
// and type); 0 for one not sized here (a normal other than XYZ, a type beyond the table).
uint8_t wl_gx_attribute_bytes(uint32_t attribute, uint32_t count, uint32_t type);
// The bytes of a vertex as GX_Begin sends it: the descriptor list (up to WL_GX_VA_END or `count`
// entries) and each directly sent attribute's bytes (`bytes`, by attribute); a matrix index is a
// byte, an index one or two. 0 when a direct attribute's size is not known.
uint32_t wl_gx_vertex_bytes(const WlGxVtxDesc *desc, unsigned count, const uint8_t bytes[WL_GX_VA_ATTRIBUTES]);
// Whether GX reads a vertex of this descriptor right: it misreads one whose attributes are its
// position and matrix indices alone (a vertex carrying a matrix index must send another attribute
// with it, a colour no channel reads where it needs none).
bool wl_gx_vertex_shape_ok(const WlGxVtxDesc *desc, unsigned count);

// GX_SetVtxAttrFmt, its size remembered for the flush (format 0..7)
void wl_gx_direct_vertex_format(uint8_t format, uint32_t attribute, uint32_t count, uint32_t type, uint32_t frac);
// The bytes of a vertex of `format` as the current descriptor sends it (0: a size not known).
uint32_t wl_gx_direct_vertex_bytes(uint8_t format);
// Whether GX reads the current descriptor's vertices right (wl_gx_vertex_shape_ok).
bool wl_gx_direct_vertex_shape_ok(void);
typedef enum {
    WL_GX_FLUSH_NOT_DUE = 0, // none due, or the flush turned off
    WL_GX_FLUSH_SENT,
    WL_GX_FLUSH_SIZE_UNKNOWN // due, but a direct attribute's size is not known: none sent
} WlGxFlushResult;
// The flush before a primitive of `format`, if one is due; none is due after it.
WlGxFlushResult wl_gx_direct_flush_pending(uint8_t format);
// GX_Begin (GX's primitive, GX_TRIANGLES ..), after the flush if one is due.
void wl_gx_direct_begin_format(uint8_t gx_primitive, uint8_t format, uint16_t count);
// the flushes sent, by wl_gx_direct_draw and wl_gx_direct_flush_pending
uint32_t wl_gx_direct_flushes(void);

// ---- the draw's state checked ------------------------------------------------------------------
// What the console's GPU cannot draw where Dolphin draws it, found in a draw's state: the colour
// channels and texture coordinates the transform unit makes for it (GX_SetNumChans,
// GX_SetNumTexGens), the texture maps loaded and the TEV program. A hung GPU on the Wii (its
// command processor stopped mid-primitive, Dolphin drawing the same frames) is looked for with it:
// the host's direct layer counts the draws with each finding (host/gx_direct_host.h), and the Wii's
// logs the first ones under [GXCHECK] when WiiLibs is built with WL_GX_CHECK (off: no cost).
enum {
    WL_GX_CHECK_NO_OUTPUT = 1u << 0, // neither a colour channel nor a texture coordinate
    WL_GX_CHECK_CHANNEL = 1u << 1,   // a stage's colour channel beyond the channels made
    WL_GX_CHECK_RAS_NULL = 1u << 2,  // a stage reads the rasterised colour with no channel ordered
    WL_GX_CHECK_TEXCOORD = 1u << 3,  // a stage's texture coordinate beyond the coordinates made
    WL_GX_CHECK_TEX_NULL = 1u << 4,  // a stage reads the texel with no texture map ordered
    WL_GX_CHECK_TEXMAP = 1u << 5,    // a stage's texture map holds no texture (maps_loaded)
    WL_GX_CHECK_STAGES = 1u << 6,    // no TEV stage, or more than GX has
    WL_GX_CHECKS = 7
};
// The findings (WL_GX_CHECK_*) for a draw with `chans` colour channels and `texgens` texture
// coordinates made, the texture maps holding a texture (bit n: map n) and the TEV program.
uint32_t wl_gx_check_draw(uint32_t chans, uint32_t texgens, uint32_t maps_loaded, const WlTevProgram *tev);
// A finding's short name (its bit's number), for logs.
const char *wl_gx_check_name(unsigned bit);
// The colour channels and texture coordinates the Wii's direct layer has GX make for a layout
// (wii/gx_direct_wii.c): its colours, one when it has neither colours nor coordinates (GX never
// makes neither), and coordinates up to the highest one it sends, makes or projects.
void wl_gx_direct_layout_outputs(const WlGxDirectLayout *layout, uint32_t *chans, uint32_t *texgens);
