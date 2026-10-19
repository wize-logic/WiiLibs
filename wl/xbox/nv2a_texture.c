// SPDX-License-Identifier: MIT
// The NV2A's textures on GX (nv2a_texture.h).
//
// Each NV2A texture format has a GX format it becomes: through wl/codec's Xbox conversions when
// they are exact (A8R8G8B8 and X8R8G8B8 to RGBA8, R5G6B5 to RGB565, A1R5G5B5 to RGB5A3, Y8, AY8 and
// A8 to I8 with what the byte means told to psh, A8Y8 to IA8, P8 to C8 with a TLUT, DXT1 to CMPR,
// DXT3 and DXT5 to CMPR with their alpha an I4 image of its own), the rest decoded texel by texel
// to RGBA8 and encoded again (A4R4G4B4 to RGBA8, which keeps its four bits of alpha where RGB5A3
// has three: a title's text is drawn from one; the 32-bit orders, R6G5B5, G8B8, R8B8, YUY2 and UYVY,
// the depth formats as their top eight bits).
//
// Levels: a swizzled or DXT texture's levels follow each other, each its own size (a DXT level
// whole blocks, at least one); a cube map's six faces are each all its levels, 128-byte aligned.
// GX keeps the levels (when the sampler uses them) from the first one GX can hold.

#include "wl/xbox/nv2a_texture.h"

#include <math.h>
#include <string.h>

#include "wl/base/endian.h"
#include "wl/gfx/gfx_sqrt.h"

enum { LAYOUT_SZ, LAYOUT_LU, LAYOUT_DXT };

static bool g_plain; // wl_nv2a_texture_set_plain

typedef struct FormatInfo {
    uint8_t known;
    uint8_t bytes;   // a texel's (0: DXT)
    uint8_t layout;  // LAYOUT_*
    int8_t codec;    // WlXboxFormat, -1: decoded texel by texel
    uint8_t gx;      // WlGxcFormat
    uint8_t contents;
} FormatInfo;

static FormatInfo format_info(uint8_t color) {
    FormatInfo f = {1, 0, LAYOUT_SZ, -1, WL_GXC_RGBA8, WL_XBOX_PSH_TEXTURE_ALL};

    switch (color) {
    case WL_NV2A_TEX_SZ_Y8:
    case WL_NV2A_TEX_LU_Y8:
        f.bytes = 1, f.codec = WL_XBOX_L8, f.gx = WL_GXC_I8, f.contents = WL_XBOX_PSH_TEXTURE_COLOR;
        break;
    case WL_NV2A_TEX_SZ_AY8:
    case WL_NV2A_TEX_LU_AY8:
        f.bytes = 1, f.codec = WL_XBOX_AL8, f.gx = WL_GXC_I8;
        break;
    case WL_NV2A_TEX_SZ_A8:
    case WL_NV2A_TEX_LU_A8:
        f.bytes = 1, f.codec = WL_XBOX_A8, f.gx = WL_GXC_I8, f.contents = WL_XBOX_PSH_TEXTURE_ALPHA;
        break;
    case WL_NV2A_TEX_SZ_A8Y8:
    case WL_NV2A_TEX_LU_A8Y8:
        f.bytes = 2, f.codec = WL_XBOX_A8L8, f.gx = WL_GXC_IA8;
        break;
    case WL_NV2A_TEX_SZ_A1R5G5B5:
    case WL_NV2A_TEX_LU_A1R5G5B5:
        f.bytes = 2, f.codec = WL_XBOX_A1R5G5B5, f.gx = WL_GXC_RGB5A3;
        break;
    case WL_NV2A_TEX_SZ_X1R5G5B5:
    case WL_NV2A_TEX_LU_X1R5G5B5:
    case WL_NV2A_TEX_SZ_R6G5B5:
        f.bytes = 2, f.gx = WL_GXC_RGB565;
        break;
    case WL_NV2A_TEX_SZ_A4R4G4B4:
    case WL_NV2A_TEX_LU_A4R4G4B4:
    case WL_NV2A_TEX_SZ_G8B8:
    case WL_NV2A_TEX_LU_G8B8:
    case WL_NV2A_TEX_SZ_R8B8:
    case WL_NV2A_TEX_LU_R8B8:
    case WL_NV2A_TEX_LC_CR8YB8CB8YA8:
    case WL_NV2A_TEX_LC_YB8CR8YA8CB8:
        f.bytes = 2, f.gx = WL_GXC_RGBA8;
        break;
    case WL_NV2A_TEX_SZ_R5G6B5:
    case WL_NV2A_TEX_LU_R5G6B5:
        f.bytes = 2, f.codec = WL_XBOX_R5G6B5, f.gx = WL_GXC_RGB565;
        break;
    case WL_NV2A_TEX_SZ_A8R8G8B8:
    case WL_NV2A_TEX_LU_A8R8G8B8:
        f.bytes = 4, f.codec = WL_XBOX_A8R8G8B8, f.gx = WL_GXC_RGBA8;
        break;
    case WL_NV2A_TEX_SZ_X8R8G8B8:
    case WL_NV2A_TEX_LU_X8R8G8B8:
        f.bytes = 4, f.codec = WL_XBOX_X8R8G8B8, f.gx = WL_GXC_RGBA8;
        break;
    case WL_NV2A_TEX_SZ_A8B8G8R8:
    case WL_NV2A_TEX_SZ_B8G8R8A8:
    case WL_NV2A_TEX_SZ_R8G8B8A8:
    case WL_NV2A_TEX_LU_A8B8G8R8:
    case WL_NV2A_TEX_LU_B8G8R8A8:
    case WL_NV2A_TEX_LU_R8G8B8A8:
        f.bytes = 4, f.gx = WL_GXC_RGBA8;
        break;
    case WL_NV2A_TEX_SZ_I8_A8R8G8B8:
        f.bytes = 1, f.codec = WL_XBOX_P8, f.gx = WL_GXC_C8;
        break;
    case WL_NV2A_TEX_L_DXT1:
        f.layout = LAYOUT_DXT, f.codec = WL_XBOX_DXT1, f.gx = WL_GXC_CMPR;
        break;
    case WL_NV2A_TEX_L_DXT23:
        f.layout = LAYOUT_DXT, f.codec = WL_XBOX_DXT3, f.gx = WL_GXC_CMPR;
        break;
    case WL_NV2A_TEX_L_DXT45:
        f.layout = LAYOUT_DXT, f.codec = WL_XBOX_DXT5, f.gx = WL_GXC_CMPR;
        break;
    case WL_NV2A_TEX_SZ_DEPTH_X8_Y24_FIXED:
    case WL_NV2A_TEX_SZ_DEPTH_X8_Y24_FLOAT:
    case WL_NV2A_TEX_LU_DEPTH_X8_Y24_FIXED:
        f.bytes = 4, f.gx = WL_GXC_I8, f.contents = WL_XBOX_PSH_TEXTURE_COLOR;
        break;
    case WL_NV2A_TEX_SZ_DEPTH_Y16_FIXED:
    case WL_NV2A_TEX_SZ_DEPTH_Y16_FLOAT:
    case WL_NV2A_TEX_LU_DEPTH_Y16_FIXED:
    case WL_NV2A_TEX_LU_DEPTH_Y16_FLOAT:
    case WL_NV2A_TEX_SZ_Y16:
    case 0x35: // LU_Y16
        f.bytes = 2, f.gx = WL_GXC_I8, f.contents = WL_XBOX_PSH_TEXTURE_COLOR;
        break;
    case WL_NV2A_TEX_SZ_V16U16:
    case WL_NV2A_TEX_LU_V16U16:
        f.bytes = 4, f.gx = WL_GXC_RGBA8;
        break;
    default:
        f.known = 0;
        break;
    }
    // the linear formats
    switch (color) {
    case WL_NV2A_TEX_LU_A1R5G5B5:
    case WL_NV2A_TEX_LU_R5G6B5:
    case WL_NV2A_TEX_LU_A8R8G8B8:
    case WL_NV2A_TEX_LU_Y8:
    case WL_NV2A_TEX_LU_R8B8:
    case WL_NV2A_TEX_LU_G8B8:
    case WL_NV2A_TEX_LU_AY8:
    case WL_NV2A_TEX_LU_X1R5G5B5:
    case WL_NV2A_TEX_LU_A4R4G4B4:
    case WL_NV2A_TEX_LU_X8R8G8B8:
    case WL_NV2A_TEX_LU_A8:
    case WL_NV2A_TEX_LU_A8Y8:
    case WL_NV2A_TEX_LC_CR8YB8CB8YA8:
    case WL_NV2A_TEX_LC_YB8CR8YA8CB8:
    case WL_NV2A_TEX_LU_DEPTH_X8_Y24_FIXED:
    case WL_NV2A_TEX_LU_DEPTH_Y16_FIXED:
    case WL_NV2A_TEX_LU_DEPTH_Y16_FLOAT:
    case 0x35:
    case WL_NV2A_TEX_LU_V16U16:
    case WL_NV2A_TEX_LU_A8B8G8R8:
    case WL_NV2A_TEX_LU_B8G8R8A8:
    case WL_NV2A_TEX_LU_R8G8B8A8:
        f.layout = LAYOUT_LU;
        break;
    default:
        break;
    }
    return f;
}

