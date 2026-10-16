// SPDX-License-Identifier: MIT
// GX driven directly (gx_direct.h) over libogc. The transform unit's flush before a primitive is
// OpenCE-Wii's (d3d8_gx.c, "GX's transform unit kept from hanging"), moved here with the vertex
// layout it needs to know: a strip of eight vertices of zeros in the draw's own format.
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
#include "wl/platform/gx_pipe_words.h"
#include "wl/platform/wii/gx_flush_wii.h"
#include "wl/platform/wii/gx_wii_internal.h"

#define DIRECT_FORMAT GX_VTXFMT7
#define XF_FLUSH_VERTICES 8

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
    float texgen[8][2][4];
    // a vertex of an indexed layout as the FIFO takes it: each item an index (0) or a number of
    // words; whether every attribute is indexed
    uint8_t order[12], order_count;
    // the layout's projected coordinates
    unsigned projected;
    // the lit channels' light as loaded (wl_gx_direct_lighting)
    WlGxDirectLighting lighting;
    bool lighting_known;
    bool in_primitive;
    // the texture object each map holds as wl_gx_direct_texture loaded it (bit n of maps_known: map
    // n's is known; wl_gx_wii_texmaps_forget clears it)
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
    d.maps_known &= (uint8_t)~maps;
}

void wl_gx_wii_texmaps_loaded(uint32_t maps) {
    d.maps_known &= (uint8_t)~maps;
    d.maps_loaded |= (uint8_t)maps;
}

// ---- the bisection (wl_gx_bisect_set) -----------------------------------------------------------

#define TLUT_COPIES 256u
#define TLUT_COPY_BYTES 512u

// a mid grey, opaque: RGBA8's 4x4 tile, its AR half then its GB half
static uint8_t fixed_texels[64] __attribute__((aligned(32)));
// the TLUTs' copies (in the program's own memory, MEM1), each slot used in turn
static uint8_t tlut_copies[TLUT_COPIES][TLUT_COPY_BYTES] __attribute__((aligned(32)));
static uint32_t tlut_next;

void wl_gx_bisect_set(const WlGxBisect *bisect) {
    unsigned i;

    d.bisect = *bisect;
    d.bisect_on = bisect->no_textures || bisect->no_tlut || bisect->tlut_mem1 || bisect->max_draws != 0 ||
                  bisect->no_quads || bisect->no_z_restore || bisect->no_copies;
    for (i = 0; i < 32u; i += 2u) {
        fixed_texels[i] = 0xFF;
        fixed_texels[i + 1u] = 0x80;
        fixed_texels[32u + i] = 0x80;
        fixed_texels[32u + i + 1u] = 0x80;
    }
    DCFlushRange(fixed_texels, sizeof fixed_texels);
    GX_InitTexObj(&d.fixed, fixed_texels, 4, 4, GX_TF_RGBA8, GX_REPEAT, GX_REPEAT, GX_FALSE);
    GX_InitTexObjFilterMode(&d.fixed, GX_NEAR, GX_NEAR);
}

