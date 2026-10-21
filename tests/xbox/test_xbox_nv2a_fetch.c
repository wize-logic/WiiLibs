// SPDX-License-Identifier: MIT
// wl/xbox/nv2a_kelvin: the one-pass fetch (wl_nv2a_set_fetch_cuts). Each of the fetch's eight sets of
// formats of one stride (the skinned positions and vertices, the static vertex, the bytes' set), on
// arrays with and without indices and on inline vertices, fetched a vertex at a time with its
// attributes together, gives what the attribute-at-a-time loops give and what wl_nv2a_draw_fetch_some
// gives for each vertex, bit for bit, over the formats' edge values (shorts 0x7FFF, 0x8000, 0xFFFF,
// bytes 0 and 255, the packed normal's fields at their ends, floats as their bits: zeros, infinities,
// NaNs, denormals); a set with strides that differ is fetched an attribute at a time. The arrays'
// format compare by its dirty bit: a write to a format makes the next draw of arrays read it, also
// across inline and immediate draws between (the layout's dirty groups carried; as wave 5 left it, the
// switch "oldfetch", they were dropped there), an offset alone keeps the formats; random runs of writes
// and draws give the draws of a layout made at each draw.
#include <string.h>

#include "greatest.h"
#include "nv2a_test_util.h"
#include "wl/xbox/nv2a_internal.h"

static uint32_t g_rng = 0x6C8E9CF5u;
static uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static uint32_t format_word(uint32_t type, uint32_t size, uint32_t stride) {
    return stride << 8 | size << 4 | type;
}

// ---- the eight sets against the attribute-at-a-time loops -----------------------------------------

typedef struct FetchSet {
    const char *name;
    unsigned count;
    uint8_t type[5], size[5];
} FetchSet;

// (nv2a_kelvin.c's ROWS_*, in their order: the streamed attributes' formats by attribute number)
static const FetchSet k_sets[] = {
    {"skinned position", 3, {WL_NV2A_VTX_F, WL_NV2A_VTX_UB_OGL, WL_NV2A_VTX_S1}, {3, 2, 1}},
    {"skinned position, uv", 4, {WL_NV2A_VTX_F, WL_NV2A_VTX_S1, WL_NV2A_VTX_UB_D3D, WL_NV2A_VTX_S1}, {3, 2, 2, 1}},
    {"skinned", 5, {WL_NV2A_VTX_F, WL_NV2A_VTX_CMP, WL_NV2A_VTX_S1, WL_NV2A_VTX_UB_OGL, WL_NV2A_VTX_S1},
     {3, 1, 2, 2, 1}},
    {"skinned, no uv", 4, {WL_NV2A_VTX_F, WL_NV2A_VTX_CMP, WL_NV2A_VTX_UB_D3D, WL_NV2A_VTX_S1}, {3, 1, 2, 1}},
    {"position", 1, {WL_NV2A_VTX_F}, {3}},
    {"position, uv", 2, {WL_NV2A_VTX_F, WL_NV2A_VTX_S1}, {3, 2}},
    {"static", 3, {WL_NV2A_VTX_F, WL_NV2A_VTX_F, WL_NV2A_VTX_UB_D3D}, {3, 2, 4}},
    {"bytes", 3, {WL_NV2A_VTX_UB_OGL, WL_NV2A_VTX_S32K, WL_NV2A_VTX_UB_D3D}, {3, 1, 3}},
};

enum { VERTICES = 96, MAX_ELEMENT = 160, DATA_BYTES = 64 * 1024 };

static const uint32_t k_float_edges[] = {
    0x00000000u, 0x80000000u, 0x3F800000u, 0xBF800000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00001u,
    0x7F800001u, 0x7FBFFFFFu, 0x00000001u, 0x807FFFFFu, 0x7F7FFFFFu, 0xFF7FFFFFu, 0x00800000u, 0x4B400000u,
};
static const uint16_t k_short_edges[] = {0x0000u, 0x0001u, 0x7FFFu, 0x7FFEu, 0x8000u, 0x8001u, 0xFFFFu, 0xC000u};
static const uint8_t k_byte_edges[] = {0x00u, 0x01u, 0x7Fu, 0x80u, 0xFEu, 0xFFu};
// (11-bit fields: 0, 1, 1023, 1024 (-1024, clamped), 1025, 2047; 10-bit: 0, 1, 511, 512 (-512), 513, 1023)
static const uint16_t k_cmp11_edges[] = {0u, 1u, 1023u, 1024u, 1025u, 2047u};
static const uint16_t k_cmp10_edges[] = {0u, 1u, 511u, 512u, 513u, 1023u};

