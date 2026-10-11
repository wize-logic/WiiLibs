// SPDX-License-Identifier: MIT
// The GX texture codec: encoders and decoders for the formats the GX texture unit reads, shared by
// wl/gxtex (offline tools) and wl/gfx (runtime textures). Integer only, portable, no allocation,
// big-endian output.
//
// Pixels are RGBA8, row-major, 4 bytes each. Encoded data is tile-ordered exactly as the GPU reads it;
// dimensions are rounded up to the format's tile. The encoders fill the padding by repeating the
// last row and column of the image (colour-indexed encoders pad with index 0), so a block that
// straddles the edge does not see a phantom colour.
//
// Decoders expand the way the hardware does: 565 and 4-bit values replicate their top bits, CMPR blends the
// two endpoints with weights 5/8 : 3/8 ((5a + 3b) >> 3, not S3TC's 2/3 : 1/3).
#pragma once

#include <stddef.h>
#include <stdint.h>

// I4..RGBA8 match the hardware's GX_TF_* values; CMPR, C4 and C8 follow in this order (their GX_TF_*
// values differ). wl/gfx/tex_codec.c maps its own WlTexFormat onto this enum.
typedef enum {
    WL_GXC_I4 = 0, WL_GXC_I8, WL_GXC_IA4, WL_GXC_IA8, WL_GXC_RGB565,
    WL_GXC_RGB5A3, WL_GXC_RGBA8, WL_GXC_CMPR,
    WL_GXC_C4, WL_GXC_C8, // colour-indexed: encoded from and decoded to index bytes (the ci calls)
    WL_GXC_FORMAT_COUNT
} WlGxcFormat;

// Palette entry formats, in GX_TL_* order.
typedef enum { WL_GXC_TLUT_IA8 = 0, WL_GXC_TLUT_RGB565, WL_GXC_TLUT_RGB5A3 } WlGxcTlutFormat;

#define WL_GXC_MAX_DIM 16384u

// Bytes of an encoded w x h image (tile-padded), 0 for a bad format or a dimension of 0 or above WL_GXC_MAX_DIM.
size_t wl_gxc_tex_bytes(WlGxcFormat fmt, uint32_t w, uint32_t h);

// ---- colour conversions -------------------------------------------------------------------------
// Round-to-nearest quantisers; the decoders replicate the top bits so exact values round-trip.
uint16_t wl_gxc_rgb565_encode(const uint8_t rgba[4]);
void wl_gxc_rgb565_decode(uint32_t v, uint8_t rgba[4]);   // alpha 255
// Opaque (alpha rounds to 7/7) pixels use the 555 form with bit 15 set, the rest 3-bit alpha and 444 colour.
uint16_t wl_gxc_rgb5a3_encode(const uint8_t rgba[4]);
void wl_gxc_rgb5a3_decode(uint32_t v, uint8_t rgba[4]);

// ---- CMPR (S3TC-like, 8-byte sub-blocks of 4x4 texels) -----------------------------------------
// The four palette colours of a sub-block's endpoints (RGB565 c0, c1). c0 > c1: four colours, with
// indices 2 and 3 at 5/8:3/8 and 3/8:5/8 of the way; otherwise index 2 is the average and index 3 is
// transparent black.
void wl_gxc_cmpr_palette(uint32_t c0, uint32_t c1, uint8_t pal[4][4]);
// Encodes 16 pixels (row-major). Pixels with alpha < 128 select the transparent mode. Deterministic:
// the endpoints are the two most distant of the block's distinct 565 colours.
void wl_gxc_cmpr_encode_block(const uint8_t px[16][4], uint8_t out[8]);
void wl_gxc_cmpr_decode_block(const uint8_t in[8], uint8_t px[16][4]);

// ---- whole images -------------------------------------------------------------------------------
// Encodes `rgba` (w*h*4 bytes) as `fmt` into `dst` (`cap` bytes). Returns the bytes written, 0 when the
// format is a colour-indexed one (use wl_gxc_encode_ci), the arguments are bad or `cap` is too small.
// I4/I8 take the luma (alpha = intensity on read), IA4/IA8 luma and alpha, RGB565 drops alpha, RGB5A3 keeps 3 bits.
size_t wl_gxc_encode(WlGxcFormat fmt, const uint8_t *rgba, uint32_t w, uint32_t h, void *dst, size_t cap);
// Colour-indexed: `indices` is w*h bytes (C4 uses the low 4 bits; I4 data in 0..15 packs the same way as C4).
size_t wl_gxc_encode_ci(WlGxcFormat fmt, const uint8_t *indices, uint32_t w, uint32_t h, void *dst, size_t cap);
// What the GPU shows for the encoded data (`src_len` bytes at `src`, at least wl_gxc_tex_bytes). `rgba_out` is
// w*h*4 bytes. I4/I8: intensity in all four channels; IA: intensity in rgb. Returns the bytes read, 0 on bad arguments.
size_t wl_gxc_decode(WlGxcFormat fmt, const void *src, size_t src_len, uint32_t w, uint32_t h, uint8_t *rgba_out);
size_t wl_gxc_decode_ci(WlGxcFormat fmt, const void *src, size_t src_len, uint32_t w, uint32_t h, uint8_t *indices_out);

// Palette entries: `rgba` (count*4 bytes) to `count` big-endian 16-bit entries (count*2 bytes written), and back.
void wl_gxc_tlut_encode(WlGxcTlutFormat fmt, const uint8_t *rgba, uint32_t count, void *dst);
void wl_gxc_tlut_decode(WlGxcTlutFormat fmt, const void *src, uint32_t count, uint8_t *rgba_out);
