// SPDX-License-Identifier: MIT
// Host stub of the direct GX layer (gx_direct_host.h).
#include <stdlib.h>
#include <string.h>

#include "wl/platform/host/gx_direct_host.h"
#include "wl/platform/host/gx_renderer_host.h"

static WlHostGxDirect s;
static bool g_flush_due, g_flush_off, g_layout_known;
// a renderer's own vertex formats: the sizes set, and the descriptor a test gives
static uint8_t g_format_bytes[8][WL_GX_VA_ATTRIBUTES];
static WlGxVtxDesc g_descriptor[WL_GX_VA_ATTRIBUTES + 1];
static unsigned g_descriptor_count;
static void (*g_hook)(void *user, WlGxDirectPrim prim, const WlGxDirectVertex *vertices, uint32_t count);
static void *g_hook_user;

const WlHostGxDirect *wl_host_gx_direct(void) {
    return &s;
}

void wl_host_gx_direct_reset(void) {
    memset(&s, 0, sizeof s);
    memset(g_format_bytes, 0, sizeof g_format_bytes);
    g_flush_due = g_layout_known = false;
    g_descriptor_count = 0;
}

void wl_host_gx_direct_hook(void (*hook)(void *user, WlGxDirectPrim prim, const WlGxDirectVertex *vertices,
                                         uint32_t count),
                            void *user) {
    g_hook = hook;
    g_hook_user = user;
}