static uint32_t at_least_1(uint32_t v) {
    return v != 0 ? v : 1u;
}

// the bytes of a guest level (one face, one slice of a volume per depth)
static uint32_t level_bytes(const WlNv2aTexture *t, uint32_t level) {
    uint32_t w = at_least_1(t->width >> level), h = at_least_1(t->height >> level);
    uint32_t d = at_least_1(t->depth >> level);

    if (t->compressed) {
        uint32_t block = t->color == WL_NV2A_TEX_L_DXT1 ? 8u : 16u;
        return ((w + 3u) / 4u) * ((h + 3u) / 4u) * block * d;
    }
    return w * h * d * t->texel_bytes;
}

static uint32_t level_offset(const WlNv2aTexture *t, uint32_t level) {
    uint32_t offset = 0, l;

    for (l = 0; l < level; l++) {
        offset += level_bytes(t, l);
    }
    return offset;
}

bool wl_nv2a_texture(const WlNv2aKelvin *k, unsigned stage, WlNv2aTexture *out) {
    uint32_t base = WL_NV2A_K_TEXTURE_STRIDE * (stage & 3u);
    uint32_t offset = wl_nv2a_k(k, WL_NV2A_K_SET_TEXTURE_OFFSET + base);
    uint32_t format = wl_nv2a_k(k, WL_NV2A_K_SET_TEXTURE_FORMAT + base);
    uint32_t address = wl_nv2a_k(k, WL_NV2A_K_SET_TEXTURE_ADDRESS + base);
    uint32_t filter = wl_nv2a_k(k, WL_NV2A_K_SET_TEXTURE_FILTER + base);
    uint32_t palette = wl_nv2a_k(k, WL_NV2A_K_SET_TEXTURE_PALETTE + base);
    const WlNv2aDma *dma = (format & WL_NV2A_TEXFMT_DMA_MASK) == 2u ? &k->dma_b : &k->dma_a;
    FormatInfo info;
    uint32_t l;

    memset(out, 0, sizeof *out);
    if (format == 0) {
        return false;
    }
    out->format_word = format;
    out->rect_word = wl_nv2a_k(k, WL_NV2A_K_SET_TEXTURE_IMAGE_RECT + base);
    out->phys = dma->base + offset;
    out->color = (uint8_t)((format >> WL_NV2A_TEXFMT_COLOR_SHIFT) & 0xFFu);
    info = format_info(out->color);
    out->known = info.known;
    out->texel_bytes = info.bytes;
    out->contents = info.contents;
    out->compressed = info.layout == LAYOUT_DXT;
    out->linear = info.layout == LAYOUT_LU;
    out->swizzled = info.layout == LAYOUT_SZ;
    out->cube = (format & WL_NV2A_TEXFMT_CUBEMAP) != 0;
    out->volume = ((format >> WL_NV2A_TEXFMT_DIMENSIONS_SHIFT) & 0xFu) == 3u;
    out->enabled = (wl_nv2a_k(k, WL_NV2A_K_SET_TEXTURE_CONTROL0 + base) >> 30) & 1u;
    out->wrap_u = (uint8_t)(address & 0xFu);
    out->wrap_v = (uint8_t)((address >> 8) & 0xFu);
    out->border_color = wl_nv2a_k(k, WL_NV2A_K_SET_TEXTURE_BORDER_COLOR + base);
    out->min_filter = (uint8_t)((filter >> WL_NV2A_TEXFILTER_MIN_SHIFT) & 0xFFu);
    out->mag_filter = (uint8_t)((filter >> WL_NV2A_TEXFILTER_MAG_SHIFT) & 0xFu);
    if (out->color == WL_NV2A_TEX_SZ_V16U16 || out->color == WL_NV2A_TEX_LU_V16U16) {
        out->signs = (uint8_t)(filter >> 28);
    }
    if (out->linear) {
        out->width = out->rect_word >> 16;
        out->height = out->rect_word & 0xFFFFu;
        out->depth = 1;
        out->levels = 1;
        out->pitch = wl_nv2a_k(k, WL_NV2A_K_SET_TEXTURE_CONTROL1 + base) >> 16;
        if (out->pitch < out->width * out->texel_bytes) {
            out->pitch = out->width * out->texel_bytes;
        }
        out->face_bytes = out->pitch * out->height;
        out->total_bytes = out->face_bytes;
    } else {
        out->width = 1u << ((format >> WL_NV2A_TEXFMT_SIZE_U_SHIFT) & 0xFu);
        out->height = 1u << ((format >> WL_NV2A_TEXFMT_SIZE_V_SHIFT) & 0xFu);
        out->depth = out->volume ? 1u << ((format >> WL_NV2A_TEXFMT_SIZE_P_SHIFT) & 0xFu) : 1u;
        out->levels = at_least_1((format >> WL_NV2A_TEXFMT_LEVELS_SHIFT) & 0xFu);
        out->face_bytes = 0;
        for (l = 0; l < out->levels; l++) {
            out->face_bytes += level_bytes(out, l);
        }
        if (out->cube) {
            out->face_bytes = (out->face_bytes + 127u) & ~127u;
        }
        out->total_bytes = out->face_bytes * (out->cube ? 6u : 1u);
    }
    if (out->color == WL_NV2A_TEX_SZ_I8_A8R8G8B8) {
        const WlNv2aDma *palette_dma = (palette & 1u) ? &k->dma_b : &k->dma_a;

        out->palette_phys = palette_dma->base + (palette & ~0x3Fu);
        out->palette_entries = 256u >> ((palette >> 2) & 3u);
    }
    return out->width != 0 && out->height != 0 && out->total_bytes != 0;
}

