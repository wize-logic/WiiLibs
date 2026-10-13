// SPDX-License-Identifier: MIT
// The transform unit's flush over libogc (gx_direct.h): its state, and the flush for a renderer's
// own vertex formats. Moved from OpenCE-Wii's d3d8_gx.c (CC0: xf_flush_after, vertex_format,
// vertex_bytes, gx_flush_pending, gx_begin). It calls libogc's GX alone, so a renderer's host
// harness over a stand-in of GX (OpenCE-Wii's tools/wii_draw_bench.py) links it as it is.
#include <ogc/gx.h>

#include "wl/platform/gx_direct.h"
#include "wl/platform/wii/gx_flush_wii.h"

#define XF_FLUSH_VERTICES 8

_Static_assert(WL_GX_VA_TEX7MTXIDX == GX_VA_TEX7MTXIDX && WL_GX_VA_POS == GX_VA_POS &&
                   WL_GX_VA_NRM == GX_VA_NRM && WL_GX_VA_CLR0 == GX_VA_CLR0 &&
                   WL_GX_VA_CLR1 == GX_VA_CLR1 && WL_GX_VA_TEX0 == GX_VA_TEX0 &&
                   WL_GX_VA_TEX7 == GX_VA_TEX7 && WL_GX_VA_ATTRIBUTES == GX_VA_MAXATTR &&
                   WL_GX_VA_END == GX_VA_NULL,
               "vertex attributes are GX's");
_Static_assert(WL_GX_IN_NONE == GX_NONE && WL_GX_IN_DIRECT == GX_DIRECT &&
                   WL_GX_IN_INDEX8 == GX_INDEX8 && WL_GX_IN_INDEX16 == GX_INDEX16,
               "vertex inputs are GX's");
_Static_assert(WL_GX_TYPE_U8 == GX_U8 && WL_GX_TYPE_S16 == GX_S16 && WL_GX_TYPE_F32 == GX_F32 &&
                   WL_GX_COUNT_POS_XY == GX_POS_XY && WL_GX_COUNT_POS_XYZ == GX_POS_XYZ &&
                   WL_GX_COUNT_NRM_XYZ == GX_NRM_XYZ && WL_GX_COUNT_TEX_S == GX_TEX_S,
               "vertex formats are GX's");
_Static_assert(sizeof(GXVtxDesc) == sizeof(WlGxVtxDesc), "a descriptor entry is two bytes");

static struct {
    bool due, off;
    uint32_t flushes;
    // the bytes of each attribute sent directly, by vertex format, as wl_gx_direct_vertex_format
    // set them (0: not set through it)
    uint8_t bytes[8][WL_GX_VA_ATTRIBUTES];
} f;

void wl_gx_direct_flush_after(void) {
    f.due = true;
}

void wl_gx_direct_set_xf_flush(bool on) {
    f.off = !on;
}

bool wl_gx_flush_wii_off(void) {
    return f.off;
}

bool wl_gx_flush_wii_take(void) {
    bool send = f.due && !f.off;

    f.due = false;
    return send;
}

void wl_gx_flush_wii_count(void) {
    f.flushes++;
}

uint32_t wl_gx_direct_flushes(void) {
    return f.flushes;
}

void wl_gx_direct_vertex_format(uint8_t format, uint32_t attribute, uint32_t count, uint32_t type,
                                uint32_t frac) {
    GX_SetVtxAttrFmt(format, attribute, count, type, frac);
    if (attribute < WL_GX_VA_ATTRIBUTES) {
        f.bytes[format & 7u][attribute] = wl_gx_attribute_bytes(attribute, count, type);
    }
}

// the descriptor as GX_Begin will send it (libogc's own record of it)
static unsigned descriptor(WlGxVtxDesc out[GX_MAX_VTXDESC_LISTSIZE]) {
    GXVtxDesc list[GX_MAX_VTXDESC_LISTSIZE];
    unsigned i;

    GX_GetVtxDescv(list);
    for (i = 0; i < GX_MAX_VTXDESC_LISTSIZE; i++) {
        out[i].attr = list[i].attr;
        out[i].type = list[i].type;
    }
    return GX_MAX_VTXDESC_LISTSIZE;
}

uint32_t wl_gx_direct_vertex_bytes(uint8_t format) {
    WlGxVtxDesc list[GX_MAX_VTXDESC_LISTSIZE];
    unsigned count = descriptor(list);

    return wl_gx_vertex_bytes(list, count, f.bytes[format & 7u]);
}

bool wl_gx_direct_vertex_shape_ok(void) {
    WlGxVtxDesc list[GX_MAX_VTXDESC_LISTSIZE];
    unsigned count = descriptor(list);

    return wl_gx_vertex_shape_ok(list, count);
}

WlGxFlushResult wl_gx_direct_flush_pending(uint8_t format) {
    unsigned long bytes;

    if (!wl_gx_flush_wii_take()) {
        return WL_GX_FLUSH_NOT_DUE;
    }
    bytes = (unsigned long)wl_gx_direct_vertex_bytes(format) * XF_FLUSH_VERTICES;
    if (bytes == 0) {
        return WL_GX_FLUSH_SIZE_UNKNOWN;
    }
    // (zeros, a word at a time and then bytes: all one point, which draws nothing)
    GX_Begin(GX_TRIANGLESTRIP, format, XF_FLUSH_VERTICES);
    for (; bytes >= 4; bytes -= 4) {
        GX_Color1u32(0);
    }
    for (; bytes != 0; bytes--) {
        GX_Position1x8(0);
    }
    GX_End();
    f.flushes++;
    return WL_GX_FLUSH_SENT;
}

void wl_gx_direct_begin_format(uint8_t gx_primitive, uint8_t format, uint16_t count) {
    wl_gx_direct_flush_pending(format);
    GX_Begin(gx_primitive, format, count);
}
