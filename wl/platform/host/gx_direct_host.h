// SPDX-License-Identifier: MIT
// Host stub of the direct GX layer (gx_direct.h): what was asked, kept for tests; nothing is drawn
// here, but a renderer set with wl_host_gx_set_renderer (gx_renderer_host.h) draws each primitive
// with the state kept.
#pragma once

#include "wl/platform/gx_direct.h"

enum { WL_HOST_GX_DIRECT_KEPT = 256 };
enum { WL_HOST_GX_MAP_NONE = 0, WL_HOST_GX_MAP_TEXTURE, WL_HOST_GX_MAP_RAMP };

typedef struct WlHostGxDirect {
    WlGxDirectLayout layout;
    // the transform unit's matrices (wl_gx_direct_position_matrix, wl_gx_direct_texgen): set or
    // the identity
    bool position_matrix_set;
    float position_matrix[3][4];
    struct {
        bool set;
        uint8_t source; // WlGxDirectTexSource
        float m[2][4];
    } texgen[8];
    // the lit channels' light (wl_gx_direct_lighting), applied to the vertices as they arrive
    WlGxDirectLighting lighting;
    float projection[4][4];
    bool orthographic;
    float viewport[4];
    uint32_t scissor[4];
    WlGxBlendMode blend_mode;
    WlGxBlendFactor blend_source, blend_destination;
    bool z_test, z_write, z_early;
    WlGxCompare z_func, alpha_func;
    uint8_t alpha_ref;
    bool color_update, alpha_update;
    WlGxCull cull;
    const WlGxTexObj *textures[8];
    WlGxSampler samplers[8];
    // each texture map as GX holds it: what was loaded last (WL_HOST_GX_MAP_*), a copy of the
    // object, and a colour-indexed one's TLUT
    uint8_t map_kind[8];
    WlGxTexObj map_objects[8];
    WlGxTlut tluts[8];
    bool tlut_loaded[8];
    uint32_t begins, invalidates;
    // primitives that got fewer vertices than they said
    uint32_t short_draws;
    // the last draw's primitive, and its first vertices (up to WL_HOST_GX_DIRECT_KEPT)
    WlGxDirectPrim prim;
    uint32_t count;
    WlGxDirectVertex vertices[WL_HOST_GX_DIRECT_KEPT];
    WlGxDirectStats stats;
    // the indexed attributes' arrays (wl_gx_direct_array: WL_GX_DIRECT_ARRAY_*), the vertices sent
    // through indices, and how often the arrays were said to have changed
    const void *arrays[12];
    uint32_t array_strides[12];
    uint32_t indexed_vertices, arrays_changed;
    // a renderer's own vertex formats: its primitives begun (wl_gx_direct_begin_format), the last's
    // GX primitive, format and count, and the bytes of the last flush sent before one
    uint32_t format_begins;
    uint8_t format_primitive, format;
    uint16_t format_count;
    uint32_t flush_bytes;
} WlHostGxDirect;

const WlHostGxDirect *wl_host_gx_direct(void);
void wl_host_gx_direct_reset(void);
// a hook called with every draw (NULL: none), for a test that wants each one
void wl_host_gx_direct_hook(void (*hook)(void *user, WlGxDirectPrim prim, const WlGxDirectVertex *vertices,
                                         uint32_t count),
                            void *user);
// the vertex descriptor GX would hold (GX_GetVtxDescv's list), for a test of the flush before a
// renderer's own vertex formats: the host has no GX_SetVtxDesc
void wl_host_gx_direct_descriptor(const WlGxVtxDesc *desc, unsigned count);
