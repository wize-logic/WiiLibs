// SPDX-License-Identifier: MIT
// GX driven directly (gx_direct.h) over libogc. The transform unit's flush before a primitive is
// OpenCE-Wii's (d3d8_gx.c, "GX's transform unit kept from hanging"), moved here with the vertex
// layout it needs to know: a strip of eight vertices of zeros in the draw's own format.
#include <malloc.h>
#include <ogc/cache.h>
#include <ogc/gu.h>
#include <ogc/gx.h>
#include <string.h>

#include "wl/platform/gx_direct.h"
#include "wl/platform/gx_shadow.h"

// the write-gather pipe's stores (gx_pipe_words.h): the pipe read once for a call
#define WL_PIPE_T WGPipe *
#define WL_PIPE_W32(pipe, v) ((pipe)->U32 = (v))
#define WL_PIPE_W16(pipe, v) ((pipe)->U16 = (v))
#define WL_PIPE_PPC_FP64 // (two words a move: lfd and stfd)
#include "wl/platform/gx_pipe_words.h"
#include "wl/platform/wii/gx_flush_wii.h"
#include "wl/platform/wii/gx_wii_internal.h"

#define DIRECT_FORMAT GX_VTXFMT7
#define XF_FLUSH_VERTICES 8
// a sphere map's matrices (WlGxDirectLayout.texcoords_sphere): (x, y, z + 1), and after the
// normalize the disc's (x / 2 + 1/2, y / 2 + 1/2, 1)
#define SPHERE_MTX GX_TEXMTX9
#define SPHERE_POST GX_DTTMTX0

_Static_assert(WL_GX_BM_SUBTRACT == GX_BM_SUBTRACT && WL_GX_BM_BLEND == GX_BM_BLEND, "blend modes are GX's");
_Static_assert(WL_GX_BL_INVDSTALPHA == GX_BL_INVDSTALPHA && WL_GX_BL_OTHERCLR == GX_BL_SRCCLR &&
                   WL_GX_BL_OTHERCLR == GX_BL_DSTCLR,
               "blend factors are GX's");
_Static_assert((int)WL_GX_ALWAYS == GX_ALWAYS && (int)WL_GX_GEQUAL == GX_GEQUAL, "compare functions are GX's");
_Static_assert((int)WL_GX_CULL_BACK == GX_CULL_BACK && (int)WL_GX_CULL_FRONT == GX_CULL_FRONT, "cull modes are GX's");

static struct {
    WlGxDirectLayout layout;
    bool layout_known;
    // the transform unit's matrices as loaded: the position matrix's (identity or not) and each
    // coordinate's generator, so that what is asked again is not sent again
    bool position_identity;
    Mtx position;
    uint8_t texgen_set; // bit n: coordinate n has a matrix of its own
    uint8_t texgen_source[8];
    // each coordinate's input as the transform unit numbers them: the vertex's coordinates in order
    // (GX_TG_TEX0 + the coordinates the vertex sends before it; wl_gx_direct_layout)
    uint8_t tex_input[8];
    bool gaps_logged;
    float texgen[8][2][4];
    // a vertex of an indexed layout as the FIFO takes it: each item an index (0) or a number of
    // words; whether every attribute is indexed
    uint8_t order[12], order_count;
    uint32_t vertex_bytes; // (a vertex's bytes: the FIFO's model)
    // the layout's projected coordinates
    unsigned projected;
    // the lit channels' light as loaded (wl_gx_direct_lighting)
    WlGxDirectLighting lighting;
    bool lighting_known;
    // the channels' material registers as set (wl_gx_direct_material; bit c of material_known)
    uint32_t material[2];
    uint8_t material_known;
    bool in_primitive;
    // (the registers each map holds: gx_shadow.h's, wl_gx_wii_texmaps_forget forgets them) and the
    // texture object each was last given, as it was before its load (bit n of maps_known)
    GXTexObj maps[GX_MAX_TEXMAP];
    uint8_t maps_known;
    // the maps that have been given a texture, by this layer or behind its back (never cleared: GX's
    // maps keep what they were loaded with)
    uint8_t maps_loaded;
    // the bisection's switches (wl_gx_bisect_set), whether any is on, the primitives of this frame,
    // the fixed texture that stands for the textures left out, and the TLUTs' copies in MEM1
    WlGxBisect bisect;
    bool bisect_on;
    uint32_t frame_draws;
    GXTexObj fixed;
    // the colour channels and texture coordinates GX makes for the layout
    uint32_t chans, texgens;
    WlGxDirectStats stats;
#ifdef WL_GX_CHECK
    uint32_t check_findings;
#endif
} d;

#ifdef WL_GX_CHECK
// the draw's state checked (wl_gx_check_draw): the first findings logged with the state, then a count
static void check_draw(void) {
    const WlTevProgram *tev = wl_gx_wii_tev_last();
    uint32_t found = wl_gx_check_draw(d.chans, d.texgens, d.maps_loaded, tev), bit, i;

    if (found == 0) {
        return;
    }
    d.check_findings++;
    if (d.check_findings > 8u && (d.check_findings & 1023u) != 0) {
        return;
    }
    for (bit = 0; bit < WL_GX_CHECKS; bit++) {
        if (found & (1u << bit)) {
            WL_LOG_E("GXCHECK", "draw %u (finding %u): %s", (unsigned)d.stats.draws, (unsigned)d.check_findings,
                     wl_gx_check_name(bit));
        }
    }
    WL_LOG_E("GXCHECK", "  layout indexed %04x colours %u texcoords %02x s %02x generated %02x projected %02x lit %x; "
                        "channels %u, coordinates %u, maps loaded %02x",
             d.layout.indexed, d.layout.colors, d.layout.texcoords, d.layout.texcoord_s,
             d.layout.texcoords_generated, d.layout.texcoords_projected, d.layout.lit, (unsigned)d.chans,
             (unsigned)d.texgens, d.maps_loaded);
    for (i = 0; tev != NULL && i < tev->stage_count && i < WL_TEV_MAX_STAGES; i++) {
        const WlTevStage *s = &tev->stages[i];

        WL_LOG_E("GXCHECK", "  stage %u: coordinate %d map %d channel %d, colour %u %u %u %u, alpha %u %u %u %u",
                 (unsigned)i, s->texcoord == WL_TEV_NONE ? -1 : s->texcoord, s->texmap == WL_TEV_NONE ? -1 : s->texmap,
                 s->channel == WL_TEV_NONE ? -1 : s->channel, s->color.in[0], s->color.in[1], s->color.in[2],
                 s->color.in[3], s->alpha.in[0], s->alpha.in[1], s->alpha.in[2], s->alpha.in[3]);
    }
}
#endif