// ---- texels -----------------------------------------------------------------------------------

static uint8_t expand5(uint32_t v) {
    return (uint8_t)((v << 3) | (v >> 2));
}

static uint8_t expand6(uint32_t v) {
    return (uint8_t)((v << 2) | (v >> 4));
}

static uint8_t clamp_byte(int v) {
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

static void yuv_rgba(int y, int u, int v, uint8_t rgba[4]) {
    int c = y - 16, d = u - 128, e = v - 128;

    rgba[0] = clamp_byte((298 * c + 409 * e + 128) >> 8);
    rgba[1] = clamp_byte((298 * c - 100 * d - 208 * e + 128) >> 8);
    rgba[2] = clamp_byte((298 * c + 516 * d + 128) >> 8);
    rgba[3] = 255;
}

static void set_rgba(uint8_t rgba[4], uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    rgba[0] = (uint8_t)r;
    rgba[1] = (uint8_t)g;
    rgba[2] = (uint8_t)b;
    rgba[3] = (uint8_t)a;
}

// V16U16 (a bump map's two 16-bit components, U in the low half) as D3D reads it: (U, V, 1, 1). A
// channel the filter register reads as signed (`signs`: its bits 31:28, R and G bits 1 and 2) is
// clamped to [0, 1], as wl/xbox/psh takes signed values on GX; an unsigned one is its high byte.
static uint8_t v16_channel(uint32_t v, bool is_signed) {
    if (is_signed) {
        int32_t s = (int32_t)(int16_t)(uint16_t)v;

        return s <= 0 ? 0u : (uint8_t)(s >> 7);
    }
    return (uint8_t)(v >> 8);
}

static void v16u16_rgba(const uint8_t *row, uint32_t x, uint8_t signs, uint8_t rgba[4]) {
    uint32_t v = wl_rd_le32(row + 4 * x);

    set_rgba(rgba, v16_channel(v & 0xFFFFu, (signs & 2u) != 0), v16_channel(v >> 16, (signs & 4u) != 0), 255, 255);
}

void wl_nv2a_texel_rgba(uint8_t color, const uint8_t *row, uint32_t x, const uint8_t *palette, uint8_t rgba[4]) {
    switch (color) {
    case WL_NV2A_TEX_SZ_Y8:
    case WL_NV2A_TEX_LU_Y8:
        set_rgba(rgba, row[x], row[x], row[x], 255);
        break;
    case WL_NV2A_TEX_SZ_AY8:
    case WL_NV2A_TEX_LU_AY8:
        set_rgba(rgba, row[x], row[x], row[x], row[x]);
        break;
    case WL_NV2A_TEX_SZ_A8:
    case WL_NV2A_TEX_LU_A8:
        set_rgba(rgba, 255, 255, 255, row[x]);
        break;
    case WL_NV2A_TEX_SZ_A8Y8:
    case WL_NV2A_TEX_LU_A8Y8: {
        uint32_t v = wl_rd_le16(row + 2 * x);
        set_rgba(rgba, v & 0xFFu, v & 0xFFu, v & 0xFFu, v >> 8);
        break;
    }
    case WL_NV2A_TEX_SZ_A1R5G5B5:
    case WL_NV2A_TEX_LU_A1R5G5B5:
    case WL_NV2A_TEX_SZ_X1R5G5B5:
    case WL_NV2A_TEX_LU_X1R5G5B5: {
        uint32_t v = wl_rd_le16(row + 2 * x);
        bool opaque = color == WL_NV2A_TEX_SZ_X1R5G5B5 || color == WL_NV2A_TEX_LU_X1R5G5B5 || (v & 0x8000u);
        set_rgba(rgba, expand5((v >> 10) & 31u), expand5((v >> 5) & 31u), expand5(v & 31u), opaque ? 255u : 0u);
        break;
    }
    case WL_NV2A_TEX_SZ_A4R4G4B4:
    case WL_NV2A_TEX_LU_A4R4G4B4: {
        uint32_t v = wl_rd_le16(row + 2 * x);
        set_rgba(rgba, ((v >> 8) & 15u) * 17u, ((v >> 4) & 15u) * 17u, (v & 15u) * 17u, (v >> 12) * 17u);
        break;
    }
    case WL_NV2A_TEX_SZ_R5G6B5:
    case WL_NV2A_TEX_LU_R5G6B5: {
        uint32_t v = wl_rd_le16(row + 2 * x);
        set_rgba(rgba, expand5(v >> 11), expand6((v >> 5) & 63u), expand5(v & 31u), 255);
        break;
    }
    case WL_NV2A_TEX_SZ_R6G5B5: {
        uint32_t v = wl_rd_le16(row + 2 * x);
        set_rgba(rgba, expand6(v >> 10), expand5((v >> 5) & 31u), expand5(v & 31u), 255);
        break;
    }
    case WL_NV2A_TEX_SZ_G8B8:
    case WL_NV2A_TEX_LU_G8B8: {
        uint32_t v = wl_rd_le16(row + 2 * x);
        set_rgba(rgba, 0, v >> 8, v & 0xFFu, 255);
        break;
    }
    case WL_NV2A_TEX_SZ_R8B8:
    case WL_NV2A_TEX_LU_R8B8: {
        uint32_t v = wl_rd_le16(row + 2 * x);
        set_rgba(rgba, v >> 8, 0, v & 0xFFu, 255);
        break;
    }
    case WL_NV2A_TEX_SZ_A8R8G8B8:
    case WL_NV2A_TEX_LU_A8R8G8B8:
        set_rgba(rgba, row[4 * x + 2], row[4 * x + 1], row[4 * x], row[4 * x + 3]);
        break;
    case WL_NV2A_TEX_SZ_X8R8G8B8:
    case WL_NV2A_TEX_LU_X8R8G8B8:
        set_rgba(rgba, row[4 * x + 2], row[4 * x + 1], row[4 * x], 255);
        break;
    case WL_NV2A_TEX_SZ_A8B8G8R8:
    case WL_NV2A_TEX_LU_A8B8G8R8:
        set_rgba(rgba, row[4 * x], row[4 * x + 1], row[4 * x + 2], row[4 * x + 3]);
        break;
    case WL_NV2A_TEX_SZ_B8G8R8A8:
    case WL_NV2A_TEX_LU_B8G8R8A8:
        set_rgba(rgba, row[4 * x + 1], row[4 * x + 2], row[4 * x + 3], row[4 * x]);
        break;
    case WL_NV2A_TEX_SZ_R8G8B8A8:
    case WL_NV2A_TEX_LU_R8G8B8A8:
        set_rgba(rgba, row[4 * x + 3], row[4 * x + 2], row[4 * x + 1], row[4 * x]);
        break;
    case WL_NV2A_TEX_SZ_I8_A8R8G8B8:
        if (palette != NULL) {
            const uint8_t *e = palette + 4u * row[x];
            set_rgba(rgba, e[2], e[1], e[0], e[3]);
        } else {
            set_rgba(rgba, row[x], row[x], row[x], 255);
        }
        break;
    case WL_NV2A_TEX_LC_CR8YB8CB8YA8: {
        // YUY2: bytes Y0, U, Y1, V for a pair of texels
        const uint8_t *p = row + 4 * (x >> 1);
        yuv_rgba(p[(x & 1u) ? 2 : 0], p[1], p[3], rgba);
        break;
    }
    case WL_NV2A_TEX_LC_YB8CR8YA8CB8: {
        // UYVY: bytes U, Y0, V, Y1
        const uint8_t *p = row + 4 * (x >> 1);
        yuv_rgba(p[(x & 1u) ? 3 : 1], p[0], p[2], rgba);
        break;
    }
    case WL_NV2A_TEX_SZ_V16U16:
    case WL_NV2A_TEX_LU_V16U16:
        v16u16_rgba(row, x, 0, rgba);
        break;
    case WL_NV2A_TEX_SZ_DEPTH_X8_Y24_FIXED:
    case WL_NV2A_TEX_SZ_DEPTH_X8_Y24_FLOAT:
    case WL_NV2A_TEX_LU_DEPTH_X8_Y24_FIXED: {
        uint32_t v = wl_rd_le32(row + 4 * x) >> 24;
        set_rgba(rgba, v, v, v, 255);
        break;
    }
    default: {
        // the 16-bit depths and Y16: the top byte
        uint32_t v = wl_rd_le16(row + 2 * x) >> 8;
        set_rgba(rgba, v, v, v, 255);
        break;
    }
    }
}

// ---- the sphere map ---------------------------------------------------------------------------

// 1 / sqrt(x) for x > 0, finite: the estimate (frsqrte's 5 bits on the Wii) and three steps of
// Newton's method in double (36 bits or more: a float's worth, no division)
static inline double reciprocal_root(double x) {
    double y = wl_rsqrt_estimate(x);

    y = y * (1.5 - 0.5 * x * y * y);
    y = y * (1.5 - 0.5 * x * y * y);
    y = y * (1.5 - 0.5 * x * y * y);
    return y;
}

// (x / m + 1/2 with d normalized and m = 2 |d + (0, 0, 1)| is x k + 1/2 with k = 1 / (2 sqrt(2 L (L +
// z))), L = |d|: two reciprocal roots, no division, for the 2-3% of a frame the five divisions and two
// exact roots cost on the Wii)
void wl_nv2a_sphere_coordinates_double(const float direction[3], float st[2]) {
    double x = direction[0], y = direction[1], z = direction[2];
    double length2 = x * x + y * y + z * z, length, q;

    if (!(length2 > 1.0e-24 && length2 < 1.0e76)) {
        st[0] = st[1] = 0.5f;
        return;
    }
    length = length2 * reciprocal_root(length2);
    q = 2.0 * length * (length + z);
    if (!(q > 2.5e-13 * length2)) {
        // straight behind (m below 1e-6): the disc's edge
        st[0] = 0.5f;
        st[1] = 1.0f;
        return;
    }
    q = 0.5 * reciprocal_root(q);
    st[0] = (float)(x * q + 0.5);
    st[1] = (float)(y * q + 0.5);
}

void wl_nv2a_sphere_coordinates(const float direction[3], float st[2]) {
    wl_nv2a_sphere_coordinates_inline(direction, st);
}

// (`root` sqrtf or wl_sqrtf_exact: the same bits, newlib's a bit at a time and about four times the
// instructions on the Wii)
static inline void sphere_direction(float s, float t, float direction[3], bool exact) {
    float a = 2.0f * s - 1.0f, b = 2.0f * t - 1.0f, r2 = a * a + b * b, c;

    if (r2 > 1.0f) {
        float r = exact ? wl_sqrtf_exact(r2) : sqrtf(r2);
        a /= r;
        b /= r;
        r2 = 1.0f;
    }
    c = exact ? wl_sqrtf_exact(1.0f - r2) : sqrtf(1.0f - r2);
    // the reflection of the view direction about the half vector (a, b, c)
    direction[0] = 2.0f * c * a;
    direction[1] = 2.0f * c * b;
    direction[2] = 2.0f * c * c - 1.0f;
}

void wl_nv2a_sphere_direction(float s, float t, float direction[3]) {
    sphere_direction(s, t, direction, false);
}

// a cube map's face and its texel coordinates (0..1) for a direction (Direct3D's faces: +x, -x,
// +y, -y, +z, -z)
static unsigned cube_face(const float d[3], float *s, float *t) {
    float ax = fabsf(d[0]), ay = fabsf(d[1]), az = fabsf(d[2]), sc, tc, ma;
    unsigned face;

    if (ax >= ay && ax >= az) {
        ma = ax;
        face = d[0] >= 0.0f ? 0u : 1u;
        sc = d[0] >= 0.0f ? -d[2] : d[2];
        tc = -d[1];
    } else if (ay >= az) {
        ma = ay;
        face = d[1] >= 0.0f ? 2u : 3u;
        sc = d[0];
        tc = d[1] >= 0.0f ? d[2] : -d[2];
    } else {
        ma = az;
        face = d[2] >= 0.0f ? 4u : 5u;
        sc = d[2] >= 0.0f ? d[0] : -d[0];
        tc = -d[1];
    }
    if (!(ma > 0.0f)) {
        *s = *t = 0.5f;
        return 4;
    }
    *s = (sc / ma + 1.0f) * 0.5f;
    *t = (tc / ma + 1.0f) * 0.5f;
    return face;
}

// ---- conversion -------------------------------------------------------------------------------

static size_t round32(size_t v) {
    return (v + 31u) & ~(size_t)31u;
}

static bool pow2(uint32_t v) {
    return v != 0 && (v & (v - 1u)) == 0;
}

// the layout with `skip` of the largest levels GX could show left out besides (a cube map's sphere
// halved as many times, down to 8 x 8): false when the texture has no such smaller image
static bool layout_make(const WlNv2aTexture *t, bool mipmaps, uint32_t skip, WlNv2aGxLayout *out) {
    FormatInfo info = format_info(t->color);
    uint32_t w = t->width, h = t->height, l;

    memset(out, 0, sizeof *out);
    if (!info.known || w == 0 || h == 0) {
        return false;
    }
    if (t->cube) {
        uint32_t size = 2u * w;

        if (size > WL_NV2A_SPHERE_MAX) {
            size = WL_NV2A_SPHERE_MAX;
        }
        if (size < 8u) {
            size = 8u;
        }
        for (l = 0; l < skip; l++) {
            if (size <= 8u) {
                return false;
            }
            size >>= 1;
        }
        out->sphere = true;
        out->format = WL_GXC_RGBA8;
        out->width = out->height = size;
        out->levels = 1;
        out->image_bytes = wl_gxc_tex_bytes(WL_GXC_RGBA8, size, size);
        out->total_bytes = round32(out->image_bytes);
        return true;
    }
    while ((w > WL_NV2A_GX_MAX_SIZE || h > WL_NV2A_GX_MAX_SIZE) && out->first_level + 1u < t->levels) {
        out->first_level++;
        w = at_least_1(w >> 1);
        h = at_least_1(h >> 1);
    }
    if (w > WL_NV2A_GX_MAX_SIZE || h > WL_NV2A_GX_MAX_SIZE) {
        return false;
    }
    for (l = 0; l < skip; l++) {
        // (a level the guest keeps, of 8 texels a side at least; not a linear texture's, whose
        // coordinates count its texels; past the guest's levels, the smallest halved, but for a
        // DXT's blocks and a P8's indices)
        if (t->linear || (w >> 1) < 8u || (h >> 1) < 8u) {
            return false;
        }
        if (out->first_level + 1u < t->levels && !out->shrink) {
            out->first_level++;
        } else if (info.layout != LAYOUT_DXT && info.gx != WL_GXC_C8) {
            out->shrink++;
        } else {
            return false;
        }
        w >>= 1;
        h >>= 1;
    }
    out->format = (WlGxcFormat)info.gx;
    out->width = w;
    out->height = h;
    out->levels = mipmaps && !t->linear && !out->shrink && pow2(w) && pow2(h) ? t->levels - out->first_level : 1u;
    for (l = 0; l < out->levels; l++) {
        out->image_bytes += wl_gxc_tex_bytes(out->format, at_least_1(w >> l), at_least_1(h >> l));
    }
    if (t->color == WL_NV2A_TEX_L_DXT23 || t->color == WL_NV2A_TEX_L_DXT45) {
        out->separate_alpha = true;
        for (l = 0; l < out->levels; l++) {
            out->alpha_bytes += wl_gxc_tex_bytes(WL_GXC_I4, at_least_1(w >> l), at_least_1(h >> l));
        }
    }
    out->total_bytes = round32(out->image_bytes) + round32(out->alpha_bytes);
    if (out->format == WL_GXC_C8) {
        out->tlut_format = WL_GXC_TLUT_RGB5A3;
        out->tlut_entries = t->palette_entries != 0 ? t->palette_entries : 256u;
        out->tlut_offset = out->total_bytes;
        out->total_bytes += round32(2u * out->tlut_entries);
    }
    return out->image_bytes != 0;
}

bool wl_nv2a_texture_layout(const WlNv2aTexture *t, bool mipmaps, WlNv2aGxLayout *out) {
    return layout_make(t, mipmaps, 0, out);
}

bool wl_nv2a_texture_layout_reduced(const WlNv2aTexture *t, bool mipmaps, uint32_t skip, WlNv2aGxLayout *out) {
    return layout_make(t, mipmaps, skip, out);
}

size_t wl_nv2a_texture_scratch_bytes(const WlNv2aTexture *t, const WlNv2aGxLayout *layout) {
    FormatInfo info = format_info(t->color);
    size_t texels;

    if (layout->sphere) {
        // six faces' RGBA, a face unswizzled, the sphere's RGBA
        return 6u * (size_t)t->width * t->height * 4u + (size_t)t->width * t->height * 4u +
               (size_t)layout->width * layout->height * 4u;
    }
    if (info.codec >= 0 && !layout->shrink) {
        return 0;
    }
    texels = (size_t)at_least_1(t->width >> layout->first_level) * at_least_1(t->height >> layout->first_level);
    // a level unswizzled, and as RGBA (and halved: within the RGBA)
    return texels * (info.bytes + 4u);
}

// a level decoded to RGBA (`unswizzled` scratch for a swizzled one)
static void decode_level(const WlNv2aTexture *t, uint32_t level, const uint8_t *src, const uint8_t *palette,
                         uint8_t *unswizzled, uint8_t *rgba) {
    uint32_t w = at_least_1(t->width >> level), h = at_least_1(t->height >> level), x, y;
    const uint8_t *texels = src;
    size_t pitch = t->linear ? t->pitch : (size_t)w * t->texel_bytes;

    if (t->swizzled) {
        wl_xbox_unswizzle(src, level_bytes(t, level), w, h, t->texel_bytes, unswizzled,
                          (size_t)w * h * t->texel_bytes);
        texels = unswizzled;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            if (t->signs != 0) {
                v16u16_rgba(texels + (size_t)y * pitch, x, t->signs, rgba + ((size_t)y * w + x) * 4u);
            } else {
                wl_nv2a_texel_rgba(t->color, texels + (size_t)y * pitch, x, palette,
                                   rgba + ((size_t)y * w + x) * 4u);
            }
        }
    }
}

