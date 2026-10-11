// SPDX-License-Identifier: MIT
// Xbox (Direct3D 8) textures to GX textures: the Xbox's swizzled (Morton order) and linear layouts, S3TC/DXT
// blocks, and the conversions of each Xbox format to the GX texel format that shows it. Companion of gx_tex.h:
// integer only, portable, no allocation, big-endian output, the same results on little- and big-endian hosts.
//
// Xbox texels are little-endian: A8R8G8B8 is the bytes B, G, R, A; the 16-bit formats are little-endian words.
// DXT textures are always linear, rows of 4x4 blocks (8 bytes for DXT1, 16 for DXT3 and DXT5); every other
// format is either swizzled (a power-of-two size, texels in Morton order) or linear (rows `pitch` bytes apart).
//
// GX output is tile-ordered as gx_tex.h describes (dimensions rounded up to the format's tile). Unlike the
// encoders of gx_tex.h, which repeat the image's last row and column, these converters move or recode data
// that already is in blocks and fill the padding with zero bytes. The padding is never sampled.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "wl/codec/gx_tex.h"

// The texel formats of Direct3D 8 that textures use (D3DFMT_ values in the comments, swizzled and linear).
typedef enum {
    WL_XBOX_A8R8G8B8 = 0, // 0x06, 0x12
    WL_XBOX_X8R8G8B8,     // 0x07, 0x1E  (alpha ignored: reads as 255)
    WL_XBOX_R5G6B5,       // 0x05, 0x11
    WL_XBOX_A1R5G5B5,     // 0x02, 0x10
    WL_XBOX_A4R4G4B4,     // 0x04, 0x1D
    WL_XBOX_L8,           // 0x00, 0x13
    WL_XBOX_A8,           // 0x19, 0x1F
    WL_XBOX_AL8,          // 0x01, 0x1B
    WL_XBOX_A8L8,         // 0x1A, 0x20
    WL_XBOX_P8,           // 0x0B  (palettised: 8-bit indices; the palette is D3DCOLOR entries)
    WL_XBOX_DXT1,         // 0x0C  (linear blocks only)
    WL_XBOX_DXT3,         // 0x0E
    WL_XBOX_DXT5,         // 0x0F
    WL_XBOX_FORMAT_COUNT
} WlXboxFormat;

// Bytes per texel of a non-DXT format, 0 for DXT and for a bad format.
size_t wl_xbox_texel_bytes(WlXboxFormat fmt);

// Bytes of one level of a w x h Xbox texture. A DXT level is ceil(w/4) x ceil(h/4) blocks. For the others a
// non-zero `pitch` is the bytes between rows of a linear level (>= w * texel bytes), and 0 means packed rows
// (also what a swizzled level is). 0 for a bad format, a zero or oversized (> WL_GXC_MAX_DIM) dimension or a
// pitch that is too small.
size_t wl_xbox_level_bytes(WlXboxFormat fmt, uint32_t w, uint32_t h, uint32_t pitch);

// ---- swizzle (Morton order) ---------------------------------------------------------------------
// The bits of a swizzled texel index that come from x and from y, as the Xbox lays them out: the low bits
// alternate x, y, x, y ... while both sizes have bits left, then the rest come from the larger side. w and h
// are powers of two. Returns 0 (and writes nothing) for any other size, 1 on success.
int wl_xbox_swizzle_masks(uint32_t w, uint32_t h, uint32_t *mask_x, uint32_t *mask_y);

// Swizzled -> linear (rows of w texels, no padding) and back. `texel` is 1..16 bytes. Both sizes are powers of
// two <= WL_GXC_MAX_DIM. They return the bytes written (w*h*texel), 0 for bad arguments or a too-small `cap`
// or source. src and dst must not overlap.
size_t wl_xbox_unswizzle(const void *src, size_t src_len, uint32_t w, uint32_t h, uint32_t texel, void *dst, size_t cap);
size_t wl_xbox_swizzle(const void *src, size_t src_len, uint32_t w, uint32_t h, uint32_t texel, void *dst, size_t cap);

// ---- Xbox format to GX --------------------------------------------------------------------------
// The GX format that shows an Xbox format as it is (no re-quantising):
//   A8R8G8B8, X8R8G8B8 -> RGBA8     R5G6B5 -> RGB565        A1R5G5B5, A4R4G4B4 -> RGB5A3
//   L8, A8, AL8 -> I8 (the byte is kept; which channels it means is the caller's)
//   A8L8 -> IA8     P8 -> C8        DXT1, DXT3, DXT5 -> CMPR (DXT3/5: the colour half only; see below)
// WL_GXC_FORMAT_COUNT for a bad format.
WlGxcFormat wl_xbox_gx_format(WlXboxFormat fmt);

