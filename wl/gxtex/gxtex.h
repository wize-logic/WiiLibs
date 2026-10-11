// SPDX-License-Identifier: MIT
// GX texture encoding and Nintendo DS graphics conversion (offline tooling; portable C).
//
// - GX textures: RGBA / indexed / intensity images to the block layouts the Wii's GX texture unit
//   reads (RGBA8, RGB565, RGB5A3, I4, C4, C8 with an RGB5A3 TLUT, CMPR), and an exact decoder.
// - DS 2D resources: NCLR palettes, NCGR character data, NCER cells and NANR cell animations, a
//   cell renderer, and the conversion of a cell set to a palettised sprite sheet.
// - DS textures: the TEX0 block of NSBTX (BTX0) / NSBMD (BMD0) files, exact RGBA decoding of the
//   seven DS texture formats and their conversion to GX formats.
// - A minimal PNG writer (stored deflate) for previews.
//
// Layouts are summarised in README.md. Inputs are untrusted: every offset and count is checked.
// Byte buffers are always read with the endian helpers, never cast to structs. Outputs are
// deterministic.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "wl/base/base.h"

/* GX hardware texture format numbers (GX_TF_*) and TLUT formats (GX_TL_*). */
#define WL_GX_TF_I4 0x0
#define WL_GX_TF_I8 0x1
#define WL_GX_TF_IA4 0x2
#define WL_GX_TF_IA8 0x3
#define WL_GX_TF_RGB565 0x4
#define WL_GX_TF_RGB5A3 0x5
#define WL_GX_TF_RGBA8 0x6
#define WL_GX_TF_C4 0x8
#define WL_GX_TF_C8 0x9
#define WL_GX_TF_CMPR 0xE
#define WL_GX_TL_IA8 0x0
#define WL_GX_TL_RGB565 0x1
#define WL_GX_TL_RGB5A3 0x2

/* Owned RGBA8 image (w * h * 4 bytes, row-major). */
typedef struct {
    uint32_t w, h;
    uint8_t *rgba;
} WlImage;
void wl_image_free(WlImage *im);

/* ===================================================================================== */
/* GX textures                                                                           */
/* ===================================================================================== */

/* An encoded GX texture (one level). `data` is malloc'd and holds the texel blocks exactly as GX
 * reads them (tile order, big-endian), `size` bytes, zero-padded to whole tiles and to a multiple
 * of 32 bytes. Colour-indexed textures carry their TLUT as `ntlut` big-endian 16-bit entries. */
typedef struct {
    uint32_t fmt;      /* WL_GX_TF_* */
    uint32_t w, h;
    uint8_t *data;
    size_t size;
    uint32_t tlut_fmt; /* WL_GX_TL_RGB5A3 when ntlut > 0 */
    uint32_t ntlut;
    uint8_t tlut[512];
} WlGxTex;
void wl_gx_tex_free(WlGxTex *t);

/* Bytes of the texel blocks of a w x h texture in fmt (I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8,
 * C4, C8, CMPR), rounded up to whole tiles; 0 for another format. */
size_t wl_gx_tex_size(uint32_t fmt, uint32_t w, uint32_t h);

/* RGB5A3 word of an RGBA pixel (wl_gxc_rgb5a3_encode): pixels whose alpha rounds to opaque
 * (>= 237) keep 5-5-5 bits (exact for DS colours), the others use 3-bit alpha and 4-4-4 colour;
 * every channel rounds to nearest. */
uint16_t wl_gx_rgb5a3(const uint8_t rgba[4]);
void wl_gx_rgb5a3_to_rgba(uint16_t v, uint8_t out[4]);

/* Indexed image (one index a pixel, row-major) to C4 (bpp 4) or C8 (bpp 8) with an RGB5A3 TLUT
 * built from pal (ncol <= 16 or 256 RGBA entries). WL_ERR_RANGE for an index >= ncol. */
WlResult wl_gx_tex_from_indexed(const uint8_t *idx, uint32_t w, uint32_t h, uint32_t bpp,
                                const uint8_t (*pal)[4], uint32_t ncol, WlGxTex *out);
/* 4-bit intensity image (one value 0..15 a pixel, row-major) to I4. */
WlResult wl_gx_tex_from_i4(const uint8_t *lum, uint32_t w, uint32_t h, WlGxTex *out);
/* RGBA image to fmt (WL_GX_TF_RGBA8, RGB5A3, RGB565 or CMPR) through wl/codec; the tile padding
 * repeats the last row and column. CMPR fits each 4x4 block with the two most distant colours
 * (exact for blocks of at most two colours); alpha < 128 is transparent. */
WlResult wl_gx_tex_from_rgba(const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t fmt,
                             WlGxTex *out);
/* Decodes untrusted texel data to RGBA (RGBA8, RGB565, RGB5A3, I4, C4, C8, CMPR). C4 / C8 need
 * an RGB5A3 TLUT (`tlut` holds ntlut big-endian entries); an index past it is WL_ERR_FORMAT. */
WlResult wl_gx_decode(uint32_t fmt, uint32_t w, uint32_t h, const uint8_t *data, size_t size,
                      uint32_t tlut_fmt, const uint8_t *tlut, uint32_t ntlut, WlImage *im);