// one attribute's bytes for an element: its edge values (`edge` picks them) or random ones
static void attribute_write(uint8_t *p, uint8_t type, uint8_t size, unsigned edge) {
    unsigned c;

    switch (type) {
    case WL_NV2A_VTX_F:
        for (c = 0; c < size; c++) {
            wl_wr_le32(p + 4 * c, edge != 0 ? k_float_edges[(edge + 5u * c) % 16u] : rnd());
        }
        break;
    case WL_NV2A_VTX_S1:
    case WL_NV2A_VTX_S32K:
        for (c = 0; c < size; c++) {
            wl_wr_le16(p + 2 * c, edge != 0 ? k_short_edges[(edge + 3u * c) % 8u] : (uint16_t)rnd());
        }
        break;
    case WL_NV2A_VTX_UB_OGL:
    case WL_NV2A_VTX_UB_D3D:
        for (c = 0; c < size; c++) {
            p[c] = edge != 0 ? k_byte_edges[(edge + c) % 6u] : (uint8_t)rnd();
        }
        break;
    case WL_NV2A_VTX_CMP:
        wl_wr_le32(p, edge != 0 ? (uint32_t)k_cmp11_edges[edge % 6u] | (uint32_t)k_cmp11_edges[(edge / 6u) % 6u] << 11 |
                                      (uint32_t)k_cmp10_edges[(edge / 36u) % 6u] << 22
                                : rnd());
        break;
    default:
        break;
    }
}

typedef enum { DRAW_INDEXED, DRAW_RUN, DRAW_INLINE } DrawKind;

static uint8_t g_data[DATA_BYTES];
static uint32_t g_indices[VERTICES];
static WlNv2aKelvin g_k;
static float g_want[VERTICES][WL_NV2A_ATTRIBS][4], g_rows[VERTICES][WL_NV2A_ATTRIBS][4],
    g_loops[VERTICES][WL_NV2A_ATTRIBS][4], g_elements_rows[VERTICES][WL_NV2A_ATTRIBS][4],
    g_elements_loops[VERTICES][WL_NV2A_ATTRIBS][4];

// the vertices fetched by the plan (wl_nv2a_fetch_vertices over all, then by elements in runs of
// WL_NV2A_FETCH_BATCH as the GX path asks), with the fetch's cuts as `cuts`; the plan's set returned
static uint8_t fetch_all(const WlNv2aDraw *draw, uint32_t attributes, bool cuts, float (*by_vertices)[WL_NV2A_ATTRIBS][4],
                         float (*by_elements)[WL_NV2A_ATTRIBS][4], const uint32_t *order) {
    WlNv2aFetchPlan plan;
    uint32_t elements[VERTICES], n, done;

    wl_nv2a_set_fetch_cuts(cuts);
    wl_nv2a_fetch_plan(draw, attributes, &plan);
    memset(by_vertices, 0x5A, sizeof(float) * VERTICES * WL_NV2A_ATTRIBS * 4);
    memset(by_elements, 0xA5, sizeof(float) * VERTICES * WL_NV2A_ATTRIBS * 4);
    wl_nv2a_fetch_constants(&plan, &g_k, by_vertices, VERTICES);
    wl_nv2a_fetch_constants(&plan, &g_k, by_elements, VERTICES);
    wl_nv2a_fetch_vertices(&plan, draw, order, draw->count, by_vertices);
    for (n = 0; n < draw->count; n++) {
        elements[n] = wl_nv2a_fetch_element(&plan, order[n]);
    }
    for (done = 0; done < draw->count; done += WL_NV2A_FETCH_BATCH) {
        uint32_t run = draw->count - done < WL_NV2A_FETCH_BATCH ? draw->count - done : WL_NV2A_FETCH_BATCH;

        wl_nv2a_fetch_elements(&plan, draw, elements + done, run, by_elements + done);
    }
    wl_nv2a_set_fetch_cuts(true);
    return plan.rows;
}

static enum greatest_test_res set_run(const FetchSet *set, DrawKind kind, bool strides_differ) {
    static uint32_t order[VERTICES];
    unsigned offsets[5], attribs[5], i, v, c;
    uint32_t stride, used = 0, attributes, streamed = 0;
    WlNv2aDraw draw;
    uint8_t rows_on, rows_off;

