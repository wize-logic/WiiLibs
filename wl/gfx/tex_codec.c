// SPDX-License-Identifier: MIT
// GX texture encoders and decoders (tex_codec.h): thin wrappers over the shared codec in wl/codec/gx_tex.c.
#include "wl/gfx/tex_codec.h"

#include "wl/codec/gx_tex.h"

// Tile width, height and bytes per tile per format (matches wl_gxc_tex_bytes, without its size cap).
static const uint8_t k_tile[WL_TEX_FORMAT_COUNT][3] = {{8, 8, 32}, {8, 4, 32}, {8, 4, 32}, {4, 4, 32}, {4, 4, 32},
                                                       {4, 4, 32}, {4, 4, 64}, {8, 8, 32}, {8, 8, 32}, {8, 4, 32}};

// The shared codec numbers its formats in this module's order.
_Static_assert((int)WL_TEX_I4 == (int)WL_GXC_I4 && (int)WL_TEX_I8 == (int)WL_GXC_I8 && (int)WL_TEX_IA4 == (int)WL_GXC_IA4 &&
                   (int)WL_TEX_IA8 == (int)WL_GXC_IA8 && (int)WL_TEX_RGB565 == (int)WL_GXC_RGB565 &&
                   (int)WL_TEX_RGB5A3 == (int)WL_GXC_RGB5A3 && (int)WL_TEX_RGBA8 == (int)WL_GXC_RGBA8 &&
                   (int)WL_TEX_CMPR == (int)WL_GXC_CMPR && (int)WL_TEX_C4 == (int)WL_GXC_C4 &&
                   (int)WL_TEX_C8 == (int)WL_GXC_C8 && (int)WL_TEX_FORMAT_COUNT == (int)WL_GXC_FORMAT_COUNT,
               "WlTexFormat and WlGxcFormat must match");
_Static_assert((int)WL_TLUT_IA8 == (int)WL_GXC_TLUT_IA8 && (int)WL_TLUT_RGB565 == (int)WL_GXC_TLUT_RGB565 &&
                   (int)WL_TLUT_RGB5A3 == (int)WL_GXC_TLUT_RGB5A3,
               "WlTlutFormat and WlGxcTlutFormat must match");

size_t wl_tex_encode(WlTexFormat fmt, const uint8_t *rgba, uint32_t w, uint32_t h, void *dst, size_t cap) {
    return wl_gxc_encode((WlGxcFormat)fmt, rgba, w, h, dst, cap);
}

size_t wl_tex_encode_ci(WlTexFormat fmt, const uint8_t *indices, uint32_t w, uint32_t h, void *dst, size_t cap) {
    if (fmt != WL_TEX_C4 && fmt != WL_TEX_C8) { // the shared call also packs I4 index data (wl/gxtex)
        return 0;
    }
    return wl_gxc_encode_ci((WlGxcFormat)fmt, indices, w, h, dst, cap);
}

// The callers hand over exactly the encoded size of a w x h image of `fmt`.
void wl_tex_decode(WlTexFormat fmt, const void *src, uint32_t w, uint32_t h, uint8_t *rgba_out) {
    wl_gxc_decode((WlGxcFormat)fmt, src, wl_tex_bytes(fmt, w, h), w, h, rgba_out);
}

void wl_tex_decode_ci(WlTexFormat fmt, const void *src, uint32_t w, uint32_t h, uint8_t *indices_out) {
    wl_gxc_decode_ci((WlGxcFormat)fmt, src, wl_tex_bytes(fmt, w, h), w, h, indices_out);
}

void wl_tlut_encode(WlTlutFormat fmt, const uint8_t *rgba, uint32_t count, void *dst) {
    wl_gxc_tlut_encode((WlGxcTlutFormat)fmt, rgba, count, dst);
}

void wl_tlut_decode(WlTlutFormat fmt, const void *src, uint32_t count, uint8_t *rgba_out) {
    wl_gxc_tlut_decode((WlGxcTlutFormat)fmt, src, count, rgba_out);
}

void wl_tex_box_down(const uint8_t *rgba, uint32_t w, uint32_t h, uint8_t *out) {
    uint32_t ow = w > 1 ? w / 2 : 1;
    uint32_t oh = h > 1 ? h / 2 : 1;
    for (uint32_t y = 0; y < oh; y++) {
        for (uint32_t x = 0; x < ow; x++) {
            for (uint32_t k = 0; k < 4; k++) {
                uint32_t x0 = w > 1 ? x * 2u : 0u;
                uint32_t y0 = h > 1 ? y * 2u : 0u;
                uint32_t x1 = w > 1 ? x0 + 1u : 0u;
                uint32_t y1 = h > 1 ? y0 + 1u : 0u;
                uint32_t s = (uint32_t)rgba[((size_t)y0 * w + x0) * 4u + k] + (uint32_t)rgba[((size_t)y0 * w + x1) * 4u + k] +
                             (uint32_t)rgba[((size_t)y1 * w + x0) * 4u + k] + (uint32_t)rgba[((size_t)y1 * w + x1) * 4u + k];
                out[((size_t)y * ow + x) * 4u + k] = (uint8_t)((s + 2u) >> 2);
            }
        }
    }
}

int wl_tex_gx_format(WlTexFormat fmt) {
    // GX_TF_I4 .. GX_TF_RGBA8 are 0..6, GX_TF_CMPR 14, GX_TF_CI4 8, GX_TF_CI8 9.
    static const int8_t k_gx[WL_TEX_FORMAT_COUNT] = {0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0xE, 0x8, 0x9};
    return (unsigned)fmt < WL_TEX_FORMAT_COUNT ? k_gx[fmt] : -1;
}

size_t wl_tex_bytes(WlTexFormat fmt, uint32_t width, uint32_t height) {
    if ((unsigned)fmt >= WL_TEX_FORMAT_COUNT) {
        return 0;
    }
    size_t tx = (width + k_tile[fmt][0] - 1u) / k_tile[fmt][0];
    size_t ty = (height + k_tile[fmt][1] - 1u) / k_tile[fmt][1];
    return tx * ty * k_tile[fmt][2];
}

size_t wl_tex_bytes_mips(WlTexFormat fmt, uint32_t width, uint32_t height, uint32_t mips) {
    size_t total = 0;
    for (uint32_t i = 0; i < (mips == 0 ? 1u : mips); i++) {
        uint32_t w = i < 32u ? width >> i : 0u;
        uint32_t h = i < 32u ? height >> i : 0u;
        total += wl_tex_bytes(fmt, w == 0 ? 1u : w, h == 0 ? 1u : h);
    }
    return total;
}
