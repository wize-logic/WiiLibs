// SPDX-License-Identifier: MIT
// wl/xbox/nv2a_texture and nv2a_texcache: a texture stage's registers decoded (swizzled, linear,
// DXT, cube and P8 textures: sizes, levels, faces, palettes), textures converted into GX's formats
// and decoded back to what the NV2A would sample (the codec's exact conversions and the texel
// decoders of the rest), mip levels, a cube map's sphere map, and the cache: kept, found changed,
// evicted within its budget; checked through a write watch (no texels read while its pages see no
// store), its images in an arena of its own (room made by evicting, a texture without room not
// tried again in the frame, one with smaller levels kept from them and made whole again later); a
// capture begun inside a frame reads the textures checked before it again.
#include <math.h>
#include <string.h>

#include "greatest.h"
#include "nv2a_test_util.h"
#include "wl/xbox/nv2a_capture.h"
#include "wl/xbox/nv2a_texcache.h"
#include "wl/xbox/nv2a_texture.h"

static uint32_t tex_format(uint32_t color, uint32_t log_u, uint32_t log_v, uint32_t levels, bool cube) {
    return 1u | (cube ? WL_NV2A_TEXFMT_CUBEMAP : 0) | (2u << WL_NV2A_TEXFMT_DIMENSIONS_SHIFT) |
           color << WL_NV2A_TEXFMT_COLOR_SHIFT | levels << WL_NV2A_TEXFMT_LEVELS_SHIFT |
           log_u << WL_NV2A_TEXFMT_SIZE_U_SHIFT | log_v << WL_NV2A_TEXFMT_SIZE_V_SHIFT;
}

static void set_texture(TGuest *g, unsigned stage, uint32_t offset, uint32_t format) {
    t_m1(g, WL_NV2A_K_SET_TEXTURE_OFFSET + 64 * stage, offset);
    t_m1(g, WL_NV2A_K_SET_TEXTURE_FORMAT + 64 * stage, format);
    t_m1(g, WL_NV2A_K_SET_TEXTURE_CONTROL0 + 64 * stage, 1u << 30);
}

TEST stage_registers_decoded(void) {
    TGuest g;
    WlNv2aTexture t;
    const WlNv2aKelvin *k;
    ASSERT(t_create(&g, NULL) != NULL);
    t_setup_3d(&g);
    k = wl_nv2a_kelvin(g.dev);
    // a swizzled A8R8G8B8 64x32 of three levels
    set_texture(&g, 0, 0x100000, tex_format(WL_NV2A_TEX_SZ_A8R8G8B8, 6, 5, 3, false));
    t_m1(&g, WL_NV2A_K_SET_TEXTURE_ADDRESS, WL_NV2A_WRAP_MIRROR | WL_NV2A_WRAP_CLAMP_TO_EDGE << 8);
    t_m1(&g, WL_NV2A_K_SET_TEXTURE_FILTER,
         WL_NV2A_FILTER_LINEAR_MIP_NEAREST << WL_NV2A_TEXFILTER_MIN_SHIFT | WL_NV2A_FILTER_NEAREST << WL_NV2A_TEXFILTER_MAG_SHIFT);
    // a linear X8R8G8B8 100x50, pitch 512
    set_texture(&g, 1, 0x180000, 1u | WL_NV2A_TEX_LU_X8R8G8B8 << 8 | 2u << 4);
    t_m1(&g, WL_NV2A_K_SET_TEXTURE_IMAGE_RECT + 64, 100u << 16 | 50u);
    t_m1(&g, WL_NV2A_K_SET_TEXTURE_CONTROL1 + 64, 512u << 16);
    // a DXT1 cube 16x16 of two levels (DMA B), and a P8 with a palette of 64 entries
    set_texture(&g, 2, 0x1C0000, (tex_format(WL_NV2A_TEX_L_DXT1, 4, 4, 2, true) & ~3u) | 2u);
    set_texture(&g, 3, 0x1E0000, tex_format(WL_NV2A_TEX_SZ_I8_A8R8G8B8, 3, 3, 1, false));
    t_m1(&g, WL_NV2A_K_SET_TEXTURE_PALETTE + 192, 0x1F0000u | 2u << 2);
    t_kick(&g);

    ASSERT(wl_nv2a_texture(k, 0, &t));
    ASSERT_EQ(0x100000u, t.phys);
    ASSERT_EQ(64u, t.width);
    ASSERT_EQ(32u, t.height);
    ASSERT_EQ(3u, t.levels);
    ASSERT_EQ(1u, (unsigned)t.swizzled);
    ASSERT_EQ(4u, t.texel_bytes);
    ASSERT_EQ((64u * 32u + 32u * 16u + 16u * 8u) * 4u, t.total_bytes);
    ASSERT_EQ((unsigned)WL_NV2A_WRAP_MIRROR, (unsigned)t.wrap_u);
    ASSERT_EQ((unsigned)WL_NV2A_WRAP_CLAMP_TO_EDGE, (unsigned)t.wrap_v);
    ASSERT_EQ((unsigned)WL_NV2A_FILTER_LINEAR_MIP_NEAREST, (unsigned)t.min_filter);
    ASSERT_EQ(1u, (unsigned)t.enabled);

    ASSERT(wl_nv2a_texture(k, 1, &t));
    ASSERT_EQ(1u, (unsigned)t.linear);
    ASSERT_EQ(100u, t.width);
    ASSERT_EQ(50u, t.height);
    ASSERT_EQ(512u, t.pitch);
    ASSERT_EQ(512u * 50u, t.total_bytes);

    ASSERT(wl_nv2a_texture(k, 2, &t));
    ASSERT_EQ(1u, (unsigned)t.compressed);
    ASSERT_EQ(1u, (unsigned)t.cube);
    // a face: 16x16 (16 blocks) and 8x8 (4 blocks) of 8 bytes, 160 bytes, aligned to 256
    ASSERT_EQ(256u, t.face_bytes);
    ASSERT_EQ(6u * 256u, t.total_bytes);

    ASSERT(wl_nv2a_texture(k, 3, &t));
    ASSERT_EQ(0x1F0000u, t.palette_phys);
    ASSERT_EQ(64u, t.palette_entries);
    t_destroy(&g);
    PASS();
}