    // (the set's attributes: distinct, in increasing order, among the sixteen)
    for (i = 0; i < set->count;) {
        unsigned a = rnd() % WL_NV2A_ATTRIBS;

        if (!(streamed & (1u << a))) {
            streamed |= 1u << a;
            i++;
        }
    }
    for (i = 0, v = 0; v < WL_NV2A_ATTRIBS; v++) {
        if (streamed & (1u << v)) {
            attribs[i++] = v;
        }
    }
    // (each attribute's start in the vertex: in an order of their own, 2-byte aligned, a gap now and then)
    {
        unsigned perm[5] = {0, 1, 2, 3, 4};

        for (i = set->count; i > 1; i--) {
            unsigned j = rnd() % i, t = perm[i - 1];

            perm[i - 1] = perm[j];
            perm[j] = t;
        }
        for (i = 0; i < set->count; i++) {
            unsigned a = perm[i];

            used += 2u * (rnd() % 3u == 0 ? rnd() % 3u : 0u);
            offsets[a] = used;
            used += wl_nv2a_attrib_bytes(set->type[a], set->size[a]);
            used = (used + 1u) & ~1u;
        }
    }
    stride = ((used + 3u) & ~3u) + 4u * (rnd() % 3u);
    ASSERTm("a vertex fits", stride * (MAX_ELEMENT + 8u) + 64u <= DATA_BYTES);

    for (c = 0; c < DATA_BYTES; c++) {
        g_data[c] = (uint8_t)rnd();
    }
    for (v = 0; v < MAX_ELEMENT + 8u; v++) {
        for (i = 0; i < set->count; i++) {
            // (edge values for most elements: each set's fields meet each edge in some vertex)
            unsigned edge = rnd() % 4u == 0 ? 0u : 1u + (v * 7u + i * 13u) % 251u;

            attribute_write(g_data + v * stride + offsets[i], set->type[i], set->size[i], edge);
        }
    }
    for (i = 0; i < WL_NV2A_ATTRIBS; i++) {
        for (c = 0; c < 4; c++) {
            g_k.current[i][c] = (float)(int)(rnd() % 2001u - 1000u) / 8.0f;
        }
    }

    memset(&draw, 0, sizeof draw);
    draw.source = kind == DRAW_INLINE ? WL_NV2A_SOURCE_INLINE : WL_NV2A_SOURCE_ARRAYS;
    draw.count = VERTICES;
    draw.streamed = streamed;
    draw.index_base = kind == DRAW_INDEXED ? 3u + rnd() % 8u : 0u;
    draw.indices = kind == DRAW_INDEXED ? g_indices : NULL;
    for (v = 0; v < VERTICES; v++) {
        g_indices[v] = draw.index_base + rnd() % (MAX_ELEMENT - 16u);
    }
    draw.inline_data = g_data;
    draw.inline_stride = stride;
    for (i = 0; i < set->count; i++) {
        WlNv2aAttrib *attrib = &draw.attribs[attribs[i]];

        attrib->type = set->type[i];
        attrib->size = set->size[i];
        attrib->stride = stride;
        attrib->data = g_data + (size_t)draw.index_base * stride + offsets[i];
        attrib->inline_offset = (uint16_t)offsets[i];
    }
    if (strides_differ) {
        draw.attribs[attribs[set->count - 1u]].stride = stride + 4u;
    }
    // (the streamed attributes asked for, and some current values: an attribute the draw does not stream)
    attributes = streamed | (rnd() & 0xFFFFu & ~streamed);
    for (v = 0; v < VERTICES; v++) {
        order[v] = kind == DRAW_RUN || kind == DRAW_INLINE ? (v * 37u) % VERTICES : v;
    }