void wl_gx_direct_begin(void) {
    unsigned n;

    wl_gx_invalidate_shadow();
    s.begins++;
    s.position_matrix_set = false;
    for (n = 0; n < 8; n++) {
        s.texgen[n].set = false;
    }
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
    unsigned n;

    if (g_layout_known && memcmp(&s.layout, layout, sizeof s.layout) == 0) {
        return;
    }
    s.layout = *layout;
    for (n = 0; n < 8; n++) {
        s.texgen[n].set = false;
    }
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

void wl_gx_direct_vertex_words(const uint32_t *words, uint32_t count) {
    WlGxDirectVertex v;
    uint32_t i = 3, n;

    // (the words back into a vertex, by the layout, as the FIFO would read them)
    memset(&v, 0, sizeof v);
    if (count < 3) {
        return;
    }
    memcpy(v.position, words, sizeof v.position);
    for (n = 0; n < s.layout.colors && n < 2 && i < count; n++, i++) {
        v.color[n][0] = (uint8_t)(words[i] >> 24);
        v.color[n][1] = (uint8_t)(words[i] >> 16);
        v.color[n][2] = (uint8_t)(words[i] >> 8);
        v.color[n][3] = (uint8_t)words[i];
    }
    for (n = 0; n < 8; n++) {
        if (!(s.layout.texcoords & (1u << n))) {
            continue;
        }
        if (i < count) {
            memcpy(&v.texcoord[n][0], &words[i++], sizeof(float));
        }
        if (!(s.layout.texcoord_s & (1u << n)) && i < count) {
            memcpy(&v.texcoord[n][1], &words[i++], sizeof(float));
        }
    }
    wl_gx_direct_vertex(&v);
}

void wl_gx_direct_array(unsigned attribute, const void *data, uint32_t stride) {
    if (attribute < 11u) {
        s.arrays[attribute] = data;
        s.array_strides[attribute] = stride;
    }
}

static float be_float(const uint8_t *p) {
    uint32_t bits = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    float f;

    memcpy(&f, &bits, sizeof f);
    return f;
}

// an indexed attribute's element, or NULL when its array was not set
static const uint8_t *element(unsigned attribute, uint16_t index) {
    const uint8_t *array = s.arrays[attribute];

    return array != NULL ? array + (size_t)index * s.array_strides[attribute] : NULL;
}

void wl_gx_direct_vertex_index(uint16_t index, const uint32_t *words, uint32_t count) {
    WlGxDirectVertex v;
    const uint8_t *e;
    uint32_t i = 0, n;

    // (the vertex as GX would read it: an indexed attribute from its array, the others' words)
    memset(&v, 0, sizeof v);
    if (s.layout.indexed & (1u << WL_GX_DIRECT_ARRAY_POSITION)) {
        if ((e = element(WL_GX_DIRECT_ARRAY_POSITION, index)) != NULL) {
            v.position[0] = be_float(e);
            v.position[1] = be_float(e + 4);
            v.position[2] = be_float(e + 8);
        }
    } else {
        for (n = 0; n < 3 && i < count; n++) {
            memcpy(&v.position[n], &words[i++], sizeof(float));
        }
    }
    for (n = 0; n < s.layout.colors && n < 2; n++) {
        if (s.layout.indexed & (1u << (WL_GX_DIRECT_ARRAY_COLOR0 + n))) {
            if ((e = element(WL_GX_DIRECT_ARRAY_COLOR0 + n, index)) != NULL) {
                memcpy(v.color[n], e, 4);
            }
        } else if (i < count) {
            v.color[n][0] = (uint8_t)(words[i] >> 24);
            v.color[n][1] = (uint8_t)(words[i] >> 16);
            v.color[n][2] = (uint8_t)(words[i] >> 8);
            v.color[n][3] = (uint8_t)words[i];
            i++;
        }
    }
    for (n = 0; n < 8; n++) {
        bool one = (s.layout.texcoord_s & (1u << n)) != 0;

        if (!(s.layout.texcoords & (1u << n))) {
            continue;
        }
        if (s.layout.indexed & (1u << (WL_GX_DIRECT_ARRAY_TEXCOORD0 + n))) {
            if ((e = element(WL_GX_DIRECT_ARRAY_TEXCOORD0 + n, index)) != NULL) {
                v.texcoord[n][0] = be_float(e);
                v.texcoord[n][1] = one ? 0.0f : be_float(e + 4);
            }
            continue;
        }
        if (i < count) {
            memcpy(&v.texcoord[n][0], &words[i++], sizeof(float));
        }
        if (!one && i < count) {
            memcpy(&v.texcoord[n][1], &words[i++], sizeof(float));
        }
    }
    if (g_in_primitive) {
        s.indexed_vertices++;
    }
    wl_gx_direct_vertex(&v);
}

void wl_gx_direct_arrays_changed(void) {
    s.arrays_changed++;
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
    {
        const WlHostGxRenderer *r = wl_host_gx_renderer();

        if (r != NULL && r->draw != NULL) {
            r->draw(r->user, s.prim, g_vertices, g_received);
        }
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

void wl_gx_direct_position_matrix(const float m[3][4]) {
    s.position_matrix_set = m != NULL;
    if (m != NULL) {
        memcpy(s.position_matrix, m, sizeof s.position_matrix);
    }
}

void wl_gx_direct_texgen(unsigned n, WlGxDirectTexSource source, const float m[2][4]) {
    if (n >= 8) {
        return;
    }
    s.texgen[n].set = m != NULL;
    s.texgen[n].source = (uint8_t)source;
    if (m != NULL) {
        memcpy(s.texgen[n].m, m, sizeof s.texgen[n].m);
    }
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
    if (map >= 8) {
        return;
    }
    s.textures[map] = tex;
    s.samplers[map] = sampler != NULL ? *sampler : tex->sampler;
    s.map_kind[map] = WL_HOST_GX_MAP_TEXTURE;
    s.map_objects[map] = *tex;
    s.map_objects[map].sampler = s.samplers[map];
    if (tlut != NULL && (tex->format == WL_GX_TEX_C4 || tex->format == WL_GX_TEX_C8)) {
        // (a TLUT slot holds the last palette loaded into it, whichever map's object reads it)
        unsigned other;

        for (other = 0; other < 8; other++) {
            if (s.map_kind[other] == WL_HOST_GX_MAP_TEXTURE && s.map_objects[other].tlut == tex->tlut) {
                s.tluts[other] = *tlut;
                s.tlut_loaded[other] = true;
            }
        }
    }
    s.stats.textures++;
}

void wl_host_gx_direct_ramp(uint32_t map) {
    if (map < 8) {
        s.map_kind[map] = WL_HOST_GX_MAP_RAMP;
        s.textures[map] = NULL;
    }
}

void wl_gx_direct_texture_invalidate(void) {
    const WlHostGxRenderer *r = wl_host_gx_renderer();

    s.invalidates++;
    if (r != NULL && r->texture_invalidate != NULL) {
        r->texture_invalidate(r->user);
    }
}

void wl_gx_direct_stats(WlGxDirectStats *out) {
    *out = s.stats;
}

// ---- a renderer's own vertex formats ---------------------------------------------------------------

void wl_host_gx_direct_descriptor(const WlGxVtxDesc *desc, unsigned count) {
    if (count > WL_GX_VA_ATTRIBUTES + 1) {
        count = WL_GX_VA_ATTRIBUTES + 1;
    }
    memcpy(g_descriptor, desc, count * sizeof *desc);
    g_descriptor_count = count;
}

void wl_gx_direct_vertex_format(uint8_t format, uint32_t attribute, uint32_t count, uint32_t type, uint32_t frac) {
    (void)frac;
    if (attribute < WL_GX_VA_ATTRIBUTES) {
        g_format_bytes[format & 7u][attribute] = wl_gx_attribute_bytes(attribute, count, type);
    }
}

uint32_t wl_gx_direct_vertex_bytes(uint8_t format) {
    return wl_gx_vertex_bytes(g_descriptor, g_descriptor_count, g_format_bytes[format & 7u]);
}

bool wl_gx_direct_vertex_shape_ok(void) {
    return wl_gx_vertex_shape_ok(g_descriptor, g_descriptor_count);
}

WlGxFlushResult wl_gx_direct_flush_pending(uint8_t format) {
    bool send = g_flush_due && !g_flush_off;
    uint32_t bytes;

    g_flush_due = false;
    if (!send) {
        return WL_GX_FLUSH_NOT_DUE;
    }
    bytes = wl_gx_direct_vertex_bytes(format) * 8u;
    if (bytes == 0) {
        return WL_GX_FLUSH_SIZE_UNKNOWN;
    }
    s.stats.flushes++;
    s.flush_bytes = bytes;
    return WL_GX_FLUSH_SENT;
}

void wl_gx_direct_begin_format(uint8_t gx_primitive, uint8_t format, uint16_t count) {
    wl_gx_direct_flush_pending(format);
    s.format_begins++;
    s.format_primitive = gx_primitive;
    s.format = format;
    s.format_count = count;
}

uint32_t wl_gx_direct_flushes(void) {
    return s.stats.flushes;
}