void wl_gx_wii_texmaps_forget(uint32_t maps) {
    wl_gx_shadow_texture_forget(maps);
    d.maps_known &= (uint8_t)~maps;
}

void wl_gx_wii_texmaps_loaded(uint32_t maps) {
    wl_gx_wii_texmaps_forget(maps);
    d.maps_loaded |= (uint8_t)maps;
}

// ---- the bisection (wl_gx_bisect_set) -----------------------------------------------------------

#define TLUT_COPIES 256u
#define TLUT_COPY_BYTES 512u

// a mid grey, opaque: RGBA8's 4x4 tile, its AR half then its GB half
static uint8_t fixed_texels[64] __attribute__((aligned(32)));
// the TLUTs' copies (of the heap, MEM1 first), each slot used in turn: had when the switch
// tlut_mem1 is first set (128 KB the program's BSS held whether the bisection ran or not)
static uint8_t (*tlut_copies)[TLUT_COPY_BYTES];
static uint32_t tlut_next;

void wl_gx_bisect_set(const WlGxBisect *bisect) {
    unsigned i;

    d.bisect = *bisect;
    d.bisect_on = bisect->no_textures || bisect->no_tlut || bisect->tlut_mem1 || bisect->max_draws != 0 ||
                  bisect->no_quads || bisect->no_z_restore || bisect->no_copies || bisect->late_z || bisect->vtx_inval;
    if (bisect->tlut_mem1 && tlut_copies == NULL) {
        tlut_copies = memalign(32, TLUT_COPIES * TLUT_COPY_BYTES);
    }
    for (i = 0; i < 32u; i += 2u) {
        fixed_texels[i] = 0xFF;
        fixed_texels[i + 1u] = 0x80;
        fixed_texels[32u + i] = 0x80;
        fixed_texels[32u + i + 1u] = 0x80;
    }
    DCFlushRange(fixed_texels, sizeof fixed_texels);
    GX_InitTexObj(&d.fixed, fixed_texels, 4, 4, GX_TF_RGBA8, GX_REPEAT, GX_REPEAT, GX_FALSE);
    GX_InitTexObjFilterMode(&d.fixed, GX_NEAR, GX_NEAR);
    if (wl_gx_ready()) {
        wl_gx_wii_regions_apply();
    }
}

// ---- the texture memory's regions (wl_gx_wii_regions_apply) -------------------------------------
// GX caches each map's texels in a region of its 1 MB texture memory (an even bank, 0..512K, and an
// odd one, 512K..1M); libogc gives each map 32K of each (in three sets by format, which overlap),
// and its TLUTs 256K of the odd bank. A large texture (Halo's are 256x256 RGBA8 and DXT1, the back
// buffer's restore 640x480 RGBA8 and Z24X8) thrashes 32K. Here maps 0-2 (the first stages, and the
// restore's colour and depth) have 128K of each bank, maps 3-5 32K, maps 6 and 7 (the rare separate
// alphas) share 32K with the others (map 7's even half with map 6's, their odd halves with maps 4's and
// 5's); the TLUTs are packed at the top of the odd bank (512 bytes for each 256-entry slot, 2K for the
// four large ones), out of every region's way. As libogc does, colour-indexed textures and CMPR without
// mipmaps are cached in the even bank only (32K + 32K of the map's even part: GX keeps the TLUTs in the
// odd bank). Regions overlap only as libogc's sets do: within a map, or a rare map's with another's.
#define REGION_MAPS 8u
#define TLUT_REGIONS 20u
#define TLUT_BASE 0x000F8000u // (0xF8000..0xFA000 the 16 slots, 0xFA000..0xFC000 the four large ones)

static GXTexRegion tex_regions[REGION_MAPS], even_regions[REGION_MAPS];
static GXTlutRegion tlut_regions[TLUT_REGIONS];
static GXTexRegionCallback libogc_tex_region;
static GXTlutRegionCallback libogc_tlut_region;
// the callbacks GX_LoadTexObj calls now (NULL before the first wl_gx_wii_regions_apply)
static GXTexRegionCallback tex_region;
static GXTlutRegionCallback tlut_region;

static GXTexRegion *map_region(GXTexObj *obj, u8 map) {
    u32 fmt = GX_GetTexObjFmt(obj);

    map = map < REGION_MAPS ? map : 0u;
    if ((fmt >= GX_TF_CI4 && fmt <= GX_TF_CI14) || (fmt == GX_TF_CMPR && GX_GetTexObjMipMap(obj) == 0)) {
        return &even_regions[map];
    }
    return &tex_regions[map];
}

static GXTlutRegion *slot_region(u32 name) {
    return &tlut_regions[name < TLUT_REGIONS ? name : 0u];
}

void wl_gx_wii_regions_apply(void) {
    // even bank, odd bank (addresses and sizes) for each map
    static const struct {
        uint32_t even, odd;
        u8 even_size, odd_size;
    } k_maps[REGION_MAPS] = {
        {0x00000u, 0x80000u, GX_TEXCACHE_128K, GX_TEXCACHE_128K}, {0x20000u, 0xA0000u, GX_TEXCACHE_128K, GX_TEXCACHE_128K},
        {0x40000u, 0xC0000u, GX_TEXCACHE_128K, GX_TEXCACHE_128K}, {0x60000u, 0xE0000u, GX_TEXCACHE_32K, GX_TEXCACHE_32K},
        {0x68000u, 0xE8000u, GX_TEXCACHE_32K, GX_TEXCACHE_32K},   {0x70000u, 0xF0000u, GX_TEXCACHE_32K, GX_TEXCACHE_32K},
        {0x78000u, 0xE8000u, GX_TEXCACHE_32K, GX_TEXCACHE_32K},   {0x78000u, 0xF0000u, GX_TEXCACHE_32K, GX_TEXCACHE_32K},
    };
    // the even-bank-only regions (colour-indexed, CMPR without mipmaps): 32K + 32K
    static const uint32_t k_even[REGION_MAPS][2] = {
        {0x00000u, 0x08000u}, {0x20000u, 0x28000u}, {0x40000u, 0x48000u}, {0x60000u, 0x68000u},
        {0x68000u, 0x70000u}, {0x70000u, 0x78000u}, {0x70000u, 0x78000u}, {0x70000u, 0x78000u},
    };
    static bool made;
    unsigned i;
    GXTexRegionCallback tex_was;
    GXTlutRegionCallback tlut_was;

    if (!made) {
        for (i = 0; i < REGION_MAPS; i++) {
            GX_InitTexCacheRegion(&tex_regions[i], GX_FALSE, k_maps[i].even, k_maps[i].even_size, k_maps[i].odd,
                                  k_maps[i].odd_size);
            GX_InitTexCacheRegion(&even_regions[i], GX_FALSE, k_even[i][0], GX_TEXCACHE_32K, k_even[i][1],
                                  GX_TEXCACHE_32K);
        }
        for (i = 0; i < TLUT_REGIONS; i++) {
            GX_InitTlutRegion(&tlut_regions[i], i < 16u ? TLUT_BASE + i * 0x200u : TLUT_BASE + 0x2000u + (i - 16u) * 0x800u,
                              i < 16u ? GX_TLUT_256 : GX_TLUT_1K);
        }
        made = true;
    }
    tex_was = GX_SetTexRegionCallback(map_region);
    tlut_was = GX_SetTlutRegionCallback(slot_region);
    if (tex_was != map_region) {
        libogc_tex_region = tex_was; // (GX_Init's own, set again by each GX_Init)
    }
    if (tlut_was != slot_region) {
        libogc_tlut_region = tlut_was;
    }
    tex_region = map_region;
    tlut_region = slot_region;
    if (d.bisect.regions_libogc && libogc_tex_region != NULL && libogc_tlut_region != NULL) {
        (void)GX_SetTexRegionCallback(libogc_tex_region);
        (void)GX_SetTlutRegionCallback(libogc_tlut_region);
        tex_region = libogc_tex_region;
        tlut_region = libogc_tlut_region;
    }
    // (what the maps and slots hold is now somewhere else)
    wl_gx_wii_texmaps_forget(0xFFu);
    wl_gx_shadow_tlut_forget(WL_GX_SHADOW_TLUT_SLOTS);
    wl_gx_tlut_invalidate();
    GX_InvalidateTexAll();
    wl_gx_wii_note_commands();
}

