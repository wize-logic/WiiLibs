// SPDX-License-Identifier: MIT
// GX driven directly (gx_direct.h) over libogc. The transform unit's flush before a primitive is
// OpenCE-Wii's (d3d8_gx.c, "GX's transform unit kept from hanging"), moved here with the vertex
// layout it needs to know: a strip of eight vertices of zeros in the draw's own format.
#include <ogc/gu.h>
#include <ogc/gx.h>
#include <string.h>

#include "wl/platform/gx_direct.h"
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
    bool in_primitive;
    WlGxDirectStats stats;
} d;

void wl_gx_direct_begin(void) {
    Mtx identity;
    unsigned stage;

    wl_gx_invalidate_shadow();
    wl_gx_tlut_invalidate();
    d.layout_known = false;
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

void wl_gx_direct_layout(const WlGxDirectLayout *layout) {
    unsigned n, texgens = 0;

    if (d.layout_known && memcmp(&d.layout, layout, sizeof d.layout) == 0) {
        return;
    }
    d.layout = *layout;
    d.layout_known = true;
    d.stats.layouts++;
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxAttrFmt(DIRECT_FORMAT, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    if (layout->colors >= 1) {
        GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
        GX_SetVtxAttrFmt(DIRECT_FORMAT, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    }
    if (layout->colors >= 2) {
        GX_SetVtxDesc(GX_VA_CLR1, GX_DIRECT);
        GX_SetVtxAttrFmt(DIRECT_FORMAT, GX_VA_CLR1, GX_CLR_RGBA, GX_RGBA8, 0);
    }
    for (n = 0; n < 8; n++) {
        if (layout->texcoords & (1u << n)) {
            texgens = n + 1;
        }
    }
    for (n = 0; n < texgens; n++) {
        if (!(layout->texcoords & (1u << n))) {
            // (a coordinate no stage reads, below one that is: generated from the position, which
            // every vertex has, rather than from an input the vertex does not send)
            GX_SetTexCoordGen((u16)(GX_TEXCOORD0 + n), GX_TG_MTX2x4, GX_TG_POS, GX_IDENTITY);
            continue;
        }
        GX_SetVtxDesc((u8)(GX_VA_TEX0 + n), GX_DIRECT);
        GX_SetVtxAttrFmt(DIRECT_FORMAT, GX_VA_TEX0 + n, (layout->texcoord_s & (1u << n)) ? GX_TEX_S : GX_TEX_ST, GX_F32,
                         0);
        GX_SetTexCoordGen((u16)(GX_TEXCOORD0 + n), GX_TG_MTX2x4, GX_TG_TEX0 + n, GX_IDENTITY);
    }
    GX_SetNumTexGens(texgens);
    GX_SetNumChans(layout->colors);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetChanCtrl(GX_COLOR1A1, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    wl_gx_wii_note_commands();
}

static inline void emit(const WlGxDirectVertex *v) {
    unsigned n;

    GX_Position3f32(v->position[0], v->position[1], v->position[2]);
    if (d.layout.colors >= 1) {
        GX_Color4u8(v->color[0][0], v->color[0][1], v->color[0][2], v->color[0][3]);
    }
    if (d.layout.colors >= 2) {
        GX_Color4u8(v->color[1][0], v->color[1][1], v->color[1][2], v->color[1][3]);
    }
    for (n = 0; n < 8; n++) {
        if (!(d.layout.texcoords & (1u << n))) {
            continue;
        }
        if (d.layout.texcoord_s & (1u << n)) {
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
    if (wl_gx_flush_wii_take()) {
        static const WlGxDirectVertex zero;

        GX_Begin(GX_TRIANGLESTRIP, DIRECT_FORMAT, XF_FLUSH_VERTICES);
        for (i = 0; i < XF_FLUSH_VERTICES; i++) {
            emit(&zero);
        }
        GX_End();
        wl_gx_flush_wii_count();
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

void wl_gx_direct_projection(const float m[4][4], bool orthographic) {
    Mtx44 matrix;

    memcpy(matrix, m, sizeof matrix);
    GX_LoadProjectionMtx(matrix, orthographic ? GX_ORTHOGRAPHIC : GX_PERSPECTIVE);
}

void wl_gx_direct_viewport(float x, float y, float w, float h) {
    GX_SetViewport(x, y, w, h, 0.0f, 1.0f);
}

void wl_gx_direct_scissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    GX_SetScissor(x, y, w, h);
}

void wl_gx_direct_blend(WlGxBlendMode mode, WlGxBlendFactor source, WlGxBlendFactor destination) {
    GX_SetBlendMode((u8)mode, (u8)source, (u8)destination, GX_LO_CLEAR);
}

void wl_gx_direct_zmode(bool test, WlGxCompare func, bool write) {
    GX_SetZMode(test ? GX_TRUE : GX_FALSE, (u8)func, write ? GX_TRUE : GX_FALSE);
}

void wl_gx_direct_zcompare_early(bool early) {
    GX_SetZCompLoc(early ? GX_TRUE : GX_FALSE);
}

void wl_gx_direct_alpha_compare(WlGxCompare func, uint8_t ref) {
    GX_SetAlphaCompare((u8)func, ref, GX_AOP_AND, GX_ALWAYS, 0);
}

void wl_gx_direct_color_update(bool color, bool alpha) {
    GX_SetColorUpdate(color ? GX_TRUE : GX_FALSE);
    GX_SetAlphaUpdate(alpha ? GX_TRUE : GX_FALSE);
}

void wl_gx_direct_cull(WlGxCull cull) {
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
    if (tlut != NULL && (tex->format == WL_GX_TEX_C4 || tex->format == WL_GX_TEX_C8)) {
        GXTlutObj table;

        memcpy(&table, tlut->opaque, sizeof table);
        GX_LoadTlut(&table, tex->tlut);
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
    GX_LoadTexObj(&obj, (u8)(GX_TEXMAP0 + map));
    d.stats.textures++;
    wl_gx_wii_note_commands();
}

void wl_gx_direct_texture_invalidate(void) {
    GX_InvalidateTexAll();
}

void wl_gx_direct_stats(WlGxDirectStats *out) {
    *out = d.stats;
    out->flushes = wl_gx_direct_flushes();
}
