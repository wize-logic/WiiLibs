// SPDX-License-Identifier: MIT
// wl/xbox/nv2a_texture and nv2a_texcache: a texture stage's registers decoded (swizzled, linear,
// DXT, cube and P8 textures: sizes, levels, faces, palettes), textures converted into GX's formats
// and decoded back to what the NV2A would sample (the codec's exact conversions and the texel
// decoders of the rest), mip levels, a cube map's sphere map, and the cache: kept, found changed,
// evicted within its budget; checked through a write watch (no texels read while its pages see no
// store), its images in an arena of its own (room made by evicting, a texture without room not
// tried again in the frame).
#include <math.h>
#include <string.h>

#include "greatest.h"
#include "nv2a_test_util.h"
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
    // all four in one frame: the last one drawn has no room (the others are the GPU's until the
    // frame ends), and is not tried again in the frame
    for (i = 0; i < 4; i++) {
        e = wl_nv2a_texcache_get(&cache, g.dev, &other[i], false, &made);
        ASSERT(i < 3 ? e != NULL : e == NULL);
    }
    ASSERT(wl_nv2a_texcache_get(&cache, g.dev, &other[3], false, &made) == NULL);
    wl_nv2a_texcache_stats(&cache, &s);
    ASSERT_EQ(1u, s.no_memory);
    ASSERT_EQ(2u, s.failed);
    // the next frame it has room again (the least recently drawn goes)
    wl_nv2a_texcache_frame_done(&cache);
    ASSERT(wl_nv2a_texcache_get(&cache, g.dev, &other[3], false, &made) != NULL);
    ASSERT(made);
    ASSERT(wl_heap_check(&cache.heap));
    wl_nv2a_texcache_clear(&cache);
    ASSERT_EQ(0u, cache.heap.live);
    t_destroy(&g);
    PASS();
}

SUITE(texture_suite) {
    RUN_TEST(stage_registers_decoded);
    RUN_TEST(conversions_sample_as_the_nv2a_would);
    RUN_TEST(swizzled_levels_and_luminance);
    RUN_TEST(dxt_and_palettes);
    RUN_TEST(cube_maps_become_sphere_maps);
    RUN_TEST(cache_keeps_finds_changes_and_evicts);
    RUN_TEST(cache_watched_and_in_an_arena);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(texture_suite);
    GREATEST_MAIN_END();
}