const WlGxBisect *wl_gx_bisect(void) {
    return &d.bisect;
}

// ---- a texture's registers (gx_shadow.h) --------------------------------------------------------
// GX_LoadTexObjPreloaded writes a map's six BP registers (seven with a TLUT) from the texture object
// and the region, each with the map's register number in its top byte, and keeps the object's mode 0
// and image 0 for the texture coordinates' scale, sent again at the next primitive. Here the same words
// are made and only those that differ from the map's last are written, while the mode 0 and image 0
// stay (libogc's record of them then holds already); otherwise libogc's whole load.
// libogc's texture object (its __gx_texobj), read as GX_LoadTexObjPreloaded reads it
typedef struct {
    u32 mode0, mode1, image0, image3, user, format, tlut;
    u16 tiles;
    u8 tile_type, flags; // (flags bit 1: not colour-indexed, no TLUT register)
} TexObjWords;
_Static_assert(sizeof(TexObjWords) == sizeof(GXTexObj), "a texture object is libogc's eight words");

// the registers of maps 0-3 (those of maps 4-7 0x20 above): mode 0, mode 1, image 0, 1 (the even
// half of the region), 2 (the odd half), 3 (the address), the TLUT
static const u8 k_texreg[WL_GX_TEXREGS] = {0x80, 0x84, 0x88, 0x8C, 0x90, 0x94, 0x98};

static inline u32 texreg(unsigned reg, u8 map, u32 value) {
    return (value & 0x00FFFFFFu) | (u32)(k_texreg[reg] + (map & 3u) + (map >= 4u ? 0x20u : 0u)) << 24;
}

// `obj` loaded into map `map`, its registers written where they differ; whether anything was written
static bool load_texture(GXTexObj *obj, u8 map) {
    TexObjWords o;
    uint32_t regs[WL_GX_TEXREGS], answer;
    unsigned count = 6, i;
    GXTexRegion *region;
    u32 halves[2];

    if (tex_region == NULL || tlut_region == NULL) {
        GX_LoadTexObj(obj, map); // (before the regions: libogc's own, the shadow told)
        wl_gx_wii_texmaps_forget(1u << map);
        return true;
    }
    // (the object the map was last given, before libogc wrote the register numbers into it: the same
    // object has the same registers, looked at no further)
    if (!d.bisect.tex_whole && (d.maps_known & (1u << map)) != 0 && memcmp(obj, &d.maps[map], sizeof *obj) == 0) {
        wl_gx_shadow_texture_kept();
        return false;
    }
    d.maps[map] = *obj;
    d.maps_known |= (uint8_t)(1u << map);
    region = tex_region(obj, map);
    memcpy(&o, obj, sizeof o);
    memcpy(halves, region, sizeof halves);
    regs[WL_GX_TEXREG_MODE0] = texreg(WL_GX_TEXREG_MODE0, map, o.mode0);
    regs[WL_GX_TEXREG_MODE1] = texreg(WL_GX_TEXREG_MODE1, map, o.mode1);
    regs[WL_GX_TEXREG_IMAGE0] = texreg(WL_GX_TEXREG_IMAGE0, map, o.image0);
    regs[WL_GX_TEXREG_EVEN] = texreg(WL_GX_TEXREG_EVEN, map, halves[0]);
    regs[WL_GX_TEXREG_ODD] = texreg(WL_GX_TEXREG_ODD, map, halves[1]);
    regs[WL_GX_TEXREG_IMAGE3] = texreg(WL_GX_TEXREG_IMAGE3, map, o.image3);
    if ((o.flags & 2u) == 0) {
        u32 tlut_words[2];

        memcpy(tlut_words, tlut_region(o.tlut), sizeof tlut_words);
        regs[WL_GX_TEXREG_TLUT] = texreg(WL_GX_TEXREG_TLUT, map, tlut_words[1]);
        count = 7;
    }
    answer = wl_gx_shadow_texture(map, regs, count, d.bisect.tex_whole);
    if ((answer & WL_GX_TEXREG_LOAD) != 0) {
        GX_LoadTexObjPreloaded(obj, region, map);
        return true;
    }
    for (i = 0; i < count; i++) {
        if ((answer & (1u << i)) != 0) {
            wgPipe->U8 = 0x61; // (a BP write: GX_LOAD_BP_REG's)
            wgPipe->U32 = regs[i];
        }
    }
    return answer != 0;
}

void wl_gx_wii_bisect_frame(void) {
    d.frame_draws = 0;
}

// a TLUT as the switch tlut_mem1 loads it: its entries copied into MEM1
static void tlut_from_mem1(const WlGxTlut *tlut, GXTlutObj *table) {
    static const u8 formats[] = {GX_TL_IA8, GX_TL_RGB565, GX_TL_RGB5A3};
    uint8_t *copy = tlut_copies[tlut_next++ % TLUT_COPIES];
    uint32_t bytes = (uint32_t)tlut->count * 2u;

    bytes = bytes <= TLUT_COPY_BYTES ? bytes : TLUT_COPY_BYTES;
    memcpy(copy, tlut->entries, bytes);
    DCFlushRange(copy, TLUT_COPY_BYTES);
    GX_InitTlutObj(table, copy, formats[tlut->format <= 2u ? tlut->format : 0u], (u16)(bytes / 2u));
}