/* wl_gx_decode of an encoded texture. */
WlResult wl_gx_tex_decode(const WlGxTex *t, WlImage *im);

/* ===================================================================================== */
/* DS 2D resources (NCLR, NCGR, NCER, NANR)                                              */
/* ===================================================================================== */

/* Palette: BGR555 little-endian entries (the view does not own the bytes). */
typedef struct {
    const uint8_t *col;
    uint32_t count;
    uint32_t bpp; /* 4 or 8 (the file's colour depth field) */
} WlNclr;
WlResult wl_nclr_open(WlNclr *p, const uint8_t *d, size_t n);
/* RGBA of entry i (alpha 255); entries past the table read as black. Index 0 is NOT made
 * transparent here. */
void wl_nclr_rgba(const WlNclr *p, uint32_t i, uint8_t out[4]);

/* Character (tile) data. */
typedef struct {
    const uint8_t *pix;
    uint32_t size;
    uint32_t bpp;                /* 4 or 8 */
    uint32_t tiles_w, tiles_h;   /* 0 when the file does not say (0xFFFF) */
    int scan;                    /* 1 = linear (scanline) pixels, 0 = 8x8 tiles */
} WlNcgr;
WlResult wl_ncgr_open(WlNcgr *g, const uint8_t *d, size_t n);

/* One object attribute (a hardware sprite) of a cell. */
typedef struct {
    int16_t x, y;       /* top-left relative to the cell origin */
    uint16_t w, h;      /* pixels */
    uint16_t tile;      /* tile index in mapping units */
    uint8_t pal;        /* palette bank (4-bit objects) */
    uint8_t flip_h, flip_v;
    uint8_t color256;   /* 8-bit objects */
    uint8_t hidden;     /* disabled by the "double size" bit without rotation */
    uint8_t xform;      /* rotation/scaling objects (not supported by the renderer) */
} WlOam;

typedef struct {
    uint32_t first, count; /* range in WlNcer.oams */
    /* VRAM transfer entry: the cell's tiles are the NCGR bytes [vt_off, vt_off + vt_size) loaded at
     * tile 0 (animated sprites keep every frame's tiles in one NCGR). */
    uint32_t vt_off, vt_size;
    uint8_t has_vt;
    uint8_t flip_h, flip_v; /* cell attribute: mirror the whole cell (positions and objects) */
} WlCell;

typedef struct {
    WlCell *cells;
    WlOam *oams;
    uint32_t ncells, noams;
    uint32_t mapping; /* 0..3: 1D with 32, 64, 128, 256 byte tile units; 4: 2D */
} WlNcer;
WlResult wl_ncer_open(WlNcer *c, const uint8_t *d, size_t n);
void wl_ncer_free(WlNcer *c);

typedef struct {
    uint16_t cell;
    uint16_t duration; /* source ticks (60 Hz frames) */
} WlNanrFrame;
typedef struct {
    uint32_t first, count; /* range in WlNanr.frames */
    uint16_t loop_start;
    uint16_t play_mode;    /* 1 forward, 2 forward loop, 3 reverse, 4 reverse loop */
    uint16_t element;      /* 0 index only; others carry a transform that is ignored */
} WlNanrSeq;
typedef struct {
    WlNanrSeq *seqs;
    WlNanrFrame *frames;
    uint32_t nseqs, nframes;
} WlNanr;
WlResult wl_nanr_open(WlNanr *a, const uint8_t *d, size_t n);
void wl_nanr_free(WlNanr *a);

/* Bounding box (x0,y0 inclusive; x1,y1 exclusive) of the objects of a cell; WL_ERR_RANGE for an
 * empty cell. */
WlResult wl_ds_cell_bounds(const WlNcer *c, uint32_t cell, int *x0, int *y0, int *x1, int *y1);
/* Draws a cell into an RGBA canvas of w x h pixels whose top-left is the cell coordinate
 * (ox, oy). Objects are drawn back to front (object 0 on top); index 0 is transparent. pal < 0
 * uses each object's own bank, otherwise every 4-bit object uses bank pal. Rotation/scaling
 * objects give WL_ERR_UNSUPPORTED. */
WlResult wl_ds_render_cell(const WlNcgr *g, const WlNclr *p, const WlNcer *c, uint32_t cell,
                           int pal, int ox, int oy, uint8_t *rgba, uint32_t w, uint32_t h);

/* ===================================================================================== */
/* Sprite sheets from the DS resources                                                   */
/* ===================================================================================== */

/* One animation of a sprite sheet: `count` stored frames starting at `first`. */
typedef struct {
    uint8_t first, count;
    uint8_t ticks;  /* hold of each frame (output ticks) unless the sheet has a hold table */
    uint8_t loop;   /* 1 loops, 0 stops on the last frame */
} WlSpriteAnim;

/* A palettised sprite sheet: nframes frames of fw x fh 4-bit palette indices (row-major, one byte
 * an index; fw and fh multiples of 8), up to 16 colours with entry 0 transparent. Every array is
 * malloc'd; wl_ds_sprite_free releases them. */