// v's bits placed, from the lowest, at the set bits of `mask` (a coordinate's bits in a swizzled index)
static uint32_t deposit(uint32_t v, uint32_t mask) {
    uint32_t out = 0, bit;

    for (bit = 1; mask != 0 && bit != 0; bit <<= 1) {
        if (v & bit) {
            out |= mask & (~mask + 1u);
        }
        mask &= mask - 1u;
    }
    return out;
}

bool wl_nv2a_texture_average(const WlNv2aTexture *t, const uint8_t *src, const uint8_t *palette, uint8_t rgba[4]) {
    FormatInfo info = format_info(t->color);
    uint32_t level = 0, w = t->width, h = t->height, nx, ny, sx, sy, c, count = 0, mask_x = 0, mask_y = 0;
    uint32_t sum[4] = {0, 0, 0, 0};
    const uint8_t *texels;

    if (!info.known || src == NULL || w == 0 || h == 0) {
        return false;
    }
    // (the first level of 8 x 8 texels or less, else level 0)
    while ((w > 8u || h > 8u) && level + 1u < t->levels) {
        level++;
        w = at_least_1(w >> 1);
        h = at_least_1(h >> 1);
    }
    texels = src + level_offset(t, level);
    if (t->swizzled && !wl_xbox_swizzle_masks(w, h, &mask_x, &mask_y)) {
        return false;
    }
    nx = w < 8u ? w : 8u;
    ny = h < 8u ? h : 8u;
    for (sy = 0; sy < ny; sy++) {
        for (sx = 0; sx < nx; sx++) {
            uint32_t x = sx * w / nx, y = sy * h / ny;
            uint8_t texel[4];

            if (t->compressed) {
                uint8_t px[16][4];
                size_t block = t->color == WL_NV2A_TEX_L_DXT1 ? 8u : 16u;

                wl_xbox_dxt_decode_block((WlXboxFormat)info.codec,
                                         texels + ((size_t)(y / 4u) * ((w + 3u) / 4u) + x / 4u) * block, px);
                memcpy(texel, px[(y & 3u) * 4u + (x & 3u)], 4);
            } else {
                const uint8_t *row;
                uint32_t at = x;

                if (t->swizzled) {
                    row = texels + (size_t)(deposit(x, mask_x) | deposit(y, mask_y)) * t->texel_bytes;
                    at = 0;
                } else {
                    row = texels + (size_t)y * t->pitch;
                }
                if (t->signs != 0) {
                    v16u16_rgba(row, at, t->signs, texel);
                } else {
                    wl_nv2a_texel_rgba(t->color, row, at, palette, texel);
                }
            }
            for (c = 0; c < 4; c++) {
                sum[c] += texel[c];
            }
            count++;
        }
    }
    for (c = 0; c < 4; c++) {
        rgba[c] = (uint8_t)((sum[c] + count / 2u) / count);
    }
    return true;
}