// a texture's GX image decoded back to RGBA, against what wl_nv2a_texel_rgba says of the guest's
// texels (exactly, or within `slack` a channel)
static int image_matches(const WlNv2aTexture *t, const WlNv2aGxLayout *layout, const uint8_t *gx, const uint8_t *linear,
                         uint32_t pitch, int slack) {
    static uint8_t rgba[64 * 64 * 4];
    uint32_t x, y;

    if (wl_gxc_decode(layout->format, gx, layout->image_bytes, t->width, t->height, rgba) == 0) {
        return 0;
    }
    for (y = 0; y < t->height; y++) {
        for (x = 0; x < t->width; x++) {
            uint8_t want[4];
            unsigned c;

            wl_nv2a_texel_rgba(t->color, linear + y * pitch, x, NULL, want);
            for (c = 0; c < 4; c++) {
                // (an intensity texture's other channels mean what psh is told: not compared)
                if ((t->contents == WL_XBOX_PSH_TEXTURE_COLOR && c == 3) ||
                    (t->contents == WL_XBOX_PSH_TEXTURE_ALPHA && c != 3)) {
                    continue;
                }
                int d = (int)rgba[(y * t->width + x) * 4 + c] - (int)want[c];

                if (d > slack || d < -slack) {
                    return 0;
                }
            }
        }
    }
    return 1;
}

TEST conversions_sample_as_the_nv2a_would(void) {
    static const struct {
        uint8_t color;
        uint32_t bytes;
        WlGxcFormat gx;
        int slack;
    } cases[] = {
        {WL_NV2A_TEX_LU_A8R8G8B8, 4, WL_GXC_RGBA8, 0}, {WL_NV2A_TEX_LU_X8R8G8B8, 4, WL_GXC_RGBA8, 0},
        {WL_NV2A_TEX_LU_A4R4G4B4, 2, WL_GXC_RGBA8, 0}, {WL_NV2A_TEX_LU_A1R5G5B5, 2, WL_GXC_RGB5A3, 9},
        {WL_NV2A_TEX_LU_R5G6B5, 2, WL_GXC_RGB565, 0},  {WL_NV2A_TEX_LU_X1R5G5B5, 2, WL_GXC_RGB565, 4},
        {WL_NV2A_TEX_LU_A8B8G8R8, 4, WL_GXC_RGBA8, 0}, {WL_NV2A_TEX_LU_B8G8R8A8, 4, WL_GXC_RGBA8, 0},
        {WL_NV2A_TEX_LU_R8G8B8A8, 4, WL_GXC_RGBA8, 0}, {WL_NV2A_TEX_LU_G8B8, 2, WL_GXC_RGBA8, 0},
        {WL_NV2A_TEX_LC_CR8YB8CB8YA8, 2, WL_GXC_RGBA8, 0},
    };
    static uint8_t src[16 * 8 * 4];
    static uint8_t dst[16384] __attribute__((aligned(32)));
    static uint8_t scratch[65536];
    unsigned i, b;

    for (b = 0; b < sizeof src; b++) {
        src[b] = (uint8_t)(b * 37u + 11u);
    }
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        WlNv2aTexture t;
        WlNv2aGxLayout layout;

        memset(&t, 0, sizeof t);
        t.color = cases[i].color;
        t.known = 1;
        t.linear = 1;
        t.width = 16;
        t.height = 8;
        t.depth = 1;
        t.levels = 1;
        t.texel_bytes = cases[i].bytes;
        t.pitch = 16 * cases[i].bytes;
        t.total_bytes = t.face_bytes = t.pitch * 8;
        ASSERT(wl_nv2a_texture_layout(&t, false, &layout));
        ASSERT_EQ(cases[i].gx, layout.format);
        ASSERT(wl_nv2a_texture_scratch_bytes(&t, &layout) <= sizeof scratch);
        ASSERT(wl_nv2a_texture_convert(&t, &layout, src, NULL, dst, scratch));
        ASSERT(image_matches(&t, &layout, dst, src, t.pitch, cases[i].slack));
    }
    PASS();
}

TEST swizzled_levels_and_luminance(void) {
    static uint8_t src[8 * 8 + 4 * 4 + 2 * 2 + 1];
    static uint8_t dst[4096] __attribute__((aligned(32)));
    static uint8_t linear[64];
    WlNv2aTexture t;
    WlNv2aGxLayout layout;
    unsigned b;

    for (b = 0; b < sizeof src; b++) {
        src[b] = (uint8_t)(b * 13u);
    }
    memset(&t, 0, sizeof t);
    t.color = WL_NV2A_TEX_SZ_Y8;
    t.known = 1;
    t.swizzled = 1;
    t.width = t.height = 8;
    t.depth = 1;
    t.levels = 4;
    t.texel_bytes = 1;
    t.contents = WL_XBOX_PSH_TEXTURE_COLOR;
    t.total_bytes = t.face_bytes = sizeof src;
    ASSERT(wl_nv2a_texture_layout(&t, true, &layout));
    ASSERT_EQ(WL_GXC_I8, layout.format);
    ASSERT_EQ(4u, layout.levels);
    ASSERT_EQ(wl_gxc_tex_bytes(WL_GXC_I8, 8, 8) + wl_gxc_tex_bytes(WL_GXC_I8, 4, 4) + wl_gxc_tex_bytes(WL_GXC_I8, 2, 2) +
                  wl_gxc_tex_bytes(WL_GXC_I8, 1, 1),
              layout.image_bytes);
    ASSERT(wl_nv2a_texture_convert(&t, &layout, src, NULL, dst, NULL));
    // level 0, unswizzled, is what the image shows
    ASSERT(wl_xbox_unswizzle(src, 64, 8, 8, 1, linear, sizeof linear) == 64);
    ASSERT(image_matches(&t, &layout, dst, linear, 8, 0));
    // without mipmaps: one level
    ASSERT(wl_nv2a_texture_layout(&t, false, &layout));
    ASSERT_EQ(1u, layout.levels);
    PASS();
}