typedef struct {
    uint32_t fw, fh, nframes;
    uint8_t *frames;           /* nframes * fw * fh */
    uint8_t pal[16][4];        /* RGBA, entry 0 = (0, 0, 0, 0) */
    uint32_t ncol;             /* entries used, including the transparent one */
    WlSpriteAnim *anims;
    uint32_t nanims;
    uint8_t *hold;             /* nframes per-frame holds (output ticks, 0 = the animation's
                                * ticks), or NULL when no animation has more than one frame */
    uint32_t ncells;           /* cells rendered (the first ncells frames are the cells in order) */
    int lossy;                 /* 1 when more than 15 colours were merged to the nearest */
} WlDsSprite;
void wl_ds_sprite_free(WlDsSprite *s);

/* NCGR + NCLR + NCER (+ NANR, may be NULL) to a sprite sheet: each cell becomes a frame of the
 * union bounding box (rounded up to 8), palettised exactly to 15 colours plus transparent; pal < 0
 * keeps the objects' banks. A sprite with more colours (two palette banks) is reduced to its 15
 * most frequent RGB555 colours, the others mapped to the nearest (lossy = 1). Each NANR sequence
 * becomes an animation; a sequence that cannot play the cells in place (out of order, or with
 * holds that conflict with another sequence) gets its own copies of the frames after the cells.
 * Durations are divided by tick_div (1 keeps 60 Hz ticks, 2 gives 30 Hz), rounded half up and
 * clamped to 1..255. At most 200 cells, 255 stored frames, 512 x 512 pixels a frame (WL_ERR_LIMIT).
 * WL_ERR_UNSUPPORTED for rotation/scaling objects. */
WlResult wl_ds_sprite_build(const WlNcgr *g, const WlNclr *p, const WlNcer *c, const WlNanr *a,
                            int pal, uint32_t tick_div, WlDsSprite *out);
/* The cells stacked top to bottom into one C4 texture (e.g. two-frame icons); exact palettisation
 * only (WL_ERR_LIMIT beyond 15 colours). */
WlResult wl_ds_cells_tex(const WlNcgr *g, const WlNclr *p, const WlNcer *c, int pal, WlGxTex *out);

/* ===================================================================================== */
/* TEX0 (DS 3D textures)                                                                 */
/* ===================================================================================== */

typedef struct {
    uint32_t w, h;
    uint32_t fmt;    /* DS format 1..7 */
    int transp;      /* colour 0 transparent (formats 2, 3, 4) */
    uint32_t off;    /* byte offset in the texture (or compressed texture) data */
} WlTexInfo;

typedef struct {
    const uint8_t *d; /* the whole file */
    size_t n;
    size_t base;      /* TEX0 block offset */
    uint32_t ntex, npal;
    size_t tex_dict, pal_dict;
    size_t tex_data, tex_size, comp_data, comp_size, comp_idx, pal_data, pal_size;
} WlTex0;

/* Conversion policy for DS texture formats. */
typedef struct {
    int cmpr; /* 1: 4x4-compressed textures become CMPR (lossy); 0: exact RGB5A3 */
} WlTex0Policy;

/* Finds the TEX0 block of a BTX0 or BMD0 file (or a raw TEX0 block when the data starts with
 * "TEX0"); WL_ERR_FORMAT for anything else, WL_ERR_NOT_FOUND when there is no TEX0. */
WlResult wl_tex0_open(WlTex0 *t, const uint8_t *d, size_t n);
WlResult wl_tex0_info(const WlTex0 *t, uint32_t i, WlTexInfo *info);
/* Palette i: its bytes (BGR555) and the entry count it spans. */
WlResult wl_tex0_palette(const WlTex0 *t, uint32_t i, const uint8_t **col, uint32_t *count);
/* Palette that goes with texture i: the one named like the texture plus "_pl", else the one
 * named like it, else palette i when the counts match, else palette 0; *how is 0..3 in that
 * order. WL_ERR_NOT_FOUND when the file has no palette and the texture needs one. */
WlResult wl_tex0_pair_palette(const WlTex0 *t, uint32_t i, uint32_t *pal, int *how);
/* Exact RGBA decode of texture i with palette pal (ignored for direct colour). */
WlResult wl_tex0_decode(const WlTex0 *t, uint32_t i, uint32_t pal, WlImage *im);
/* Texture i as a GX texture under the policy (pol may be NULL: exact): paletted formats stay
 * C4 / C8 with a TLUT, A3I5 and A5I3 with partial alpha become RGBA8, two-level alpha and direct
 * colour become RGB5A3, 4x4-compressed become RGB5A3 or CMPR. out->fmt is the GX format. */
WlResult wl_tex0_to_gx(const WlTex0 *t, uint32_t i, uint32_t pal, const WlTex0Policy *pol,
                       WlGxTex *out);

/* ===================================================================================== */
/* PNG (previews only; stored deflate, no compression)                                   */
/* ===================================================================================== */
/* RGBA8 PNG of w x h pixels; *out is malloc'd. */
WlResult wl_png_encode(const uint8_t *rgba, uint32_t w, uint32_t h, uint8_t **out, size_t *len);