    rows_on = fetch_all(&draw, attributes, true, g_rows, g_elements_rows, order);
    rows_off = fetch_all(&draw, attributes, false, g_loops, g_elements_loops, order);
    if (draw.source == WL_NV2A_SOURCE_INLINE || !strides_differ) {
        ASSERTm(set->name, rows_on != 0);
    } else {
        ASSERT_EQm("strides that differ: an attribute at a time", 0, rows_on);
    }
    ASSERT_EQm("the cuts off: an attribute at a time", 0, rows_off);
    for (v = 0; v < VERTICES; v++) {
        memset(g_want[v], 0, sizeof g_want[v]);
        wl_nv2a_draw_fetch_some(&draw, &g_k, order[v], attributes, g_want[v]);
        for (i = 0; i < WL_NV2A_ATTRIBS; i++) {
            if (!(attributes & (1u << i))) {
                continue;
            }
            ASSERT_MEM_EQm(set->name, g_want[v][i], g_rows[v][i], sizeof g_want[v][i]);
            ASSERT_MEM_EQm(set->name, g_want[v][i], g_loops[v][i], sizeof g_want[v][i]);
            ASSERT_MEM_EQm(set->name, g_want[v][i], g_elements_rows[v][i], sizeof g_want[v][i]);
            ASSERT_MEM_EQm(set->name, g_want[v][i], g_elements_loops[v][i], sizeof g_want[v][i]);
        }
    }
    PASS();
}

TEST each_set_fetched_together_matches_the_loops(void) {
    unsigned s, kind, trial;

    for (s = 0; s < sizeof k_sets / sizeof k_sets[0]; s++) {
        for (kind = DRAW_INDEXED; kind <= DRAW_INLINE; kind++) {
            for (trial = 0; trial < 24; trial++) {
                CHECK_CALL(set_run(&k_sets[s], (DrawKind)kind, false));
            }
        }
    }
    PASS();
}

TEST a_set_of_strides_that_differ_is_fetched_by_attribute(void) {
    unsigned s, trial;

    for (s = 0; s < sizeof k_sets / sizeof k_sets[0]; s++) {
        if (k_sets[s].count < 2) {
            continue;
        }
        for (trial = 0; trial < 8; trial++) {
            CHECK_CALL(set_run(&k_sets[s], DRAW_INDEXED, true));
            CHECK_CALL(set_run(&k_sets[s], DRAW_RUN, true));
        }
    }
    PASS();
}

// ---- the arrays' formats compared by their dirty bit --------------------------------------------------

typedef struct LastDraw {
    const uint8_t *ram;
    unsigned draws, array_draws;
    uint8_t source;
    WlNv2aAttrib attribs[WL_NV2A_ATTRIBS];
    uint32_t streamed;
    float first[WL_NV2A_ATTRIBS][4];
    uint32_t hash;
} LastDraw;

static uint32_t hash_word(uint32_t hash, uint32_t word) {
    return (hash ^ word) * 16777619u;
}

static uint32_t last_draw(void *user, const WlNv2aKelvin *k, const WlNv2aDraw *draw) {
    LastDraw *log = user;
    uint32_t h = log->hash;
    unsigned a;

    log->draws++;
    log->source = draw->source;
    log->streamed = draw->streamed;
    memcpy(log->attribs, draw->attribs, sizeof log->attribs);
    if (draw->source == WL_NV2A_SOURCE_ARRAYS) {
        log->array_draws++;
        wl_nv2a_draw_fetch(draw, k, 0, log->first);
    }
    h = hash_word(h, (uint32_t)draw->source | (uint32_t)draw->primitive << 8);
    h = hash_word(h, draw->count);
    h = hash_word(h, draw->streamed);
    for (a = 0; a < WL_NV2A_ATTRIBS; a++) {
        const WlNv2aAttrib *at = &draw->attribs[a];

        if (draw->source != WL_NV2A_SOURCE_ARRAYS) {
            continue;
        }
        h = hash_word(h, at->data != NULL ? (uint32_t)(at->data - log->ram) : 0xFFFFFFFFu);
        h = hash_word(h, at->stride);
        h = hash_word(h, (uint32_t)at->type | (uint32_t)at->size << 8);
    }
    log->hash = h;
    return 0;
}

static void draw_two(TGuest *g) {
    t_m1(g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_LINES);
    t_m1(g, WL_NV2A_K_ARRAY_ELEMENT16, 1u << 16 | 0u);
    t_m1(g, WL_NV2A_K_SET_BEGIN_END, 0);
    t_kick(g);
}

static void draw_inline(TGuest *g, unsigned words) {
    unsigned i;

    t_m1(g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_POINTS);
    t_word(g, WL_NV2A_CMD_NON_INCREASING | ((uint32_t)words << 18) | WL_NV2A_K_INLINE_ARRAY);
    for (i = 0; i < words; i++) {
        t_word(g, t_f((float)i));
    }
    t_m1(g, WL_NV2A_K_SET_BEGIN_END, 0);
    t_kick(g);
}

