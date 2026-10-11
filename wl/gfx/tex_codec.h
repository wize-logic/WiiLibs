// SPDX-License-Identifier: MIT
// GX texture encoders and decoders for RGBA8 images (the public GX tile layouts; see README.md).
//
// Meant for textures generated at run time (ramps, placeholders, synthetic test textures) and for
// host tests that verify the layouts. Pixels are RGBA8, row-major, 4 bytes each. Encoded data is
// exactly what the GX texture unit reads (tile-ordered, big-endian); dimensions are rounded up to the
// format's tile and the padding repeats the image's last row and column (indexed formats: index 0).
// Thin wrappers over the shared codec in wl/codec/gx_tex.h. Not a fast path: asset textures should be
// encoded offline.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <stdbool.h>

// Texture formats, in this module's own order (not the GX hardware numbers; wl_tex_gx_format maps
// them). I4/I8/IA4/IA8 are intensity formats, C4/C8 colour-indexed (they need a TLUT).
typedef enum {
    WL_TEX_I4 = 0, WL_TEX_I8, WL_TEX_IA4, WL_TEX_IA8, WL_TEX_RGB565,
    WL_TEX_RGB5A3, WL_TEX_RGBA8, WL_TEX_CMPR,
    WL_TEX_C4, WL_TEX_C8,
    WL_TEX_FORMAT_COUNT
} WlTexFormat;
// Palette (TLUT) entry formats; the values are the GX ones (GX_TL_IA8, GX_TL_RGB565, GX_TL_RGB5A3).
typedef enum { WL_TLUT_IA8 = 0, WL_TLUT_RGB565, WL_TLUT_RGB5A3 } WlTlutFormat;

// The GX hardware format number (GX_TF_*) of `fmt`, or -1 for an invalid one.
int wl_tex_gx_format(WlTexFormat fmt);
// Bytes of one level of a w x h texture (rounded up to whole tiles); 0 for an invalid format.
size_t wl_tex_bytes(WlTexFormat fmt, uint32_t width, uint32_t height);
// Bytes of a mip chain of `mips` levels (0 counts as 1), each level halved down to 1.
size_t wl_tex_bytes_mips(WlTexFormat fmt, uint32_t width, uint32_t height, uint32_t mips);

// Encodes `rgba` (w*h*4 bytes) as `fmt` into `dst` (`cap` bytes). Returns the bytes written, 0 when the
// format is a colour-indexed one (use wl_tex_encode_ci), the arguments are bad or `cap` is too small.
// I4/I8 take the luma (alpha = intensity on read), IA4/IA8 luma and alpha, RGB565 drops alpha, RGB5A3
// keeps 3 bits of alpha (and 444 colour) unless the alpha rounds to opaque, CMPR fits the two most
// distant 565 colours of each 4x4 block (integer only).
size_t wl_tex_encode(WlTexFormat fmt, const uint8_t *rgba, uint32_t w, uint32_t h, void *dst, size_t cap);
// Colour-indexed: `indices` is w*h bytes (C4 uses the low 4 bits).
size_t wl_tex_encode_ci(WlTexFormat fmt, const uint8_t *indices, uint32_t w, uint32_t h, void *dst, size_t cap);
// What the GPU shows for the encoded data: the exact inverse of the encoder's layout, with the
// hardware's expansion rules (565 and 4-bit values replicate their top bits, CMPR blends 5/8 : 3/8).
// `rgba_out` is w*h*4 bytes. I4/I8: intensity in all four channels; IA: intensity in rgb.
void wl_tex_decode(WlTexFormat fmt, const void *src, uint32_t w, uint32_t h, uint8_t *rgba_out);
void wl_tex_decode_ci(WlTexFormat fmt, const void *src, uint32_t w, uint32_t h, uint8_t *indices_out);

// Palette entries: `rgba` (count*4 bytes) to `count` big-endian 16-bit entries (count*2 bytes written).
void wl_tlut_encode(WlTlutFormat fmt, const uint8_t *rgba, uint32_t count, void *dst);
void wl_tlut_decode(WlTlutFormat fmt, const void *src, uint32_t count, uint8_t *rgba_out);

// Halves an image with a 2x2 box filter into `out` ((w/2)*(h/2)*4 bytes; w and h even, or 1).
void wl_tex_box_down(const uint8_t *rgba, uint32_t w, uint32_t h, uint8_t *out);
