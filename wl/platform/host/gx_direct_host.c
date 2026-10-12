// SPDX-License-Identifier: MIT
// Host stub of the direct GX layer (gx_direct_host.h).
#include <stdlib.h>
#include <string.h>

#include "wl/platform/host/gx_direct_host.h"

static WlHostGxDirect s;
static bool g_flush_due, g_flush_off, g_layout_known;
static void (*g_hook)(void *user, WlGxDirectPrim prim, const WlGxDirectVertex *vertices, uint32_t count);
static void *g_hook_user;

const WlHostGxDirect *wl_host_gx_direct(void) {
    return &s;
}

void wl_host_gx_direct_reset(void) {
    memset(&s, 0, sizeof s);
    g_flush_due = g_layout_known = false;
}

void wl_host_gx_direct_hook(void (*hook)(void *user, WlGxDirectPrim prim, const WlGxDirectVertex *vertices,
                                         uint32_t count),
                            void *user) {
    g_hook = hook;
    g_hook_user = user;
}

void wl_gx_direct_begin(void) {
    wl_gx_invalidate_shadow();
    s.begins++;
    g_flush_due = true;
    g_layout_known = false;
}

void wl_gx_direct_flush_after(void) {
    g_flush_due = true;
}

void wl_gx_direct_set_xf_flush(bool on) {
    g_flush_off = !on;
}

void wl_gx_direct_layout(const WlGxDirectLayout *layout) {
    if (g_layout_known && memcmp(&s.layout, layout, sizeof s.layout) == 0) {
        return;
    }
    s.layout = *layout;
    g_layout_known = true;
    s.stats.layouts++;
}

static WlGxDirectVertex *g_vertices;
static uint32_t g_capacity, g_expected, g_received;
static bool g_in_primitive;

void wl_gx_direct_primitive_begin(WlGxDirectPrim prim, uint32_t count) {
    g_in_primitive = false;
    if (count == 0 || count > 65535u || !g_layout_known) {
        return;
    }
    if (count > g_capacity) {
        WlGxDirectVertex *grown = realloc(g_vertices, count * sizeof *grown);
        if (grown == NULL) {
            return;
        }
        g_vertices = grown;
        g_capacity = count;
    }
    if (g_flush_due && !g_flush_off) {
        s.stats.flushes++;
    }
    g_flush_due = false;
    g_in_primitive = true;
    g_expected = count;
    g_received = 0;
    s.prim = prim;
    s.count = count;
    s.stats.draws++;
    s.stats.vertices += count;
}

void wl_gx_direct_vertex(const WlGxDirectVertex *vertex) {
    if (g_in_primitive && g_received < g_expected) {
        g_vertices[g_received] = *vertex;
        if (g_received < WL_HOST_GX_DIRECT_KEPT) {
            s.vertices[g_received] = *vertex;
        }
        g_received++;
    }
}

void wl_gx_direct_primitive_end(void) {
    if (!g_in_primitive) {
        return;
    }
    g_in_primitive = false;
    if (g_received != g_expected) {
        s.short_draws++;
    }
    if (g_hook != NULL) {
        g_hook(g_hook_user, s.prim, g_vertices, g_received);
    }
}

void wl_gx_direct_draw(WlGxDirectPrim prim, const WlGxDirectVertex *vertices, uint32_t count) {
    uint32_t i;

    wl_gx_direct_primitive_begin(prim, count);
    for (i = 0; i < count; i++) {
        wl_gx_direct_vertex(&vertices[i]);
    }
    wl_gx_direct_primitive_end();
}

void wl_gx_direct_projection(const float m[4][4], bool orthographic) {
    memcpy(s.projection, m, sizeof s.projection);
    s.orthographic = orthographic;
}

void wl_gx_direct_viewport(float x, float y, float w, float h) {
    s.viewport[0] = x;
    s.viewport[1] = y;
    s.viewport[2] = w;
    s.viewport[3] = h;
}

void wl_gx_direct_scissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    s.scissor[0] = x;
    s.scissor[1] = y;
    s.scissor[2] = w;
    s.scissor[3] = h;
}

void wl_gx_direct_blend(WlGxBlendMode mode, WlGxBlendFactor source, WlGxBlendFactor destination) {
    s.blend_mode = mode;
    s.blend_source = source;
    s.blend_destination = destination;
}

void wl_gx_direct_zmode(bool test, WlGxCompare func, bool write) {
    s.z_test = test;
    s.z_func = func;
    s.z_write = write;
}

void wl_gx_direct_zcompare_early(bool early) {
    s.z_early = early;
}

void wl_gx_direct_alpha_compare(WlGxCompare func, uint8_t ref) {
    s.alpha_func = func;
    s.alpha_ref = ref;
}

void wl_gx_direct_color_update(bool color, bool alpha) {
    s.color_update = color;
    s.alpha_update = alpha;
}

void wl_gx_direct_cull(WlGxCull cull) {
    s.cull = cull;
}

void wl_gx_direct_texture(uint32_t map, const WlGxTexObj *tex, const WlGxSampler *sampler, const WlGxTlut *tlut) {
    (void)tlut;
    if (map >= 8) {
        return;
    }
    s.textures[map] = tex;
    s.samplers[map] = sampler != NULL ? *sampler : tex->sampler;
    s.stats.textures++;
}

void wl_gx_direct_texture_invalidate(void) {
    s.invalidates++;
}

void wl_gx_direct_stats(WlGxDirectStats *out) {
    *out = s.stats;
}