static void draw_immediate(TGuest *g) {
    t_m1(g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_POINTS);
    t_method(g, WL_NV2A_K_SET_VERTEX3F, 3);
    t_word(g, t_f(1.0f));
    t_word(g, t_f(2.0f));
    t_word(g, t_f(3.0f));
    t_m1(g, WL_NV2A_K_SET_BEGIN_END, 0);
    t_kick(g);
}

// v0 three floats at 0x40000 (stride 16), v3 a D3DCOLOR at 0x40000 + 12; v0 read as two normalized
// shorts once its format is written, and at 0x50000 once its offset is
static enum greatest_test_res dirty_run(bool cuts) {
    LastDraw log;
    WlNv2aBackend backend;
    TGuest g;

    memset(&log, 0, sizeof log);
    memset(&backend, 0, sizeof backend);
    backend.user = &log;
    backend.draw = last_draw;
    wl_nv2a_set_fetch_cuts(cuts);
    ASSERT(t_create(&g, &backend) != NULL);
    log.ram = g.ram;
    t_setup_3d(&g);
    t_w32(&g, 0x40000, t_f(1.0f));
    t_w32(&g, 0x40004, t_f(2.0f));
    t_w32(&g, 0x40008, t_f(3.0f));
    t_w32(&g, 0x4000C, 0xFF102030u);
    t_w32(&g, 0x50000, 0x7FFF0000u);
    t_w32(&g, 0x50004, 0u);
    t_m1(&g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_OFFSET + 0, 0x40000);
    t_m1(&g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_FORMAT + 0, format_word(WL_NV2A_VTX_F, 3, 16));
    t_m1(&g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_OFFSET + 12, 0x4000C);
    t_m1(&g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_FORMAT + 12, format_word(WL_NV2A_VTX_UB_D3D, 4, 16));
    draw_two(&g);
    ASSERT_EQ(1u, log.array_draws);
    ASSERT_EQ((uint8_t)WL_NV2A_VTX_F, log.attribs[0].type);
    ASSERT_EQ(3u, log.attribs[0].size);
    ASSERT_EQ(3.0f, log.first[0][2]);

    // a format written: the next draw of arrays reads it
    t_m1(&g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_FORMAT + 0, format_word(WL_NV2A_VTX_S1, 2, 16));
    draw_two(&g);
    ASSERT_EQ(2u, log.array_draws);
    ASSERT_EQ((uint8_t)WL_NV2A_VTX_S1, log.attribs[0].type);
    ASSERT_EQ(2u, log.attribs[0].size);
    ASSERT_EQ(0.0f, log.first[0][2]);

    // an offset alone: the formats as they were, the start moved
    t_m1(&g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_OFFSET + 0, 0x50000);
    draw_two(&g);
    ASSERT_EQ((uint8_t)WL_NV2A_VTX_S1, log.attribs[0].type);
    ASSERT_EQ(0x50000u, (uint32_t)(log.attribs[0].data - g.ram));
    ASSERT_EQ(0.0f, log.first[0][0]);
    ASSERT_EQ(1.0f, log.first[0][1]);

    // a format written, then an inline draw and an immediate one: the draw of arrays after them still
    // reads the new format (the layout's dirty groups carried over the draws without arrays)
    t_m1(&g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_FORMAT + 0, format_word(WL_NV2A_VTX_F, 2, 16));
    draw_inline(&g, 8);
    ASSERT_EQ((uint8_t)WL_NV2A_SOURCE_INLINE, log.source);
    draw_immediate(&g);
    ASSERT_EQ((uint8_t)WL_NV2A_SOURCE_IMMEDIATE, log.source);
    t_m1(&g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_OFFSET + 0, 0x40000);
    draw_two(&g);
    ASSERT_EQ(4u, log.array_draws);
    ASSERT_EQ((uint8_t)WL_NV2A_VTX_F, log.attribs[0].type);
    ASSERT_EQ(2u, log.attribs[0].size);
    ASSERT_EQ(1.0f, log.first[0][0]);
    ASSERT_EQ(2.0f, log.first[0][1]);
    ASSERT_EQ(0.0f, log.first[0][2]);