TEST dxt_and_palettes(void) {
    static uint8_t src[16 * 16];
    static uint8_t palette[64 * 4];
    static uint8_t dst[8192] __attribute__((aligned(32)));
    WlNv2aTexture t;
    WlNv2aGxLayout layout;
    unsigned b;

    for (b = 0; b < sizeof src; b++) {
        src[b] = (uint8_t)(b * 7u + 3u);
    }
    // DXT3 16x16: CMPR and an I4 alpha after it
    memset(&t, 0, sizeof t);
    t.color = WL_NV2A_TEX_L_DXT23;
    t.known = 1;
    t.compressed = 1;
    t.width = t.height = 16;
    t.depth = 1;
    t.levels = 1;
    t.total_bytes = t.face_bytes = 16 * 16;
    ASSERT(wl_nv2a_texture_layout(&t, false, &layout));
    ASSERT_EQ(WL_GXC_CMPR, layout.format);
    ASSERT(layout.separate_alpha);
    ASSERT_EQ(wl_gxc_tex_bytes(WL_GXC_I4, 16, 16), layout.alpha_bytes);
    ASSERT(wl_nv2a_texture_convert(&t, &layout, src, NULL, dst, NULL));
    {
        // the alpha of texel (5, 0): DXT3's nibble, in I4
        uint8_t rgba[4], alpha[16];
        ASSERT(wl_gxc_texel_read(WL_GXC_I4, dst + ((layout.image_bytes + 31) & ~31u), layout.alpha_bytes, 16, 16, 5, 0,
                                 rgba));
        wl_xbox_dxt_alpha_block(WL_XBOX_DXT3, src + 16, alpha);
        ASSERT_EQ(alpha[1] >> 4, rgba[0] >> 4);
    }
    // P8 8x8 with 64 entries: C8 and an RGB5A3 TLUT
    memset(&t, 0, sizeof t);
    t.color = WL_NV2A_TEX_SZ_I8_A8R8G8B8;
    t.known = 1;
    t.swizzled = 1;
    t.width = t.height = 8;
    t.depth = 1;
    t.levels = 1;
    t.texel_bytes = 1;
    t.palette_entries = 64;
    t.total_bytes = t.face_bytes = 64;
    for (b = 0; b < sizeof palette; b++) {
        palette[b] = (uint8_t)(b * 5u);
    }
    ASSERT(wl_nv2a_texture_layout(&t, false, &layout));
    ASSERT_EQ(WL_GXC_C8, layout.format);
    ASSERT_EQ(64u, layout.tlut_entries);
    ASSERT(wl_nv2a_texture_convert(&t, &layout, src, palette, dst, NULL));
    {
        uint8_t want[4], got[4];
        uint8_t tlut_entry[2];
        wl_xbox_palette_to_tlut(WL_GXC_TLUT_RGB5A3, palette + 4 * 9, 1, tlut_entry);
        ASSERT_EQ(tlut_entry[0], dst[layout.tlut_offset + 2 * 9]);
        ASSERT_EQ(tlut_entry[1], dst[layout.tlut_offset + 2 * 9 + 1]);
        (void)want;
        (void)got;
    }
    PASS();
}

TEST cube_maps_become_sphere_maps(void) {
    static const uint32_t colours[6] = {0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0xFF00FFFFu, 0xFFFFFFFFu, 0xFF000000u};
    static uint8_t src[6 * 4096];
    static uint8_t dst[65536] __attribute__((aligned(32)));
    static uint8_t scratch[65536];
    WlNv2aTexture t;
    WlNv2aGxLayout layout;
    unsigned f, i;
    uint8_t rgba[4];

    // a 32x32 A8R8G8B8 cube, each face one colour
    memset(&t, 0, sizeof t);
    t.color = WL_NV2A_TEX_SZ_A8R8G8B8;
    t.known = 1;
    t.swizzled = 1;
    t.cube = 1;
    t.width = t.height = 32;
    t.depth = 1;
    t.levels = 1;
    t.texel_bytes = 4;
    t.face_bytes = 4096;
    t.total_bytes = 6 * 4096;
    for (f = 0; f < 6; f++) {
        for (i = 0; i < 1024; i++) {
            wl_wr_le32(src + f * 4096 + 4 * i, colours[f]);
        }
    }
    ASSERT(wl_nv2a_texture_layout(&t, false, &layout));
    ASSERT(layout.sphere);
    ASSERT(wl_nv2a_texture_scratch_bytes(&t, &layout) <= sizeof scratch);
    ASSERT(wl_nv2a_texture_convert(&t, &layout, src, NULL, dst, scratch));
    // the middle: straight ahead, +z (face 4, white); near the rim: behind, -z (face 5, black)
    ASSERT(wl_gxc_texel_read(WL_GXC_RGBA8, dst, layout.image_bytes, layout.width, layout.height, layout.width / 2,
                             layout.height / 2, rgba));
    ASSERT_EQ(255, rgba[0]);
    ASSERT_EQ(255, rgba[2]);
    ASSERT(wl_gxc_texel_read(WL_GXC_RGBA8, dst, layout.image_bytes, layout.width, layout.height, layout.width / 2, 0,
                             rgba));
    ASSERT_EQ(0, rgba[0]);
    ASSERT_EQ(0, rgba[1]);
    ASSERT_EQ(0, rgba[2]);
    // to the right: +x, face 0, whose ARGB word 0xFF0000FF is blue
    ASSERT(wl_gxc_texel_read(WL_GXC_RGBA8, dst, layout.image_bytes, layout.width, layout.height,
                             layout.width / 2 + layout.width * 3 / 10, layout.height / 2, rgba));
    ASSERT_EQ(0, rgba[0]);
    ASSERT_EQ(255, rgba[2]);
    // the mapping and its inverse
    {
        float d[3], st[2];
        wl_nv2a_sphere_direction(0.3f, 0.6f, d);
        wl_nv2a_sphere_coordinates(d, st);
        ASSERT(fabsf(st[0] - 0.3f) < 1e-4f);
        ASSERT(fabsf(st[1] - 0.6f) < 1e-4f);
    }
    // the mapping without divisions against its definition (d normalized, m = 2 |d + (0, 0, 1)|) over
    // directions of every length and side; zero, and straight behind
    {
        float d[3], st[2];
        int n;

        for (n = 0; n < 4000; n++) {
            double scale = pow(10.0, (double)(n % 13) - 6.0), x, y, z, l, m;

            d[0] = (float)(sin(n * 0.37) * scale);
            d[1] = (float)(cos(n * 0.91) * scale);
            d[2] = (float)(sin(n * 1.73 + 0.5) * scale);
            x = d[0];
            y = d[1];
            z = d[2];
            l = sqrt(x * x + y * y + z * z);
            m = 2.0 * sqrt(x * x / (l * l) + y * y / (l * l) + (z / l + 1.0) * (z / l + 1.0));
            if (m < 1.0e-3) {
                continue;
            }
            wl_nv2a_sphere_coordinates(d, st);
            ASSERT(fabs(st[0] - (x / l / m + 0.5)) < 1.0e-6);
            ASSERT(fabs(st[1] - (y / l / m + 0.5)) < 1.0e-6);
        }
        d[0] = d[1] = d[2] = 0.0f;
        wl_nv2a_sphere_coordinates(d, st);
        ASSERT(st[0] == 0.5f && st[1] == 0.5f);
        d[2] = -3.0f;
        wl_nv2a_sphere_coordinates(d, st);
        ASSERT(st[0] == 0.5f && st[1] == 1.0f);
    }
    PASS();
}