// a cube map's six faces decoded to RGBA into `faces` (`unswizzled`: a face's scratch)
static void faces_decode(const WlNv2aTexture *t, const uint8_t *src, const uint8_t *palette, uint8_t *faces,
                         uint8_t *unswizzled) {
    size_t face_texels = (size_t)t->width * t->height;
    unsigned f;

    for (f = 0; f < 6; f++) {
        const uint8_t *face = src + (size_t)f * t->face_bytes;

        if (t->compressed) {
            // (a DXT face through CMPR's decoder would round its colours: decoded block by block)
            uint32_t bx, by, blocks_x = (t->width + 3u) / 4u, block = t->color == WL_NV2A_TEX_L_DXT1 ? 8u : 16u;
            WlXboxFormat dxt = t->color == WL_NV2A_TEX_L_DXT1   ? WL_XBOX_DXT1
                               : t->color == WL_NV2A_TEX_L_DXT23 ? WL_XBOX_DXT3
                                                                 : WL_XBOX_DXT5;

            for (by = 0; by < (t->height + 3u) / 4u; by++) {
                for (bx = 0; bx < blocks_x; bx++) {
                    uint8_t px[16][4];
                    unsigned p;

                    wl_xbox_dxt_decode_block(dxt, face + ((size_t)by * blocks_x + bx) * block, px);
                    for (p = 0; p < 16; p++) {
                        uint32_t x = bx * 4u + (p & 3u), y = by * 4u + (p >> 2);

                        if (x < t->width && y < t->height) {
                            memcpy(faces + ((size_t)f * face_texels + (size_t)y * t->width + x) * 4u, px[p], 4);
                        }
                    }
                }
            }
        } else {
            decode_level(t, 0, face, palette, unswizzled, faces + (size_t)f * face_texels * 4u);
        }
    }
}

