// SPDX-License-Identifier: MIT
// Host stub of the direct GX layer (gx_direct_host.h).
#include <stdlib.h>
#include <string.h>

#include "wl/platform/host/gx_direct_host.h"

#include "wl/platform/gx_shadow.h"
#include "wl/platform/host/gx_renderer_host.h"
#include "wl/platform/host/tev_host.h"

static WlHostGxDirect s;
static bool g_flush_due, g_flush_off, g_layout_known;
// the console's commands counted alike (gx_shadow.h): the position matrix as loaded (identity or not),
// a vertex's bytes in the layout
static bool g_position_identity = true;
static uint32_t g_vertex_bytes;
// the channels' material registers (wl_gx_direct_material), the colours a layout takes from them
static uint8_t g_material[2][4], g_material_known;
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
    g_position_identity = true;
    for (n = 0; n < 8; n++) {
        s.texgen[n].set = false;
    }
    g_flush_due = true;
    g_layout_known = false;
    g_material_known = 0;
}

void wl_gx_direct_flush_after(void) {
    g_flush_due = true;
}

void wl_gx_direct_set_xf_flush(bool on) {
    g_flush_off = !on;
}

// (the Wii's bisection switches: kept, and nothing on the host left out for them)
static WlGxBisect g_bisect;

void wl_gx_bisect_set(const WlGxBisect *bisect) {
    g_bisect = *bisect;
}

const WlGxBisect *wl_gx_bisect(void) {
    return &g_bisect;
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
    {
        uint32_t chans, texgens;

        wl_gx_direct_layout_outputs(layout, &chans, &texgens);
        g_vertex_bytes = wl_gx_shadow_layout_vertex_bytes(layout);
        wl_gx_shadow_fifo(WL_GX_FIFO_LAYOUT, wl_gx_shadow_layout_bytes(texgens, chans, layout->lit != 0));
    }
}

void wl_platform_heap_exempt(bool on) __attribute__((weak));

static WlGxDirectVertex *g_vertices;
static uint32_t g_capacity, g_expected, g_received;
static bool g_in_primitive;