// a light's falloff cube (a 32x32 Y8 cube, linear, a disc of light on +x, the other faces dark): its
// lit face in place of the sphere map, read through the direction's projection onto it; a cube lit
// on every face stays a sphere map
TEST a_falloff_cube_becomes_its_lit_face(void) {
    static uint8_t src[6 * 1024];
    static uint8_t dst[65536] __attribute__((aligned(32)));
    static uint8_t scratch[65536];
    WlNv2aTexture t;
    WlNv2aGxLayout layout;
    unsigned x, y;
    uint8_t rgba[4];
    float st[2];

    memset(&t, 0, sizeof t);
    t.color = WL_NV2A_TEX_LU_Y8;
    t.known = 1;
    t.linear = 1;
    t.cube = 1;
    t.width = t.height = 32;
    t.pitch = 32;
    t.depth = 1;
    t.levels = 1;
    t.texel_bytes = 1;
    t.face_bytes = 1024;
    t.total_bytes = 6 * 1024;
    memset(src, 0, sizeof src);
    for (y = 0; y < 32; y++) {
        for (x = 0; x < 32; x++) {
            float dx = (float)x - 15.5f, dy = (float)y - 15.5f;
            src[y * 32 + x] = dx * dx + dy * dy < 100.0f ? 255 : 0;
        }
    }
    ASSERT(wl_nv2a_texture_layout(&t, false, &layout));
    ASSERT(wl_nv2a_texture_scratch_bytes(&t, &layout) <= sizeof scratch);
    wl_nv2a_texture_spot(&t, &layout, src, NULL, scratch);
    ASSERT_EQ(1, layout.spot);
    ASSERT(wl_nv2a_texture_convert(&t, &layout, src, NULL, dst, scratch));
    // the face fills the image: lit in the middle, dark at its edge
    ASSERT(wl_gxc_texel_read(WL_GXC_RGBA8, dst, layout.image_bytes, layout.width, layout.height, layout.width / 2,
                             layout.height / 2, rgba));
    ASSERT_EQ(255, rgba[0]);
    ASSERT(wl_gxc_texel_read(WL_GXC_RGBA8, dst, layout.image_bytes, layout.width, layout.height, 0,
                             layout.height / 2, rgba));
    ASSERT_EQ(0, rgba[0]);
    // +x straight on: the middle; half way up the face; behind the light: beyond the face
    wl_nv2a_spot_coordinates((const float[3]){1.0f, 0.0f, 0.0f}, layout.spot, st);
    ASSERT(fabsf(st[0] - 0.5f) < 1e-6f && fabsf(st[1] - 0.5f) < 1e-6f);
    wl_nv2a_spot_coordinates((const float[3]){2.0f, 1.0f, 0.0f}, layout.spot, st);
    ASSERT(fabsf(st[0] - 0.5f) < 1e-6f && fabsf(st[1] - 0.25f) < 1e-6f);
    wl_nv2a_spot_coordinates((const float[3]){-1.0f, 0.2f, 0.0f}, layout.spot, st);
    ASSERT(st[0] < 0.0f && st[1] < 0.0f);
    // light on every face: a sphere map
    memset(src, 200, sizeof src);
    wl_nv2a_texture_spot(&t, &layout, src, NULL, scratch);
    ASSERT_EQ(0, layout.spot);
    PASS();
}

// a texture of one level kept smaller: its image halved by a box filter (a texel checker of black
// and white comes out grey), in its own GX format
TEST a_texture_without_levels_halved(void) {
    static uint8_t src[16 * 16 * 4], swizzled[16 * 16 * 4];
    static uint8_t dst[4096] __attribute__((aligned(32)));
    static uint8_t scratch[16 * 16 * 8];
    WlNv2aTexture t;
    WlNv2aGxLayout layout;
    unsigned x, y;
    uint8_t rgba[4];

    memset(&t, 0, sizeof t);
    t.color = WL_NV2A_TEX_SZ_A8R8G8B8;
    t.known = 1;
    t.swizzled = 1;
    t.width = t.height = 16;
    t.depth = 1;
    t.levels = 1;
    t.texel_bytes = 4;
    t.face_bytes = t.total_bytes = sizeof src;
    for (y = 0; y < 16; y++) {
        for (x = 0; x < 16; x++) {
            wl_wr_le32(src + (y * 16 + x) * 4, (x + y) % 2 ? 0xFFFFFFFFu : 0xFF000000u);
        }
    }
    wl_xbox_swizzle(src, sizeof src, 16, 16, 4, swizzled, sizeof swizzled);
    ASSERT(wl_nv2a_texture_layout_reduced(&t, false, 1, &layout));
    ASSERT_EQ(8u, layout.width);
    ASSERT_EQ(WL_GXC_RGBA8, layout.format);
    ASSERT(wl_nv2a_texture_scratch_bytes(&t, &layout) <= sizeof scratch);
    ASSERT(wl_nv2a_texture_convert(&t, &layout, swizzled, NULL, dst, scratch));
    ASSERT(wl_gxc_texel_read(WL_GXC_RGBA8, dst, layout.image_bytes, 8, 8, 3, 5, rgba));
    ASSERT_EQ(128, rgba[0]);
    ASSERT_EQ(255, rgba[3]);
    PASS();
}

TEST cache_keeps_finds_changes_and_evicts(void) {
    static WlNv2aTexCache cache;
    TGuest g;
    WlNv2aTexCacheConfig cfg;
    WlNv2aTexture t;
    WlNv2aTexCacheStats s;
    const WlNv2aTexEntry *e, *again;
    bool made;
    unsigned i;
    ASSERT(t_create(&g, NULL) != NULL);
    t_setup_3d(&g);
    for (i = 0; i < 16 * 16 * 4; i++) {
        g.ram[0x100000 + i] = (uint8_t)i;
    }
    set_texture(&g, 0, 0x100000, tex_format(WL_NV2A_TEX_SZ_A8R8G8B8, 4, 4, 1, false));
    t_kick(&g);
    ASSERT(wl_nv2a_texture(wl_nv2a_kelvin(g.dev), 0, &t));
    memset(&cfg, 0, sizeof cfg);
    cfg.budget = 3000; // room for two 16x16 RGBA8 images (1 KiB each), not three
    wl_nv2a_texcache_init(&cache, &cfg);

    e = wl_nv2a_texcache_get(&cache, g.dev, &t, false, &made);
    ASSERT(e != NULL);
    ASSERT(made);
    ASSERT_EQ(WL_GXC_RGBA8, e->layout.format);
    again = wl_nv2a_texcache_get(&cache, g.dev, &t, false, &made);
    ASSERT(again == e);
    ASSERT_FALSE(made);
    // rewritten: seen at the next frame's first use, made again
    g.ram[0x100000] ^= 0xFF;
    wl_nv2a_texcache_frame_done(&cache);
    again = wl_nv2a_texcache_get(&cache, g.dev, &t, false, &made);
    ASSERT(again != NULL);
    ASSERT(made);
    ASSERT(again->dynamic);
    // found changed once: checked at every use, within the frame too
    g.ram[0x100004] ^= 0xFF;
    again = wl_nv2a_texcache_get(&cache, g.dev, &t, false, &made);
    ASSERT(made);
    // three textures in a budget of two: the least recently used goes
    for (i = 1; i <= 2; i++) {
        WlNv2aTexture other = t;
        other.phys = 0x100000 + 0x1000 * i;
        other.format_word ^= i << 28; // (another key)
        ASSERT(wl_nv2a_texcache_get(&cache, g.dev, &other, false, &made) != NULL);
        wl_nv2a_texcache_frame_done(&cache);
    }
    wl_nv2a_texcache_stats(&cache, &s);
    ASSERT(s.evicted >= 1);
    ASSERT(s.bytes <= 3000);
    ASSERT_EQ(3u, s.converted);
    ASSERT_EQ(2u, s.reconverted);
    wl_nv2a_texcache_clear(&cache);
    t_destroy(&g);
    PASS();
}