// a texel's light (the largest of its colour channels)
static uint32_t texel_light(const uint8_t *rgba) {
    uint32_t v = rgba[0] > rgba[1] ? rgba[0] : rgba[1];

    return v > rgba[2] ? v : rgba[2];
}

// the spot's face (1 + its number) of decoded faces, or 0: one face holds all but 1/8 of the
// light, and its edge is dark (its texels' mean under 1/8 of its brightest)
static uint8_t spot_face(const WlNv2aTexture *t, const uint8_t *faces) {
    size_t face_texels = (size_t)t->width * t->height, n;
    uint64_t sum[6] = {0}, total = 0;
    uint64_t edge = 0, edges = 0;
    uint32_t brightest = 0, x, y;
    unsigned f, lit = 0;

    if (t->width < 8 || t->height < 8) {
        return 0;
    }
    for (f = 0; f < 6; f++) {
        for (n = 0; n < face_texels; n++) {
            sum[f] += texel_light(faces + ((size_t)f * face_texels + n) * 4u);
        }
        total += sum[f];
        if (sum[f] > sum[lit]) {
            lit = f;
        }
    }
    if (total == 0 || (total - sum[lit]) * 8u > total) {
        return 0;
    }
    for (n = 0; n < face_texels; n++) {
        uint32_t v = texel_light(faces + ((size_t)lit * face_texels + n) * 4u);

        brightest = v > brightest ? v : brightest;
    }
    for (y = 0; y < t->height; y++) {
        for (x = 0; x < t->width; x++) {
            if (x == 0 || y == 0 || x + 1 == t->width || y + 1 == t->height) {
                edge += texel_light(faces + ((size_t)lit * face_texels + (size_t)y * t->width + x) * 4u);
                edges++;
            }
        }
    }
    if (edge * 8u > (uint64_t)brightest * edges) {
        return 0;
    }
    return (uint8_t)(lit + 1u);
}

void wl_nv2a_texture_spot(const WlNv2aTexture *t, WlNv2aGxLayout *layout, const uint8_t *src, const uint8_t *palette,
                          void *scratch) {
    size_t face_texels = (size_t)t->width * t->height;
    uint8_t *faces = scratch;

    layout->spot = 0;
    if (!layout->sphere || src == NULL || scratch == NULL) {
        return;
    }
    faces_decode(t, src, palette, faces, faces + 6u * face_texels * 4u);
    layout->spot = spot_face(t, faces);
}

void wl_nv2a_spot_coordinates(const float direction[3], unsigned spot, float st[2]) {
    float x = direction[0], y = direction[1], z = direction[2], ma, sc, tc, u, v;

    // (Direct3D's faces, as cube_face reads them)
    switch (spot) {
    case 1: ma = x, sc = -z, tc = -y; break;
    case 2: ma = -x, sc = z, tc = -y; break;
    case 3: ma = y, sc = x, tc = z; break;
    case 4: ma = -y, sc = x, tc = -z; break;
    case 5: ma = z, sc = x, tc = -y; break;
    default: ma = -z, sc = -x, tc = -y; break;
    }
    if (!(ma > 1.0e-6f)) {
        // (off the face's side of the light: its dark edge)
        st[0] = st[1] = -1.0f;
        return;
    }
    u = sc / ma;
    v = tc / ma;
    u = u < -3.0f ? -3.0f : u > 3.0f ? 3.0f : u;
    v = v < -3.0f ? -3.0f : v > 3.0f ? 3.0f : v;
    st[0] = (u + 1.0f) * 0.5f;
    st[1] = (v + 1.0f) * 0.5f;
}