void wl_gx_direct_begin(void) {
    Mtx identity;
    unsigned stage;

    wl_gx_wii_own_thread();
    wl_gx_invalidate_shadow(); // (wl_gx's own TLUT shadow too; the slots' palettes are kept: gx_shadow.h)
    d.layout_known = false;
    d.lighting_known = false;
    d.material_known = 0;
    d.position_identity = true;
    d.texgen_set = 0;
    wl_gx_direct_flush_after();
    guMtxIdentity(identity);
    GX_LoadPosMtxImm(identity, GX_PNMTX0);
    GX_SetCurrentMtx(GX_PNMTX0);
    GX_LoadTexMtxImm(identity, GX_IDENTITY, GX_MTX3x4);
    {
        // (a sphere map's generator: WlGxDirectLayout.texcoords_sphere)
        Mtx to_pole = {{1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f, 1.0f}};
        Mtx to_disc = {{0.5f, 0.0f, 0.0f, 0.5f}, {0.0f, 0.5f, 0.0f, 0.5f}, {0.0f, 0.0f, 0.0f, 1.0f}};

        GX_LoadTexMtxImm(to_pole, SPHERE_MTX, GX_MTX3x4);
        GX_LoadTexMtxImm(to_disc, SPHERE_POST, GX_MTX3x4);
    }
    GX_SetNumIndStages(0);
    for (stage = 0; stage < GX_MAX_TEVSTAGE; stage++) {
        GX_SetTevDirect((u8)stage);
    }
    GX_SetMisc(GX_MT_XF_FLUSH, wl_gx_flush_wii_off() ? GX_XF_FLUSH_NONE : GX_XF_FLUSH_SAFE);
    wl_gx_wii_note_commands();
}

// an attribute's input in the layout (GX_INDEX16 or GX_DIRECT), its place in an indexed vertex's
// order noted (an index, or its words after the words before it)
static u8 input_of(const WlGxDirectLayout *layout, unsigned attribute, uint8_t words) {
    if (attribute < 16u && (layout->indexed & (1u << attribute))) {
        d.order[d.order_count++] = 0;
        return GX_INDEX16;
    }
    if (d.order_count != 0 && d.order[d.order_count - 1] != 0) {
        d.order[d.order_count - 1] = (uint8_t)(d.order[d.order_count - 1] + words);
    } else {
        d.order[d.order_count++] = words;
    }
    return GX_DIRECT;
}

// A lit layout's channels: a lit one's material and ambient the registers', its light n and the
// lighting's extra lights, the signed dot product (or the clamped one), no attenuation; the others
// the vertex's colour
static void lit_channels(void) {
    static const s32 channels[4] = {GX_COLOR0, GX_ALPHA0, GX_COLOR1, GX_ALPHA1};
    unsigned n;

    for (n = 0; n < 4; n++) {
        if (d.layout.lit & (1u << n)) {
            GX_SetChanCtrl(channels[n], GX_ENABLE, GX_SRC_REG, GX_SRC_REG,
                           (u8)((GX_LIGHT0 << n) | (d.lighting.extra[n] & 0xFu) << 4),
                           (d.lighting.clamped & (1u << n)) ? GX_DF_CLAMP : GX_DF_SIGNED, GX_AF_NONE);
        } else {
            GX_SetChanCtrl(channels[n], GX_DISABLE, GX_SRC_REG,
                           (d.layout.colors_register & (1u << (n / 2u))) ? GX_SRC_REG : GX_SRC_VTX, GX_LIGHTNULL,
                           GX_DF_NONE, GX_AF_NONE);
        }
    }
}