const WlGxBisect *wl_gx_bisect(void) {
    return &d.bisect;
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
    wl_gx_invalidate_shadow();
    wl_gx_tlut_invalidate();
    d.layout_known = false;
    d.lighting_known = false;
    d.position_identity = true;
    d.texgen_set = 0;
    wl_gx_direct_flush_after();
    guMtxIdentity(identity);
    GX_LoadPosMtxImm(identity, GX_PNMTX0);
    GX_SetCurrentMtx(GX_PNMTX0);
    GX_LoadTexMtxImm(identity, GX_IDENTITY, GX_MTX3x4);
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

void wl_gx_direct_layout(const WlGxDirectLayout *layout) {
    unsigned n, texgens = 0;

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
    if (layout->lit) {
        GX_SetVtxDesc(GX_VA_NRM, input_of(layout, WL_GX_DIRECT_ARRAY_NORMAL, 3));
        GX_SetVtxAttrFmt(DIRECT_FORMAT, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
    }
    // (projected coordinates: the normal's three numbers, or the normal, binormal and tangent's nine)
    d.projected = 0;
    for (n = 0; n < 8; n++) {
        d.projected += (layout->texcoords_projected >> n) & 1u;
    }
    if (d.projected == 1u) {
        GX_SetVtxDesc(GX_VA_NRM, input_of(layout, 0xFFu, 3));
        GX_SetVtxAttrFmt(DIRECT_FORMAT, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
    } else if (d.projected > 1u) {
        GX_SetVtxDesc(GX_VA_NBT, input_of(layout, 0xFFu, 9));
        GX_SetVtxAttrFmt(DIRECT_FORMAT, GX_VA_NBT, GX_NRM_NBT, GX_F32, 0);
    }
    if (layout->colors >= 1) {
        GX_SetVtxDesc(GX_VA_CLR0, input_of(layout, WL_GX_DIRECT_ARRAY_COLOR0, 1));
        GX_SetVtxAttrFmt(DIRECT_FORMAT, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    }
    if (layout->colors >= 2) {
        GX_SetVtxDesc(GX_VA_CLR1, input_of(layout, WL_GX_DIRECT_ARRAY_COLOR0 + 1, 1));
        GX_SetVtxAttrFmt(DIRECT_FORMAT, GX_VA_CLR1, GX_CLR_RGBA, GX_RGBA8, 0);
    }
    for (n = 0; n < 8; n++) {
        if ((layout->texcoords | layout->texcoords_generated | layout->texcoords_projected) & (1u << n)) {
            texgens = n + 1;
        }
    }
    for (n = 0; n < texgens; n++) {
        if (layout->texcoords_projected & (1u << n)) {
            // (from the normal, binormal or tangent that carries it, through a 3x4 identity: GX
            // divides s and t by q at each pixel)
            static const u32 sources[3] = {GX_TG_NRM, GX_TG_BINRM, GX_TG_TANGENT};
            unsigned k, before = 0;

            for (k = 0; k < n; k++) {
                before += (layout->texcoords_projected >> k) & 1u;
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
        GX_SetVtxDesc((u8)(GX_VA_TEX0 + n),
                      input_of(layout, WL_GX_DIRECT_ARRAY_TEXCOORD0 + n, (layout->texcoord_s & (1u << n)) ? 1 : 2));
        GX_SetVtxAttrFmt(DIRECT_FORMAT, GX_VA_TEX0 + n, (layout->texcoord_s & (1u << n)) ? GX_TEX_S : GX_TEX_ST, GX_F32,
                         0);
        GX_SetTexCoordGen((u16)(GX_TEXCOORD0 + n), GX_TG_MTX2x4, GX_TG_TEX0 + n, GX_IDENTITY);
    }
    // (never no channel and no coordinate at once: the console's GPU stops for good on a primitive
    // with neither that follows others in the pipeline, where Dolphin draws it. One channel then,
    // from the registers, which no stage of such a draw reads; wl_gx_direct_layout_outputs)
    wl_gx_direct_layout_outputs(layout, &d.chans, &d.texgens);
    GX_SetNumTexGens((u8)d.texgens);
    GX_SetNumChans((u8)d.chans);
    wl_gx_wii_hang_event('L', layout->indexed | (uint32_t)layout->colors << 16 | (uint32_t)layout->lit << 20 |
                                  (uint32_t)layout->texcoords_projected << 24,
                         layout->texcoords | (uint32_t)layout->texcoords_generated << 8 |
                             (uint32_t)layout->texcoord_s << 16);
    if (!layout->lit) {
        // (a channel the vertex has no colour for: its material the register's)
        GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, layout->colors >= 1 ? GX_SRC_VTX : GX_SRC_REG,
                       GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
        GX_SetChanCtrl(GX_COLOR1A1, GX_DISABLE, GX_SRC_REG, layout->colors >= 2 ? GX_SRC_VTX : GX_SRC_REG,
                       GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    } else {
        // (a lit channel: its material and ambient the registers', its light n, the signed dot
        // product, no attenuation; the others the vertex's colour)
        static const s32 channels[4] = {GX_COLOR0, GX_ALPHA0, GX_COLOR1, GX_ALPHA1};

        for (n = 0; n < 4; n++) {
            if (layout->lit & (1u << n)) {
                GX_SetChanCtrl(channels[n], GX_ENABLE, GX_SRC_REG, GX_SRC_REG, (u8)(GX_LIGHT0 << n), GX_DF_SIGNED,
                               GX_AF_NONE);
            } else {
                GX_SetChanCtrl(channels[n], GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
            }
        }
    }
    wl_gx_wii_note_commands();
}

void wl_gx_direct_lighting(const WlGxDirectLighting *lighting) {
    static const GXColor white = {255, 255, 255, 255};
    Mtx normal;
    unsigned n, r;

    if (d.lighting_known && memcmp(&d.lighting, lighting, sizeof d.lighting) == 0) {
        return;
    }
    d.lighting = *lighting;
    d.lighting_known = true;
    for (r = 0; r < 3; r++) {
        normal[r][0] = lighting->normal_matrix[r][0];
        normal[r][1] = lighting->normal_matrix[r][1];
        normal[r][2] = lighting->normal_matrix[r][2];
        normal[r][3] = 0.0f;
    }
    GX_LoadNrmMtxImm(normal, GX_PNMTX0);
    for (n = 0; n < 4; n++) {
        GXLightObj light;
        const float *dir = lighting->direction[n];
        GXColor color = {lighting->color[n][0], lighting->color[n][1], lighting->color[n][2], lighting->color[n][3]};

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
        unsigned sent = 0;

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
        wl_gx_pipe_words(wgPipe, words, count);
    }
}

void wl_gx_direct_vertices_words(const uint32_t *const *vertices, uint32_t n, uint32_t count) {
    if (d.in_primitive) {
        wl_gx_pipe_vertices(wgPipe, vertices, n, count);
    }
}

void wl_gx_direct_array(unsigned attribute, const void *data, uint32_t stride) {
    u32 gx_attribute = attribute == WL_GX_DIRECT_ARRAY_POSITION ? (u32)GX_VA_POS
                       : attribute == WL_GX_DIRECT_ARRAY_NORMAL ? (u32)GX_VA_NRM
                       : attribute < WL_GX_DIRECT_ARRAY_TEXCOORD0
                           ? (u32)(GX_VA_CLR0 + attribute - WL_GX_DIRECT_ARRAY_COLOR0)
                           : (u32)(GX_VA_TEX0 + attribute - WL_GX_DIRECT_ARRAY_TEXCOORD0);

    if (attribute > WL_GX_DIRECT_ARRAY_NORMAL) {
        return;
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
        wl_gx_pipe_vertices_index(wgPipe, d.order, d.order_count, indices, base, n, words, count);
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
                          (d.layout.texcoords & (1u << n)) ? (u32)(GX_TG_TEX0 + n) : (u32)GX_TG_POS, GX_IDENTITY);
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
                      source == WL_GX_DIRECT_TG_POSITION ? (u32)GX_TG_POS : (u32)(GX_TG_TEX0 + n),
                      (u32)(GX_TEXMTX0 + 3u * n));
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
        if ((d.maps_known & (1u << map)) == 0 || memcmp(&d.fixed, &d.maps[map], sizeof d.fixed) != 0) {
            GX_LoadTexObj(&d.fixed, (u8)(GX_TEXMAP0 + map));
            wl_gx_wii_hang_event('T', (uint32_t)(uintptr_t)fixed_texels,
                                 map << 28 | (uint32_t)WL_GX_TEX_RGBA8 << 22 | 4u << 11 | 4u);
            d.maps[map] = d.fixed;
            d.maps_known |= (uint8_t)(1u << map);
            d.maps_loaded |= (uint8_t)(1u << map);
            wl_gx_wii_note_commands();
        }
        return;
    }
    if (tlut != NULL && (tex->format == WL_GX_TEX_C4 || tex->format == WL_GX_TEX_C8)) {
        GXTlutObj table;

        if (d.bisect_on && d.bisect.tlut_mem1) {
            tlut_from_mem1(tlut, &table);
        } else {
            memcpy(&table, tlut->opaque, sizeof table);
        }
        GX_LoadTlut(&table, tex->tlut);
        wl_gx_wii_hang_event('U', (uint32_t)(uintptr_t)tlut->entries, (uint32_t)tex->tlut << 16 | tlut->count);
        wl_gx_tlut_invalidate();
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
    if ((d.maps_known & (1u << map)) != 0 && memcmp(&obj, &d.maps[map], sizeof obj) == 0) {
        // (the map holds it: its registers as they would be written; GX's texture cache is
        // wl_gx_direct_texture_invalidate's)
        d.stats.textures_kept++;
        return;
    }
    GX_LoadTexObj(&obj, (u8)(GX_TEXMAP0 + map));
    wl_gx_wii_hang_event('T', (uint32_t)(uintptr_t)GX_GetTexObjData(&obj),
                         map << 28 | (uint32_t)(tex->format & 63u) << 22 | (tex->width & 2047u) << 11 |
                             (tex->height & 2047u));
    d.maps[map] = obj;
    d.maps_known |= (uint8_t)(1u << map);
    d.maps_loaded |= (uint8_t)(1u << map);
    wl_gx_wii_note_commands();
}

void wl_gx_direct_texture_invalidate(void) {
    GX_InvalidateTexAll();
}

void wl_gx_direct_stats(WlGxDirectStats *out) {
    *out = d.stats;
    out->flushes = wl_gx_direct_flushes();
    wl_gx_shadow_counts(&out->state_calls, &out->state_sent);
}
