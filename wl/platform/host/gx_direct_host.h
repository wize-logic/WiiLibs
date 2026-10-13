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
} WlHostGxDirect;

const WlHostGxDirect *wl_host_gx_direct(void);
void wl_host_gx_direct_reset(void);
// a hook called with every draw (NULL: none), for a test that wants each one
void wl_host_gx_direct_hook(void (*hook)(void *user, WlGxDirectPrim prim, const WlGxDirectVertex *vertices,
                                         uint32_t count),
                            void *user);