// Converts one level of a non-DXT texture to wl_xbox_gx_format(fmt), tile-ordered. `swizzled` selects Morton
// order (pitch is ignored) or linear rows `pitch` bytes apart (0: packed). Returns the bytes written
// (wl_gxc_tex_bytes of the GX format), 0 for bad arguments, a DXT format (use the calls below), a too-small
// `cap` or a source shorter than wl_xbox_level_bytes.
//   - Colour: 16-bit and 32-bit texels are reordered and, for the 16-bit formats with alpha, re-packed bit for
//     bit, with no 8-bit round trip. A1R5G5B5 with alpha set is RGB5A3's 1RRRRRGGGGGBBBBB form as it is,
//     otherwise 0AAARRRRGGGGBBBB with alpha 0 and the top four bits of each colour. A4R4G4B4 with alpha 15 is
//     the opaque form with each 4-bit colour widened to five bits (v<<1 | v>>3), otherwise 0AAARRRRGGGGBBBB with
//     the top three bits of alpha. X8R8G8B8 gets alpha 255.
//   - A8L8 is stored alpha first, then luminance, as IA8 reads it.
//   - Padding texels are zero.
size_t wl_xbox_to_gx(WlXboxFormat fmt, int swizzled, uint32_t pitch, const void *src, size_t src_len,
                     uint32_t w, uint32_t h, void *dst, size_t cap);

// ---- DXT ----------------------------------------------------------------------------------------
// DXT to CMPR, losslessly: the 8-byte colour block (the whole block for DXT1, the second half for DXT3/5) moves
// to GX's big-endian endpoints and index order (texel 0 in the two high bits), four blocks to an 8x8 tile in
// row-major order. DXT1 blocks keep their meaning (c0 > c1: four colours, else three and a transparent one).
// A DXT3/5 block is always four-colour mode, which CMPR is only for c0 > c1: where c0 < c1 the endpoints are
// swapped and the indices with them (0<->1, 2<->3); where c0 == c1 every index becomes 0. Returns
// wl_gxc_tex_bytes(WL_GXC_CMPR, w, h), 0 for bad arguments, a non-DXT format, a too-small `cap` or a source
// shorter than wl_xbox_level_bytes. Padding blocks are zero.
size_t wl_xbox_dxt_to_cmpr(WlXboxFormat dxt, const void *src, size_t src_len, uint32_t w, uint32_t h, void *dst, size_t cap);

// The alpha of a DXT3 or DXT5 level as an I4 texture (the top four bits of each alpha; exact for DXT3), the
// partner of wl_xbox_dxt_to_cmpr when alpha is a texture of its own. Returns wl_gxc_tex_bytes(WL_GXC_I4, w, h).
// Texels past w x h are zero. 0 as above, and for DXT1 (which has no alpha channel to split).
size_t wl_xbox_dxt_alpha_i4(WlXboxFormat dxt, const void *src, size_t src_len, uint32_t w, uint32_t h, void *dst, size_t cap);

// One block's 16 alphas, row-major, 0..255: DXT3 widens each nibble (x * 17), DXT5 interpolates its 8-value
// table (the 6-value form when a0 <= a1, with 0 and 255 as the last two). `block` is the 8 alpha bytes.
void wl_xbox_dxt_alpha_block(WlXboxFormat dxt, const uint8_t block[8], uint8_t alpha[16]);

// Decodes one block (8 or 16 bytes) to 16 RGBA texels, row-major, the S3TC way: 565 endpoints widened by
// replicating the top bits, the two inner colours at 2/3 : 1/3 and 1/3 : 2/3 (DXT1 with c0 <= c1: the mean and
// transparent black). DXT1 alpha is 0 for a transparent texel, else 255.
void wl_xbox_dxt_decode_block(WlXboxFormat dxt, const uint8_t *block, uint8_t px[16][4]);

// DXT straight to a GX texture that holds colour and alpha: `target` is WL_GXC_RGBA8, WL_GXC_RGB5A3 or
// WL_GXC_RGB565 (the alpha dropped). Decoded as wl_xbox_dxt_decode_block; one 4x4 block is one GX tile.
// Returns wl_gxc_tex_bytes(target, w, h), 0 for bad arguments as above.
size_t wl_xbox_dxt_to_gx(WlXboxFormat dxt, WlGxcFormat target, const void *src, size_t src_len,
                         uint32_t w, uint32_t h, void *dst, size_t cap);

// ---- palettes -----------------------------------------------------------------------------------
// An Xbox palette (D3DCOLOR entries: the bytes B, G, R, A) to `count` big-endian TLUT entries of `fmt`,
// for a P8 texture converted to C8. count <= 256. `dst` takes count * 2 bytes.
void wl_xbox_palette_to_tlut(WlGxcTlutFormat fmt, const uint8_t *bgra, uint32_t count, void *dst);