// the texel of decoded faces a sphere map's direction reads (into `out`, RGBA)
static inline void sphere_texel(const WlNv2aTexture *t, const uint8_t *faces, const float d[3], uint8_t *out) {
    float s, tt;
    unsigned face = cube_face(d, &s, &tt);
    uint32_t x = (uint32_t)(s * (float)t->width), y = (uint32_t)(tt * (float)t->height);

    x = x >= t->width ? t->width - 1u : x;
    y = y >= t->height ? t->height - 1u : y;
    memcpy(out, faces + (((size_t)face * t->height + y) * t->width + x) * 4u, 4);
}

// a cube map's sphere map (or its spot's face) from its decoded faces, `sphere` scratch for its RGBA
static bool sphere_from_faces(const WlNv2aTexture *t, const WlNv2aGxLayout *layout, const uint8_t *faces,
                              uint8_t *sphere, uint8_t *dst) {
    size_t face_texels = (size_t)t->width * t->height;
    uint32_t size = layout->width, i, j;

    if (layout->spot >= 1 && layout->spot <= 6) {
        // a spot's face, filling the image, its edge texels dark (what a direction off it reads)
        const uint8_t *face = faces + (size_t)(layout->spot - 1u) * face_texels * 4u;

        for (j = 0; j < size; j++) {
            for (i = 0; i < size; i++) {
                uint32_t x = (uint32_t)(((uint64_t)i * t->width) / size), y = (uint32_t)(((uint64_t)j * t->height) / size);
                uint8_t *texel = sphere + ((size_t)j * size + i) * 4u;

                memcpy(texel, face + ((size_t)y * t->width + x) * 4u, 4);
                if (i == 0 || j == 0 || i + 1 == size || j + 1 == size) {
                    texel[0] = texel[1] = texel[2] = 0;
                }
            }
        }
        return wl_gxc_encode(WL_GXC_RGBA8, sphere, size, size, dst, layout->image_bytes) != 0;
    }
    if (!g_plain && pow2(size)) {
        // A quarter's directions, the other three's by their signs: of a size a power of two, texel
        // i's a = 2 (i + 1/2) / size - 1 is exact, and texel size - 1 - i's is -a, so its direction
        // is this one's with x negated, bit for bit (the same for rows and y)
        uint32_t half = size / 2u;

        for (j = 0; j < half; j++) {
            for (i = 0; i < half; i++) {
                float d[3], m[3];

                sphere_direction(((float)i + 0.5f) / (float)size, ((float)j + 0.5f) / (float)size, d, true);
                sphere_texel(t, faces, d, sphere + ((size_t)j * size + i) * 4u);
                m[0] = -d[0], m[1] = d[1], m[2] = d[2];
                sphere_texel(t, faces, m, sphere + ((size_t)j * size + (size - 1u - i)) * 4u);
                m[1] = -d[1];
                sphere_texel(t, faces, m, sphere + ((size_t)(size - 1u - j) * size + (size - 1u - i)) * 4u);
                m[0] = d[0];
                sphere_texel(t, faces, m, sphere + ((size_t)(size - 1u - j) * size + i) * 4u);
            }
        }
        return wl_gxc_encode(WL_GXC_RGBA8, sphere, size, size, dst, layout->image_bytes) != 0;
    }
    for (j = 0; j < size; j++) {
        for (i = 0; i < size; i++) {
            float d[3];

            sphere_direction(((float)i + 0.5f) / (float)size, ((float)j + 0.5f) / (float)size, d, !g_plain);
            sphere_texel(t, faces, d, sphere + ((size_t)j * size + i) * 4u);
        }
    }
    return wl_gxc_encode(WL_GXC_RGBA8, sphere, size, size, dst, layout->image_bytes) != 0;
}

static bool convert_sphere(const WlNv2aTexture *t, const WlNv2aGxLayout *layout, const uint8_t *src,
                           const uint8_t *palette, uint8_t *dst, uint8_t *scratch) {
    size_t face_texels = (size_t)t->width * t->height;
    uint8_t *faces = scratch, *unswizzled = scratch + 6u * face_texels * 4u;

    faces_decode(t, src, palette, faces, unswizzled);
    return sphere_from_faces(t, layout, faces, unswizzled + face_texels * 4u, dst);
}

// an A4R4G4B4 level straight to GX's RGBA8 tiles, each channel's four bits doubled (as
// wl_nv2a_texel_rgba): a tile's texels are at the same offsets from its first texel in every tile
// (a swizzled index is its x part OR its y part, and a tile's corner has none of the bits below
// it; a linear level's rows are `pitch` apart), so the offsets are made once. 0 when the level is
// not whole tiles of a size swizzled or read in `size` bytes (decoded texel by texel instead).
static size_t a4r4g4b4_to_rgba8(const WlNv2aTexture *t, const uint8_t *src, size_t size, uint32_t w, uint32_t h,
                                uint8_t *out, size_t cap) {
    uint32_t mask_x = 0, mask_y = 0, offs[16], tx, ty, i;
    size_t pitch = t->linear ? t->pitch : (size_t)w * 2u;

    if ((w & 3u) != 0 || (h & 3u) != 0 || cap < (size_t)w * h * 4u ||
        (t->swizzled ? !wl_xbox_swizzle_masks(w, h, &mask_x, &mask_y) || size < (size_t)w * h * 2u
                     : pitch < (size_t)w * 2u || size < (size_t)(h - 1u) * pitch + (size_t)w * 2u)) {
        return 0;
    }
    for (i = 0; i < 16; i++) {
        offs[i] = t->swizzled ? (deposit(i & 3u, mask_x) | deposit(i >> 2, mask_y)) * 2u
                              : (uint32_t)((i >> 2) * pitch + (i & 3u) * 2u);
    }
    for (ty = 0; ty < h; ty += 4) {
        size_t base_y = t->swizzled ? (size_t)deposit(ty, mask_y) * 2u : (size_t)ty * pitch;

        for (tx = 0; tx < w; tx += 4, out += 64) {
            const uint8_t *in = src + base_y + (t->swizzled ? (size_t)deposit(tx, mask_x) * 2u : (size_t)tx * 2u);

            for (i = 0; i < 16; i++) {
                uint32_t ar = in[offs[i] + 1u], gb = in[offs[i]];

                // (each nibble doubled: 0xAR to 0xAARR)
                ar = (ar & 0xF0u) << 8 | (ar & 0xF0u) << 4 | (ar & 0x0Fu) << 4 | (ar & 0x0Fu);
                gb = (gb & 0xF0u) << 8 | (gb & 0xF0u) << 4 | (gb & 0x0Fu) << 4 | (gb & 0x0Fu);
                out[i * 2u] = (uint8_t)(ar >> 8);
                out[i * 2u + 1u] = (uint8_t)ar;
                out[32u + i * 2u] = (uint8_t)(gb >> 8);
                out[32u + i * 2u + 1u] = (uint8_t)gb;
            }
        }
    }
    return (size_t)w * h * 4u;
}