// A write watch over the test's guest RAM, as the runtime's MMU gives it: a store is told by hand
// (w_store), page granular.
static struct {
    uint32_t epoch;
    uint32_t page_epoch[T_RAM / 4096u];
    unsigned arms, looks;
} g_w;

static uint32_t w_watch(void *user, uint32_t phys, uint32_t bytes) {
    (void)user;
    (void)phys;
    (void)bytes;
    g_w.arms++;
    return g_w.epoch;
}

static bool w_written(void *user, uint32_t phys, uint32_t bytes, uint32_t stamp) {
    uint32_t p;
    (void)user;
    g_w.looks++;
    for (p = phys / 4096u; p <= (phys + bytes - 1u) / 4096u; p++) {
        if (g_w.page_epoch[p] > stamp) {
            return true;
        }
    }
    return false;
}

static void w_store(TGuest *g, uint32_t phys, uint8_t value) {
    g->ram[phys] = value;
    g_w.page_epoch[phys / 4096u] = ++g_w.epoch;
}

TEST cache_watched_and_in_an_arena(void) {
    static WlNv2aTexCache cache;
    static uint8_t arena[4096 + 32];
    TGuest g;
    WlNv2aTexCacheConfig cfg;
    WlNv2aTexture t, big, other[4];
    WlNv2aTexCacheStats s;
    const WlNv2aTexEntry *e;
    bool made;
    unsigned i;
    ASSERT(t_create(&g, NULL) != NULL);
    t_setup_3d(&g);
    memset(&g_w, 0, sizeof g_w);
    g_w.epoch = 1;
    for (i = 0; i < 16 * 16 * 4; i++) {
        g.ram[0x100000 + i] = (uint8_t)i;
    }
    // a 16x16 A8R8G8B8 (1 KiB), and a 512x256 one (512 KiB: above the whole hash)
    set_texture(&g, 0, 0x100000, tex_format(WL_NV2A_TEX_SZ_A8R8G8B8, 4, 4, 1, false));
    set_texture(&g, 1, 0x200000, tex_format(WL_NV2A_TEX_SZ_A8R8G8B8, 9, 8, 1, false));
    t_kick(&g);
    ASSERT(wl_nv2a_texture(wl_nv2a_kelvin(g.dev), 0, &t));
    ASSERT(wl_nv2a_texture(wl_nv2a_kelvin(g.dev), 1, &big));
    memset(&cfg, 0, sizeof cfg);
    cfg.watch = w_watch;
    cfg.written = w_written;
    wl_nv2a_texcache_init(&cache, &cfg);

    ASSERT(wl_nv2a_texcache_get(&cache, g.dev, &t, false, &made) != NULL);
    ASSERT(made);
    wl_nv2a_texcache_stats(&cache, &s);
    ASSERT_EQ(1u, s.hashed);
    // no store since: the next frames' checks read nothing
    for (i = 0; i < 3; i++) {
        wl_nv2a_texcache_frame_done(&cache);
        ASSERT(wl_nv2a_texcache_get(&cache, g.dev, &t, false, &made) != NULL);
        ASSERT_FALSE(made);
    }
    wl_nv2a_texcache_stats(&cache, &s);
    ASSERT_EQ(1u, s.hashed);
    ASSERT_EQ(3u, s.watched);
    // a store that changes a texel: hashed and made again
    w_store(&g, 0x100010, 0xEE);
    wl_nv2a_texcache_frame_done(&cache);
    e = wl_nv2a_texcache_get(&cache, g.dev, &t, false, &made);
    ASSERT(e != NULL);
    ASSERT(made);
    // a store of the same byte (or beside the texture, in its page): hashed, kept
    w_store(&g, 0x100010, 0xEE);
    w_store(&g, 0x100800, 0x11);
    wl_nv2a_texcache_frame_done(&cache);
    ASSERT(wl_nv2a_texcache_get(&cache, g.dev, &t, false, &made) == e);
    ASSERT_FALSE(made);
    // ...and from then on the watch again
    wl_nv2a_texcache_frame_done(&cache);
    ASSERT(wl_nv2a_texcache_get(&cache, g.dev, &t, false, &made) == e);
    wl_nv2a_texcache_stats(&cache, &s);
    ASSERT_EQ(3u, s.hashed);
    ASSERT_EQ(4u, s.watched);
    ASSERT_EQ(1u, s.reconverted);
    // the large one is never hashed under the watch: a store to its pages has it made again
    ASSERT(wl_nv2a_texcache_get(&cache, g.dev, &big, false, &made) != NULL);
    ASSERT(made);
    wl_nv2a_texcache_frame_done(&cache);
    ASSERT(wl_nv2a_texcache_get(&cache, g.dev, &big, false, &made) != NULL);
    ASSERT_FALSE(made);
    w_store(&g, 0x200000 + 300000, 0x22); // (one the samples would not see)
    wl_nv2a_texcache_frame_done(&cache);
    ASSERT(wl_nv2a_texcache_get(&cache, g.dev, &big, false, &made) != NULL);
    ASSERT(made);
    wl_nv2a_texcache_stats(&cache, &s);
    ASSERT_EQ(3u, s.hashed);
    ASSERT_EQ(2u, s.reconverted);
    wl_nv2a_texcache_clear(&cache);

    // an arena with room for three 1 KiB images (each with its heap's 32-byte header)
    memset(&cfg, 0, sizeof cfg);
    cfg.arena = arena;
    cfg.arena_bytes = sizeof arena;
    wl_nv2a_texcache_init(&cache, &cfg);
    for (i = 0; i < 4; i++) {
        other[i] = t;
        other[i].phys = 0x100000 + 0x1000 * i;
        other[i].format_word ^= i << 28; // (another key)
    }
    // one a frame: the fourth evicts the first, nothing fails
    for (i = 0; i < 4; i++) {
        ASSERT(wl_nv2a_texcache_get(&cache, g.dev, &other[i], false, &made) != NULL);
        wl_nv2a_texcache_frame_done(&cache);
    }
    wl_nv2a_texcache_stats(&cache, &s);
    ASSERT_EQ(0u, s.failed);
    ASSERT(s.evicted >= 1);
    // all four in one frame: the last one drawn has no room whole (the others are the GPU's until
    // the frame ends), and is kept halved (it has no smaller level of its own: a box filter's)
    for (i = 0; i < 4; i++) {
        e = wl_nv2a_texcache_get(&cache, g.dev, &other[i], false, &made);
        ASSERT(e != NULL);
    }
    ASSERT_EQ(1u, (unsigned)e->reduced);
    ASSERT_EQ(8u, e->layout.width);
    ASSERT_EQ(1u, (unsigned)e->layout.shrink);
    wl_nv2a_texcache_stats(&cache, &s);
    ASSERT_EQ(0u, s.no_memory);
    ASSERT_EQ(0u, s.failed);
    // the next frame it is drawn as it is kept
    wl_nv2a_texcache_frame_done(&cache);
    ASSERT(wl_nv2a_texcache_get(&cache, g.dev, &other[3], false, &made) != NULL);
    ASSERT(wl_heap_check(&cache.heap));
    wl_nv2a_texcache_clear(&cache);
    ASSERT_EQ(0u, cache.heap.live);
    t_destroy(&g);
    PASS();
}