void wl_gx_direct_primitive_begin(WlGxDirectPrim prim, uint32_t count) {
    g_in_primitive = false;
    if (count == 0 || count > 65535u || !g_layout_known) {
        return;
    }
    if (count > g_capacity) {
        // (the recorder is the host's alone: kept out of the host heap's count, mem_census.h)
        if (wl_platform_heap_exempt != NULL) {
            wl_platform_heap_exempt(true);
        }
        WlGxDirectVertex *grown = realloc(g_vertices, count * sizeof *grown);
        if (wl_platform_heap_exempt != NULL) {
            wl_platform_heap_exempt(false);
        }
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
    wl_gx_shadow_fifo(WL_GX_FIFO_VERTEX, 3u + count * g_vertex_bytes);
    g_expected = count;
    g_received = 0;
    s.prim = prim;
    s.count = count;
    s.stats.draws++;
    s.stats.vertices += count;
    {
        uint32_t chans, texgens, maps = 0, found, bit;

        wl_gx_direct_layout_outputs(&s.layout, &chans, &texgens);
        for (bit = 0; bit < 8; bit++) {
            maps |= s.map_kind[bit] != WL_HOST_GX_MAP_NONE ? 1u << bit : 0u;
        }
        found = wl_gx_check_draw(chans, texgens, maps, wl_host_tev_last());
        for (bit = 0; found != 0 && bit < WL_GX_CHECKS; bit++) {
            if ((found & (1u << bit)) && s.check_draws[bit]++ == 0) {
                s.check_first_draw[bit] = s.stats.draws;
            }
        }
    }
}

// a lit channel's component as GX's lighting makes it: the ambient plus the signed dot product of the
// normal (through the normal matrix) and the light's direction, times the light's colour, clamped
static uint8_t lit_component(const float normal[3], unsigned channel, unsigned component) {
    const WlGxDirectLighting *l = &s.lighting;
    float value = (float)l->ambient[channel / 2u][component] / 255.0f, n[3];
    unsigned r, k;

    for (r = 0; r < 3; r++) {
        n[r] = l->normal_matrix[r][0] * normal[0] + l->normal_matrix[r][1] * normal[1] +
               l->normal_matrix[r][2] * normal[2];
    }
    // (its own light, then its extras: light 4 + k)
    for (k = 0; k < 5u; k++) {
        const float *direction = k == 0 ? l->direction[channel] : l->extra_direction[k - 1u];
        const uint8_t *color = k == 0 ? l->color[channel] : l->extra_color[k - 1u];
        float dot;

        if (k != 0 && !(l->extra[channel] & (1u << (k - 1u)))) {
            continue;
        }
        dot = n[0] * direction[0] + n[1] * direction[1] + n[2] * direction[2];
        if ((l->clamped & (1u << channel)) && !(dot > 0.0f)) {
            dot = 0.0f;
        }
        value += dot * ((float)color[component] / 255.0f);
    }
    value = value > 1.0f ? 1.0f : value > 0.0f ? value : 0.0f;
    return (uint8_t)(value * 255.0f + 0.5f);
}

void wl_gx_direct_vertex(const WlGxDirectVertex *vertex) {
    if (g_in_primitive && g_received < g_expected) {
        g_vertices[g_received] = *vertex;
        if (s.layout.lit) {
            WlGxDirectVertex *v = &g_vertices[g_received];
            unsigned channel, c;

            for (channel = 0; channel < 4u; channel++) {
                if (!(s.layout.lit & (1u << channel))) {
                    continue;
                }
                if (channel & 1u) {
                    v->color[channel / 2u][3] = lit_component(v->normal, channel, 3);
                } else {
                    for (c = 0; c < 3u; c++) {
                        v->color[channel / 2u][c] = lit_component(v->normal, channel, c);
                    }
                }
            }
            vertex = v;
        }
        if (g_received < WL_HOST_GX_DIRECT_KEPT) {
            s.vertices[g_received] = *vertex;
        }
        g_received++;
    }
}

// the bytes of a vertex GX reads in a layout, as the Wii's direct layer describes it
// (wii/gx_direct_wii.c: a 16-bit index for an indexed attribute; floats; colours RGBA8), and
// whether the layout is one that layer can describe at all
static uint32_t wire_bytes(const WlGxDirectLayout *layout, bool *describable, uint32_t *indexes) {
    uint32_t bytes = 0;
    unsigned n, projected = 0;

#define WL_WIRE_INDEXED(attribute) ((layout->indexed & (1u << (attribute))) ? ((*indexes)++, true) : false)
    *indexes = 0;
    bytes += WL_WIRE_INDEXED(WL_GX_DIRECT_ARRAY_POSITION) ? 2u : 12u;

    for (n = 0; n < 8; n++) {
        projected += (layout->texcoords_projected >> n) & 1u;
    }
    // (a lit layout's normal and the projected coordinates: no more than the normal, binormal and
    // tangent hold, the normal not indexed beside projected ones; a sphere map's a projected one)
    *describable = (layout->texcoords_sphere & (uint8_t)~layout->texcoords_projected) == 0 &&
                   !(layout->lit && projected != 0 && (layout->indexed & (1u << WL_GX_DIRECT_ARRAY_NORMAL))) &&
                   projected + (layout->lit ? 1u : 0u) <= WL_GX_DIRECT_PROJECTED;
    projected += layout->lit && projected != 0 ? 1u : 0u;
    if (projected == 1u) {
        bytes += 12u;
    } else if (projected > 1u) {
        bytes += 36u;
    } else if (layout->lit) {
        bytes += WL_WIRE_INDEXED(WL_GX_DIRECT_ARRAY_NORMAL) ? 2u : 12u;
    }
    for (n = 0; n < layout->colors && n < 2u; n++) {
        bytes += (layout->colors_register & (1u << n)) ? 0u : WL_WIRE_INDEXED(WL_GX_DIRECT_ARRAY_COLOR0 + n) ? 2u : 4u;
    }
    for (n = 0; n < 8; n++) {
        if (layout->texcoords & (1u << n)) {
            bytes += WL_WIRE_INDEXED(WL_GX_DIRECT_ARRAY_TEXCOORD0 + n) ? 2u
                     : (layout->texcoord_s & (1u << n))                             ? 4u
                                                                                    : 8u;
        }
    }
#undef WL_WIRE_INDEXED
    return bytes;
}

static void wire_check(uint32_t sent) {
    bool describable;
    uint32_t indexes, read = wire_bytes(&s.layout, &describable, &indexes);

    if (sent == read && describable) {
        return;
    }
    if (s.wire_mismatches++ == 0) {
        s.wire_first_draw = s.stats.draws;
        s.wire_first_sent = sent;
        s.wire_first_read = describable ? read : 0;
        s.wire_first_layout = s.layout;
    }
}

void wl_gx_direct_vertex_words(const uint32_t *words, uint32_t count) {
    WlGxDirectVertex v;
    uint32_t i = 3, n;

    // (the words back into a vertex, by the layout, as the FIFO would read them)
    memset(&v, 0, sizeof v);
    if (g_in_primitive) {
        wire_check(count * 4u);
    }
    if (count < 3) {
        return;
    }
    memcpy(v.position, words, sizeof v.position);
    if (s.layout.lit && i + 3u <= count) {
        memcpy(v.normal, &words[i], sizeof v.normal);
        i += 3u;
    }
    if (s.layout.texcoords_projected) {
        unsigned projected = s.layout.lit ? 1u : 0u, k;

        // (the projected coordinates' (s, t, q), as the normal or normal, binormal and tangent)
        for (n = 0; n < 8; n++) {
            if ((s.layout.texcoords_projected & (1u << n)) && i + 3u <= count) {
                memcpy(&v.texcoord[n][0], &words[i], sizeof(float));
                memcpy(&v.texcoord[n][1], &words[i + 1u], sizeof(float));
                memcpy(&v.texcoord_q[n], &words[i + 2u], sizeof(float));
                i += 3u;
                projected++;
            }
        }
        for (k = projected; projected > 1u && k < 3u; k++) {
            i += 3u;
        }
    }
    for (n = 0; n < s.layout.colors && n < 2; n++) {
        if (s.layout.colors_register & (1u << n)) {
            // (GX's channel reads the register: the colour every vertex would have sent)
            memcpy(v.color[n], g_material[n], 4);
        } else if (i < count) {
            v.color[n][0] = (uint8_t)(words[i] >> 24);
            v.color[n][1] = (uint8_t)(words[i] >> 16);
            v.color[n][2] = (uint8_t)(words[i] >> 8);
            v.color[n][3] = (uint8_t)words[i];
            i++;
        }
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

void wl_gx_direct_vertices_words(const uint32_t *const *vertices, uint32_t n, uint32_t count) {
    uint32_t v;

    for (v = 0; v < n; v++) {
        wl_gx_direct_vertex_words(vertices[v], count);
    }
}

void wl_gx_direct_vertices_index(const uint32_t *indices, uint32_t base, uint32_t n, const uint32_t *words,
                                 uint32_t count) {
    uint32_t v;

    for (v = 0; v < n; v++) {
        wl_gx_direct_vertex_index((uint16_t)(indices != NULL ? indices[v] - base : v), words, count);
    }
}

void wl_gx_direct_array(unsigned attribute, const void *data, uint32_t stride) {
    if (attribute <= WL_GX_DIRECT_ARRAY_NORMAL) {
        (void)wl_gx_shadow_array(attribute, data, stride, g_bisect.arrays_always); // (counted)
    }
    if (attribute < 12u) {
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
    if (g_in_primitive) {
        // (sent: an index for each indexed attribute, then the words while they last)
        bool describable;
        uint32_t indexes, read = wire_bytes(&s.layout, &describable, &indexes), direct;

        direct = read > indexes * 2u ? (read - indexes * 2u) / 4u : 0;
        wire_check(indexes * 2u + 4u * (count < direct ? count : direct));
    }
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
    if (s.layout.lit && (s.layout.indexed & (1u << WL_GX_DIRECT_ARRAY_NORMAL))) {
        if ((e = element(WL_GX_DIRECT_ARRAY_NORMAL, index)) != NULL) {
            v.normal[0] = be_float(e);
            v.normal[1] = be_float(e + 4);
            v.normal[2] = be_float(e + 8);
        }
    } else if (s.layout.lit) {
        for (n = 0; n < 3 && i < count; n++) {
            memcpy(&v.normal[n], &words[i++], sizeof(float));
        }
    }
    for (n = 0; n < s.layout.colors && n < 2; n++) {
        if (s.layout.colors_register & (1u << n)) {
            memcpy(v.color[n], g_material[n], 4);
        } else if (s.layout.indexed & (1u << (WL_GX_DIRECT_ARRAY_COLOR0 + n))) {
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

void wl_gx_direct_material(unsigned c, uint32_t rgba) {
    const uint8_t bytes[4] = {(uint8_t)(rgba >> 24), (uint8_t)(rgba >> 16), (uint8_t)(rgba >> 8), (uint8_t)rgba};

    if (c > 1u || ((g_material_known & (1u << c)) && memcmp(g_material[c], bytes, 4) == 0)) {
        return;
    }
    memcpy(g_material[c], bytes, 4);
    g_material_known |= (uint8_t)(1u << c);
    wl_gx_shadow_fifo(WL_GX_FIFO_STATE, WL_GX_FIFO_MATERIAL_BYTES);
}

void wl_gx_direct_vertices_position_index(const uint32_t *indices, uint32_t base, uint32_t first, uint32_t n,
                                          const uint32_t *const *vertices, uint32_t skip, uint32_t count) {
    uint32_t v;

    for (v = 0; v < n; v++) {
        wl_gx_direct_vertex_index((uint16_t)(indices != NULL ? indices[first + v] - base : first + v), vertices[v] + skip,
                                  count);
    }
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

void wl_gx_direct_lighting(const WlGxDirectLighting *lighting) {
    s.lighting = *lighting;
    g_material_known = 0; // (the Wii's lighting sets the materials white)
}

void wl_gx_direct_position_matrix(const float m[3][4]) {
    // (the console's load counted: sent unless GX holds it)
    if (m == NULL ? !g_position_identity
                  : g_position_identity || memcmp(s.position_matrix, m, sizeof s.position_matrix) != 0) {
        wl_gx_shadow_fifo(WL_GX_FIFO_MATRIX, WL_GX_FIFO_POSITION_BYTES);
    }
    g_position_identity = m == NULL;
    s.position_matrix_set = m != NULL;
    if (m != NULL) {
        memcpy(s.position_matrix, m, sizeof s.position_matrix);
    }
}

void wl_gx_direct_texgen(unsigned n, WlGxDirectTexSource source, const float m[2][4]) {
    if (n >= 8) {
        return;
    }
    // (the console's loads counted: a matrix and its generator, or the generator back to the identity)
    if (m == NULL) {
        wl_gx_shadow_fifo(WL_GX_FIFO_MATRIX, s.texgen[n].set ? WL_GX_FIFO_TEXGEN_BYTES : 0u);
    } else if (!s.texgen[n].set || s.texgen[n].source != (uint8_t)source ||
               memcmp(s.texgen[n].m, m, sizeof s.texgen[n].m) != 0) {
        wl_gx_shadow_fifo(WL_GX_FIFO_MATRIX, WL_GX_FIFO_TEXMTX_BYTES + WL_GX_FIFO_TEXGEN_BYTES);
    }
    s.texgen[n].set = m != NULL;
    s.texgen[n].source = (uint8_t)source;
    if (m != NULL) {
        memcpy(s.texgen[n].m, m, sizeof s.texgen[n].m);
    }
}

void wl_gx_direct_projection(const float m[4][4], bool orthographic) {
    float value[17];

    memcpy(value, m, 16 * sizeof(float));
    value[16] = orthographic ? 1.0f : 0.0f;

    (void)wl_gx_shadow_differs(WL_GX_SHADOW_PROJECTION, value, sizeof value); // (counted)
    memcpy(s.projection, m, sizeof s.projection);
    s.orthographic = orthographic;
}

void wl_gx_direct_viewport(float x, float y, float w, float h) {
    const float value[4] = {x, y, w, h};

    (void)wl_gx_shadow_differs(WL_GX_SHADOW_VIEWPORT, value, sizeof value); // (counted)
    s.viewport[0] = x;
    s.viewport[1] = y;
    s.viewport[2] = w;
    s.viewport[3] = h;
}

void wl_gx_direct_scissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    const uint32_t value[4] = {x, y, w, h};

    (void)wl_gx_shadow_differs(WL_GX_SHADOW_SCISSOR, value, sizeof value); // (counted)
    s.scissor[0] = x;
    s.scissor[1] = y;
    s.scissor[2] = w;
    s.scissor[3] = h;
}

void wl_gx_direct_blend(WlGxBlendMode mode, WlGxBlendFactor source, WlGxBlendFactor destination) {
    const uint8_t value[3] = {(uint8_t)mode, (uint8_t)source, (uint8_t)destination};

    (void)wl_gx_shadow_differs(WL_GX_SHADOW_BLEND, value, sizeof value); // (counted)
    s.blend_mode = mode;
    s.blend_source = source;
    s.blend_destination = destination;
}

void wl_gx_direct_zmode(bool test, WlGxCompare func, bool write) {
    const uint8_t value[3] = {test, (uint8_t)func, write};

    (void)wl_gx_shadow_differs(WL_GX_SHADOW_ZMODE, value, sizeof value); // (counted)
    s.z_test = test;
    s.z_func = func;
    s.z_write = write;
}

void wl_gx_direct_zcompare_early(bool early) {
    const uint8_t value = early;

    (void)wl_gx_shadow_differs(WL_GX_SHADOW_ZEARLY, &value, sizeof value); // (counted)
    s.z_early = early;
}

void wl_gx_direct_alpha_compare(WlGxCompare func, uint8_t ref) {
    const uint8_t value[2] = {(uint8_t)func, ref};

    (void)wl_gx_shadow_differs(WL_GX_SHADOW_ALPHA_COMPARE, value, sizeof value); // (counted)
    s.alpha_func = func;
    s.alpha_ref = ref;
}

void wl_gx_direct_color_update(bool color, bool alpha) {
    const uint8_t value[2] = {color, alpha};

    (void)wl_gx_shadow_differs(WL_GX_SHADOW_COLOR_UPDATE, value, sizeof value); // (counted)
    s.color_update = color;
    s.alpha_update = alpha;
}

void wl_gx_direct_cull(WlGxCull cull) {
    const uint8_t value = (uint8_t)cull;

    (void)wl_gx_shadow_differs(WL_GX_SHADOW_CULL, &value, sizeof value); // (counted)
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
    {
        // (the console's registers counted, gx_shadow.h: the wraps and filters as the Wii binds them)
        const WlGxSampler *sm = &s.samplers[map];
        bool pow2 = (tex->width & (tex->width - 1u)) == 0 && (tex->height & (tex->height - 1u)) == 0;
        uint32_t regs[WL_GX_TEXREGS], minf = sm->min_filter;
        bool ci = tex->format == WL_GX_TEX_C4 || tex->format == WL_GX_TEX_C8;

        if (tex->mips <= 1u && minf >= WL_GX_FILTER_NEAREST_MIP_NEAREST) {
            minf = (minf & 1u) != 0 ? WL_GX_FILTER_LINEAR : WL_GX_FILTER_NEAREST;
        }
        wl_gx_shadow_texture_model(regs, (uintptr_t)wl_host_gx_tex_image(tex), tex->format, tex->width, tex->height,
                                   tex->mips, pow2 ? sm->wrap_s : WL_GX_WRAP_CLAMP, pow2 ? sm->wrap_t : WL_GX_WRAP_CLAMP,
                                   minf, sm->mag_filter, tex->mips > 1u ? sm->lod_bias : tex->sampler.lod_bias,
                                   tex->tlut, wl_gx_shadow_texture_region(tex->format, tex->mips, g_bisect.regions_libogc));
        if (wl_gx_shadow_texture(map, regs, ci ? 7u : 6u, g_bisect.tex_whole) == 0) {
            s.stats.textures_kept++;
        }
    }
    if (tlut != NULL && (tex->format == WL_GX_TEX_C4 || tex->format == WL_GX_TEX_C8)) {
        // (a TLUT slot holds the last palette loaded into it, whichever map's object reads it; the
        // loads the Wii would send counted, gx_shadow.h)
        unsigned other;

        (void)wl_gx_shadow_tlut_differs(tex->tlut, tlut->entries, (uint32_t)tlut->count * 2u, tlut->format);
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
        wl_gx_shadow_texture_forget(1u << map); // (loaded behind the shadow, as on the console)
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
    wl_gx_shadow_stats(out);
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