void wl_nv2a_texture_set_plain(bool plain) {
    g_plain = plain;
}

// The bytes of an image no conversion writes zeroed: the image's and its alpha's pads to 32 bytes,
// and a TLUT's past the palette's entries (all of it without a palette). Every level's conversion
// writes each byte of its GX size (wl/codec's, the tile loops here, wl_gxc_encode).
static void zero_pads(const WlNv2aGxLayout *layout, uint8_t *dst, bool palette) {
    size_t image_end = round32(layout->image_bytes);

    memset(dst + layout->image_bytes, 0, image_end - layout->image_bytes);
    if (layout->alpha_bytes != 0) {
        memset(dst + image_end + layout->alpha_bytes, 0, round32(layout->alpha_bytes) - layout->alpha_bytes);
    }
    if (layout->format == WL_GXC_C8) {
        size_t from = palette ? 2u * (size_t)layout->tlut_entries : 0u;

        memset(dst + layout->tlut_offset + from, 0, round32(2u * (size_t)layout->tlut_entries) - from);
    }
}

bool wl_nv2a_texture_convert(const WlNv2aTexture *t, const WlNv2aGxLayout *layout, const uint8_t *src,
                             const uint8_t *palette, void *dst, void *scratch) {
    FormatInfo info = format_info(t->color);
    uint8_t *out = dst, *alpha = (uint8_t *)dst + round32(layout->image_bytes);
    size_t done = 0, alpha_done = 0;
    uint32_t l;

    if (src == NULL || dst == NULL || !info.known) {
        return false;
    }
    if (g_plain) {
        memset(dst, 0, layout->total_bytes);
    } else {
        zero_pads(layout, dst, palette != NULL);
    }
    if (layout->sphere) {
        return scratch != NULL && convert_sphere(t, layout, src, palette, out, scratch);
    }
    if (layout->shrink) {
        // (the smallest level halved, a box of 2^shrink texels a side averaged into each)
        uint32_t level = layout->first_level, w = at_least_1(t->width >> level), h = at_least_1(t->height >> level);
        uint32_t box = 1u << layout->shrink, x, y, i, j, c;
        uint8_t *unswizzled = scratch, *rgba;

        if (scratch == NULL || w / box != layout->width || h / box != layout->height) {
            return false;
        }
        rgba = unswizzled + (size_t)w * h * t->texel_bytes;
        decode_level(t, level, src + level_offset(t, level), palette, unswizzled, rgba);
        for (y = 0; y < layout->height; y++) {
            for (x = 0; x < layout->width; x++) {
                for (c = 0; c < 4; c++) {
                    uint32_t sum = 0;

                    for (j = 0; j < box; j++) {
                        for (i = 0; i < box; i++) {
                            sum += rgba[(((size_t)y * box + j) * w + (size_t)x * box + i) * 4u + c];
                        }
                    }
                    // (in place: each texel written after the boxes before it were read)
                    rgba[((size_t)y * layout->width + x) * 4u + c] = (uint8_t)((sum + box * box / 2u) / (box * box));
                }
            }
        }
        return wl_gxc_encode(layout->format, rgba, layout->width, layout->height, out, layout->image_bytes) != 0;
    }
    for (l = 0; l < layout->levels; l++) {
        uint32_t level = layout->first_level + l;
        uint32_t w = at_least_1(t->width >> level), h = at_least_1(t->height >> level);
        const uint8_t *level_src = src + level_offset(t, level);
        size_t level_size = t->linear ? t->total_bytes : level_bytes(t, level);
        size_t cap = wl_gxc_tex_bytes(layout->format, w, h), made;

        if (info.layout == LAYOUT_DXT) {
            made = wl_xbox_dxt_to_cmpr((WlXboxFormat)info.codec, level_src, level_size, w, h, out + done, cap);
            if (layout->separate_alpha) {
                size_t alpha_cap = wl_gxc_tex_bytes(WL_GXC_I4, w, h);

                if (wl_xbox_dxt_alpha_i4((WlXboxFormat)info.codec, level_src, level_size, w, h, alpha + alpha_done,
                                         alpha_cap) == 0) {
                    return false;
                }
                alpha_done += alpha_cap;
            }
        } else if (info.codec >= 0) {
            made = wl_xbox_to_gx((WlXboxFormat)info.codec, t->swizzled, t->linear ? t->pitch : 0, level_src,
                                 level_size, w, h, out + done, cap);
        } else {
            uint8_t *unswizzled = scratch, *rgba;

            made = (t->color == WL_NV2A_TEX_SZ_A4R4G4B4 || t->color == WL_NV2A_TEX_LU_A4R4G4B4) &&
                           layout->format == WL_GXC_RGBA8
                       ? a4r4g4b4_to_rgba8(t, level_src, level_size, w, h, out + done, cap)
                       : 0;
            if (made == 0) {
                if (scratch == NULL) {
                    return false;
                }
                rgba = unswizzled + (size_t)w * h * t->texel_bytes;
                decode_level(t, level, level_src, palette, unswizzled, rgba);
                made = wl_gxc_encode(layout->format, rgba, w, h, out + done, cap);
            }
        }
        if (made == 0) {
            return false;
        }
        done += cap;
    }
    if (layout->format == WL_GXC_C8) {
        if (palette != NULL) {
            wl_xbox_palette_to_tlut(layout->tlut_format, palette, layout->tlut_entries,
                                    (uint8_t *)dst + layout->tlut_offset);
        }
    }
    return true;
}

bool wl_nv2a_texture_convert_cube(const WlNv2aTexture *t, WlNv2aGxLayout *layout, bool spots, const uint8_t *src,
                                  const uint8_t *palette, void *dst, void *scratch) {
    size_t face_texels = (size_t)t->width * t->height;
    uint8_t *faces = scratch, *unswizzled;

    if (!layout->sphere || g_plain) {
        // (as before: the faces decoded for the spot's test, and again for the image)
        if (spots) {
            wl_nv2a_texture_spot(t, layout, src, palette, scratch);
        } else {
            layout->spot = 0;
        }
        return wl_nv2a_texture_convert(t, layout, src, palette, dst, scratch);
    }
    layout->spot = 0;
    if (src == NULL || dst == NULL || scratch == NULL || !format_info(t->color).known) {
        return false;
    }
    unswizzled = faces + 6u * face_texels * 4u;
    faces_decode(t, src, palette, faces, unswizzled);
    if (spots) {
        layout->spot = spot_face(t, faces);
    }
    zero_pads(layout, dst, palette != NULL);
    return sphere_from_faces(t, layout, faces, unswizzled + face_texels * 4u, dst);
}