// An arena in pieces: a large image empties the window of images beside a gap whose newest was drawn
// longest ago, not every image by age until one happens to fit.
TEST cache_arena_window(void) {
    static WlNv2aTexCache cache;
    static uint8_t arena[8 * (1024 + 32) + 32];
    static const unsigned order[] = {2, 4, 6, 3, 5, 7}; // their last frames, oldest first
    TGuest g;
    WlNv2aTexCacheConfig cfg;
    WlNv2aTexture t, wide, other[8];
    WlNv2aTexCacheStats s;
    bool made;
    unsigned i;
    uint32_t evicted;
    ASSERT(t_create(&g, NULL) != NULL);
    t_setup_3d(&g);
    // 16x16 A8R8G8B8 images (1 KiB), and a 32x16 one (2 KiB: two of them side by side)
    set_texture(&g, 0, 0x100000, tex_format(WL_NV2A_TEX_SZ_A8R8G8B8, 4, 4, 1, false));
    set_texture(&g, 1, 0x200000, tex_format(WL_NV2A_TEX_SZ_A8R8G8B8, 5, 4, 1, false));
    t_kick(&g);
    ASSERT(wl_nv2a_texture(wl_nv2a_kelvin(g.dev), 0, &t));
    ASSERT(wl_nv2a_texture(wl_nv2a_kelvin(g.dev), 1, &wide));
    memset(&cfg, 0, sizeof cfg);
    cfg.arena = arena;
    cfg.arena_bytes = sizeof arena;
    wl_nv2a_texcache_init(&cache, &cfg);
    for (i = 0; i < 8; i++) {
        other[i] = t;
        other[i].phys = 0x100000 + 0x1000 * i;
        other[i].format_word ^= i << 28; // (another key)
        ASSERT(wl_nv2a_texcache_get(&cache, g.dev, &other[i], false, &made) != NULL); // (in address order)
    }
    for (i = 0; i < 6; i++) {
        wl_nv2a_texcache_frame_done(&cache);
        ASSERT(wl_nv2a_texcache_get(&cache, g.dev, &other[order[i]], false, &made) != NULL);
        ASSERT_FALSE(made);
    }
    wl_nv2a_texcache_frame_done(&cache);
    ASSERT(wl_nv2a_texcache_get(&cache, g.dev, &other[0], false, &made) != NULL);
    ASSERT(wl_nv2a_texcache_get(&cache, g.dev, &other[1], false, &made) != NULL);
    wl_nv2a_texcache_stats(&cache, &s);
    evicted = s.evicted;
    // 0 and 1 are drawn in this frame; by age 2, 4, 6 would go before 3 makes 2 and 3 a window
    ASSERT(wl_nv2a_texcache_get(&cache, g.dev, &wide, false, &made) != NULL);
    ASSERT(made);
    wl_nv2a_texcache_stats(&cache, &s);
    ASSERT_EQ(2u, s.evicted - evicted);
    ASSERT_EQ(0u, s.no_memory);
    for (i = 4; i < 8; i++) {
        ASSERT(wl_nv2a_texcache_get(&cache, g.dev, &other[i], false, &made) != NULL);
        ASSERT_FALSE(made); // (kept)
    }
    // with nothing left that may go, a second wide one finds no window and evicts nothing
    {
        WlNv2aTexture wide2 = wide;
        wide2.phys = 0x300000;
        evicted = s.evicted;
        ASSERT(wl_nv2a_texcache_get(&cache, g.dev, &wide2, false, &made) == NULL);
        wl_nv2a_texcache_stats(&cache, &s);
        ASSERT_EQ(evicted, s.evicted);
        ASSERT_EQ(1u, s.no_memory);
    }
    ASSERT(wl_heap_check(&cache.heap));
    wl_nv2a_texcache_clear(&cache);
    ASSERT_EQ(0u, cache.heap.live);
    t_destroy(&g);
    PASS();
}

