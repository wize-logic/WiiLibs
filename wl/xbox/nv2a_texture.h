// SPDX-License-Identifier: MIT
// The NV2A's textures (nv2a_texture.c): a texture stage's registers decoded (where the texels are,
// their format, size, layout, levels and faces, the sampler), and a texture converted into GX's
// formats (wl/codec's Xbox conversions where they are exact, the rest decoded to RGBA8 and encoded
// again), a cube map made into the sphere map wl/xbox/psh samples it as. The host-side cache of
// converted textures, keyed by (address, format, size) and checked by a hash of the texels, is
// nv2a_texcache.h.
#pragma once

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wl/codec/gx_tex.h"
#include "wl/codec/xbox_tex.h"
#include "wl/gfx/gfx_sqrt.h"
#include "wl/xbox/nv2a_kelvin.h"

enum {
    // GX's largest texture side
    WL_NV2A_GX_MAX_SIZE = 1024,
    // a cube map's sphere map, at most this many texels a side
    WL_NV2A_SPHERE_MAX = 256
};

typedef struct WlNv2aTexture {
    uint32_t phys;          // level 0's guest-physical address (face 0's for a cube map)
    uint32_t format_word;   // SET_TEXTURE_FORMAT
    uint32_t rect_word;     // SET_TEXTURE_IMAGE_RECT (linear textures)
    uint32_t palette_phys;  // a P8 texture's palette (D3DCOLOR entries)
    uint32_t palette_entries;
    uint8_t color;          // WL_NV2A_TEX_* (the format word's bits 15:8)
    uint8_t swizzled, compressed, linear, cube, volume;
    uint8_t known;          // a format the conversion knows
    uint32_t width, height, depth;
    uint32_t levels;        // in memory (1 for a linear texture)
    uint32_t pitch;         // a linear texture's bytes between rows
    uint32_t texel_bytes;   // 0 for the DXT formats
    uint32_t face_bytes;    // all levels of a face (a cube map's faces this far apart)
    uint32_t total_bytes;   // what the texture takes in guest memory
    // what the stage's sampler says
    uint8_t enabled;        // CONTROL0's enable bit
    uint8_t wrap_u, wrap_v; // WL_NV2A_WRAP_*
    uint32_t border_color;  // SET_TEXTURE_BORDER_COLOR (ARGB), read outside by WL_NV2A_WRAP_BORDER
    uint8_t min_filter, mag_filter; // WL_NV2A_FILTER_*
    // the channels the filter register reads as signed (its bits 31:28: A, R, G, B), kept for a
    // format whose texels change with it (V16U16), else 0
    uint8_t signs;
    // what the texels hold, as wl/xbox/psh takes it (WL_XBOX_PSH_TEXTURE_*)
    uint8_t contents;
} WlNv2aTexture;

// Stage `stage`'s texture (0..3). false when the stage has no texture to sample: its offset or
// format unset, a size of 0.
bool wl_nv2a_texture(const WlNv2aKelvin *k, unsigned stage, WlNv2aTexture *out);
// the stage's texture shader program (WL_NV2A_STAGE_*)
static inline unsigned wl_nv2a_stage_program(const WlNv2aKelvin *k, unsigned stage) {
    return (wl_nv2a_k(k, WL_NV2A_K_SET_SHADER_STAGE_PROGRAM) >> (5u * stage)) & 0x1Fu;
}

// ---- conversion to GX -----------------------------------------------------------------------
typedef struct WlNv2aGxLayout {
    WlGxcFormat format;     // of the image (C8: with a TLUT)
    uint32_t width, height; // of its first level
    uint32_t levels;        // GX levels, one after the other in tile order
    uint32_t first_level;   // the guest level the first GX level is (levels larger than GX's
                            // largest are left out)
    size_t image_bytes;     // all levels
    // DXT3 and DXT5: their alpha as an I4 image after the colour's, image_bytes on
    bool separate_alpha;
    size_t alpha_bytes;
    // a P8 texture's TLUT: tlut_entries big-endian entries of tlut_format after the images
    WlGxcTlutFormat tlut_format;
    uint32_t tlut_entries;
    size_t tlut_offset;
    // the guest's first_level image halved this many times by a box filter, one level (a texture
    // without smaller levels of its own kept smaller for want of memory)
    uint8_t shrink;
    // a cube map's sphere map (RGBA8, one level)
    bool sphere;
    // a light's falloff cube (one face lit, the others and its edges dark): that face (1 + Direct3D's
    // face number: +x, -x, +y, -y, +z, -z) in place of the sphere map, read through
    // wl_nv2a_spot_coordinates; 0 a sphere map (wl_nv2a_texture_spot sets it)
    uint8_t spot;
    size_t total_bytes;     // all of it (32-byte multiples between the parts)
} WlNv2aGxLayout;

// What the texture becomes on GX: false when it cannot (a format not known, a size GX cannot
// show). `mipmaps`: keep the levels (the sampler uses them).
bool wl_nv2a_texture_layout(const WlNv2aTexture *t, bool mipmaps, WlNv2aGxLayout *out);
// The same texture smaller, for want of memory: `skip` more of its largest levels left out (the
// guest's own smaller levels, first_level on; past them, its smallest kept halved by a box filter
// (shrink), but for DXT and P8; none under 8 texels a side; not a linear texture), a cube map's
// sphere halved `skip` times (down to 8 x 8). Coordinates are unchanged (normalized).
// false when the texture has no image that small.
bool wl_nv2a_texture_layout_reduced(const WlNv2aTexture *t, bool mipmaps, uint32_t skip, WlNv2aGxLayout *out);
// Converts the texture (its guest bytes `src`, total_bytes of them; `palette` a P8's entries, or
// NULL) into `dst` (layout->total_bytes, 32-byte aligned). `scratch` takes
// wl_nv2a_texture_scratch_bytes of the texture for the decoding. false on bad input.
bool wl_nv2a_texture_convert(const WlNv2aTexture *t, const WlNv2aGxLayout *layout, const uint8_t *src,
                             const uint8_t *palette, void *dst, void *scratch);