    // the same with no array write at all between: a format, an immediate draw, then a draw of arrays.
    // (As wave 5 left it, wl_nv2a_set_fetch_cuts(false), a draw without arrays drops the groups and
    // the draw of arrays after it keeps the layout of before: v3 still streamed. The cuts carry them.)
    t_m1(&g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_FORMAT + 12, 0u);
    draw_immediate(&g);
    draw_two(&g);
    ASSERT_EQ(5u, log.array_draws);
    if (cuts) {
        ASSERT_EQ(0u, log.streamed & (1u << 3));
        ASSERT_EQ(1u, log.streamed & 1u);
    } else {
        ASSERT_EQ(9u, log.streamed);
    }

    // the same value written again: nothing changes
    t_m1(&g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_FORMAT + 0, format_word(WL_NV2A_VTX_F, 2, 16));
    draw_two(&g);
    ASSERT_EQ((uint8_t)WL_NV2A_VTX_F, log.attribs[0].type);
    ASSERT_EQ(2u, log.attribs[0].size);
    t_destroy(&g);
    wl_nv2a_set_fetch_cuts(true);
    PASS();
}

TEST a_written_format_reaches_the_next_draw_of_arrays(void) {
    CHECK_CALL(dirty_run(true));
    CHECK_CALL(dirty_run(false));
    PASS();
}

// random formats, offsets and the values again, among draws of arrays, inline and immediate draws: the
// same draws with the layout kept and its formats compared by the dirty bit as with a layout made at each
// draw (wl_nv2a_set_push_cuts3 and 4 off)
static enum greatest_test_res random_run(bool kept, uint32_t *hash, unsigned *draws) {
    static const uint8_t types[] = {WL_NV2A_VTX_UB_D3D, WL_NV2A_VTX_S1, WL_NV2A_VTX_F, WL_NV2A_VTX_UB_OGL,
                                    WL_NV2A_VTX_S32K, WL_NV2A_VTX_CMP};
    LastDraw log;
    WlNv2aBackend backend;
    TGuest g;
    unsigned step;

    memset(&log, 0, sizeof log);
    log.hash = 2166136261u;
    memset(&backend, 0, sizeof backend);
    backend.user = &log;
    backend.draw = last_draw;
    wl_nv2a_set_push_cuts3(kept);
    wl_nv2a_set_push_cuts4(kept);
    g_rng = 0x1F123BB5u;
    ASSERT(t_create(&g, &backend) != NULL);
    log.ram = g.ram;
    t_setup_3d(&g);
    for (step = 0; step < 4000; step++) {
        uint32_t what = rnd() % 12u, a = rnd() % 4u;

        if (what < 3u) {
            t_m1(&g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_OFFSET + 4 * a, 0x40000u + 4u * (rnd() % 4096u));
        } else if (what < 6u) {
            t_m1(&g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_FORMAT + 4 * a,
                 format_word(types[rnd() % sizeof types], rnd() % 5u, rnd() % 2u == 0 ? 32u : 4u * (rnd() % 16u)));
        } else if (what == 6u) {
            t_m1(&g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_FORMAT + 4 * a,
                 wl_nv2a_k(wl_nv2a_kelvin(g.dev), WL_NV2A_K_SET_VERTEX_DATA_ARRAY_FORMAT + 4 * a));
        } else if (what == 7u) {
            draw_inline(&g, 1u + rnd() % 24u);
        } else if (what == 8u) {
            draw_immediate(&g);
        } else {
            draw_two(&g);
        }
    }
    ASSERT_EQ(g.put, t_get(&g));
    *hash = log.hash;
    *draws = log.array_draws;
    t_destroy(&g);
    wl_nv2a_set_push_cuts3(true);
    wl_nv2a_set_push_cuts4(true);
    PASS();
}

TEST random_writes_and_draws_match_a_layout_each_draw(void) {
    uint32_t hash_kept = 0, hash_each = 0;
    unsigned draws_kept = 0, draws_each = 0;

    CHECK_CALL(random_run(true, &hash_kept, &draws_kept));
    CHECK_CALL(random_run(false, &hash_each, &draws_each));
    ASSERT(draws_kept > 500);
    ASSERT_EQ(draws_each, draws_kept);
    ASSERT_EQ(hash_each, hash_kept);
    PASS();
}

SUITE(fetch_suite) {
    RUN_TEST(each_set_fetched_together_matches_the_loops);
    RUN_TEST(a_set_of_strides_that_differ_is_fetched_by_attribute);
    RUN_TEST(a_written_format_reaches_the_next_draw_of_arrays);
    RUN_TEST(random_writes_and_draws_match_a_layout_each_draw);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(fetch_suite);
    GREATEST_MAIN_END();
}