// No room for a texture with mip levels, even with every image not drawn in the frame given back:
// kept from its next level (normalized coordinates, the same texture smaller) rather than not at
// all, and made whole again once textures not drawn for long enough give theirs back.
TEST cache_keeps_smaller_for_want_of_memory(void) {
    static WlNv2aTexCache cache;
    // room for one 32x32 RGBA8 image (4 KiB) and two 16x16 ones (1 KiB), each with its heap header
    static uint8_t arena[(4096 + 32) + 2 * (1024 + 32) + 32];
    TGuest g;
    WlNv2aTexCacheConfig cfg;
    WlNv2aTexture t, big, other[3];
    WlNv2aTexCacheStats s;
    WlNv2aGxLayout layout;
    const WlNv2aTexEntry *e;
    uint8_t rgba[4];
    bool made;
    unsigned i, f;
    ASSERT(t_create(&g, NULL) != NULL);
    t_setup_3d(&g);
    // a swizzled A8R8G8B8 32x32 of three levels: level 0 red, level 1 green, level 2 blue
    for (i = 0; i < 32 * 32; i++) {
        wl_wr_le32(&g.ram[0x200000 + 4 * i], 0xFFFF0000u);
    }
    for (i = 0; i < 16 * 16; i++) {
        wl_wr_le32(&g.ram[0x200000 + 4096 + 4 * i], 0xFF00FF00u);
    }
    for (i = 0; i < 8 * 8; i++) {
        wl_wr_le32(&g.ram[0x200000 + 4096 + 1024 + 4 * i], 0xFF0000FFu);
    }
    set_texture(&g, 0, 0x100000, tex_format(WL_NV2A_TEX_SZ_A8R8G8B8, 4, 4, 1, false));
    set_texture(&g, 1, 0x200000, tex_format(WL_NV2A_TEX_SZ_A8R8G8B8, 5, 5, 3, false));
    t_kick(&g);
    ASSERT(wl_nv2a_texture(wl_nv2a_kelvin(g.dev), 0, &t));
    ASSERT(wl_nv2a_texture(wl_nv2a_kelvin(g.dev), 1, &big));

    // the layouts: levels left out, none under 8 texels a side, a linear texture never
    ASSERT(wl_nv2a_texture_layout_reduced(&big, false, 1, &layout));
    ASSERT_EQ(16u, layout.width);
    ASSERT_EQ(1u, layout.first_level);
    ASSERT_EQ(1024u, (unsigned)layout.image_bytes);
    ASSERT(wl_nv2a_texture_layout_reduced(&big, true, 2, &layout));
    ASSERT_EQ(8u, layout.width);
    ASSERT_EQ(1u, layout.levels);
    ASSERT_FALSE(wl_nv2a_texture_layout_reduced(&big, false, 3, &layout));
    // (a texture of one level: halved by a box filter, down to 8 texels a side)
    ASSERT(wl_nv2a_texture_layout_reduced(&t, false, 1, &layout));
    ASSERT_EQ(8u, layout.width);
    ASSERT_EQ(1u, (unsigned)layout.shrink);
    ASSERT_FALSE(wl_nv2a_texture_layout_reduced(&t, false, 2, &layout));

    memset(&cfg, 0, sizeof cfg);
    cfg.arena = arena;
    cfg.arena_bytes = sizeof arena;
    wl_nv2a_texcache_init(&cache, &cfg);
    for (i = 0; i < 3; i++) {
        other[i] = t;
        other[i].phys = 0x100000 + 0x1000 * i;
        other[i].format_word ^= i << 28; // (another key)
    }
    // three 16x16 drawn in the frame: the 32x32 has no room whole, and is kept from level 1
    for (i = 0; i < 3; i++) {
        ASSERT(wl_nv2a_texcache_get(&cache, g.dev, &other[i], false, &made) != NULL);
    }
    e = wl_nv2a_texcache_get(&cache, g.dev, &big, false, &made);
    ASSERT(e != NULL);
    ASSERT(made);
    ASSERT_EQ(1u, (unsigned)e->reduced);
    ASSERT_EQ(16u, e->layout.width);
    ASSERT_EQ(32u, (unsigned)e->width); // (the guest's size)
    ASSERT(wl_gxc_texel_read(WL_GXC_RGBA8, e->image, e->layout.image_bytes, 16, 16, 3, 5, rgba));
    ASSERT_EQ(0, rgba[0]);
    ASSERT_EQ(255, rgba[1]);
    wl_nv2a_texcache_stats(&cache, &s);
    ASSERT_EQ(1u, s.reduced);
    ASSERT_EQ(0u, s.no_memory);
    // drawn alone in the frames after: whole again only once the others have not been drawn for
    // WL_NV2A_TEXCACHE_RESTORE_FRAMES frames (their images then give way)
    for (f = 1; f < WL_NV2A_TEXCACHE_RESTORE_FRAMES; f++) {
        wl_nv2a_texcache_frame_done(&cache);
        e = wl_nv2a_texcache_get(&cache, g.dev, &big, false, &made);
        ASSERT(e != NULL);
        ASSERT_FALSE(made);
        ASSERT_EQ(1u, (unsigned)e->reduced);
    }
    wl_nv2a_texcache_frame_done(&cache);
    wl_nv2a_texcache_stats(&cache, &s);
    ASSERT_EQ(1u, s.frame_reduced); // (drawn smaller in the frame just finished)
    e = wl_nv2a_texcache_get(&cache, g.dev, &big, false, &made);
    ASSERT(e != NULL);
    ASSERT(made);
    ASSERT_EQ(0u, (unsigned)e->reduced);
    ASSERT_EQ(32u, e->layout.width);
    ASSERT(wl_gxc_texel_read(WL_GXC_RGBA8, e->image, e->layout.image_bytes, 32, 32, 7, 9, rgba));
    ASSERT_EQ(255, rgba[0]);
    ASSERT_EQ(0, rgba[1]);
    wl_nv2a_texcache_stats(&cache, &s);
    ASSERT_EQ(1u, s.restored);
    wl_nv2a_texcache_frame_done(&cache);
    wl_nv2a_texcache_stats(&cache, &s);
    ASSERT_EQ(0u, s.frame_reduced);
    ASSERT(s.evicted >= 1 && s.evicted <= 3);
    ASSERT_EQ(4096u + 1024u * (3u - s.evicted), (unsigned)s.bytes);
    ASSERT(wl_heap_check(&cache.heap));
    wl_nv2a_texcache_clear(&cache);
    ASSERT_EQ(0u, cache.heap.live);
    t_destroy(&g);
    PASS();
}

static unsigned g_gpu_waits;

static void count_gpu_wait(void) {
    g_gpu_waits++;
}