size_t wl_nv2a_texture_scratch_bytes(const WlNv2aTexture *t, const WlNv2aGxLayout *layout);

// One texel of a linear (unswizzled) level as RGBA8: the decoder of the formats without an exact GX
// equivalent, and the reference for the others. `row` is the level's row y.
void wl_nv2a_texel_rgba(uint8_t color, const uint8_t *row, uint32_t x, const uint8_t *palette, uint8_t rgba[4]);

// The texture's average colour, from up to 64 of its texels (a level of 8 x 8 or less, else a grid
// over level 0; a cube map's first face): what is bound in its place while it cannot be (no memory,
// a format not known). false when its texels cannot be read so (an unknown format).
bool wl_nv2a_texture_average(const WlNv2aTexture *t, const uint8_t *src, const uint8_t *palette, uint8_t rgba[4]);

// Whether a cube map is a light's falloff (a spot: one face carries the light, the other faces and
// that face's edge texels are dark), from its texels (`scratch`: wl_nv2a_texture_scratch_bytes of its
// sphere layout): sets layout->spot to 1 + the face, else 0. A spot's sphere map, sampled at
// coordinates made per vertex, spreads a narrow cone over whole triangles; its face read through the
// direction's projection onto it keeps the cone's shape.
void wl_nv2a_texture_spot(const WlNv2aTexture *t, WlNv2aGxLayout *layout, const uint8_t *src, const uint8_t *palette,
                          void *scratch);
// A direction's place on a spot's face (layout->spot): its projection onto the face's plane, 0..1
// across the face, and beyond it (to the face's dark edge) for a direction off it or behind.
void wl_nv2a_spot_coordinates(const float direction[3], unsigned spot, float st[2]);

// A direction's place on the sphere map a cube map becomes (the mirror ball's mapping: x / m + 1/2,
// y / m + 1/2 with m = 2 |d + (0, 0, 1)|, d normalized), and its inverse. The coordinates are a
// float's worth (within 2^-22 of the exact ones, the disc's centre for a direction too short or too
// long and its edge straight behind), made as the Wii's fused batches make them
// (wl_nv2a_sphere_coordinates_inline); _double is the double-precision way before it (three Newton's
// steps for each of two roots, its L + z cancelling nearly straight behind), kept for an A/B.
void wl_nv2a_sphere_coordinates(const float direction[3], float st[2]);
void wl_nv2a_sphere_coordinates_double(const float direction[3], float st[2]);
void wl_nv2a_sphere_direction(float s, float t, float direction[3]);

// (a * b + c in one rounding where the processor has it: PowerPC's fmadds)
static inline float wl_nv2a_madd(float a, float b, float c) {
#if defined(__PPC__) || defined(__powerpc__)
    return __builtin_fmaf(a, b, c);
#else
    return a * b + c;
#endif
}

// 1 / sqrt(x) for x > 0 (half_x = x / 2): the estimate and two Newton's steps in single precision, as
// tools/xbox_vsh_pairs.c's pr_fused_root
static inline float wl_nv2a_sphere_root(float x, float half_x) {
    float y = (float)wl_rsqrt_estimate(x);

    y = y * wl_nv2a_madd(-half_x, y * y, 1.5f);
    y = y * wl_nv2a_madd(-half_x, y * y, 1.5f);
    return y;
}

// With n = d / |d|: x k + 1/2, y k + 1/2 for k = 1 / (2 sqrt(2 (1 + z))). Toward the back (z < 0), 1 +
// z is r / (1 - z), r = x x + y y, which cancels nothing: k is a / 2 / sqrt(2 r a) there, a = 1 + |z|,
// and 1 / 2 / sqrt(2 a) in front. Single precision, two roots of two steps each, no division: the
// fused batches' arithmetic (the same bits where the compiler keeps the same roundings), in place of
// the double precision's three steps a root (4% of a10's time on the Wii)
static inline void wl_nv2a_sphere_coordinates_inline(const float direction[3], float st[2]) {
    float x = direction[0], y = direction[1], z = direction[2];
    float across = x * x + y * y, length2 = wl_nv2a_madd(z, z, across), root, a, s, u, k, nx, ny, nz;

    if (!(length2 > 1.0e-24f && length2 <= 3.4028235e38f)) {
        st[0] = st[1] = 0.5f;
        return;
    }
    root = wl_nv2a_sphere_root(length2, length2 * 0.5f);
    nx = x * root;
    ny = y * root;
    nz = z * root;
    a = 1.0f + fabsf(nz);
    s = nz >= 0.0f ? 1.0f : across * (root * root);
    if (!(s > 2.5e-13f)) {
        // (straight behind: the disc's edge)
        st[0] = 0.5f;
        st[1] = 1.0f;
        return;
    }
    u = a * s;
    k = wl_nv2a_sphere_root(u + u, u) * (nz >= 0.0f ? 0.5f : a * 0.5f);
    st[0] = wl_nv2a_madd(nx, k, 0.5f);
    st[1] = wl_nv2a_madd(ny, k, 0.5f);
}