void wl_gx_direct_layout(const WlGxDirectLayout *layout) {
    unsigned n, texgens = 0, texcoord_inputs;

    if (d.layout_known && memcmp(&d.layout, layout, sizeof d.layout) == 0) {
        return;
    }
    d.layout = *layout;
    d.layout_known = true;
    d.texgen_set = 0;
    d.stats.layouts++;
    d.order_count = 0;
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, input_of(layout, WL_GX_DIRECT_ARRAY_POSITION, 3));
    GX_SetVtxAttrFmt(DIRECT_FORMAT, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    // (a lit layout's normal and the projected coordinates: the normal's three numbers, or the
    // normal, binormal and tangent's nine, a lit layout's normal first)
    d.projected = layout->lit ? 1u : 0u;
    for (n = 0; n < 8; n++) {
        d.projected += (layout->texcoords_projected >> n) & 1u;
    }
    if (d.projected == 1u && layout->lit) {
        GX_SetVtxDesc(GX_VA_NRM, input_of(layout, WL_GX_DIRECT_ARRAY_NORMAL, 3));
        GX_SetVtxAttrFmt(DIRECT_FORMAT, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
    } else if (d.projected == 1u) {
        GX_SetVtxDesc(GX_VA_NRM, input_of(layout, 0xFFu, 3));
        GX_SetVtxAttrFmt(DIRECT_FORMAT, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
    } else if (d.projected > 1u) {
        GX_SetVtxDesc(GX_VA_NBT, input_of(layout, 0xFFu, 9));
        GX_SetVtxAttrFmt(DIRECT_FORMAT, GX_VA_NBT, GX_NRM_NBT, GX_F32, 0);
    }
    if (layout->colors >= 1 && !(layout->colors_register & 1u)) {
        GX_SetVtxDesc(GX_VA_CLR0, input_of(layout, WL_GX_DIRECT_ARRAY_COLOR0, 1));
        GX_SetVtxAttrFmt(DIRECT_FORMAT, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    }
    if (layout->colors >= 2 && !(layout->colors_register & 2u)) {
        GX_SetVtxDesc(GX_VA_CLR1, input_of(layout, WL_GX_DIRECT_ARRAY_COLOR0 + 1, 1));
        GX_SetVtxAttrFmt(DIRECT_FORMAT, GX_VA_CLR1, GX_CLR_RGBA, GX_RGBA8, 0);
    }
    for (n = 0; n < 8; n++) {
        if ((layout->texcoords | layout->texcoords_generated | layout->texcoords_projected) & (1u << n)) {
            texgens = n + 1;
        }
    }
    // (the coordinates the vertex sends go in the descriptor's first slots, in order, and each
    // generator reads the input its coordinate went in. The transform unit is told how many
    // coordinates a vertex has, not which (XF 0x1008), so a layout of TEX0, TEX2 and TEX3 has GX
    // read TEX2's numbers as its input 1 and TEX3's as input 2, and coordinate 3 an input no vertex
    // sends: the console's GPU stopped for good within the first few draws of such a layout (a10's
    // and a30's benches alike), where Dolphin, which files each coordinate by its name, drew them.
    // The bytes are the same either way. The switch gxtexgap puts the gaps back)
    texcoord_inputs = 0;
    for (n = 0; n < 8; n++) {
        bool sent = (layout->texcoords & (1u << n)) != 0;

        d.tex_input[n] = (u8)(GX_TG_TEX0 + (d.bisect.texcoord_gaps || !sent ? n : texcoord_inputs));
        texcoord_inputs += sent ? 1u : 0u;
    }
    if (!d.gaps_logged && !d.bisect.texcoord_gaps && layout->texcoords != (1u << texcoord_inputs) - 1u) {
        d.gaps_logged = true;
        WL_LOG_I("GX", "texture coordinates %02x sent as the first %u of the vertex (the first layout with a gap)",
                 (unsigned)layout->texcoords, texcoord_inputs);
    }
    for (n = 0; n < texgens; n++) {
        if (layout->texcoords_projected & (1u << n)) {
            // (from the normal, binormal or tangent that carries it, through a 3x4 identity: GX
            // divides s and t by q at each pixel)
            static const u32 sources[3] = {GX_TG_NRM, GX_TG_BINRM, GX_TG_TANGENT};
            unsigned k, before = layout->lit ? 1u : 0u;

            for (k = 0; k < n; k++) {
                before += (layout->texcoords_projected >> k) & 1u;
            }
            if (layout->texcoords_sphere & (1u << n)) {
                // (a sphere map's: d + (0, 0, 1) normalized, then halved and moved by 1/2: SPHERE_MTX
                // and SPHERE_POST, wl_gx_direct_begin's)
                GX_SetTexCoordGen2((u16)(GX_TEXCOORD0 + n), GX_TG_MTX3x4, sources[before < 3u ? before : 2u],
                                   SPHERE_MTX, GX_TRUE, SPHERE_POST);
                continue;
            }
            GX_SetTexCoordGen((u16)(GX_TEXCOORD0 + n), GX_TG_MTX3x4, sources[before < 3u ? before : 2u], GX_IDENTITY);
            continue;
        }
        if (!(layout->texcoords & (1u << n))) {
            // (a coordinate no stage reads, below one that is: generated from the position, which
            // every vertex has, rather than from an input the vertex does not send)
            GX_SetTexCoordGen((u16)(GX_TEXCOORD0 + n), GX_TG_MTX2x4, GX_TG_POS, GX_IDENTITY);
            continue;
        }
        GX_SetVtxDesc((u8)(GX_VA_TEX0 + (d.tex_input[n] - GX_TG_TEX0)),
                      input_of(layout, WL_GX_DIRECT_ARRAY_TEXCOORD0 + n, (layout->texcoord_s & (1u << n)) ? 1 : 2));
        GX_SetVtxAttrFmt(DIRECT_FORMAT, GX_VA_TEX0 + (d.tex_input[n] - GX_TG_TEX0),
                         (layout->texcoord_s & (1u << n)) ? GX_TEX_S : GX_TEX_ST, GX_F32, 0);
        GX_SetTexCoordGen((u16)(GX_TEXCOORD0 + n), GX_TG_MTX2x4, d.tex_input[n], GX_IDENTITY);
    }
    // (never no channel and no coordinate at once: the console's GPU stops for good on a primitive
    // with neither that follows others in the pipeline, where Dolphin draws it. One channel then,
    // from the registers, which no stage of such a draw reads; wl_gx_direct_layout_outputs)
    wl_gx_direct_layout_outputs(layout, &d.chans, &d.texgens);
    GX_SetNumTexGens((u8)d.texgens);
    GX_SetNumChans((u8)d.chans);
    d.vertex_bytes = wl_gx_shadow_layout_vertex_bytes(layout);
    wl_gx_shadow_fifo(WL_GX_FIFO_LAYOUT, wl_gx_shadow_layout_bytes(d.texgens, d.chans, layout->lit != 0));
    wl_gx_wii_hang_event('L', layout->indexed | (uint32_t)layout->colors << 16 | (uint32_t)layout->lit << 20 |
                                  (uint32_t)layout->texcoords_projected << 24,
                         layout->texcoords | (uint32_t)layout->texcoords_generated << 8 |
                             (uint32_t)layout->texcoord_s << 16);
    if (!layout->lit) {
        // (a channel the vertex has no colour for, or one whose colour is the register's: its material the
        // register's)
        GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG,
                       layout->colors >= 1 && !(layout->colors_register & 1u) ? GX_SRC_VTX : GX_SRC_REG,
                       GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
        GX_SetChanCtrl(GX_COLOR1A1, GX_DISABLE, GX_SRC_REG,
                       layout->colors >= 2 && !(layout->colors_register & 2u) ? GX_SRC_VTX : GX_SRC_REG,
                       GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    } else {
        lit_channels();
    }
    wl_gx_wii_note_commands();
}

void wl_gx_direct_lighting(const WlGxDirectLighting *lighting) {
    static const GXColor white = {255, 255, 255, 255};
    Mtx normal;
    unsigned n, r;

    bool channels;

    if (d.lighting_known && memcmp(&d.lighting, lighting, sizeof d.lighting) == 0) {
        return;
    }
    // (a lit layout's channels again where the extra lights or the dot products' kind changed)
    channels = d.layout_known && d.layout.lit &&
               (memcmp(d.lighting.extra, lighting->extra, sizeof lighting->extra) != 0 ||
                d.lighting.clamped != lighting->clamped);
    d.lighting = *lighting;
    d.lighting_known = true;
    if (channels) {
        lit_channels();
    }
    for (r = 0; r < 3; r++) {
        normal[r][0] = lighting->normal_matrix[r][0];
        normal[r][1] = lighting->normal_matrix[r][1];
        normal[r][2] = lighting->normal_matrix[r][2];
        normal[r][3] = 0.0f;
    }
    GX_LoadNrmMtxImm(normal, GX_PNMTX0);
    for (n = 0; n < 8; n++) {
        GXLightObj light;
        const float *dir = n < 4 ? lighting->direction[n] : lighting->extra_direction[n - 4];
        const uint8_t *c = n < 4 ? lighting->color[n] : lighting->extra_color[n - 4];
        GXColor color = {c[0], c[1], c[2], c[3]};

        // (an extra light only where a channel adds it)
        if (n >= 4 && !((lighting->extra[0] | lighting->extra[1] | lighting->extra[2] | lighting->extra[3]) &
                        (1u << (n - 4)))) {
            continue;
        }

        // (a light at infinity: its position far along the direction, as libogc's specular one)
        GX_InitLightPos(&light, dir[0] * 1.0e18f, dir[1] * 1.0e18f, dir[2] * 1.0e18f);
        GX_InitLightDir(&light, 0.0f, 0.0f, 0.0f);
        GX_InitLightAttn(&light, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
        GX_InitLightColor(&light, color);
        GX_LoadLightObj(&light, (u8)(GX_LIGHT0 << n));
    }
    for (n = 0; n < 2; n++) {
        GXColor ambient = {lighting->ambient[n][0], lighting->ambient[n][1], lighting->ambient[n][2],
                           lighting->ambient[n][3]};

        GX_SetChanAmbColor(n == 0 ? GX_COLOR0A0 : GX_COLOR1A1, ambient);
        GX_SetChanMatColor(n == 0 ? GX_COLOR0A0 : GX_COLOR1A1, white);
    }
    d.material_known = 0;
    wl_gx_wii_note_commands();
}

void wl_gx_direct_material(unsigned c, uint32_t rgba) {
    GXColor color = {(u8)(rgba >> 24), (u8)(rgba >> 16), (u8)(rgba >> 8), (u8)rgba};

    if (c > 1u || ((d.material_known & (1u << c)) && d.material[c] == rgba)) {
        return;
    }
    d.material[c] = rgba;
    d.material_known |= (uint8_t)(1u << c);
    GX_SetChanMatColor(c == 0 ? GX_COLOR0A0 : GX_COLOR1A1, color);
    wl_gx_shadow_fifo(WL_GX_FIFO_STATE, WL_GX_FIFO_MATERIAL_BYTES);
    wl_gx_wii_note_commands();
}

static inline void emit(const WlGxDirectVertex *v) {
    unsigned n;

    // (an indexed attribute sends its index: the vertex's element 0)
    if (d.layout.indexed & (1u << WL_GX_DIRECT_ARRAY_POSITION)) {
        GX_Position1x16(0);
    } else {
        GX_Position3f32(v->position[0], v->position[1], v->position[2]);
    }
    if (d.layout.lit && (d.layout.indexed & (1u << WL_GX_DIRECT_ARRAY_NORMAL))) {
        GX_Normal1x16(0);
    } else if (d.layout.lit) {
        GX_Normal3f32(v->normal[0], v->normal[1], v->normal[2]);
    }
    if (d.layout.texcoords_projected) {
        unsigned sent = d.layout.lit ? 1u : 0u;

        // (the projected coordinates as the normal, or normal, binormal and tangent)
        for (n = 0; n < 8; n++) {
            if (d.layout.texcoords_projected & (1u << n)) {
                GX_Normal3f32(v->texcoord[n][0], v->texcoord[n][1], v->texcoord_q[n]);
                sent++;
            }
        }
        for (; d.projected > 1u && sent < 3u; sent++) {
            GX_Normal3f32(0.0f, 0.0f, 0.0f);
        }
    }
    for (n = 0; n < d.layout.colors && n < 2; n++) {
        if (d.layout.colors_register & (1u << n)) {
            continue;
        }
        if (d.layout.indexed & (1u << (WL_GX_DIRECT_ARRAY_COLOR0 + n))) {
            GX_Color1x16(0);
        } else {
            GX_Color4u8(v->color[n][0], v->color[n][1], v->color[n][2], v->color[n][3]);
        }
    }
    for (n = 0; n < 8; n++) {
        if (!(d.layout.texcoords & (1u << n))) {
            continue;
        }
        if (d.layout.indexed & (1u << (WL_GX_DIRECT_ARRAY_TEXCOORD0 + n))) {
            GX_TexCoord1x16(0);
        } else if (d.layout.texcoord_s & (1u << n)) {
            GX_TexCoord1f32(v->texcoord[n][0]);
        } else {
            GX_TexCoord2f32(v->texcoord[n][0], v->texcoord[n][1]);
        }
    }
}

void wl_gx_direct_primitive_begin(WlGxDirectPrim prim, uint32_t count) {
    static const u8 prims[] = {GX_POINTS,    GX_LINES,         GX_LINESTRIP, GX_TRIANGLES,
                               GX_TRIANGLESTRIP, GX_TRIANGLEFAN, GX_QUADS};
    uint32_t i;

    d.in_primitive = false;
    if (count == 0 || count > 65535u || (unsigned)prim >= sizeof prims || !d.layout_known) {
        return;
    }
    if (d.bisect_on && d.bisect.max_draws != 0 && d.frame_draws++ >= d.bisect.max_draws) {
        return; // (left out: its vertices go nowhere)
    }
    if (d.bisect_on && d.bisect.vtx_inval) {
        GX_InvVtxCache();
    }
    // (noted before its commands: the flush, the state GX_Begin sends, the primitive)
    wl_gx_wii_hang_event('P', (uint32_t)prim << 16 | count, d.stats.draws);
#ifdef WL_GX_CHECK
    check_draw();
#endif
    if (wl_gx_flush_wii_take()) {
        static const WlGxDirectVertex zero;

        GX_Begin(GX_TRIANGLESTRIP, DIRECT_FORMAT, XF_FLUSH_VERTICES);
        for (i = 0; i < XF_FLUSH_VERTICES; i++) {
            emit(&zero);
        }
        GX_End();
        wl_gx_flush_wii_count();
        wl_gx_wii_hang_event('X', 0, 0);
    }
    GX_Begin(prims[prim], DIRECT_FORMAT, (u16)count);
    wl_gx_shadow_fifo(WL_GX_FIFO_VERTEX, 3u + count * d.vertex_bytes);
    d.in_primitive = true;
    d.stats.draws++;
    d.stats.vertices += count;
}

void wl_gx_direct_vertex(const WlGxDirectVertex *vertex) {
    if (d.in_primitive) {
        emit(vertex);
    }
}

void wl_gx_direct_vertex_words(const uint32_t *words, uint32_t count) {
    if (d.in_primitive) {
        if (d.bisect.pipe32) {
            wl_gx_pipe_words(wgPipe, words, count);
        } else {
            wl_gx_pipe_words64(wgPipe, words, count);
        }
    }
}

void wl_gx_direct_vertices_words(const uint32_t *const *vertices, uint32_t n, uint32_t count) {
    if (d.in_primitive) {
        if (d.bisect.pipe32) {
            wl_gx_pipe_vertices(wgPipe, vertices, n, count);
        } else {
            wl_gx_pipe_vertices64(wgPipe, vertices, n, count);
        }
    }
}

void wl_gx_direct_array(unsigned attribute, const void *data, uint32_t stride) {
    u32 gx_attribute = attribute == WL_GX_DIRECT_ARRAY_POSITION ? (u32)GX_VA_POS
                       : attribute == WL_GX_DIRECT_ARRAY_NORMAL ? (u32)GX_VA_NRM
                       : attribute < WL_GX_DIRECT_ARRAY_TEXCOORD0
                           ? (u32)(GX_VA_CLR0 + attribute - WL_GX_DIRECT_ARRAY_COLOR0)
                           : (u32)(GX_VA_TEX0 + attribute - WL_GX_DIRECT_ARRAY_TEXCOORD0);

    if (attribute > WL_GX_DIRECT_ARRAY_NORMAL || !wl_gx_shadow_array(attribute, data, stride, d.bisect.arrays_always)) {
        return; // (or GX holds it: GX_SetArray's two CP writes sent at every call before; gxarrayall)
    }
    GX_SetArray(gx_attribute, (void *)data, (u8)stride);
    wl_gx_wii_hang_event('A', (uint32_t)(uintptr_t)data, gx_attribute << 8 | stride);
    wl_gx_wii_note_commands();
}

void wl_gx_direct_vertex_index(uint16_t index, const uint32_t *words, uint32_t count) {
    unsigned i, n;

    if (!d.in_primitive) {
        return;
    }
    for (i = 0; i < d.order_count; i++) {
        if (d.order[i] == 0) {
            wgPipe->U16 = index;
            continue;
        }
        for (n = 0; n < d.order[i] && count != 0; n++, count--) {
            wgPipe->U32 = *words++;
        }
    }
}

void wl_gx_direct_vertices_index(const uint32_t *indices, uint32_t base, uint32_t n, const uint32_t *words,
                                 uint32_t count) {
    if (d.in_primitive) {
        if (d.bisect.pipe32) {
            wl_gx_pipe_vertices_index(wgPipe, d.order, d.order_count, indices, base, n, words, count);
        } else {
            wl_gx_pipe_vertices_index64(wgPipe, d.order, d.order_count, indices, base, n, words, count);
        }
    }
}

void wl_gx_direct_vertices_position_index(const uint32_t *indices, uint32_t base, uint32_t first, uint32_t n,
                                          const uint32_t *const *vertices, uint32_t skip, uint32_t count) {
    uint32_t v;

    if (!d.in_primitive) {
        return;
    }
    for (v = 0; v < n; v++) {
        wgPipe->U16 = (u16)(indices != NULL ? indices[first + v] - base : first + v);
        if (d.bisect.pipe32) {
            wl_gx_pipe_words(wgPipe, vertices[v] + skip, count);
        } else {
            wl_gx_pipe_words64(wgPipe, vertices[v] + skip, count);
        }
    }
}

void wl_gx_direct_arrays_changed(void) {
    GX_InvVtxCache();
}

void wl_gx_direct_primitive_end(void) {
    if (d.in_primitive) {
        GX_End();
        d.in_primitive = false;
        wl_gx_wii_note_commands();
    }
}

void wl_gx_direct_draw(WlGxDirectPrim prim, const WlGxDirectVertex *vertices, uint32_t count) {
    uint32_t i;

    wl_gx_direct_primitive_begin(prim, count);
    if (!d.in_primitive) {
        return;
    }
    for (i = 0; i < count; i++) {
        emit(&vertices[i]);
    }
    wl_gx_direct_primitive_end();
}

void wl_gx_direct_position_matrix(const float m[3][4]) {
    Mtx matrix;

    if (m == NULL) {
        if (d.position_identity) {
            return;
        }
        guMtxIdentity(matrix);
        d.position_identity = true;
    } else {
        if (!d.position_identity && memcmp(d.position, m, sizeof d.position) == 0) {
            return;
        }
        memcpy(matrix, m, sizeof matrix);
        memcpy(d.position, m, sizeof d.position);
        d.position_identity = false;
    }
    GX_LoadPosMtxImm(matrix, GX_PNMTX0);
    wl_gx_shadow_fifo(WL_GX_FIFO_MATRIX, WL_GX_FIFO_POSITION_BYTES);
    wl_gx_wii_note_commands();
}

void wl_gx_direct_texgen(unsigned n, WlGxDirectTexSource source, const float m[2][4]) {
    Mtx matrix;

    if (n >= 8) {
        return;
    }
    if (m == NULL) {
        if (!(d.texgen_set & (1u << n))) {
            return;
        }
        d.texgen_set &= (uint8_t)~(1u << n);
        GX_SetTexCoordGen((u16)(GX_TEXCOORD0 + n), GX_TG_MTX2x4,
                          (d.layout.texcoords & (1u << n)) ? (u32)d.tex_input[n] : (u32)GX_TG_POS, GX_IDENTITY);
        wl_gx_shadow_fifo(WL_GX_FIFO_MATRIX, WL_GX_FIFO_TEXGEN_BYTES);
        wl_gx_wii_note_commands();
        return;
    }
    if ((d.texgen_set & (1u << n)) && d.texgen_source[n] == (uint8_t)source &&
        memcmp(d.texgen[n], m, sizeof d.texgen[n]) == 0) {
        return;
    }
    d.texgen_set |= (uint8_t)(1u << n);
    d.texgen_source[n] = (uint8_t)source;
    memcpy(d.texgen[n], m, sizeof d.texgen[n]);
    memset(matrix, 0, sizeof matrix);
    memcpy(matrix, m, sizeof d.texgen[n]);
    GX_LoadTexMtxImm(matrix, (u32)(GX_TEXMTX0 + 3u * n), GX_MTX2x4);
    GX_SetTexCoordGen((u16)(GX_TEXCOORD0 + n), GX_TG_MTX2x4,
                      source == WL_GX_DIRECT_TG_POSITION ? (u32)GX_TG_POS : (u32)d.tex_input[n],
                      (u32)(GX_TEXMTX0 + 3u * n));
    wl_gx_shadow_fifo(WL_GX_FIFO_MATRIX, WL_GX_FIFO_TEXMTX_BYTES + WL_GX_FIFO_TEXGEN_BYTES);
    wl_gx_wii_note_commands();
}

void wl_gx_direct_projection(const float m[4][4], bool orthographic) {
    Mtx44 matrix;
    float value[17];

    memcpy(value, m, 16 * sizeof(float));
    value[16] = orthographic ? 1.0f : 0.0f;

    if (!wl_gx_shadow_differs(WL_GX_SHADOW_PROJECTION, value, sizeof value)) {
        return;
    }
    memcpy(matrix, m, sizeof matrix);
    GX_LoadProjectionMtx(matrix, orthographic ? GX_ORTHOGRAPHIC : GX_PERSPECTIVE);
}

void wl_gx_direct_viewport(float x, float y, float w, float h) {
    const float value[4] = {x, y, w, h};

    if (!wl_gx_shadow_differs(WL_GX_SHADOW_VIEWPORT, value, sizeof value)) {
        return;
    }
    GX_SetViewport(x, y, w, h, 0.0f, 1.0f);
}

void wl_gx_direct_scissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    const uint32_t value[4] = {x, y, w, h};

    if (!wl_gx_shadow_differs(WL_GX_SHADOW_SCISSOR, value, sizeof value)) {
        return;
    }
    GX_SetScissor(x, y, w, h);
}

void wl_gx_direct_blend(WlGxBlendMode mode, WlGxBlendFactor source, WlGxBlendFactor destination) {
    const uint8_t value[3] = {(uint8_t)mode, (uint8_t)source, (uint8_t)destination};

    if (!wl_gx_shadow_differs(WL_GX_SHADOW_BLEND, value, sizeof value)) {
        return;
    }
    GX_SetBlendMode((u8)mode, (u8)source, (u8)destination, GX_LO_CLEAR);
}

void wl_gx_direct_zmode(bool test, WlGxCompare func, bool write) {
    const uint8_t value[3] = {test, (uint8_t)func, write};

    if (!wl_gx_shadow_differs(WL_GX_SHADOW_ZMODE, value, sizeof value)) {
        return;
    }
    GX_SetZMode(test ? GX_TRUE : GX_FALSE, (u8)func, write ? GX_TRUE : GX_FALSE);
}

void wl_gx_direct_zcompare_early(bool early) {
    early = early && !(d.bisect_on && d.bisect.late_z);
    const uint8_t value = early;

    if (!wl_gx_shadow_differs(WL_GX_SHADOW_ZEARLY, &value, sizeof value)) {
        return;
    }
    GX_SetZCompLoc(early ? GX_TRUE : GX_FALSE);
}

void wl_gx_direct_alpha_compare(WlGxCompare func, uint8_t ref) {
    const uint8_t value[2] = {(uint8_t)func, ref};

    if (!wl_gx_shadow_differs(WL_GX_SHADOW_ALPHA_COMPARE, value, sizeof value)) {
        return;
    }
    GX_SetAlphaCompare((u8)func, ref, GX_AOP_AND, GX_ALWAYS, 0);
}

void wl_gx_direct_color_update(bool color, bool alpha) {
    const uint8_t value[2] = {color, alpha};

    if (!wl_gx_shadow_differs(WL_GX_SHADOW_COLOR_UPDATE, value, sizeof value)) {
        return;
    }
    GX_SetColorUpdate(color ? GX_TRUE : GX_FALSE);
    GX_SetAlphaUpdate(alpha ? GX_TRUE : GX_FALSE);
}

void wl_gx_direct_cull(WlGxCull cull) {
    const uint8_t value = (uint8_t)cull;

    if (!wl_gx_shadow_differs(WL_GX_SHADOW_CULL, &value, sizeof value)) {
        return;
    }
    GX_SetCullMode((u8)cull);
}

void wl_gx_direct_texture(uint32_t map, const WlGxTexObj *tex, const WlGxSampler *sampler, const WlGxTlut *tlut) {
    static const u8 wrap_map[] = {GX_CLAMP, GX_REPEAT, GX_MIRROR};
    const WlGxSampler *s = sampler != NULL ? sampler : &tex->sampler;
    GXTexObj obj;
    u8 minf = s->min_filter, wrap_s = s->wrap_s, wrap_t = s->wrap_t;
    bool pow2 = (tex->width & (tex->width - 1u)) == 0 && (tex->height & (tex->height - 1u)) == 0;

    if (map >= GX_MAX_TEXMAP) {
        return;
    }
    if (d.bisect_on && (d.bisect.no_textures ||
                        (d.bisect.no_tlut && (tex->format == WL_GX_TEX_C4 || tex->format == WL_GX_TEX_C8)))) {
        d.stats.textures++;
        obj = d.fixed;
        if (load_texture(&obj, (u8)(GX_TEXMAP0 + map))) {
            wl_gx_wii_hang_event('T', (uint32_t)(uintptr_t)fixed_texels,
                                 map << 28 | (uint32_t)WL_GX_TEX_RGBA8 << 22 | 4u << 11 | 4u);
            d.maps_loaded |= (uint8_t)(1u << map);
            wl_gx_wii_note_commands();
        }
        return;
    }
    if (tlut != NULL && (tex->format == WL_GX_TEX_C4 || tex->format == WL_GX_TEX_C8) &&
        (wl_gx_shadow_tlut_differs(tex->tlut, tlut->entries, (uint32_t)tlut->count * 2u, tlut->format) ||
         d.bisect.tlut_always)) {
        // (loaded only when its slot holds other bytes: a TLUT load waits for GX's texture unit twice)
        GXTlutObj table;

        if (d.bisect_on && d.bisect.tlut_mem1 && tlut_copies != NULL) {
            tlut_from_mem1(tlut, &table);
        } else {
            memcpy(&table, tlut->opaque, sizeof table);
        }
        GX_LoadTlut(&table, tex->tlut);
        wl_gx_wii_hang_event('U', (uint32_t)(uintptr_t)tlut->entries, (uint32_t)tex->tlut << 16 | tlut->count);
        wl_gx_wii_tlut_forget(tex->tlut);
    }
    if (!pow2) {
        wrap_s = wrap_t = WL_GX_WRAP_CLAMP;
    }
    if (tex->mips <= 1u && minf >= WL_GX_FILTER_NEAREST_MIP_NEAREST) {
        minf = (minf & 1u) != 0 ? (u8)WL_GX_FILTER_LINEAR : (u8)WL_GX_FILTER_NEAREST;
    }
    memcpy(&obj, tex->opaque, sizeof obj);
    GX_InitTexObjWrapMode(&obj, wrap_map[wrap_s <= 2 ? wrap_s : 0], wrap_map[wrap_t <= 2 ? wrap_t : 0]);
    GX_InitTexObjFilterMode(&obj, minf, s->mag_filter);
    if (tex->mips > 1u) {
        GX_InitTexObjLODBias(&obj, s->lod_bias);
    }
    d.stats.textures++;
    if (!load_texture(&obj, (u8)(GX_TEXMAP0 + map))) {
        // (the map holds it: its registers as they would be written; GX's texture cache is
        // wl_gx_direct_texture_invalidate's)
        d.stats.textures_kept++;
        return;
    }
    wl_gx_wii_hang_event('T', (uint32_t)(uintptr_t)GX_GetTexObjData(&obj),
                         map << 28 | (uint32_t)(tex->format & 63u) << 22 | (tex->width & 2047u) << 11 |
                             (tex->height & 2047u));
    d.maps_loaded |= (uint8_t)(1u << map);
    wl_gx_wii_note_commands();
}

void wl_gx_direct_texture_invalidate(void) {
    GX_InvalidateTexAll();
}

void wl_gx_direct_stats(WlGxDirectStats *out) {
    *out = d.stats;
    out->flushes = wl_gx_direct_flushes();
    wl_gx_shadow_stats(out);
}