// Frames overlapped (gpu_lag 1): a texture drawn in every frame is still made whole again (beside its
// smaller image, which goes back once the GPU is past it); and an image finding no room while only
// the frame before's images hold the memory waits for the GPU once, rather than being kept smaller.
TEST cache_overlapped_restores_and_waits(void) {
    static WlNv2aTexCache cache;
    // (four 16x16 images and the 32x32's smaller one, and 2 KiB: no room for the 32x32 beside the four,
    // and its whole image fits only where they were)
    static uint8_t arena[5 * (1024 + 32) + 2048 + 32];
    TGuest g;
    WlNv2aTexCacheConfig cfg;
    WlNv2aTexture t, big, other[4];
    const WlNv2aTexEntry *e;
    bool made;
    unsigned i, f;
    ASSERT(t_create(&g, NULL) != NULL);
    t_setup_3d(&g);
    set_texture(&g, 0, 0x100000, tex_format(WL_NV2A_TEX_SZ_A8R8G8B8, 4, 4, 1, false));
    set_texture(&g, 1, 0x200000, tex_format(WL_NV2A_TEX_SZ_A8R8G8B8, 5, 5, 3, false));
    t_kick(&g);
    ASSERT(wl_nv2a_texture(wl_nv2a_kelvin(g.dev), 0, &t));
    ASSERT(wl_nv2a_texture(wl_nv2a_kelvin(g.dev), 1, &big));
    for (i = 0; i < 4; i++) {
        other[i] = t;
        other[i].phys = 0x100000 + 0x1000 * i;
        other[i].format_word ^= i << 28;
    }

    // drawn in every frame: whole again after WL_NV2A_TEXCACHE_RESTORE_FRAMES frames all the same
    memset(&cfg, 0, sizeof cfg);
    cfg.arena = arena;
    cfg.arena_bytes = sizeof arena;
    cfg.gpu_lag = 1;
    wl_nv2a_texcache_init(&cache, &cfg);
    for (i = 0; i < 4; i++) {
        ASSERT(wl_nv2a_texcache_get(&cache, g.dev, &other[i], false, &made) != NULL);
    }
    e = wl_nv2a_texcache_get(&cache, g.dev, &big, false, &made);
    ASSERT(e != NULL && e->reduced == 1);
    for (f = 1; f < WL_NV2A_TEXCACHE_RESTORE_FRAMES; f++) {
        wl_nv2a_texcache_frame_done(&cache);
        e = wl_nv2a_texcache_get(&cache, g.dev, &big, false, &made);
        ASSERT(e != NULL && e->reduced == 1);
    }
    wl_nv2a_texcache_frame_done(&cache);
    e = wl_nv2a_texcache_get(&cache, g.dev, &big, false, &made);
    ASSERT(e != NULL);
    ASSERT(made);
    ASSERT_EQ(0u, (unsigned)e->reduced);
    ASSERT_EQ(32u, e->layout.width);
    ASSERT_EQ(1u, cache.pending_count); // (the smaller image: the GPU may be drawing the frame before from it)
    wl_nv2a_texcache_frame_done(&cache);
    wl_nv2a_texcache_frame_done(&cache);
    ASSERT_EQ(0u, cache.pending_count);
    ASSERT(wl_heap_check(&cache.heap));
    wl_nv2a_texcache_clear(&cache);
    ASSERT_EQ(0u, cache.heap.live);

    // the others drawn in the frame before alone: the GPU waited for, and the texture whole at once
    cfg.gpu_wait = count_gpu_wait;
    wl_nv2a_texcache_init(&cache, &cfg);
    g_gpu_waits = 0;
    for (i = 0; i < 4; i++) {
        ASSERT(wl_nv2a_texcache_get(&cache, g.dev, &other[i], false, &made) != NULL);
    }
    wl_nv2a_texcache_frame_done(&cache);
    e = wl_nv2a_texcache_get(&cache, g.dev, &big, false, &made);
    ASSERT(e != NULL);
    ASSERT_EQ(0u, (unsigned)e->reduced);
    ASSERT_EQ(1u, g_gpu_waits);
    ASSERT(wl_heap_check(&cache.heap));
    wl_nv2a_texcache_clear(&cache);
    ASSERT_EQ(0u, cache.heap.live);
    t_destroy(&g);
    PASS();
}

static WlResult sink_count(void *user, const void *data, uint32_t bytes) {
    (void)data;
    *(uint32_t *)user += bytes;
    return WL_OK;
}

// A capture begun inside a frame: the textures checked before it are read again at their next use
// (the capture must hold every texture its draws read), once.
TEST cache_reads_again_when_a_capture_begins(void) {
    static WlNv2aTexCache cache;
    TGuest g;
    WlNv2aTexture t;
    WlNv2aTexCacheStats s;
    WlNv2aCaptureSink sink;
    uint32_t written = 0, before;
    bool made;
    ASSERT(t_create(&g, NULL) != NULL);
    t_setup_3d(&g);
    set_texture(&g, 0, 0x100000, tex_format(WL_NV2A_TEX_SZ_A8R8G8B8, 4, 4, 1, false));
    t_kick(&g);
    ASSERT(wl_nv2a_texture(wl_nv2a_kelvin(g.dev), 0, &t));
    wl_nv2a_texcache_init(&cache, NULL);
    ASSERT(wl_nv2a_texcache_get(&cache, g.dev, &t, false, &made) != NULL);
    ASSERT(wl_nv2a_texcache_get(&cache, g.dev, &t, false, &made) != NULL);
    wl_nv2a_texcache_stats(&cache, &s);
    ASSERT_EQ(1u, s.checks);
    sink.user = &written;
    sink.write = sink_count;
    ASSERT_EQ(WL_OK, wl_nv2a_capture_begin(g.dev, &sink));
    before = written;
    ASSERT(wl_nv2a_texcache_get(&cache, g.dev, &t, false, &made) != NULL);
    ASSERT_FALSE(made);
    ASSERT(written >= before + 16u * 16u * 4u); // (its texels in the capture)
    ASSERT(wl_nv2a_texcache_get(&cache, g.dev, &t, false, &made) != NULL);
    wl_nv2a_texcache_stats(&cache, &s);
    ASSERT_EQ(2u, s.checks);
    wl_nv2a_capture_end(g.dev);
    wl_nv2a_texcache_clear(&cache);
    t_destroy(&g);
    PASS();
}

SUITE(texture_suite) {
    RUN_TEST(stage_registers_decoded);
    RUN_TEST(conversions_sample_as_the_nv2a_would);
    RUN_TEST(swizzled_levels_and_luminance);
    RUN_TEST(dxt_and_palettes);
    RUN_TEST(cube_maps_become_sphere_maps);
    RUN_TEST(a_falloff_cube_becomes_its_lit_face);
    RUN_TEST(a_texture_without_levels_halved);
    RUN_TEST(cache_keeps_finds_changes_and_evicts);
    RUN_TEST(cache_watched_and_in_an_arena);
    RUN_TEST(cache_arena_window);
    RUN_TEST(cache_keeps_smaller_for_want_of_memory);
    RUN_TEST(cache_overlapped_restores_and_waits);
    RUN_TEST(cache_reads_again_when_a_capture_begins);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(texture_suite);
    GREATEST_MAIN_END();
}
