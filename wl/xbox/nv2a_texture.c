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

enum { LAYOUT_SZ, LAYOUT_LU, LAYOUT_DXT };

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
    out->min_filter = (uint8_t)((filter >> WL_NV2A_TEXFILTER_MIN_SHIFT) & 0xFFu);
    out->mag_filter = (uint8_t)((filter >> WL_NV2A_TEXFILTER_MAG_SHIFT) & 0xFu);
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

void wl_nv2a_sphere_coordinates(const float direction[3], float st[2]) {
    float x = direction[0], y = direction[1], z = direction[2];
    float length = sqrtf(x * x + y * y + z * z), m;

    if (!(length > 1.0e-12f)) {
        st[0] = st[1] = 0.5f;
        return;
    }
    x /= length;
    y /= length;
    z /= length;
    m = 2.0f * sqrtf(x * x + y * y + (z + 1.0f) * (z + 1.0f));
    if (!(m > 1.0e-6f)) {
        // straight behind: the disc's edge
        st[0] = 0.5f;
        st[1] = 1.0f;
        return;
    }
    st[0] = x / m + 0.5f;
    st[1] = y / m + 0.5f;
}

void wl_nv2a_sphere_direction(float s, float t, float direction[3]) {
    float a = 2.0f * s - 1.0f, b = 2.0f * t - 1.0f, r2 = a * a + b * b, c;

    if (r2 > 1.0f) {
        float r = sqrtf(r2);
        a /= r;
        b /= r;
        r2 = 1.0f;
    }
    c = sqrtf(1.0f - r2);
    // the reflection of the view direction about the half vector (a, b, c)
    direction[0] = 2.0f * c * a;
    direction[1] = 2.0f * c * b;
    direction[2] = 2.0f * c * c - 1.0f;
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

bool wl_nv2a_texture_layout(const WlNv2aTexture *t, bool mipmaps, WlNv2aGxLayout *out) {
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
    out->format = (WlGxcFormat)info.gx;
    out->width = w;
    out->height = h;
    out->levels = mipmaps && !t->linear && pow2(w) && pow2(h) ? t->levels - out->first_level : 1u;
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

size_t wl_nv2a_texture_scratch_bytes(const WlNv2aTexture *t, const WlNv2aGxLayout *layout) {
    FormatInfo info = format_info(t->color);
    size_t texels;

    if (layout->sphere) {
        // six faces' RGBA, a face unswizzled, the sphere's RGBA
        return 6u * (size_t)t->width * t->height * 4u + (size_t)t->width * t->height * 4u +
               (size_t)layout->width * layout->height * 4u;
    }
    if (info.codec >= 0) {
        return 0;
    }
    texels = (size_t)at_least_1(t->width >> layout->first_level) * at_least_1(t->height >> layout->first_level);
    // a level unswizzled, and as RGBA
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
            wl_nv2a_texel_rgba(t->color, texels + (size_t)y * pitch, x, palette, rgba + ((size_t)y * w + x) * 4u);
        }
    }
}

static bool convert_sphere(const WlNv2aTexture *t, const WlNv2aGxLayout *layout, const uint8_t *src,
                           const uint8_t *palette, uint8_t *dst, uint8_t *scratch) {
    size_t face_texels = (size_t)t->width * t->height;
    uint8_t *faces = scratch, *unswizzled = scratch + 6u * face_texels * 4u, *sphere = unswizzled + face_texels * 4u;
    uint32_t size = layout->width, i, j;
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
    for (j = 0; j < size; j++) {
        for (i = 0; i < size; i++) {
            float d[3], s, tt;
            uint32_t x, y;
            unsigned face;

            wl_nv2a_sphere_direction(((float)i + 0.5f) / (float)size, ((float)j + 0.5f) / (float)size, d);
            face = cube_face(d, &s, &tt);
            x = (uint32_t)(s * (float)t->width);
            y = (uint32_t)(tt * (float)t->height);
            x = x >= t->width ? t->width - 1u : x;
            y = y >= t->height ? t->height - 1u : y;
            memcpy(sphere + ((size_t)j * size + i) * 4u, faces + ((size_t)face * face_texels + (size_t)y * t->width + x) * 4u,
                   4);
        }
    }
    return wl_gxc_encode(WL_GXC_RGBA8, sphere, size, size, dst, layout->image_bytes) != 0;
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
    memset(dst, 0, layout->total_bytes);
    if (layout->sphere) {
        return scratch != NULL && convert_sphere(t, layout, src, palette, out, scratch);
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

            if (scratch == NULL) {
                return false;
            }
            rgba = unswizzled + (size_t)w * h * t->texel_bytes;
            decode_level(t, level, level_src, palette, unswizzled, rgba);
            made = wl_gxc_encode(layout->format, rgba, w, h, out + done, cap);
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
