// SPDX-License-Identifier: MIT
// The Kelvin class's state (nv2a_kelvin.h): each method's word kept, the windows loaded (the
// vertex program and its constants, four words a slot, the slot moving on after the fourth word),
// the context DMAs resolved, the viewport also written into the constants the vertex program
// reads it from (c[58] the scale, c[59] the offset: Direct3D's c[-38] and c[-37]), the
// attributes' current values set by the immediate methods, and the begin/end batches handed to
// the backend as draws.
//
// A batch is what a begin/end gets: DRAW_ARRAYS' ranges and ARRAY_ELEMENT16/32's indices into the
// vertex arrays, INLINE_ARRAY's words, or the vertices the immediate methods complete (a write of
// attribute 0's last component completes one, its registers then as they are). A long batch is
// handed over in parts, each a whole number of primitives: a strip goes on from its last two
// vertices (at an even vertex, so its triangles keep their winding), a fan from its first and last,
// a line strip from its last; a line loop's parts are line strips and its first vertex closes it.

#include "wl/xbox/nv2a_internal.h"

#include <stdlib.h>
#include <string.h>

#include "wl/base/endian.h"

// ---- decoding helpers -----------------------------------------------------------------------

static float word_float(uint32_t word) {
    float value;
    memcpy(&value, &word, sizeof value);
    return value;
}

static float normalized(int value, float maximum) {
    float result = (float)value * (1.0f / maximum);
    return result < -1.0f ? -1.0f : result;
}

static int sign_extend(uint32_t value, unsigned bits) {
    uint32_t sign = 1u << (bits - 1u);
    return (int)((value ^ sign) - sign);
}

uint32_t wl_nv2a_attrib_bytes(uint8_t type, uint8_t size) {
    if (size == 0) {
        return 0;
    }
    switch (type) {
    case WL_NV2A_VTX_UB_D3D:
    case WL_NV2A_VTX_UB_OGL:
        return size;
    case WL_NV2A_VTX_S1:
    case WL_NV2A_VTX_S32K:
        return 2u * size;
    case WL_NV2A_VTX_F:
        return size == 7 ? 12u : 4u * size;
    case WL_NV2A_VTX_CMP:
        return 4u;
    default:
        return 0;
    }
}

// (the draws' cuts of wave 5: batch_draw's draw kept between draws, its attributes written again
// alone (layout_arrays_kept); the fetch's plan made from the attributes asked for alone, its formats from
// a table, unnormalized shorts converted by their bits, the current values written a word at a time
// (wl_nv2a_fetch_plan, wl_nv2a_fetch_constants); an attribute's decode by the fetch's tables and
// conversions (attrib_decode_cut); false: as wave 4 left them, a file "oldpush4" in the title's folder,
// WL_XB_OLDPUSH4 on the host and in the replay)
static bool g_push_cuts4 = true;

void wl_nv2a_set_push_cuts4(bool on) {
    g_push_cuts4 = on;
}

// (the fetch's cuts of wave 6: a vertex's attributes fetched together when their formats are one of
// the sets fetch_rows knows (WlNv2aFetchPlan.rows), the arrays' formats compared only when one was
// written; false: as wave 5 left them, a file "oldfetch" in the title's folder, WL_XB_OLDFETCH on the
// host and in the replay)
static bool g_fetch_cuts = true;

void wl_nv2a_set_fetch_cuts(bool on) {
    g_fetch_cuts = on;
}

static void attrib_decode_cut(uint8_t type, uint8_t size, const uint8_t *data, float out[4]);

void wl_nv2a_attrib_decode(uint8_t type, uint8_t size, const uint8_t *data, float out[4]) {
    unsigned i;

    if (g_push_cuts4) {
        attrib_decode_cut(type, size, data, out);
        return;
    }
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    switch (type) {
    case WL_NV2A_VTX_UB_D3D:
        if (size == 4) {
            // a D3DCOLOR: bytes B, G, R, A
            out[0] = data[2] * (1.0f / 255.0f);
            out[1] = data[1] * (1.0f / 255.0f);
            out[2] = data[0] * (1.0f / 255.0f);
            out[3] = data[3] * (1.0f / 255.0f);
            break;
        }
        for (i = 0; i < size && i < 4; i++) {
            out[i] = data[i] * (1.0f / 255.0f);
        }
        break;
    case WL_NV2A_VTX_UB_OGL:
        for (i = 0; i < size && i < 4; i++) {
            out[i] = data[i] * (1.0f / 255.0f);
        }
        break;
    case WL_NV2A_VTX_S1:
        for (i = 0; i < size && i < 4; i++) {
            out[i] = normalized((int16_t)wl_rd_le16(data + 2 * i), 32767.0f);
        }
        break;
    case WL_NV2A_VTX_S32K:
        for (i = 0; i < size && i < 4; i++) {
            out[i] = (float)(int16_t)wl_rd_le16(data + 2 * i);
        }
        break;
    case WL_NV2A_VTX_F:
        if (size == 7) {
            // two floats and w (Direct3D's FLOAT2H)
            out[0] = word_float(wl_rd_le32(data));
            out[1] = word_float(wl_rd_le32(data + 4));
            out[3] = word_float(wl_rd_le32(data + 8));
            break;
        }
        for (i = 0; i < size && i < 4; i++) {
            out[i] = word_float(wl_rd_le32(data + 4 * i));
        }
        break;
    case WL_NV2A_VTX_CMP: {
        uint32_t word = wl_rd_le32(data);

        out[0] = normalized(sign_extend(word & 0x7FFu, 11), 1023.0f);
        out[1] = normalized(sign_extend((word >> 11) & 0x7FFu, 11), 1023.0f);
        out[2] = normalized(sign_extend(word >> 22, 10), 511.0f);
        break;
    }
    default:
        break;
    }
}

void wl_nv2a_draw_fetch(const WlNv2aDraw *draw, const WlNv2aKelvin *k, uint32_t vertex,
                        float inputs[WL_NV2A_ATTRIBS][4]) {
    wl_nv2a_draw_fetch_some(draw, k, vertex, (1u << WL_NV2A_ATTRIBS) - 1u, inputs);
}

void wl_nv2a_draw_fetch_some(const WlNv2aDraw *draw, const WlNv2aKelvin *k, uint32_t vertex, uint32_t attributes,
                             float inputs[WL_NV2A_ATTRIBS][4]) {
    unsigned a;

    if (draw->source == WL_NV2A_SOURCE_IMMEDIATE) {
        memcpy(inputs, draw->immediate[vertex], sizeof(float) * WL_NV2A_ATTRIBS * 4);
        return;
    }
    for (a = 0; a < WL_NV2A_ATTRIBS; a++) {
        const WlNv2aAttrib *attrib = &draw->attribs[a];

        if (!(attributes & (1u << a))) {
            continue;
        }
        if (!(draw->streamed & (1u << a))) {
            memcpy(inputs[a], k->current[a], sizeof inputs[a]);
        } else if (draw->source == WL_NV2A_SOURCE_ARRAYS) {
            uint32_t index = draw->indices != NULL ? draw->indices[vertex] : draw->index_base + vertex;

            wl_nv2a_attrib_decode(attrib->type, attrib->size,
                                  attrib->data + (size_t)(index - draw->index_base) * attrib->stride, inputs[a]);
        } else {
            wl_nv2a_attrib_decode(attrib->type, attrib->size,
                                  draw->inline_data + (size_t)vertex * draw->inline_stride + attrib->inline_offset,
                                  inputs[a]);
        }
    }
}

// ---- a draw's fetch decided once ---------------------------------------------------------------

enum {
    FETCH_F1,
    FETCH_F2,
    FETCH_F3,
    FETCH_F4,
    FETCH_D3DCOLOR,
    FETCH_CMP,
    FETCH_S1,    // (size 1 to 4)
    FETCH_UB,    // bytes in order, normalized (size 1 to 4)
    FETCH_S32K,  // shorts as they are (size 1 to 4; wl_nv2a_set_push_cuts4's, else FETCH_GENERIC)
    FETCH_GENERIC // wl_nv2a_attrib_decode's
};

// the conversions the formats need, as tables of what wl_nv2a_attrib_decode computes (the same
// operations, so the same bits): a byte normalized, the packed normal's 11- and 10-bit fields
// normalized. (The 750 has no integer-to-float instruction: each conversion is a store, a load and
// a subtraction of a magic double.)
static float g_byte_float[256], g_cmp11_float[2048], g_cmp10_float[1024];
static bool g_fetch_tables;
// (each type's and size's FETCH_*, wl_nv2a_set_push_cuts4's: by the type's and size's 4 bits)
static uint8_t g_fetch_format[16][16];

// an attribute's format and size together, as fetch_rows takes them (ROW_NONE: one it does not)
enum {
    ROW_NONE,
    ROW_F1, ROW_F2, ROW_F3, ROW_F4,
    ROW_D3DCOLOR,
    ROW_CMP,
    ROW_S1_1, ROW_S1_2, ROW_S1_3, ROW_S1_4,
    ROW_UB_1, ROW_UB_2, ROW_UB_3, ROW_UB_4,
    ROW_S32K_1, ROW_S32K_2, ROW_S32K_3, ROW_S32K_4,
    ROW_BITS = 5
};
// (by the type's and size's 4 bits, as g_fetch_format)
static uint8_t g_fetch_row[16][16];

static uint8_t fetch_row_of(uint8_t format, uint8_t size) {
    switch (format) {
    case FETCH_F1: return ROW_F1;
    case FETCH_F2: return ROW_F2;
    case FETCH_F3: return ROW_F3;
    case FETCH_F4: return ROW_F4;
    case FETCH_D3DCOLOR: return ROW_D3DCOLOR;
    case FETCH_CMP: return ROW_CMP;
    case FETCH_S1: return (uint8_t)(ROW_S1_1 + size - 1u);
    case FETCH_UB: return (uint8_t)(ROW_UB_1 + size - 1u);
    case FETCH_S32K: return (uint8_t)(ROW_S32K_1 + size - 1u);
    default: return ROW_NONE;
    }
}

// an attribute's FETCH_* (s32k: FETCH_S32K for its shorts, else FETCH_GENERIC)
static uint8_t fetch_format_of(uint8_t type, uint8_t size, bool s32k) {
    if (type == WL_NV2A_VTX_F && size >= 1 && size <= 4) {
        return (uint8_t)(FETCH_F1 + size - 1u);
    } else if (type == WL_NV2A_VTX_UB_D3D && size == 4) {
        return FETCH_D3DCOLOR;
    } else if (type == WL_NV2A_VTX_CMP) {
        return FETCH_CMP;
    } else if (type == WL_NV2A_VTX_S1 && size >= 1 && size <= 4) {
        return FETCH_S1;
    } else if ((type == WL_NV2A_VTX_UB_OGL || type == WL_NV2A_VTX_UB_D3D) && size >= 1 && size <= 4) {
        return FETCH_UB;
    } else if (s32k && type == WL_NV2A_VTX_S32K && size >= 1 && size <= 4) {
        return FETCH_S32K;
    }
    return FETCH_GENERIC;
}

static void fetch_tables_make(void) {
    unsigned i;

    for (i = 0; i < 256; i++) {
        uint8_t byte = (uint8_t)i;

        g_byte_float[i] = byte * (1.0f / 255.0f);
    }
    for (i = 0; i < 2048; i++) {
        g_cmp11_float[i] = normalized(sign_extend(i, 11), 1023.0f);
    }
    for (i = 0; i < 1024; i++) {
        g_cmp10_float[i] = normalized(sign_extend(i, 10), 511.0f);
    }
    for (i = 0; i < 256; i++) {
        g_fetch_format[i >> 4][i & 15u] = fetch_format_of((uint8_t)(i >> 4), (uint8_t)(i & 15u), true);
        g_fetch_row[i >> 4][i & 15u] = fetch_row_of(g_fetch_format[i >> 4][i & 15u], (uint8_t)(i & 15u));
    }
    g_fetch_tables = true;
}

// (float)value for |value| < 2^22, exactly: the float 1.5 * 2^23 + value, by its bits, less 1.5 * 2^23
static inline float small_int_float(int value) {
    uint32_t bits = 0x4B400000u + (uint32_t)value;
    float f;

    memcpy(&f, &bits, sizeof f);
    return f - 12582912.0f;
}

// the sets of formats fetch_rows fetches a vertex at a time (WlNv2aFetchPlan.rows), each streamed
// attribute's ROW_* in order after a leading 1 (fetch_plan's signature): Halo's skinned positions (the
// projection's probes: position, blend indices and weight), its skinned and static vertices
#define ROWS1(a) (1u << ROW_BITS | (unsigned)(a))
#define ROWS2(a, b) (ROWS1(a) << ROW_BITS | (unsigned)(b))
#define ROWS3(a, b, c) (ROWS2(a, b) << ROW_BITS | (unsigned)(c))
#define ROWS4(a, b, c, d) (ROWS3(a, b, c) << ROW_BITS | (unsigned)(d))
#define ROWS5(a, b, c, d, e) (ROWS4(a, b, c, d) << ROW_BITS | (unsigned)(e))
enum {
    ROWS_NONE,
    ROWS_SKIN_POSITION, // F3, UB.2, S1.1
    ROWS_SKIN_POSITION_UV, // F3, S1.2, UB.2, S1.1
    ROWS_SKIN, // F3, CMP, S1.2, UB.2, S1.1
    ROWS_SKIN_NO_UV, // F3, CMP, UB.2, S1.1
    ROWS_F3, // F3
    ROWS_F3_UV, // F3, S1.2
    ROWS_STATIC, // F3, F2, D3DCOLOR
    ROWS_BYTES, // UB.3, S32K.1, UB.3
    ROWS_KINDS,
    // (more attributes than a signature holds)
    ROWS_MAX_ATTRIBUTES = 31 / ROW_BITS
};

static uint8_t fetch_rows_kind(uint32_t signature) {
    switch (signature) {
    case ROWS3(ROW_F3, ROW_UB_2, ROW_S1_1): return ROWS_SKIN_POSITION;
    case ROWS4(ROW_F3, ROW_S1_2, ROW_UB_2, ROW_S1_1): return ROWS_SKIN_POSITION_UV;
    case ROWS5(ROW_F3, ROW_CMP, ROW_S1_2, ROW_UB_2, ROW_S1_1): return ROWS_SKIN;
    case ROWS4(ROW_F3, ROW_CMP, ROW_UB_2, ROW_S1_1): return ROWS_SKIN_NO_UV;
    case ROWS1(ROW_F3): return ROWS_F3;
    case ROWS2(ROW_F3, ROW_S1_2): return ROWS_F3_UV;
    case ROWS3(ROW_F3, ROW_F2, ROW_D3DCOLOR): return ROWS_STATIC;
    case ROWS3(ROW_UB_3, ROW_S32K_1, ROW_UB_3): return ROWS_BYTES;
    default: return ROWS_NONE;
    }
}

bool wl_nv2a_prefetch_on = true;

void wl_nv2a_set_prefetch(bool on) {
    wl_nv2a_prefetch_on = on;
}

// an array attribute's bytes joined to the plan's prefetch areas: to one of its stride whose bytes
// and these lie within one stride, else a new one (none when they are all taken)
static void prefetch_area_add(WlNv2aFetchPlan *plan, const uint8_t *data, uint32_t stride, uint32_t bytes) {
    unsigned a;

    if (bytes == 0) {
        return;
    }
    for (a = 0; a < plan->prefetch_count; a++) {
        const uint8_t *lo = plan->prefetch[a].data, *hi = lo + plan->prefetch[a].last + 1u;

        if (plan->prefetch[a].stride != stride) {
            continue;
        }
        lo = data < lo ? data : lo;
        hi = data + bytes > hi ? data + bytes : hi;
        if ((size_t)(hi - lo) <= (stride > WL_NV2A_CACHE_LINE ? stride : WL_NV2A_CACHE_LINE)) {
            plan->prefetch[a].data = lo;
            plan->prefetch[a].last = (uint32_t)(hi - lo) - 1u;
            return;
        }
    }
    if (plan->prefetch_count < WL_NV2A_PREFETCH_AREAS) {
        plan->prefetch[plan->prefetch_count].data = data;
        plan->prefetch[plan->prefetch_count].stride = stride;
        plan->prefetch[plan->prefetch_count].last = bytes - 1u;
        plan->prefetch_count++;
    }
}

void wl_nv2a_fetch_plan(const WlNv2aDraw *draw, uint32_t attributes, WlNv2aFetchPlan *plan) {
    unsigned a;

    if (!g_fetch_tables) {
        fetch_tables_make();
    }

    plan->source = draw->source;
    plan->count = 0;
    plan->constants = 0;
    plan->indices = draw->indices;
    plan->index_base = draw->index_base;
    plan->prefetch_count = 0;
    plan->rows = ROWS_NONE;
    if (draw->source == WL_NV2A_SOURCE_IMMEDIATE) {
        return;
    }
    if (g_push_cuts4) {
        // (the attributes asked for alone, in order: a draw reads a few of the sixteen)
        uint32_t want = attributes & ((1u << WL_NV2A_ATTRIBS) - 1u);
        bool arrays = draw->source == WL_NV2A_SOURCE_ARRAYS;
        unsigned n = 0;
        // (the formats' signature for fetch_rows_kind, and whether the strides differ)
        uint32_t signature = 1, strides = 0;

        plan->constants = want & ~draw->streamed;
        for (want &= draw->streamed; want != 0; want &= want - 1u) {
            const WlNv2aAttrib *attrib;

            a = (unsigned)__builtin_ctz(want);
            attrib = &draw->attribs[a];
            plan->streamed[n].attribute = (uint8_t)a;
            plan->streamed[n].type = attrib->type;
            plan->streamed[n].size = attrib->size;
            plan->streamed[n].format = g_fetch_format[attrib->type & 15u][attrib->size & 15u];
            signature = signature << ROW_BITS | g_fetch_row[attrib->type & 15u][attrib->size & 15u];
            if (arrays) {
                plan->streamed[n].data = attrib->data;
                plan->streamed[n].stride = attrib->stride;
                strides |= attrib->stride ^ plan->streamed[0].stride;
                if (WL_NV2A_PREFETCHES) {
                    prefetch_area_add(plan, attrib->data, attrib->stride,
                                      wl_nv2a_attrib_bytes(attrib->type, attrib->size));
                }
            } else {
                plan->streamed[n].data = draw->inline_data + attrib->inline_offset;
                plan->streamed[n].stride = draw->inline_stride;
            }
            n++;
        }
        plan->count = (uint8_t)n;
        // (an inline draw's attributes are all of its one stride)
        plan->rows = g_fetch_cuts && strides == 0 && n <= ROWS_MAX_ATTRIBUTES ? fetch_rows_kind(signature) : ROWS_NONE;
        return;
    }
    for (a = 0; a < WL_NV2A_ATTRIBS; a++) {
        const WlNv2aAttrib *attrib = &draw->attribs[a];

        if (!(attributes & (1u << a))) {
            continue;
        }
        if (!(draw->streamed & (1u << a))) {
            plan->constants |= 1u << a;
            continue;
        }
        plan->streamed[plan->count].attribute = (uint8_t)a;
        plan->streamed[plan->count].type = attrib->type;
        plan->streamed[plan->count].size = attrib->size;
        if (draw->source == WL_NV2A_SOURCE_ARRAYS) {
            plan->streamed[plan->count].data = attrib->data;
            plan->streamed[plan->count].stride = attrib->stride;
            if (WL_NV2A_PREFETCHES) {
                prefetch_area_add(plan, attrib->data, attrib->stride, wl_nv2a_attrib_bytes(attrib->type, attrib->size));
            }
        } else {
            plan->streamed[plan->count].data = draw->inline_data + attrib->inline_offset;
            plan->streamed[plan->count].stride = draw->inline_stride;
        }
        plan->streamed[plan->count].format = fetch_format_of(attrib->type, attrib->size, false);
        plan->count++;
    }
}

void wl_nv2a_fetch_constants(const WlNv2aFetchPlan *plan, const WlNv2aKelvin *k, float (*inputs)[WL_NV2A_ATTRIBS][4],
                             unsigned count) {
    unsigned a, n;

    if (g_push_cuts4) {
        uint32_t left;

        // (each value's four words loaded once and stored into every set, as they are)
        for (left = plan->constants & ((1u << WL_NV2A_ATTRIBS) - 1u); left != 0; left &= left - 1u) {
            uint32_t w[4];

            a = (unsigned)__builtin_ctz(left);
            memcpy(w, k->current[a], sizeof w);
            for (n = 0; n < count; n++) {
                memcpy(inputs[n][a], w, sizeof w);
            }
        }
        return;
    }
    for (a = 0; a < WL_NV2A_ATTRIBS; a++) {
        if (plan->constants & (1u << a)) {
            for (n = 0; n < count; n++) {
                memcpy(inputs[n][a], k->current[a], sizeof inputs[n][a]);
            }
        }
    }
}

// (a little-endian word into a float's place, its bits as they are: no trip through a float
// register, which on PowerPC is a store and a load)
static inline void store_word(float *out, const uint8_t *data) {
    uint32_t w = wl_rd_le32(data);

    memcpy(out, &w, sizeof w);
}

// shorts normalized and bytes normalized, `size` (1 to 4) of them a vertex: made once for each size (a
// constant there), so the components' loop is unrolled and its constants are loaded once a run
static inline __attribute__((always_inline)) void fetch_s1(const uint8_t *base, uint32_t stride,
                                                             const uint32_t *elements, uint32_t run,
                                                             float (*in)[WL_NV2A_ATTRIBS][4], unsigned attribute,
                                                             unsigned size) {
    uint32_t n;
    unsigned c;

    for (n = 0; n < run; n++) {
        const uint8_t *data = base + (size_t)elements[n] * stride;
        float *out = in[n][attribute];

        float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};

        for (c = 0; c < size; c++) {
            // (normalized's own operations, the integer made a float by its bits)
            float result = small_int_float((int16_t)wl_rd_le16(data + 2 * c)) * (1.0f / 32767.0f);

            v[c] = result < -1.0f ? -1.0f : result;
        }
        // (the values read before any is stored: a store to out could be the data's bytes for all the
        // compiler knows, and each component left out was stored and then stored again)
        out[0] = v[0], out[1] = v[1], out[2] = v[2], out[3] = v[3];
    }
}

// shorts as they are (WL_NV2A_VTX_S32K), `size` (1 to 4) of them a vertex: (float) of a short, exactly
static inline __attribute__((always_inline)) void fetch_s32k(const uint8_t *base, uint32_t stride,
                                                               const uint32_t *elements, uint32_t run,
                                                               float (*in)[WL_NV2A_ATTRIBS][4], unsigned attribute,
                                                               unsigned size) {
    uint32_t n;
    unsigned c;

    for (n = 0; n < run; n++) {
        const uint8_t *data = base + (size_t)elements[n] * stride;
        float *out = in[n][attribute];

        float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};

        for (c = 0; c < size; c++) {
            v[c] = small_int_float((int16_t)wl_rd_le16(data + 2 * c));
        }
        out[0] = v[0], out[1] = v[1], out[2] = v[2], out[3] = v[3];
    }
}

static inline __attribute__((always_inline)) void fetch_ub(const uint8_t *base, uint32_t stride,
                                                             const uint32_t *elements, uint32_t run,
                                                             float (*in)[WL_NV2A_ATTRIBS][4], unsigned attribute,
                                                             unsigned size) {
    uint32_t n;
    unsigned c;

    for (n = 0; n < run; n++) {
        const uint8_t *data = base + (size_t)elements[n] * stride;
        float *out = in[n][attribute];

        float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};

        for (c = 0; c < size; c++) {
            v[c] = g_byte_float[data[c]];
        }
        out[0] = v[0], out[1] = v[1], out[2] = v[2], out[3] = v[3];
    }
}

// one attribute of a vertex by its ROW_* (a constant where it is inlined): the values the attribute
// loops of wl_nv2a_fetch_elements give, to the bit (the components read before any is stored)
static inline __attribute__((always_inline)) void fetch_row(unsigned row, const uint8_t *data, float *out) {
    float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    unsigned c, size;

    switch (row) {
    // (the words as they are: no trip through a float register)
    case ROW_F1: {
        uint32_t w0 = wl_rd_le32(data);

        memcpy(&out[0], &w0, sizeof w0);
        out[1] = out[2] = 0.0f;
        out[3] = 1.0f;
        return;
    }
    case ROW_F2: {
        uint32_t w0 = wl_rd_le32(data), w1 = wl_rd_le32(data + 4);

        memcpy(&out[0], &w0, sizeof w0);
        memcpy(&out[1], &w1, sizeof w1);
        out[2] = 0.0f;
        out[3] = 1.0f;
        return;
    }
    case ROW_F3: {
        uint32_t w0 = wl_rd_le32(data), w1 = wl_rd_le32(data + 4), w2 = wl_rd_le32(data + 8);

        memcpy(&out[0], &w0, sizeof w0);
        memcpy(&out[1], &w1, sizeof w1);
        memcpy(&out[2], &w2, sizeof w2);
        out[3] = 1.0f;
        return;
    }
    case ROW_F4: {
        uint32_t w0 = wl_rd_le32(data), w1 = wl_rd_le32(data + 4), w2 = wl_rd_le32(data + 8),
                 w3 = wl_rd_le32(data + 12);

        memcpy(&out[0], &w0, sizeof w0);
        memcpy(&out[1], &w1, sizeof w1);
        memcpy(&out[2], &w2, sizeof w2);
        memcpy(&out[3], &w3, sizeof w3);
        return;
    }
    case ROW_D3DCOLOR:
        v[0] = g_byte_float[data[2]];
        v[1] = g_byte_float[data[1]];
        v[2] = g_byte_float[data[0]];
        v[3] = g_byte_float[data[3]];
        break;
    case ROW_CMP: {
        uint32_t word = wl_rd_le32(data);

        v[0] = g_cmp11_float[word & 0x7FFu];
        v[1] = g_cmp11_float[(word >> 11) & 0x7FFu];
        v[2] = g_cmp10_float[word >> 22];
        break;
    }
    case ROW_S1_1:
    case ROW_S1_2:
    case ROW_S1_3:
    case ROW_S1_4:
        size = row - ROW_S1_1 + 1u;
        for (c = 0; c < size; c++) {
            float result = small_int_float((int16_t)wl_rd_le16(data + 2 * c)) * (1.0f / 32767.0f);

            v[c] = result < -1.0f ? -1.0f : result;
        }
        break;
    case ROW_UB_1:
    case ROW_UB_2:
    case ROW_UB_3:
    case ROW_UB_4:
        size = row - ROW_UB_1 + 1u;
        for (c = 0; c < size; c++) {
            v[c] = g_byte_float[data[c]];
        }
        break;
    case ROW_S32K_1:
    case ROW_S32K_2:
    case ROW_S32K_3:
    case ROW_S32K_4:
        size = row - ROW_S32K_1 + 1u;
        for (c = 0; c < size; c++) {
            v[c] = small_int_float((int16_t)wl_rd_le16(data + 2 * c));
        }
        break;
    default:
        break;
    }
    out[0] = v[0], out[1] = v[1], out[2] = v[2], out[3] = v[3];
}

// `count` vertices of a plan whose formats are r0 .. r4 (constants, ROW_NONE past the last), a vertex
// at a time: its element's offset in the one stride found once, each attribute read at its own start
// from it. Each vertex's element is elements[n], or when `gather` its index's (indices[elements[n]] less
// index_base: wl_nv2a_fetch_element's, the index read straight in)
static inline __attribute__((always_inline)) void fetch_rows(const WlNv2aFetchPlan *plan, bool gather,
                                                              const uint32_t *elements, uint32_t count,
                                                              float (*inputs)[WL_NV2A_ATTRIBS][4], unsigned r0,
                                                              unsigned r1, unsigned r2, unsigned r3, unsigned r4) {
    const uint32_t stride = plan->streamed[0].stride, *indices = plan->indices, base = plan->index_base;
    const uint8_t *d0 = plan->streamed[0].data, *d1 = r1 ? plan->streamed[1].data : NULL,
                  *d2 = r2 ? plan->streamed[2].data : NULL, *d3 = r3 ? plan->streamed[3].data : NULL,
                  *d4 = r4 ? plan->streamed[4].data : NULL;
    const unsigned a0 = plan->streamed[0].attribute, a1 = r1 ? plan->streamed[1].attribute : 0u,
                   a2 = r2 ? plan->streamed[2].attribute : 0u, a3 = r3 ? plan->streamed[3].attribute : 0u,
                   a4 = r4 ? plan->streamed[4].attribute : 0u;
    uint32_t n;

    for (n = 0; n < count; n++) {
        size_t offset = (size_t)(gather ? indices[elements[n]] - base : elements[n]) * stride;
        float (*in)[4] = inputs[n];

        fetch_row(r0, d0 + offset, in[a0]);
        if (r1) {
            fetch_row(r1, d1 + offset, in[a1]);
        }
        if (r2) {
            fetch_row(r2, d2 + offset, in[a2]);
        }
        if (r3) {
            fetch_row(r3, d3 + offset, in[a3]);
        }
        if (r4) {
            fetch_row(r4, d4 + offset, in[a4]);
        }
    }
}

// fetch_rows by the plan's kind (false: none)
static bool fetch_rows_by_kind(const WlNv2aFetchPlan *plan, bool gather, const uint32_t *elements, uint32_t count,
                               float (*inputs)[WL_NV2A_ATTRIBS][4]) {
#define FETCH_ROWS(kind, r0, r1, r2, r3, r4)                                                                           \
    case kind:                                                                                                         \
        if (gather) {                                                                                                  \
            fetch_rows(plan, true, elements, count, inputs, r0, r1, r2, r3, r4);                                       \
        } else {                                                                                                       \
            fetch_rows(plan, false, elements, count, inputs, r0, r1, r2, r3, r4);                                      \
        }                                                                                                              \
        return true;
    switch (plan->rows) {
        FETCH_ROWS(ROWS_SKIN_POSITION, ROW_F3, ROW_UB_2, ROW_S1_1, ROW_NONE, ROW_NONE)
        FETCH_ROWS(ROWS_SKIN_POSITION_UV, ROW_F3, ROW_S1_2, ROW_UB_2, ROW_S1_1, ROW_NONE)
        FETCH_ROWS(ROWS_SKIN, ROW_F3, ROW_CMP, ROW_S1_2, ROW_UB_2, ROW_S1_1)
        FETCH_ROWS(ROWS_SKIN_NO_UV, ROW_F3, ROW_CMP, ROW_UB_2, ROW_S1_1, ROW_NONE)
        FETCH_ROWS(ROWS_F3, ROW_F3, ROW_NONE, ROW_NONE, ROW_NONE, ROW_NONE)
        FETCH_ROWS(ROWS_F3_UV, ROW_F3, ROW_S1_2, ROW_NONE, ROW_NONE, ROW_NONE)
        FETCH_ROWS(ROWS_STATIC, ROW_F3, ROW_F2, ROW_D3DCOLOR, ROW_NONE, ROW_NONE)
        FETCH_ROWS(ROWS_BYTES, ROW_UB_3, ROW_S32K_1, ROW_UB_3, ROW_NONE, ROW_NONE)
    default:
        return false;
    }
#undef FETCH_ROWS
}

// wl_nv2a_attrib_decode's values, to the bit, by the fetch's means: bytes and the packed normal's fields
// from the tables, shorts made floats by their bits (small_int_float), a size's components unrolled
static void attrib_decode_cut(uint8_t type, uint8_t size, const uint8_t *data, float out[4]) {
    unsigned i, n = size < 4u ? size : 4u;

    if (!g_fetch_tables) {
        fetch_tables_make();
    }
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    switch (type) {
    case WL_NV2A_VTX_UB_D3D:
        if (size == 4) {
            out[0] = g_byte_float[data[2]];
            out[1] = g_byte_float[data[1]];
            out[2] = g_byte_float[data[0]];
            out[3] = g_byte_float[data[3]];
            break;
        }
        // fall through
    case WL_NV2A_VTX_UB_OGL:
        for (i = 0; i < n; i++) {
            out[i] = g_byte_float[data[i]];
        }
        break;
    case WL_NV2A_VTX_S1:
        for (i = 0; i < n; i++) {
            float result = small_int_float((int16_t)wl_rd_le16(data + 2 * i)) * (1.0f / 32767.0f);

            out[i] = result < -1.0f ? -1.0f : result;
        }
        break;
    case WL_NV2A_VTX_S32K:
        for (i = 0; i < n; i++) {
            out[i] = small_int_float((int16_t)wl_rd_le16(data + 2 * i));
        }
        break;
    case WL_NV2A_VTX_F:
        if (size == 7) {
            store_word(&out[0], data);
            store_word(&out[1], data + 4);
            store_word(&out[3], data + 8);
            break;
        }
        for (i = 0; i < n; i++) {
            store_word(&out[i], data + 4 * i);
        }
        break;
    case WL_NV2A_VTX_CMP: {
        uint32_t word = wl_rd_le32(data);

        out[0] = g_cmp11_float[word & 0x7FFu];
        out[1] = g_cmp11_float[(word >> 11) & 0x7FFu];
        out[2] = g_cmp10_float[word >> 22];
        break;
    }
    default:
        break;
    }
}

unsigned wl_nv2a_fetch_fused_format(const WlNv2aFetchPlan *plan, unsigned i) {
    unsigned size = plan->streamed[i].size;

    switch (plan->streamed[i].format) {
    case FETCH_F1: return WL_XBOX_VSH_FUSED_F1;
    case FETCH_F2: return WL_XBOX_VSH_FUSED_F2;
    case FETCH_F3: return WL_XBOX_VSH_FUSED_F3;
    case FETCH_F4: return WL_XBOX_VSH_FUSED_F4;
    case FETCH_D3DCOLOR: return WL_XBOX_VSH_FUSED_D3DCOLOR;
    case FETCH_CMP: return WL_XBOX_VSH_FUSED_CMP;
    case FETCH_S1: return size >= 1u && size <= 4u ? WL_XBOX_VSH_FUSED_S1 + size - 1u : WL_XBOX_VSH_FUSED_FORMATS;
    case FETCH_UB: return size >= 1u && size <= 4u ? WL_XBOX_VSH_FUSED_UB + size - 1u : WL_XBOX_VSH_FUSED_FORMATS;
    case FETCH_S32K: return WL_XBOX_VSH_FUSED_S32K + size - 1u;
    default:
        return plan->streamed[i].type == WL_NV2A_VTX_S32K && size >= 1u && size <= 4u ? WL_XBOX_VSH_FUSED_S32K + size - 1u
                                                                                      : WL_XBOX_VSH_FUSED_FORMATS;
    }
}

void wl_nv2a_fetch_tables(const float **byte_float, const float **cmp11, const float **cmp10) {
    if (!g_fetch_tables) {
        fetch_tables_make();
    }
    *byte_float = g_byte_float;
    *cmp11 = g_cmp11_float;
    *cmp10 = g_cmp10_float;
}

void wl_nv2a_fetch_elements(const WlNv2aFetchPlan *plan, const WlNv2aDraw *draw, const uint32_t *all_elements,
                            uint32_t count, float (*inputs)[WL_NV2A_ATTRIBS][4]) {
    uint32_t n, done, run;
    unsigned i;

    if (plan->source == WL_NV2A_SOURCE_IMMEDIATE) {
        for (n = 0; n < count; n++) {
            memcpy(inputs[n], draw->immediate[all_elements[n]], sizeof(float) * WL_NV2A_ATTRIBS * 4);
        }
        return;
    }
    if (plan->rows != ROWS_NONE && fetch_rows_by_kind(plan, false, all_elements, count, inputs)) {
        return;
    }
    for (done = 0; done < count; done += run) {
        float (*in)[WL_NV2A_ATTRIBS][4] = inputs + done;
        const uint32_t *elements = all_elements + done;

        run = count - done < WL_NV2A_FETCH_BATCH ? count - done : WL_NV2A_FETCH_BATCH;
        // an attribute at a time for all of them (its format decided once)
        for (i = 0; i < plan->count; i++) {
            const uint8_t *base = plan->streamed[i].data;
            uint32_t stride = plan->streamed[i].stride;
            unsigned attribute = plan->streamed[i].attribute, size = plan->streamed[i].size;

            // (as wl_nv2a_attrib_decode widens them: the components left out 0, w 1)
            switch (plan->streamed[i].format) {
            case FETCH_F4:
                for (n = 0; n < run; n++) {
                    const uint8_t *data = base + (size_t)elements[n] * stride;
                    float *out = in[n][attribute];

                    store_word(&out[0], data);
                    store_word(&out[1], data + 4);
                    store_word(&out[2], data + 8);
                    store_word(&out[3], data + 12);
                }
                break;
            case FETCH_F3:
                for (n = 0; n < run; n++) {
                    const uint8_t *data = base + (size_t)elements[n] * stride;
                    float *out = in[n][attribute];

                    store_word(&out[0], data);
                    store_word(&out[1], data + 4);
                    store_word(&out[2], data + 8);
                    out[3] = 1.0f;
                }
                break;
            case FETCH_F2:
                for (n = 0; n < run; n++) {
                    const uint8_t *data = base + (size_t)elements[n] * stride;
                    float *out = in[n][attribute];

                    store_word(&out[0], data);
                    store_word(&out[1], data + 4);
                    out[2] = 0.0f;
                    out[3] = 1.0f;
                }
                break;
            case FETCH_F1:
                for (n = 0; n < run; n++) {
                    float *out = in[n][attribute];

                    store_word(&out[0], base + (size_t)elements[n] * stride);
                    out[1] = out[2] = 0.0f;
                    out[3] = 1.0f;
                }
                break;
            case FETCH_D3DCOLOR:
                // a D3DCOLOR: bytes B, G, R, A
                for (n = 0; n < run; n++) {
                    const uint8_t *data = base + (size_t)elements[n] * stride;
                    float *out = in[n][attribute];

                    out[0] = g_byte_float[data[2]];
                    out[1] = g_byte_float[data[1]];
                    out[2] = g_byte_float[data[0]];
                    out[3] = g_byte_float[data[3]];
                }
                break;
            case FETCH_CMP:
                for (n = 0; n < run; n++) {
                    uint32_t word = wl_rd_le32(base + (size_t)elements[n] * stride);
                    float *out = in[n][attribute];

                    out[0] = g_cmp11_float[word & 0x7FFu];
                    out[1] = g_cmp11_float[(word >> 11) & 0x7FFu];
                    out[2] = g_cmp10_float[word >> 22];
                    out[3] = 1.0f;
                }
                break;
            case FETCH_S1:
                switch (size) {
                case 1: fetch_s1(base, stride, elements, run, in, attribute, 1); break;
                case 2: fetch_s1(base, stride, elements, run, in, attribute, 2); break;
                case 3: fetch_s1(base, stride, elements, run, in, attribute, 3); break;
                default: fetch_s1(base, stride, elements, run, in, attribute, 4); break;
                }
                break;
            case FETCH_UB:
                switch (size) {
                case 1: fetch_ub(base, stride, elements, run, in, attribute, 1); break;
                case 2: fetch_ub(base, stride, elements, run, in, attribute, 2); break;
                case 3: fetch_ub(base, stride, elements, run, in, attribute, 3); break;
                default: fetch_ub(base, stride, elements, run, in, attribute, 4); break;
                }
                break;
            case FETCH_S32K:
                switch (size) {
                case 1: fetch_s32k(base, stride, elements, run, in, attribute, 1); break;
                case 2: fetch_s32k(base, stride, elements, run, in, attribute, 2); break;
                case 3: fetch_s32k(base, stride, elements, run, in, attribute, 3); break;
                default: fetch_s32k(base, stride, elements, run, in, attribute, 4); break;
                }
                break;
            default:
                for (n = 0; n < run; n++) {
                    wl_nv2a_attrib_decode(plan->streamed[i].type, (uint8_t)size,
                                          base + (size_t)elements[n] * stride, in[n][attribute]);
                }
                break;
            }
        }
    }
}

void wl_nv2a_fetch_vertices(const WlNv2aFetchPlan *plan, const WlNv2aDraw *draw, const uint32_t *vertices,
                            uint32_t count, float (*inputs)[WL_NV2A_ATTRIBS][4]) {
    uint32_t elements[WL_NV2A_FETCH_BATCH];
    uint32_t n, done, run;

    // (an array's indices read straight into the vertices' offsets; an inline draw's vertices are its elements)
    if (plan->rows != ROWS_NONE &&
        fetch_rows_by_kind(plan, plan->source == WL_NV2A_SOURCE_ARRAYS && plan->indices != NULL, vertices, count,
                           inputs)) {
        return;
    }
    for (done = 0; done < count; done += run) {
        run = count - done < WL_NV2A_FETCH_BATCH ? count - done : WL_NV2A_FETCH_BATCH;
        for (n = 0; n < run; n++) {
            elements[n] = wl_nv2a_fetch_element(plan, vertices[done + n]);
        }
        wl_nv2a_fetch_elements(plan, draw, elements, run, inputs + done);
    }
}

void wl_nv2a_fetch_vertex(const WlNv2aFetchPlan *plan, const WlNv2aDraw *draw, uint32_t vertex,
                          float inputs[WL_NV2A_ATTRIBS][4]) {
    wl_nv2a_fetch_vertices(plan, draw, &vertex, 1, (float (*)[WL_NV2A_ATTRIBS][4])inputs);
}

void wl_nv2a_prim_info(uint32_t primitive, WlNv2aPrimInfo *out) {
    // (a table: each draw asks twice; first, step, lines, points, strip, fan; none for any other)
    static const WlNv2aPrimInfo prims[] = {
        [WL_NV2A_PRIM_POINTS] = {1, 1, 0, 1, 0, 0},
        [WL_NV2A_PRIM_LINES] = {2, 2, 1, 0, 0, 0},
        [WL_NV2A_PRIM_LINE_LOOP] = {2, 1, 1, 0, 1, 0},
        [WL_NV2A_PRIM_LINE_STRIP] = {2, 1, 1, 0, 1, 0},
        [WL_NV2A_PRIM_TRIANGLES] = {3, 3, 0, 0, 0, 0},
        [WL_NV2A_PRIM_TRIANGLE_STRIP] = {3, 1, 0, 0, 1, 0},
        [WL_NV2A_PRIM_TRIANGLE_FAN] = {3, 1, 0, 0, 0, 1},
        [WL_NV2A_PRIM_QUADS] = {4, 4, 0, 0, 0, 0},
        [WL_NV2A_PRIM_QUAD_STRIP] = {4, 2, 0, 0, 1, 0},
        [WL_NV2A_PRIM_POLYGON] = {3, 1, 0, 0, 0, 1},
    };
    static const WlNv2aPrimInfo none;

    *out = primitive < sizeof prims / sizeof prims[0] ? prims[primitive] : none;
}

// ---- the decoded views ----------------------------------------------------------------------

void wl_nv2a_surface(const WlNv2aKelvin *k, WlNv2aSurface *out) {
    uint32_t format = wl_nv2a_k(k, WL_NV2A_K_SET_SURFACE_FORMAT);
    uint32_t h = wl_nv2a_k(k, WL_NV2A_K_SET_SURFACE_CLIP_HORIZONTAL);
    uint32_t v = wl_nv2a_k(k, WL_NV2A_K_SET_SURFACE_CLIP_VERTICAL);
    uint32_t pitch = wl_nv2a_k(k, WL_NV2A_K_SET_SURFACE_PITCH);
    unsigned aa = (format >> WL_NV2A_SURFACE_AA_SHIFT) & 0xFu;

    memset(out, 0, sizeof *out);
    out->clip_x = h & 0xFFFFu;
    out->clip_width = h >> 16;
    out->clip_y = v & 0xFFFFu;
    out->clip_height = v >> 16;
    out->color_pitch = pitch & 0xFFFFu;
    out->zeta_pitch = pitch >> 16;
    out->color_format = (uint8_t)(format & WL_NV2A_SURFACE_COLOR_MASK);
    out->zeta_format = (uint8_t)((format >> WL_NV2A_SURFACE_ZETA_SHIFT) & 0xFu);
    out->swizzled = ((format >> WL_NV2A_SURFACE_TYPE_SHIFT) & 0xFu) == WL_NV2A_SURFACE_TYPE_SWIZZLE;
    out->log2_width = (uint8_t)((format >> WL_NV2A_SURFACE_WIDTH_SHIFT) & 0xFFu);
    out->log2_height = (uint8_t)((format >> WL_NV2A_SURFACE_HEIGHT_SHIFT) & 0xFFu);
    out->color_phys = k->dma_color.base + wl_nv2a_k(k, WL_NV2A_K_SET_SURFACE_COLOR_OFFSET);
    out->zeta_phys = k->dma_zeta.base + wl_nv2a_k(k, WL_NV2A_K_SET_SURFACE_ZETA_OFFSET);
    switch (out->color_format) {
    case WL_NV2A_SURFACE_COLOR_X1R5G5B5_Z:
    case WL_NV2A_SURFACE_COLOR_X1R5G5B5_O:
    case WL_NV2A_SURFACE_COLOR_R5G6B5:
    case WL_NV2A_SURFACE_COLOR_G8B8:
        out->color_bytes = 2;
        break;
    case WL_NV2A_SURFACE_COLOR_B8:
        out->color_bytes = 1;
        break;
    case 0:
        out->color_bytes = 0;
        break;
    default:
        out->color_bytes = 4;
        break;
    }
    switch (out->zeta_format) {
    case WL_NV2A_SURFACE_ZETA_Z16:
        out->zeta_bytes = 2;
        out->depth_max = 65535.0f;
        break;
    case WL_NV2A_SURFACE_ZETA_Z24S8:
        out->zeta_bytes = 4;
        out->depth_max = 16777215.0f;
        break;
    default:
        out->zeta_bytes = 0;
        out->depth_max = 0.0f;
        break;
    }
    out->samples_x = aa == 0 ? 1 : 2;
    out->samples_y = aa == 2 ? 2 : 1;
}

void wl_nv2a_psh_words(const WlNv2aKelvin *k, uint32_t words[WL_XBOX_PSH_WORDS]) {
    unsigned i;

    for (i = 0; i < 8; i++) {
        words[WL_XBOX_PSH_ALPHA_INPUTS0 + i] = wl_nv2a_k(k, WL_NV2A_K_SET_COMBINER_ALPHA_ICW + 4 * i);
        words[WL_XBOX_PSH_CONSTANT0_0 + i] = wl_nv2a_k(k, WL_NV2A_K_SET_COMBINER_FACTOR0 + 4 * i);
        words[WL_XBOX_PSH_CONSTANT1_0 + i] = wl_nv2a_k(k, WL_NV2A_K_SET_COMBINER_FACTOR1 + 4 * i);
        words[WL_XBOX_PSH_ALPHA_OUTPUTS0 + i] = wl_nv2a_k(k, WL_NV2A_K_SET_COMBINER_ALPHA_OCW + 4 * i);
        words[WL_XBOX_PSH_RGB_INPUTS0 + i] = wl_nv2a_k(k, WL_NV2A_K_SET_COMBINER_COLOR_ICW + 4 * i);
        words[WL_XBOX_PSH_RGB_OUTPUTS0 + i] = wl_nv2a_k(k, WL_NV2A_K_SET_COMBINER_COLOR_OCW + 4 * i);
    }
    words[WL_XBOX_PSH_FINAL_INPUTS_ABCD] = wl_nv2a_k(k, WL_NV2A_K_SET_COMBINER_SPECULAR_FOG_CW0);
    words[WL_XBOX_PSH_FINAL_INPUTS_EFG] = wl_nv2a_k(k, WL_NV2A_K_SET_COMBINER_SPECULAR_FOG_CW1);
    words[WL_XBOX_PSH_COMPARE_MODE] = wl_nv2a_k(k, WL_NV2A_K_SET_SHADER_CLIP_PLANE_MODE);
    words[WL_XBOX_PSH_FINAL_CONSTANT0] = wl_nv2a_k(k, WL_NV2A_K_SET_SPECULAR_FOG_FACTOR);
    words[WL_XBOX_PSH_FINAL_CONSTANT1] = wl_nv2a_k(k, WL_NV2A_K_SET_SPECULAR_FOG_FACTOR + 4);
    words[WL_XBOX_PSH_COMBINER_COUNT] = wl_nv2a_k(k, WL_NV2A_K_SET_COMBINER_CONTROL);
    words[54] = wl_nv2a_k(k, WL_NV2A_K_SET_SHADER_STAGE_PROGRAM);
    words[55] = wl_nv2a_k(k, WL_NV2A_K_SET_DOT_RGBMAPPING);
    words[56] = wl_nv2a_k(k, WL_NV2A_K_SET_SHADER_OTHER_STAGE_INPUT);
}

uint32_t wl_nv2a_fog_rgb(const WlNv2aKelvin *k) {
    uint32_t abgr = wl_nv2a_k(k, WL_NV2A_K_SET_FOG_COLOR);

    return (abgr & 0xFFu) << 16 | (abgr & 0xFF00u) | ((abgr >> 16) & 0xFFu);
}

void wl_nv2a_program(const WlNv2aKelvin *k, const uint32_t **first, unsigned long *count) {
    uint32_t start = wl_nv2a_k(k, WL_NV2A_K_SET_TRANSFORM_PROGRAM_START);
    unsigned long n = 0;

    if (start >= WL_NV2A_PROGRAM_SLOTS) {
        *first = k->program[0];
        *count = 0;
        return;
    }
    while (start + n < WL_NV2A_PROGRAM_SLOTS) {
        n++;
        if (k->program[start + n - 1][3] & 1u) {
            break;
        }
    }
    *first = k->program[start];
    *count = n;
}

// ---- the methods ------------------------------------------------------------------------------

void wl_nv2a_kelvin_init(WlNv2a *dev) {
    WlNv2aKelvin *k = &dev->kelvin;
    unsigned a;

    memset(k, 0, sizeof *k);
    for (a = 0; a < WL_NV2A_ATTRIBS; a++) {
        k->current[a][3] = 1.0f;
    }
    k->raw[WL_NV2A_K_SET_COLOR_MASK >> 2] = 0x01010101u;
    k->raw[WL_NV2A_K_SET_DEPTH_FUNC >> 2] = WL_NV2A_FUNC_LESS;
    k->raw[WL_NV2A_K_SET_ALPHA_FUNC >> 2] = WL_NV2A_FUNC_ALWAYS;
    k->raw[WL_NV2A_K_SET_BLEND_FUNC_SFACTOR >> 2] = WL_NV2A_BLEND_ONE;
    k->raw[WL_NV2A_K_SET_BLEND_EQUATION >> 2] = WL_NV2A_BLEND_EQ_ADD;
    k->raw[WL_NV2A_K_SET_CULL_FACE >> 2] = WL_NV2A_CULL_BACK;
    k->raw[WL_NV2A_K_SET_FRONT_FACE >> 2] = WL_NV2A_FRONT_CCW;
    k->raw[WL_NV2A_K_SET_SHADE_MODEL >> 2] = WL_NV2A_SHADE_SMOOTH;
    k->dma_notifies.limit = k->dma_a.limit = k->dma_b.limit = k->dma_state.limit = k->dma_color.limit =
        k->dma_zeta.limit = k->dma_vertex_a.limit = k->dma_vertex_b.limit = k->dma_semaphore.limit =
            k->dma_report.limit = 0xFFFFFFFFu;
    k->dirty = WL_NV2A_DIRTY_ALL;
}

// the transform program's and constants' windows (0xB00-0xBFC): their own storage, the slot moving on
// after the fourth word
#define WINDOWS_SIZE (WL_NV2A_K_SET_TRANSFORM_CONSTANT + 0x80u - WL_NV2A_K_SET_TRANSFORM_PROGRAM)

// what each method is, decided once (the dispatch's tests per method, from a table): known, an
// immediate vertex method, plain (a value kept and nothing else: no window, no case of
// wl_nv2a_kelvin_method's switch), and its dirty group; for the pusher, a method a run may start at
// (wl_nv2a_bulk_method's own tests), or in a window; one of the few the pusher's runs take straight
// to their work (the load slots, begin/end: METHOD_DIRECT)
enum {
    METHOD_KNOWN = 0x8000u,
    METHOD_IMMEDIATE = 0x4000u,
    METHOD_PLAIN = 0x2000u,
    METHOD_BULK = 0x1000u,
    METHOD_WINDOW = 0x0800u,
    METHOD_DIRECT = 0x0400u,
    METHOD_DIRTY = 0x03FFu
};
static uint16_t g_method_info[WL_NV2A_K_METHODS / 4];
// how many plain methods (METHOD_PLAIN and METHOD_BULK) follow from each on, itself counted (up to 255):
// an increasing run of at most that many keeps its words' values and nothing else
static uint8_t g_plain_run[WL_NV2A_K_METHODS / 4];

static uint32_t dirty_group(uint32_t method) {
    if (method >= 0x200 && method <= 0x214) {
        return WL_NV2A_DIRTY_SURFACE;
    }
    if ((method >= 0x260 && method <= 0x28C) || (method >= 0xA60 && method <= 0xADC) ||
        (method >= 0x1E20 && method <= 0x1E24) || (method >= 0x1E40 && method <= 0x1E78) || method == 0x17F8) {
        return WL_NV2A_DIRTY_COMBINERS | WL_NV2A_DIRTY_PIXEL;
    }
    if (method == WL_NV2A_K_SET_FOG_ENABLE || method == WL_NV2A_K_SET_FOG_COLOR) {
        return WL_NV2A_DIRTY_RASTER | WL_NV2A_DIRTY_PIXEL;
    }
    if (method >= 0x1B00 && method < 0x1C00) {
        return WL_NV2A_DIRTY_TEXTURES;
    }
    if (method >= WL_NV2A_K_SET_VERTEX_DATA_ARRAY_FORMAT && method < WL_NV2A_K_SET_VERTEX_DATA_ARRAY_FORMAT + 0x40) {
        return WL_NV2A_DIRTY_ARRAYS | WL_NV2A_DIRTY_ARRAY_FORMATS;
    }
    if (method >= 0x1720 && method <= 0x179C) {
        return WL_NV2A_DIRTY_ARRAYS;
    }
    if ((method >= 0x3C0 && method <= 0x41C) || (method >= 0x420 && method <= 0x42C) ||
        (method >= 0x440 && method <= 0x9BC) || method == 0x314 || method == 0x328) {
        return WL_NV2A_DIRTY_TRANSFORM;
    }
    return WL_NV2A_DIRTY_RASTER;
}

// the methods the tracker knows (kept, and read by itself or the backends); any other is kept
// and logged once
static bool known_method(uint32_t m) {
    return (m >= 0x100 && m <= 0x110) || (m >= 0x120 && m <= 0x130) || (m >= 0x180 && m <= 0x1A8) ||
           (m >= 0x200 && m <= 0x214) || (m >= 0x260 && m <= 0x3CC + 0x30) || (m >= 0x420 && m <= 0x43C) ||
           (m >= 0x440 && m <= 0x9DC) || m == 0x9FC || (m >= 0xA10 && m <= 0xAFC) || (m >= 0xB00 && m <= 0xBFC) ||
           (m >= 0xC00 && m <= 0x13FC) || (m >= 0x147C && m <= 0x14FC) || (m >= 0x1500 && m <= 0x16CC) ||
           (m >= 0x1710 && m <= 0x17D0) || (m >= 0x17E0 && m <= 0x1818) || m == 0x181C || m == 0x1820 ||
           m == 0x1824 || (m >= 0x1880 && m <= 0x1AFC) || (m >= 0x1B00 && m <= 0x1BFC) ||
           (m >= 0x1D6C && m <= 0x1D9C) || (m >= 0x1E20 && m <= 0x1EA4);
}

static bool immediate_range(uint32_t method);

// the methods wl_nv2a_kelvin_method does more with than keep their value (its switch's cases, the
// windows, the viewport's words, the immediate vertex methods): kept in step with it
static bool special_method(uint32_t m) {
    static const uint16_t cases[] = {
        WL_NV2A_K_NOTIFY,
        WL_NV2A_K_SET_FLIP_READ,
        WL_NV2A_K_SET_FLIP_WRITE,
        WL_NV2A_K_SET_FLIP_MODULO,
        WL_NV2A_K_FLIP_INCREMENT_WRITE,
        WL_NV2A_K_FLIP_STALL,
        WL_NV2A_K_SET_CONTEXT_DMA_NOTIFIES,
        WL_NV2A_K_SET_CONTEXT_DMA_A,
        WL_NV2A_K_SET_CONTEXT_DMA_B,
        WL_NV2A_K_SET_CONTEXT_DMA_STATE,
        WL_NV2A_K_SET_CONTEXT_DMA_COLOR,
        WL_NV2A_K_SET_CONTEXT_DMA_ZETA,
        WL_NV2A_K_SET_CONTEXT_DMA_VERTEX_A,
        WL_NV2A_K_SET_CONTEXT_DMA_VERTEX_B,
        WL_NV2A_K_SET_CONTEXT_DMA_SEMAPHORE,
        WL_NV2A_K_SET_CONTEXT_DMA_REPORT,
        WL_NV2A_K_SET_TRANSFORM_PROGRAM_LOAD,
        WL_NV2A_K_SET_TRANSFORM_CONSTANT_LOAD,
        WL_NV2A_K_SET_TRANSFORM_PROGRAM_START,
        WL_NV2A_K_SET_TRANSFORM_EXECUTION_MODE,
        WL_NV2A_K_CLEAR_REPORT_VALUE,
        WL_NV2A_K_GET_REPORT,
        WL_NV2A_K_SET_BEGIN_END,
        WL_NV2A_K_ARRAY_ELEMENT16,
        WL_NV2A_K_ARRAY_ELEMENT32,
        WL_NV2A_K_DRAW_ARRAYS,
        WL_NV2A_K_INLINE_ARRAY,
        WL_NV2A_K_BACK_END_WRITE_SEMAPHORE_RELEASE,
        WL_NV2A_K_CLEAR_SURFACE,
    };
    unsigned i;

    if ((m >= WL_NV2A_K_SET_TRANSFORM_PROGRAM && m < WL_NV2A_K_SET_TRANSFORM_CONSTANT + 0x80) ||
        (m >= WL_NV2A_K_SET_VIEWPORT_OFFSET && m <= WL_NV2A_K_SET_VIEWPORT_OFFSET + 12) ||
        (m >= WL_NV2A_K_SET_VIEWPORT_SCALE && m <= WL_NV2A_K_SET_VIEWPORT_SCALE + 12) || immediate_range(m)) {
        return true;
    }
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        if (m == cases[i]) {
            return true;
        }
    }
    return false;
}

static void method_info_make(void) {
    uint32_t m;

    for (m = 0; m < WL_NV2A_K_METHODS; m += 4) {
        bool known = known_method(m);

        bool bulk = m >= 0x100u && m != WL_NV2A_K_NO_OPERATION &&
                    !(m >= WL_NV2A_PULLER_HANDLE_FIRST && m <= WL_NV2A_PULLER_HANDLE_LAST);
        bool window = m - WL_NV2A_K_SET_TRANSFORM_PROGRAM < WINDOWS_SIZE;

        g_method_info[m >> 2] = (uint16_t)((known ? METHOD_KNOWN : 0u) | (immediate_range(m) ? METHOD_IMMEDIATE : 0u) |
                                           (known && !special_method(m) ? METHOD_PLAIN : 0u) |
                                           (bulk ? METHOD_BULK : 0u) | (window ? METHOD_WINDOW : 0u) | dirty_group(m));
        if (m == WL_NV2A_K_SET_TRANSFORM_PROGRAM_LOAD || m == WL_NV2A_K_SET_TRANSFORM_CONSTANT_LOAD ||
            m == WL_NV2A_K_SET_BEGIN_END) {
            g_method_info[m >> 2] |= METHOD_DIRECT;
        }
    }
    for (m = WL_NV2A_K_METHODS; m != 0; m -= 4) {
        uint32_t i = (m - 4u) >> 2, next = m < WL_NV2A_K_METHODS ? g_plain_run[i + 1u] : 0u;

        g_plain_run[i] = (g_method_info[i] & (METHOD_PLAIN | METHOD_BULK)) == (METHOD_PLAIN | METHOD_BULK)
                             ? (uint8_t)(next < 255u ? next + 1u : 255u)
                             : 0u;
    }
}

static void note_unhandled(WlNv2a *dev, uint32_t method, uint32_t param) {
    uint32_t bit = method >> 2;

    if (!(dev->unhandled_logged[bit >> 3] & (1u << (bit & 7u)))) {
        dev->unhandled_logged[bit >> 3] |= (uint8_t)(1u << (bit & 7u));
        WL_LOG_W(WL_NV2A_TAG, "3D method %04x (%08x) kept but not interpreted", (unsigned)method, (unsigned)param);
    }
    dev->stats.unhandled++;
}

static void write_le64(uint8_t *p, uint64_t v) {
    wl_wr_le32(p, (uint32_t)v);
    wl_wr_le32(p + 4, (uint32_t)(v >> 32));
}

static void notify(WlNv2a *dev, uint32_t param) {
    WlNv2aKelvin *k = &dev->kelvin;
    uint8_t *p;

    if (k->dma_notifies.valid && k->dma_notifies.limit >= 15) {
        // the notifier: the time, a return value and the status (0: done)
        p = wl_nv2a_phys_write(dev, k->dma_notifies.base, 16);
        if (p != NULL) {
            write_le64(p, wl_nv2a_time(dev));
            wl_wr_le32(p + 8, 0);
            wl_wr_le32(p + 12, 0);
        }
    }
    dev->stats.notifies++;
    if (param & 1u) {
        // (write, then awaken: the pusher stops until the ISR has seen it, as at a trap, so two in a
        // kick are two interrupts. Whether the GPU's interrupt is on or not, as at a trap: D3D's
        // PGRAPH_FIFO write runs the channel while its ISR still has the interrupts off, and two
        // awakenings there would merge into one)
        wl_nv2a_raise_pgraph(dev, WL_NV2A_PGRAPH_INTR_NOTIFY);
        if ((dev->pgraph_intr_en & WL_NV2A_PGRAPH_INTR_NOTIFY) != 0) {
            wl_nv2a_suspend_current(dev, WL_NV2A_SUSPEND_NOTIFY);
        }
    }
}

static void report(WlNv2a *dev, uint32_t param) {
    WlNv2aKelvin *k = &dev->kelvin;
    uint32_t offset = param & 0x00FFFFFFu;
    uint8_t *p;

    if (offset + 16u > k->dma_report.limit + 1u && k->dma_report.valid) {
        return;
    }
    p = wl_nv2a_phys_write(dev, k->dma_report.base + offset, 16);
    if (p == NULL) {
        return;
    }
    write_le64(p, wl_nv2a_time(dev));
    wl_wr_le32(p + 8, (param >> 24) == WL_NV2A_REPORT_ZPASS_PIXEL_COUNT ? k->zpass_pixels : 0);
    wl_wr_le32(p + 12, 0);
    dev->stats.reports++;
}

static void semaphore_release(WlNv2a *dev, uint32_t value) {
    WlNv2aKelvin *k = &dev->kelvin;
    uint32_t offset = wl_nv2a_k(k, WL_NV2A_K_SET_SEMAPHORE_OFFSET);
    uint8_t *p;

    if (k->dma_semaphore.valid && offset + 4u > k->dma_semaphore.limit + 1u) {
        return;
    }
    p = wl_nv2a_phys_write(dev, k->dma_semaphore.base + offset, 4);
    if (p != NULL) {
        wl_wr_le32(p, value);
        dev->stats.semaphores++;
    }
}

static WlNv2aDma *context_dma(WlNv2aKelvin *k, uint32_t method) {
    switch (method) {
    case WL_NV2A_K_SET_CONTEXT_DMA_NOTIFIES:
        return &k->dma_notifies;
    case WL_NV2A_K_SET_CONTEXT_DMA_A:
        return &k->dma_a;
    case WL_NV2A_K_SET_CONTEXT_DMA_B:
        return &k->dma_b;
    case WL_NV2A_K_SET_CONTEXT_DMA_STATE:
        return &k->dma_state;
    case WL_NV2A_K_SET_CONTEXT_DMA_COLOR:
        return &k->dma_color;
    case WL_NV2A_K_SET_CONTEXT_DMA_ZETA:
        return &k->dma_zeta;
    case WL_NV2A_K_SET_CONTEXT_DMA_VERTEX_A:
        return &k->dma_vertex_a;
    case WL_NV2A_K_SET_CONTEXT_DMA_VERTEX_B:
        return &k->dma_vertex_b;
    case WL_NV2A_K_SET_CONTEXT_DMA_SEMAPHORE:
        return &k->dma_semaphore;
    case WL_NV2A_K_SET_CONTEXT_DMA_REPORT:
        return &k->dma_report;
    default:
        return NULL;
    }
}

// ---- batches ----------------------------------------------------------------------------------

static bool batch_empty(const WlNv2a *dev) {
    return dev->batch_index_count == 0 && dev->batch_inline_bytes == 0 && dev->batch_immediate_count == 0;
}

// the inline vertex's layout: each enabled attribute in order, at an offset a multiple of its
// component's size, the vertex a whole number of words
static uint32_t inline_layout(const WlNv2aKelvin *k, WlNv2aAttrib attribs[WL_NV2A_ATTRIBS], uint32_t *streamed) {
    uint32_t offset = 0;
    unsigned a;

    *streamed = 0;
    for (a = 0; a < WL_NV2A_ATTRIBS; a++) {
        uint32_t format = wl_nv2a_k(k, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_FORMAT + 4 * a);
        uint8_t type = (uint8_t)(format & 0xFu), size = (uint8_t)((format >> 4) & 0xFu);
        uint32_t bytes = wl_nv2a_attrib_bytes(type, size);
        uint32_t unit = type == WL_NV2A_VTX_S1 || type == WL_NV2A_VTX_S32K ? 2u
                        : type == WL_NV2A_VTX_UB_D3D || type == WL_NV2A_VTX_UB_OGL ? 1u
                                                                                    : 4u;

        memset(&attribs[a], 0, sizeof attribs[a]);
        if (bytes == 0) {
            continue;
        }
        offset = (offset + unit - 1u) & ~(unit - 1u);
        attribs[a].type = type;
        attribs[a].size = size;
        attribs[a].inline_offset = (uint16_t)offset;
        offset += bytes;
        *streamed |= 1u << a;
    }
    return (offset + 3u) & ~3u;
}

// (the draws' cuts of wave 4's second six: the arrays' layout kept between draws (layout_arrays), the
// 16-bit index lines in assembly on Broadway (index_run16_cut), the viewport's words straight to their
// constants (viewport_words); false: as the first six left them, a file "oldpush3" in the title's
// folder, WL_XB_OLDPUSH3 on the host and in the replay)
static bool g_push_cuts3 = true;

void wl_nv2a_set_push_cuts3(bool on) {
    g_push_cuts3 = on;
}

static unsigned g_arrays_logged;

// an enabled array's span for indices [least, last] from its start (base + offset), `stride` and
// `bytes` a vertex: its guest-physical address in *phys and its host address (NULL: not in guest RAM).
// (In 64 bits: a guest's 32-bit indices far apart made a 32-bit length wrap to a small one that
// passed the check, and the fetch read past it; a start past 4 GiB wrapped back into guest RAM, to
// other bytes than the index's.)
static const uint8_t *array_span(WlNv2a *dev, unsigned a, uint64_t start, uint32_t stride, uint32_t bytes,
                                 uint32_t least, uint32_t last, uint32_t *phys) {
    uint64_t at = start + (uint64_t)least * stride, length = (uint64_t)(last - least) * stride + bytes;
    const uint8_t *data = NULL;

    *phys = (uint32_t)at;
    if (at <= 0xFFFFFFFFu && length <= 0xFFFFFFFFu) {
        data = wl_nv2a_phys(dev, (uint32_t)at, (uint32_t)length);
    }
    if (data == NULL && g_arrays_logged++ < 8) {
        WL_LOG_W(WL_NV2A_TAG, "vertex array %u at %08x (%u bytes) not in guest RAM", a, (unsigned)at,
                 (unsigned)length);
    }
    return data;
}

// the arrays for indices [least, last]: each enabled attribute's host address of index least
static uint32_t arrays_layout(WlNv2a *dev, uint32_t least, uint32_t last, WlNv2aAttrib attribs[WL_NV2A_ATTRIBS]) {
    const WlNv2aKelvin *k = &dev->kelvin;
    uint32_t streamed = 0;
    unsigned a;

    for (a = 0; a < WL_NV2A_ATTRIBS; a++) {
        uint32_t format = wl_nv2a_k(k, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_FORMAT + 4 * a);
        uint32_t offset = wl_nv2a_k(k, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_OFFSET + 4 * a);
        uint8_t type = (uint8_t)(format & 0xFu), size = (uint8_t)((format >> 4) & 0xFu);
        uint32_t stride = format >> 8, bytes = wl_nv2a_attrib_bytes(type, size);
        const WlNv2aDma *dma = (offset & 0x80000000u) ? &k->dma_vertex_b : &k->dma_vertex_a;
        uint32_t phys;

        memset(&attribs[a], 0, sizeof attribs[a]);
        if (bytes == 0) {
            continue;
        }
        attribs[a].data = array_span(dev, a, (uint64_t)dma->base + (offset & 0x0FFFFFFFu), stride, bytes, least, last,
                                     &phys);
        if (attribs[a].data == NULL) {
            continue;
        }
        attribs[a].phys = phys;
        attribs[a].type = type;
        attribs[a].size = size;
        attribs[a].stride = stride;
        streamed |= 1u << a;
    }
    return streamed;
}

// an enabled array's start: its vertex DMA's base and its offset
static inline uint64_t array_start(const WlNv2aKelvin *k, unsigned a) {
    uint32_t offset = wl_nv2a_k(k, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_OFFSET + 4 * a);
    const WlNv2aDma *dma = (offset & 0x80000000u) ? &k->dma_vertex_b : &k->dma_vertex_a;

    return (uint64_t)dma->base + (offset & 0x0FFFFFFFu);
}

// the arrays' layout kept: each enabled array's format, stride and start, made again only when an
// array's register or a vertex DMA changed (WL_NV2A_DIRTY_ARRAYS) or the state was loaded (layout_ok)
static void layout_make(WlNv2aKelvin *k) {
    unsigned a, n = 0;

    for (a = 0; a < WL_NV2A_ATTRIBS; a++) {
        uint32_t format = wl_nv2a_k(k, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_FORMAT + 4 * a);
        uint8_t type = (uint8_t)(format & 0xFu), size = (uint8_t)((format >> 4) & 0xFu);
        uint32_t bytes = wl_nv2a_attrib_bytes(type, size);

        k->layout_formats[a] = format;
        if (bytes == 0) {
            continue;
        }
        k->layout[n].start = array_start(k, a);
        k->layout[n].stride = format >> 8;
        k->layout[n].bytes = bytes;
        k->layout[n].attrib = (uint8_t)a;
        k->layout[n].type = type;
        k->layout[n].size = size;
        n++;
    }
    k->layout_count = (uint8_t)n;
    k->layout_ok = 1;
}

// the layout's dirty groups for a draw of arrays, taken: this draw's and those kept over the draws without
// arrays since (draw_counted); as wave 5 left them, WL_NV2A_DIRTY_ARRAYS alone means the formats may
// have changed too, and the draws without arrays keep none
static inline uint32_t layout_dirty_take(WlNv2aKelvin *k) {
    uint32_t dirty;

    if (!g_fetch_cuts) {
        return k->dirty & WL_NV2A_DIRTY_ARRAYS ? WL_NV2A_DIRTY_ARRAYS | WL_NV2A_DIRTY_ARRAY_FORMATS : 0u;
    }
    dirty = (k->dirty | k->layout_dirty) & (WL_NV2A_DIRTY_ARRAYS | WL_NV2A_DIRTY_ARRAY_FORMATS);
    k->layout_dirty = 0;
    k->dirty &= ~dirty;
    return dirty;
}

// arrays_layout's work from the layout kept, into attributes already zero
static uint32_t layout_arrays(WlNv2a *dev, uint32_t least, uint32_t last, WlNv2aAttrib attribs[WL_NV2A_ATTRIBS]) {
    WlNv2aKelvin *k = &dev->kelvin;
    uint32_t streamed = 0;
    unsigned i, n;

    uint32_t dirty = layout_dirty_take(k);

    if (!k->layout_ok) {
        layout_make(k);
    } else if (dirty & WL_NV2A_DIRTY_ARRAYS) {
        // (a title moves its arrays far more often than it changes their formats: the formats compared,
        // and when they are the same the enabled arrays' starts alone made again)
        const uint32_t *formats = &k->raw[WL_NV2A_K_SET_VERTEX_DATA_ARRAY_FORMAT >> 2];
        uint32_t differ = 0;

        // (an offset or a vertex DMA alone: the formats as they were)
        if (dirty & WL_NV2A_DIRTY_ARRAY_FORMATS) {
            for (i = 0; i < WL_NV2A_ATTRIBS; i++) {
                differ |= formats[i] ^ k->layout_formats[i];
            }
        }
        if (differ != 0) {
            layout_make(k);
        } else {
            for (i = 0, n = k->layout_count; i < n; i++) {
                k->layout[i].start = array_start(k, k->layout[i].attrib);
            }
        }
    }
    for (i = 0, n = k->layout_count; i < n; i++) {
        WlNv2aAttrib *at = &attribs[k->layout[i].attrib];
        uint32_t phys;

        at->data = array_span(dev, k->layout[i].attrib, k->layout[i].start, k->layout[i].stride, k->layout[i].bytes,
                              least, last, &phys);
        if (at->data == NULL) {
            continue;
        }
        at->phys = phys;
        at->type = k->layout[i].type;
        at->size = k->layout[i].size;
        at->stride = k->layout[i].stride;
        streamed |= 1u << k->layout[i].attrib;
    }
    return streamed;
}

// layout_arrays' work into the kept draw's attributes: each array's entry written whole, the entries
// of the last draw's arrays that are not this one's zeroed (batch_draw_attribs: those that may not be zero);
// the interleaved arrays of one buffer looked up in guest memory once (not while a capture records each
// array's bytes)
static uint32_t layout_arrays_kept(WlNv2a *dev, uint32_t least, uint32_t last) {
    WlNv2aKelvin *k = &dev->kelvin;
    WlNv2aAttrib *attribs = dev->batch_draw.attribs;
    uint32_t streamed = 0, written = 0, stale;
    unsigned i, j, n;

    uint32_t dirty = layout_dirty_take(k);

    if (!k->layout_ok) {
        layout_make(k);
    } else if (dirty & WL_NV2A_DIRTY_ARRAYS) {
        const uint32_t *formats = &k->raw[WL_NV2A_K_SET_VERTEX_DATA_ARRAY_FORMAT >> 2];
        uint32_t differ = 0;

        // (an offset or a vertex DMA alone: the formats as they were)
        if (dirty & WL_NV2A_DIRTY_ARRAY_FORMATS) {
            for (i = 0; i < WL_NV2A_ATTRIBS; i++) {
                differ |= formats[i] ^ k->layout_formats[i];
            }
        }
        if (differ != 0) {
            layout_make(k);
        } else {
            for (i = 0, n = k->layout_count; i < n; i++) {
                k->layout[i].start = array_start(k, k->layout[i].attrib);
            }
        }
    }
    for (i = 0, n = k->layout_count; i < n; i = j) {
        uint32_t stride = k->layout[i].stride;
        uint64_t first = k->layout[i].start, lo = first, top = first + k->layout[i].bytes, from = 0;
        const uint8_t *host = NULL;
        unsigned m;

        // (the arrays of one buffer, interleaved: of the same stride, each starting within a stride of the
        // first, are one span of guest memory, asked for once; whether they are does not hang on the
        // indices, which move them all alike)
        for (j = i + 1; j < n && k->layout[j].stride == stride; j++) {
            uint64_t start = k->layout[j].start, end = start + k->layout[j].bytes;

            if (start + stride <= first || start >= first + stride) {
                break;
            }
            lo = start < lo ? start : lo;
            top = end > top ? end : top;
        }
        // (within guest RAM as it is, not a mirror: the span is found, or each array's own refusal logged)
        if (j - i > 1 && dev->capture == NULL) {
            uint64_t end;

            from = lo + (uint64_t)least * stride;
            end = from + (uint64_t)(last - least) * stride + (top - lo);
            if (end <= dev->ram_bytes) {
                host = wl_nv2a_phys(dev, (uint32_t)from, (uint32_t)(end - from));
            }
        }
        for (m = i; m < j; m++) {
            unsigned a = k->layout[m].attrib;
            WlNv2aAttrib *at = &attribs[a];
            uint32_t phys;

            written |= 1u << a;
            if (host != NULL) {
                // (its start's distance from the span's, less than a stride)
                uint32_t offset = (uint32_t)k->layout[m].start - (uint32_t)lo;

                at->data = host + offset;
                phys = (uint32_t)from + offset;
            } else {
                at->data = array_span(dev, a, k->layout[m].start, stride, k->layout[m].bytes, least, last, &phys);
                if (at->data == NULL) {
                    memset(at, 0, sizeof *at);
                    continue;
                }
            }
            at->phys = phys;
            at->type = k->layout[m].type;
            at->size = k->layout[m].size;
            at->stride = stride;
            at->inline_offset = 0;
            streamed |= 1u << a;
        }
    }
    for (stale = dev->batch_draw_attribs & ~written; stale != 0; stale &= stale - 1u) {
        memset(&attribs[__builtin_ctz(stale)], 0, sizeof attribs[0]);
    }
    dev->batch_draw_attribs = written;
    return streamed;
}

// the kept draw's attributes all zero (an immediate draw's)
static void draw_attribs_clear(WlNv2a *dev) {
    uint32_t stale;

    for (stale = dev->batch_draw_attribs; stale != 0; stale &= stale - 1u) {
        memset(&dev->batch_draw.attribs[__builtin_ctz(stale)], 0, sizeof dev->batch_draw.attribs[0]);
    }
    dev->batch_draw_attribs = 0;
}

static void draw_counted(WlNv2a *dev, WlNv2aDraw *draw) {
    WlNv2aKelvin *k = &dev->kelvin;
    uint32_t pixels = 0;

    draw->serial = ++k->draw_serial;
    dev->stats.draws++;
    dev->stats.vertices += draw->count;
    if (wl_nv2a_k(k, WL_NV2A_K_SET_STENCIL_TEST_ENABLE)) {
        if (dev->stats.stencil_draws++ == 0) {
            WL_LOG_W(WL_NV2A_TAG, "draw %u uses the stencil test, which GX does not have", (unsigned)draw->serial);
        }
    }
    if (!wl_nv2a_program_mode(k)) {
        if (dev->stats.fixed_function_draws++ == 0) {
            WL_LOG_W(WL_NV2A_TAG, "draw %u is in the fixed-function transform mode", (unsigned)draw->serial);
        }
    }
    if (dev->has_backend && dev->backend.draw != NULL) {
        pixels = dev->backend.draw(dev->backend.user, k, draw);
    }
    if (wl_nv2a_k(k, WL_NV2A_K_SET_ZPASS_PIXEL_COUNT_ENABLE)) {
        k->zpass_pixels += pixels;
    }
    // (a draw without arrays leaves the layout's groups for the next with them: layout_dirty_take)
    k->layout_dirty = (uint8_t)(k->layout_dirty | (k->dirty & (WL_NV2A_DIRTY_ARRAYS | WL_NV2A_DIRTY_ARRAY_FORMATS)));
    k->dirty = 0;
}

static bool g_index_bounds = true;

void wl_nv2a_set_index_bounds(bool on) {
    g_index_bounds = on;
}

// indices [at, at + n) of the batch, whose least and greatest are `least` and `last`, came: the
// bounds kept go on over them when they reach `at`
static inline void bounds_add(WlNv2a *dev, uint32_t at, uint32_t n, uint32_t least, uint32_t last) {
    if (dev->batch_bounds_at != at || n == 0) {
        return;
    }
    if (at != 0) {
        least = dev->batch_least < least ? dev->batch_least : least;
        last = dev->batch_last > last ? dev->batch_last : last;
    }
    dev->batch_least = least;
    dev->batch_last = last;
    dev->batch_bounds_at = at + n;
}

// the least and greatest of the batch's first `count` indices (those found as they came, then the rest)
static inline void batch_bounds(const WlNv2a *dev, uint32_t count, uint32_t *least_out, uint32_t *last_out) {
    uint32_t least = 0xFFFFFFFFu, last = 0, i = 0;

    if (g_index_bounds && dev->batch_bounds_at != 0 && dev->batch_bounds_at <= count) {
        least = dev->batch_least;
        last = dev->batch_last;
        i = dev->batch_bounds_at;
    }
    for (; i < count; i++) {
        uint32_t index = dev->batch_indices[i];

        least = index < least ? index : least;
        last = index > last ? index : last;
    }
    *least_out = least;
    *last_out = last;
}

// batch_draw's work in the kept draw (dev->batch_draw): no draw zeroed whole, its header written field by field
// and its attributes as the source needs (an inline draw's all written by inline_layout)
static void batch_draw_kept(WlNv2a *dev, uint32_t primitive, uint32_t count, bool continues) {
    WlNv2aKelvin *k = &dev->kelvin;
    WlNv2aDraw *draw = &dev->batch_draw;

    draw->primitive = (uint8_t)primitive;
    draw->source = dev->batch_source;
    draw->continues = continues;
    draw->count = count;
    draw->indices = NULL;
    draw->index_base = draw->index_last = 0;
    draw->inline_data = NULL;
    draw->inline_stride = 0;
    draw->immediate = NULL;
    draw->serial = 0;
    switch (dev->batch_source) {
    case WL_NV2A_SOURCE_ARRAYS: {
        uint32_t least, last;

        batch_bounds(dev, count, &least, &last);
        draw->indices = dev->batch_indices;
        draw->index_base = least;
        draw->index_last = last;
        draw->streamed = layout_arrays_kept(dev, least, last);
        break;
    }
    case WL_NV2A_SOURCE_INLINE:
        draw->inline_data = dev->batch_inline;
        draw->inline_stride = inline_layout(k, draw->attribs, &draw->streamed);
        dev->batch_draw_attribs = draw->streamed;
        break;
    default:
        draw_attribs_clear(dev);
        draw->immediate = (const float (*)[WL_NV2A_ATTRIBS][4])dev->batch_immediate;
        draw->streamed = (1u << WL_NV2A_ATTRIBS) - 1u;
        break;
    }
    draw_counted(dev, draw);
}

// hands over `count` of the batch's vertices (from the start), as one draw
static void batch_draw(WlNv2a *dev, uint32_t primitive, uint32_t count, bool continues) {
    WlNv2aKelvin *k = &dev->kelvin;
    WlNv2aDraw draw;
    WlNv2aPrimInfo info;

    wl_nv2a_prim_info(primitive, &info);
    if (count < info.first) {
        return;
    }
    if (g_push_cuts4) {
        batch_draw_kept(dev, primitive, count, continues);
        return;
    }
    memset(&draw, 0, sizeof draw);
    draw.primitive = (uint8_t)primitive;
    draw.source = dev->batch_source;
    draw.continues = continues;
    draw.count = count;
    switch (dev->batch_source) {
    case WL_NV2A_SOURCE_ARRAYS: {
        uint32_t least = 0xFFFFFFFFu, last = 0, i = 0;

        // (those found as they came, then the rest)
        if (g_index_bounds && dev->batch_bounds_at != 0 && dev->batch_bounds_at <= count) {
            least = dev->batch_least;
            last = dev->batch_last;
            i = dev->batch_bounds_at;
        }
        for (; i < count; i++) {
            uint32_t index = dev->batch_indices[i];

            least = index < least ? index : least;
            last = index > last ? index : last;
        }
        draw.indices = dev->batch_indices;
        draw.index_base = least;
        draw.index_last = last;
        draw.streamed = g_push_cuts3 ? layout_arrays(dev, least, last, draw.attribs)
                                    : arrays_layout(dev, least, last, draw.attribs);
        break;
    }
    case WL_NV2A_SOURCE_INLINE:
        draw.inline_data = dev->batch_inline;
        draw.inline_stride = inline_layout(k, draw.attribs, &draw.streamed);
        break;
    default:
        draw.immediate = (const float (*)[WL_NV2A_ATTRIBS][4])dev->batch_immediate;
        draw.streamed = (1u << WL_NV2A_ATTRIBS) - 1u;
        break;
    }
    draw_counted(dev, &draw);
}

// the vertices of the batch: kept at the front, `keep` of them from `from` (a part's tail)
static void batch_keep(WlNv2a *dev, uint32_t from, uint32_t keep, uint32_t inline_stride) {
    switch (dev->batch_source) {
    case WL_NV2A_SOURCE_ARRAYS:
        memmove(dev->batch_indices, dev->batch_indices + from, keep * sizeof *dev->batch_indices);
        dev->batch_index_count = keep;
        dev->batch_bounds_at = 0;
        break;
    case WL_NV2A_SOURCE_INLINE:
        memmove(dev->batch_inline, dev->batch_inline + (size_t)from * inline_stride, (size_t)keep * inline_stride);
        dev->batch_inline_bytes = keep * inline_stride;
        break;
    default:
        memmove(dev->batch_immediate, dev->batch_immediate + from, keep * sizeof *dev->batch_immediate);
        dev->batch_immediate_count = keep;
        break;
    }
}

static void batch_copy_vertex(WlNv2a *dev, uint32_t to, uint32_t from, uint32_t inline_stride) {
    switch (dev->batch_source) {
    case WL_NV2A_SOURCE_ARRAYS:
        dev->batch_indices[to] = dev->batch_indices[from];
        dev->batch_bounds_at = 0;
        break;
    case WL_NV2A_SOURCE_INLINE:
        memmove(dev->batch_inline + (size_t)to * inline_stride, dev->batch_inline + (size_t)from * inline_stride,
                inline_stride);
        break;
    default:
        memmove(dev->batch_immediate[to], dev->batch_immediate[from], sizeof dev->batch_immediate[0]);
        break;
    }
}

static void batch_reset(WlNv2a *dev) {
    dev->batch_index_count = 0;
    dev->batch_bounds_at = 0;
    dev->batch_inline_bytes = 0;
    dev->batch_immediate_count = 0;
    dev->batch_parts = 0;
}

// The batch handed over: all of it (final) or a part of whole primitives, the rest kept with
// the vertices the next part goes on from.
static void batch_flush(WlNv2a *dev, bool final) {
    uint32_t primitive = dev->kelvin.primitive, n, stride = 0;
    WlNv2aPrimInfo info;

    if (dev->batch_source == WL_NV2A_SOURCE_INLINE) {
        WlNv2aAttrib attribs[WL_NV2A_ATTRIBS];
        uint32_t streamed;

        stride = inline_layout(&dev->kelvin, attribs, &streamed);
        n = stride != 0 ? dev->batch_inline_bytes / stride : 0;
    } else if (dev->batch_source == WL_NV2A_SOURCE_ARRAYS) {
        n = dev->batch_index_count;
    } else {
        n = dev->batch_immediate_count;
    }
    wl_nv2a_prim_info(primitive, &info);
    if (info.first == 0) {
        batch_reset(dev);
        return;
    }
    if (final) {
        if (primitive == WL_NV2A_PRIM_LINE_LOOP && n >= 2) {
            // the loop closed: its first vertex again (kept aside when it went in parts)
            if (dev->batch_parts != 0) {
                if (dev->batch_source == WL_NV2A_SOURCE_ARRAYS && n < WL_NV2A_BATCH_INDICES) {
                    dev->batch_indices[n++] = dev->batch_loop_first[0];
                }
            } else if (dev->batch_source == WL_NV2A_SOURCE_ARRAYS ? n < WL_NV2A_BATCH_INDICES
                       : dev->batch_source == WL_NV2A_SOURCE_INLINE ? (n + 1) * stride <= WL_NV2A_BATCH_INLINE_BYTES
                                                                    : n < WL_NV2A_BATCH_IMMEDIATE) {
                batch_copy_vertex(dev, n, 0, stride);
                n++;
            }
            primitive = WL_NV2A_PRIM_LINE_STRIP;
        }
        batch_draw(dev, primitive, n, false);
        batch_reset(dev);
        return;
    }
    {
        uint32_t emit, from, keep;

        if (info.fan) {
            emit = n;
            batch_draw(dev, primitive, emit, true);
            // the fan goes on from its first and last vertex
            batch_copy_vertex(dev, 1, n - 1, stride);
            keep = 2, from = 0;
        } else if (info.strip) {
            if (primitive == WL_NV2A_PRIM_LINE_LOOP || primitive == WL_NV2A_PRIM_LINE_STRIP) {
                if (primitive == WL_NV2A_PRIM_LINE_LOOP && dev->batch_parts == 0 &&
                    dev->batch_source == WL_NV2A_SOURCE_ARRAYS) {
                    dev->batch_loop_first[0] = dev->batch_indices[0];
                }
                emit = n;
                batch_draw(dev, WL_NV2A_PRIM_LINE_STRIP, emit, true);
                from = n - 1, keep = 1;
            } else {
                emit = n & ~1u;
                batch_draw(dev, primitive, emit, true);
                from = emit - 2, keep = n - from;
            }
        } else {
            emit = n - n % info.step;
            batch_draw(dev, primitive, emit, true);
            from = emit, keep = n - emit;
        }
        batch_keep(dev, from, keep, stride);
        dev->batch_parts++;
    }
}

static bool batch_open(WlNv2a *dev, uint8_t source) {
    if (dev->kelvin.primitive == 0) {
        static unsigned logged;

        if (logged++ < 8) {
            WL_LOG_W(WL_NV2A_TAG, "vertices outside a begin/end dropped");
        }
        return false;
    }
    if (batch_empty(dev)) {
        dev->batch_source = source;
    } else if (dev->batch_source != source) {
        // another kind of vertex in the same begin/end: what came is drawn first
        if (!dev->batch_mixed) {
            dev->batch_mixed = 1;
            WL_LOG_W(WL_NV2A_TAG, "a begin/end with two kinds of vertices: drawn as two");
        }
        batch_flush(dev, true);
        dev->batch_source = source;
    }
    return true;
}

static void batch_index(WlNv2a *dev, uint32_t index) {
    if (dev->batch_index_count == WL_NV2A_BATCH_INDICES) {
        batch_flush(dev, false);
    }
    dev->batch_indices[dev->batch_index_count++] = index;
}

static void emit_immediate(WlNv2a *dev) {
    if (dev->kelvin.primitive == 0) {
        return;
    }
    if (!batch_open(dev, WL_NV2A_SOURCE_IMMEDIATE)) {
        return;
    }
    if (dev->batch_immediate_count == WL_NV2A_BATCH_IMMEDIATE) {
        batch_flush(dev, false);
    }
    memcpy(dev->batch_immediate[dev->batch_immediate_count++], dev->kelvin.current, sizeof dev->kelvin.current);
}

static void inline_word(WlNv2a *dev, uint32_t word) {
    if (!batch_open(dev, WL_NV2A_SOURCE_INLINE)) {
        return;
    }
    if (dev->batch_inline_bytes + 4u > WL_NV2A_BATCH_INLINE_BYTES) {
        WlNv2aAttrib attribs[WL_NV2A_ATTRIBS];
        uint32_t streamed, stride = inline_layout(&dev->kelvin, attribs, &streamed);

        if (stride == 0) {
            return;
        }
        // (a part of whole vertices: a vertex being written stays with the rest)
        {
            uint32_t whole = dev->batch_inline_bytes / stride * stride, partial = dev->batch_inline_bytes - whole;
            uint8_t tail[256];

            if (partial > sizeof tail) {
                partial = 0;
            }
            memcpy(tail, dev->batch_inline + whole, partial);
            dev->batch_inline_bytes = whole;
            batch_flush(dev, false);
            memcpy(dev->batch_inline + dev->batch_inline_bytes, tail, partial);
            dev->batch_inline_bytes += partial;
        }
    }
    wl_wr_le32(dev->batch_inline + dev->batch_inline_bytes, word);
    dev->batch_inline_bytes += 4;
}

static void begin_end(WlNv2a *dev, uint32_t param) {
    WlNv2aKelvin *k = &dev->kelvin;

    if (param == WL_NV2A_PRIM_END) {
        if (k->primitive != 0) {
            batch_flush(dev, true);
        }
        k->primitive = 0;
        dev->batch_mixed = 0;
        return;
    }
    if (k->primitive != 0) {
        WL_LOG_W(WL_NV2A_TAG, "begin %u inside begin %u: the first ended", (unsigned)param, (unsigned)k->primitive);
        batch_flush(dev, true);
    }
    k->primitive = param <= WL_NV2A_PRIM_POLYGON ? param : 0;
    batch_reset(dev);
}

// ---- the immediate attributes ------------------------------------------------------------------

static void set_current(WlNv2aKelvin *k, unsigned attrib, unsigned component, float value) {
    k->current[attrib][component] = value;
}

static void set_current4(WlNv2aKelvin *k, unsigned attrib, float x, float y, float z, float w) {
    k->current[attrib][0] = x;
    k->current[attrib][1] = y;
    k->current[attrib][2] = z;
    k->current[attrib][3] = w;
}

static void bytes4(WlNv2aKelvin *k, unsigned attrib, uint32_t word) {
    set_current4(k, attrib, (float)(word & 0xFFu) * (1.0f / 255.0f), (float)((word >> 8) & 0xFFu) * (1.0f / 255.0f),
                 (float)((word >> 16) & 0xFFu) * (1.0f / 255.0f), (float)(word >> 24) * (1.0f / 255.0f));
}

// the texture coordinate methods: 2F at base, 2S at base + 8, 4F at base + 0x10, 4S at base + 0x20
static bool texcoord_method(WlNv2aKelvin *k, uint32_t method, uint32_t param) {
    static const uint32_t bases[4] = {WL_NV2A_K_SET_TEXCOORD0_2F, WL_NV2A_K_SET_TEXCOORD1_2F, WL_NV2A_K_SET_TEXCOORD2_2F,
                                      WL_NV2A_K_SET_TEXCOORD3_2F};
    unsigned t;

    for (t = 0; t < 4; t++) {
        uint32_t base = bases[t], attrib = 9 + t;

        if (method == base || method == base + 4) {
            if (method == base) {
                set_current4(k, attrib, word_float(param), k->current[attrib][1], 0.0f, 1.0f);
            } else {
                set_current(k, attrib, 1, word_float(param));
            }
            return true;
        }
        if (method == base + 8) {
            set_current4(k, attrib, (float)(int16_t)(param & 0xFFFFu), (float)(int16_t)(param >> 16), 0.0f, 1.0f);
            return true;
        }
        if (method >= base + 0x10 && method <= base + 0x1C) {
            set_current(k, attrib, (method - base - 0x10) >> 2, word_float(param));
            return true;
        }
        if (method == base + 0x20 || method == base + 0x24) {
            unsigned c = method == base + 0x20 ? 0 : 2;

            set_current(k, attrib, c, (float)(int16_t)(param & 0xFFFFu));
            set_current(k, attrib, c + 1, (float)(int16_t)(param >> 16));
            return true;
        }
    }
    return false;
}

// the generic attributes' methods (SET_VERTEX_DATA*, from WL_NV2A_K_SET_VERTEX_DATA2F_M on; the
// four floats first, what a title sends most)
static void vertex_data_method(WlNv2a *dev, uint32_t method, uint32_t param) {
    WlNv2aKelvin *k = &dev->kelvin;
    float f = word_float(param);

    if (method >= WL_NV2A_K_SET_VERTEX_DATA4F_M) {
        unsigned slot = (method - WL_NV2A_K_SET_VERTEX_DATA4F_M) >> 4, c = ((method - WL_NV2A_K_SET_VERTEX_DATA4F_M) >> 2) & 3u;

        set_current(k, slot, c, f);
        if (c == 3 && slot == 0) {
            emit_immediate(dev);
        }
    } else if (method < WL_NV2A_K_SET_VERTEX_DATA2S) {
        unsigned slot = (method - WL_NV2A_K_SET_VERTEX_DATA2F_M) >> 3, c = ((method - WL_NV2A_K_SET_VERTEX_DATA2F_M) >> 2) & 1u;

        set_current(k, slot, c, f);
        if (c == 1) {
            k->current[slot][2] = 0.0f;
            k->current[slot][3] = 1.0f;
            if (slot == 0) {
                emit_immediate(dev);
            }
        }
    } else if (method < WL_NV2A_K_SET_VERTEX_DATA4UB) {
        unsigned slot = (method - WL_NV2A_K_SET_VERTEX_DATA2S) >> 2;

        set_current4(k, slot, (float)(int16_t)(param & 0xFFFFu), (float)(int16_t)(param >> 16), 0.0f, 1.0f);
        if (slot == 0) {
            emit_immediate(dev);
        }
    } else if (method < WL_NV2A_K_SET_VERTEX_DATA4S_M) {
        unsigned slot = (method - WL_NV2A_K_SET_VERTEX_DATA4UB) >> 2;

        bytes4(k, slot, param);
        if (slot == 0) {
            emit_immediate(dev);
        }
    } else {
        unsigned slot = (method - WL_NV2A_K_SET_VERTEX_DATA4S_M) >> 3, c = ((method - WL_NV2A_K_SET_VERTEX_DATA4S_M) >> 2) & 1u;

        set_current(k, slot, 2 * c, (float)(int16_t)(param & 0xFFFFu));
        set_current(k, slot, 2 * c + 1, (float)(int16_t)(param >> 16));
        if (c == 1 && slot == 0) {
            emit_immediate(dev);
        }
    }
}

static void immediate_method(WlNv2a *dev, uint32_t method, uint32_t param) {
    WlNv2aKelvin *k = &dev->kelvin;
    float f = word_float(param);

    if (method >= WL_NV2A_K_SET_VERTEX_DATA2F_M) {
        vertex_data_method(dev, method, param);
    } else if (method >= WL_NV2A_K_SET_VERTEX3F && method <= WL_NV2A_K_SET_VERTEX3F + 8) {
        unsigned c = (method - WL_NV2A_K_SET_VERTEX3F) >> 2;

        set_current(k, 0, c, f);
        if (c == 2) {
            k->current[0][3] = 1.0f;
            emit_immediate(dev);
        }
    } else if (method >= WL_NV2A_K_SET_VERTEX4F && method <= WL_NV2A_K_SET_VERTEX4F + 12) {
        unsigned c = (method - WL_NV2A_K_SET_VERTEX4F) >> 2;

        set_current(k, 0, c, f);
        if (c == 3) {
            emit_immediate(dev);
        }
    } else if (method == WL_NV2A_K_SET_VERTEX4S || method == WL_NV2A_K_SET_VERTEX4S + 4) {
        unsigned c = method == WL_NV2A_K_SET_VERTEX4S ? 0 : 2;

        set_current(k, 0, c, (float)(int16_t)(param & 0xFFFFu));
        set_current(k, 0, c + 1, (float)(int16_t)(param >> 16));
        if (c == 2) {
            emit_immediate(dev);
        }
    } else if (method >= WL_NV2A_K_SET_NORMAL3F && method <= WL_NV2A_K_SET_NORMAL3F + 8) {
        set_current(k, 2, (method - WL_NV2A_K_SET_NORMAL3F) >> 2, f);
        k->current[2][3] = 1.0f;
    } else if (method == WL_NV2A_K_SET_NORMAL3S || method == WL_NV2A_K_SET_NORMAL3S + 4) {
        if (method == WL_NV2A_K_SET_NORMAL3S) {
            set_current(k, 2, 0, normalized((int16_t)(param & 0xFFFFu), 32767.0f));
            set_current(k, 2, 1, normalized((int16_t)(param >> 16), 32767.0f));
        } else {
            set_current(k, 2, 2, normalized((int16_t)(param & 0xFFFFu), 32767.0f));
        }
    } else if (method >= WL_NV2A_K_SET_DIFFUSE_COLOR4F && method <= WL_NV2A_K_SET_DIFFUSE_COLOR4F + 12) {
        set_current(k, 3, (method - WL_NV2A_K_SET_DIFFUSE_COLOR4F) >> 2, f);
    } else if (method >= WL_NV2A_K_SET_DIFFUSE_COLOR3F && method <= WL_NV2A_K_SET_DIFFUSE_COLOR3F + 8) {
        set_current(k, 3, (method - WL_NV2A_K_SET_DIFFUSE_COLOR3F) >> 2, f);
        k->current[3][3] = 1.0f;
    } else if (method == WL_NV2A_K_SET_DIFFUSE_COLOR4I) {
        bytes4(k, 3, param);
    } else if (method >= WL_NV2A_K_SET_SPECULAR_COLOR4F && method <= WL_NV2A_K_SET_SPECULAR_COLOR4F + 12) {
        set_current(k, 4, (method - WL_NV2A_K_SET_SPECULAR_COLOR4F) >> 2, f);
    } else if (method >= WL_NV2A_K_SET_SPECULAR_COLOR3F && method <= WL_NV2A_K_SET_SPECULAR_COLOR3F + 8) {
        set_current(k, 4, (method - WL_NV2A_K_SET_SPECULAR_COLOR3F) >> 2, f);
        k->current[4][3] = 1.0f;
    } else if (method == WL_NV2A_K_SET_SPECULAR_COLOR4I) {
        bytes4(k, 4, param);
    } else if (texcoord_method(k, method, param)) {
    } else if (method == WL_NV2A_K_SET_FOG_COORD) {
        set_current(k, 5, 0, f);
    } else if (method == WL_NV2A_K_SET_WEIGHT1F) {
        set_current(k, 1, 0, f);
    } else if (method >= WL_NV2A_K_SET_WEIGHT4F && method <= WL_NV2A_K_SET_WEIGHT4F + 12) {
        set_current(k, 1, (method - WL_NV2A_K_SET_WEIGHT4F) >> 2, f);
    }
}

static bool immediate_range(uint32_t method) {
    return (method >= 0x1500 && method <= 0x16CC && method != WL_NV2A_K_SET_EDGE_FLAG) ||
           (method >= WL_NV2A_K_SET_VERTEX_DATA2F_M && method < WL_NV2A_K_SET_VERTEX_DATA4F_M + 0x100);
}

// ---- the dispatch -----------------------------------------------------------------------------

static inline void window_word(WlNv2aKelvin *k, uint32_t method, uint32_t param) {
    unsigned word = (method >> 2) & 3u;

    if (method < WL_NV2A_K_SET_TRANSFORM_CONSTANT) {
        // (a word written again as it was changes nothing: no new generation, nothing dirty)
        if (k->program_load < WL_NV2A_PROGRAM_SLOTS && k->program[k->program_load][word] != param) {
            k->program[k->program_load][word] = param;
            k->program_generation++;
            k->dirty |= WL_NV2A_DIRTY_PROGRAM;
        }
        if (word == 3) {
            k->program_load++;
        }
        k->program_word = word;
        return;
    }
    if (k->constant_load < WL_NV2A_CONSTANTS) {
        float value = word_float(param);

        if (memcmp(&k->constants[k->constant_load][word], &value, sizeof value) != 0) {
            k->constants[k->constant_load][word] = value;
            k->constants_generation++;
            k->dirty |= WL_NV2A_DIRTY_CONSTANTS;
        }
    }
    if (word == 3) {
        k->constant_load++;
    }
    k->constant_word = word;
}

void wl_nv2a_kelvin_method(WlNv2a *dev, uint32_t method, uint32_t param) {
    WlNv2aKelvin *k = &dev->kelvin;
    uint32_t info;
    bool changed;

    method &= WL_NV2A_K_METHODS - 4u;
    info = g_method_info[method >> 2];
    if (info == 0) {
        method_info_make();
        info = g_method_info[method >> 2];
    }
    if (info & METHOD_PLAIN) {
        // (the state groups are dirty when a value changes, not when it is written again)
        if (k->raw[method >> 2] != param) {
            k->raw[method >> 2] = param;
            k->dirty |= info & METHOD_DIRTY;
        }
        return;
    }
    if (!(info & METHOD_KNOWN)) {
        note_unhandled(dev, method, param);
    }

    if (method - WL_NV2A_K_SET_TRANSFORM_PROGRAM < WINDOWS_SIZE) {
        window_word(k, method, param);
        return;
    }

    // (the state groups are dirty when a value changes, not when it is written again)
    changed = k->raw[method >> 2] != param;
    k->raw[method >> 2] = param;
    if (info & METHOD_IMMEDIATE) {
        immediate_method(dev, method, param);
        return;
    }

    switch (method) {
    case WL_NV2A_K_NOTIFY:
        notify(dev, param);
        break;
    case WL_NV2A_K_SET_FLIP_READ:
        k->flip_read = param;
        break;
    case WL_NV2A_K_SET_FLIP_WRITE:
        k->flip_write = param;
        break;
    case WL_NV2A_K_SET_FLIP_MODULO:
        k->flip_modulo = param;
        break;
    case WL_NV2A_K_FLIP_INCREMENT_WRITE:
        k->flip_write = k->flip_modulo != 0 ? (k->flip_write + 1u) % k->flip_modulo : k->flip_write + 1u;
        k->frames++;
        dev->stats.frames++;
        if (dev->has_backend && dev->backend.frame_done != NULL) {
            dev->backend.frame_done(dev->backend.user, k);
        }
        break;
    case WL_NV2A_K_FLIP_STALL:
        // (nothing to wait for: the frames before are done)
        break;
    case WL_NV2A_K_SET_CONTEXT_DMA_NOTIFIES:
    case WL_NV2A_K_SET_CONTEXT_DMA_A:
    case WL_NV2A_K_SET_CONTEXT_DMA_B:
    case WL_NV2A_K_SET_CONTEXT_DMA_STATE:
    case WL_NV2A_K_SET_CONTEXT_DMA_COLOR:
    case WL_NV2A_K_SET_CONTEXT_DMA_ZETA:
    case WL_NV2A_K_SET_CONTEXT_DMA_VERTEX_A:
    case WL_NV2A_K_SET_CONTEXT_DMA_VERTEX_B:
    case WL_NV2A_K_SET_CONTEXT_DMA_SEMAPHORE:
    case WL_NV2A_K_SET_CONTEXT_DMA_REPORT:
        wl_nv2a_dma_load(dev, param, context_dma(k, method));
        k->dirty |= WL_NV2A_DIRTY_SURFACE | WL_NV2A_DIRTY_TEXTURES | WL_NV2A_DIRTY_ARRAYS;
        break;
    case WL_NV2A_K_SET_TRANSFORM_PROGRAM_LOAD:
        k->program_load = param;
        k->program_word = 0;
        break;
    case WL_NV2A_K_SET_TRANSFORM_CONSTANT_LOAD:
        k->constant_load = param;
        k->constant_word = 0;
        break;
    case WL_NV2A_K_SET_TRANSFORM_PROGRAM_START:
    case WL_NV2A_K_SET_TRANSFORM_EXECUTION_MODE:
        if (changed) {
            k->program_generation++;
            k->dirty |= WL_NV2A_DIRTY_PROGRAM;
        }
        break;
    case WL_NV2A_K_CLEAR_REPORT_VALUE:
        if (param == WL_NV2A_REPORT_ZPASS_PIXEL_COUNT) {
            k->zpass_pixels = 0;
        }
        break;
    case WL_NV2A_K_GET_REPORT:
        report(dev, param);
        break;
    case WL_NV2A_K_SET_BEGIN_END:
        begin_end(dev, param);
        break;
    case WL_NV2A_K_ARRAY_ELEMENT16:
        if (batch_open(dev, WL_NV2A_SOURCE_ARRAYS)) {
            batch_index(dev, param & 0xFFFFu);
            batch_index(dev, param >> 16);
        }
        break;
    case WL_NV2A_K_ARRAY_ELEMENT32:
        if (batch_open(dev, WL_NV2A_SOURCE_ARRAYS)) {
            batch_index(dev, param);
        }
        break;
    case WL_NV2A_K_DRAW_ARRAYS:
        if (batch_open(dev, WL_NV2A_SOURCE_ARRAYS)) {
            uint32_t first = param & 0x00FFFFFFu, count = (param >> 24) + 1u, i;

            if (count <= WL_NV2A_BATCH_INDICES - dev->batch_index_count) {
                // (room for them all: no flush to test for)
                uint32_t *to = dev->batch_indices + dev->batch_index_count;

                for (i = 0; i < count; i++) {
                    to[i] = first + i;
                }
                if (g_index_bounds) {
                    bounds_add(dev, dev->batch_index_count, count, first, first + count - 1u);
                }
                dev->batch_index_count += count;
                break;
            }
            for (i = 0; i < count; i++) {
                batch_index(dev, first + i);
            }
        }
        break;
    case WL_NV2A_K_INLINE_ARRAY:
        inline_word(dev, param);
        break;
    case WL_NV2A_K_BACK_END_WRITE_SEMAPHORE_RELEASE:
        semaphore_release(dev, param);
        break;
    case WL_NV2A_K_CLEAR_SURFACE:
        dev->stats.clears++;
        if (dev->has_backend && dev->backend.clear != NULL) {
            dev->backend.clear(dev->backend.user, k, param);
        }
        break;
    default:
        if (method >= WL_NV2A_K_SET_VIEWPORT_OFFSET && method <= WL_NV2A_K_SET_VIEWPORT_SCALE + 12 &&
            (method <= WL_NV2A_K_SET_VIEWPORT_OFFSET + 12 || method >= WL_NV2A_K_SET_VIEWPORT_SCALE)) {
            // (the viewport's words are constants: changed if the constant is, the window may have
            // written it since)
            float *slot = method >= WL_NV2A_K_SET_VIEWPORT_SCALE
                              ? &k->constants[WL_NV2A_CONSTANT_VIEWPORT_SCALE][(method - WL_NV2A_K_SET_VIEWPORT_SCALE) >> 2]
                              : &k->constants[WL_NV2A_CONSTANT_VIEWPORT_OFFSET][(method - WL_NV2A_K_SET_VIEWPORT_OFFSET) >> 2];
            float value = word_float(param);

            if (memcmp(slot, &value, sizeof value) != 0) {
                *slot = value;
                k->constants_generation++;
                k->dirty |= WL_NV2A_DIRTY_VIEWPORT | WL_NV2A_DIRTY_CONSTANTS;
            }
        } else if (changed) {
            k->dirty |= info & METHOD_DIRTY;
        }
        break;
    }
}

// (a slot's words read and written as words, whether they are a program's or a constant's floats)
typedef uint32_t __attribute__((may_alias)) SlotWord;

// one slot's four words from the push buffer at `w` into `slot` (a program's words, or a constant's
// bits): the words that change counted, as window_word counts them one by one
static inline __attribute__((always_inline)) uint32_t slot_words(void *slot, const uint8_t *w) {
    SlotWord *s = slot;
    uint32_t a = wl_rd_le32(w), b = wl_rd_le32(w + 4), c = wl_rd_le32(w + 8), d = wl_rd_le32(w + 12);
    uint32_t changes = (uint32_t)(s[0] != a) + (uint32_t)(s[1] != b) + (uint32_t)(s[2] != c) + (uint32_t)(s[3] != d);

    s[0] = a;
    s[1] = b;
    s[2] = c;
    s[3] = d;
    return changes;
}

// an increasing run inside one of the windows (wl_nv2a_kelvin_push: `method` and the run's last word
// in the same window): what window_word makes of each word, the slot and its bound kept here; the
// slots the run fills whole, four words at once
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("no-shrink-wrap-separate")))
#endif
static __attribute__((noinline)) void window_run(WlNv2aKelvin *k, uint32_t method, const uint8_t *words,
                                                 uint32_t count) {
    bool program = method < WL_NV2A_K_SET_TRANSFORM_CONSTANT;
    unsigned word = (method >> 2) & 3u, last = word;
    uint32_t load = program ? k->program_load : k->constant_load;
    uint32_t bound = program ? WL_NV2A_PROGRAM_SLOTS : WL_NV2A_CONSTANTS;
    uint32_t n = 0, changes = 0;

    while (n < count) {
        if (word == 0 && count - n >= 4u && load < bound) {
            void *slot = program ? (void *)k->program[load] : (void *)k->constants[load];

            changes += slot_words(slot, words + 4u * n);
            n += 4u;
            load++;
            last = 3;
            continue;
        }
        if (load < bound) {
            uint32_t param = wl_rd_le32(words + 4u * n), old;
            void *at = program ? (void *)&k->program[load][word] : (void *)&k->constants[load][word];

            // (the bits compared, as window_word's memcmp)
            memcpy(&old, at, sizeof old);
            if (old != param) {
                memcpy(at, &param, sizeof param);
                changes++;
            }
        }
        n++;
        last = word;
        if (word == 3) {
            load++;
            word = 0;
        } else {
            word++;
        }
    }
    if (program) {
        if (changes != 0) {
            k->program_generation += changes;
            k->dirty |= WL_NV2A_DIRTY_PROGRAM;
        }
        k->program_load = load;
        k->program_word = last;
        return;
    }
    if (changes != 0) {
        k->constants_generation += changes;
        k->dirty |= WL_NV2A_DIRTY_CONSTANTS;
    }
    k->constant_load = load;
    k->constant_word = last;
}

// how many of a run's `left` words at `w` to read before asking for the next line: to the end of
// w's line, with the line WL_NV2A_PUSH_AHEAD on asked for (Broadway reads the push buffer a miss a
// line otherwise); all of them without the prefetch
static inline __attribute__((always_inline)) uint32_t push_part(const uint8_t *w, uint32_t left) {
    uint32_t part;

    if (!wl_nv2a_prefetching()) {
        return left;
    }
    WL_NV2A_PREFETCH(w + WL_NV2A_PUSH_AHEAD);
    part = (WL_NV2A_CACHE_LINE - ((uint32_t)(uintptr_t)w & (WL_NV2A_CACHE_LINE - 1u))) >> 2;
    return part != 0 && part < left ? part : left;
}

// wl_nv2a_kelvin_methods' work, for it and wl_nv2a_kelvin_push (`ch`: the channel running, or NULL)
static inline __attribute__((always_inline)) uint32_t methods_run(WlNv2a *dev, const WlNv2aChannel *ch,
                                                                  uint32_t method, bool increasing,
                                                                  const uint8_t *words, uint32_t count) {
    WlNv2aKelvin *k = &dev->kelvin;
    uint32_t n;

    method &= WL_NV2A_K_METHODS - 4u;
    // (a run of indices into one begin/end: each word what wl_nv2a_kelvin_method makes of it, without
    // its dispatch)
    if (!increasing && (method == WL_NV2A_K_ARRAY_ELEMENT16 || method == WL_NV2A_K_ARRAY_ELEMENT32) &&
        k->primitive != 0 && batch_open(dev, WL_NV2A_SOURCE_ARRAYS)) {
        // (batch_index's work with the count kept here: stored back before a flush and at the end)
        uint32_t *indices = dev->batch_indices, at = dev->batch_index_count;
        unsigned halves = method == WL_NV2A_K_ARRAY_ELEMENT16 ? 2u : 1u, h;

        if (count != 0 && count <= (WL_NV2A_BATCH_INDICES - at) / halves) {
            // (room for the whole run: no flush to test for, and the method's value the last word's)
            const uint8_t *w = words;
            uint32_t left = count, part, from = at, least = 0xFFFFFFFFu, last = 0;

            // (a line of words at a time, the line WL_NV2A_PUSH_AHEAD on asked for first; without the
            // prefetch, the whole run in one. The run's least and greatest index as they come, a pair's
            // lesser against the least and its greater against the greatest: batch_draw's pass then
            // left out)
            for (; left != 0; left -= part) {
                part = push_part(w, left);
                if (halves == 2u && g_index_bounds) {
                    for (n = 0; n < part; n++, w += 4) {
                        uint32_t param = wl_rd_le32(w), lo = param & 0xFFFFu, hi = param >> 16;
                        uint32_t lesser = lo < hi ? lo : hi, greater = lo ^ hi ^ lesser;

                        indices[at] = lo;
                        indices[at + 1u] = hi;
                        at += 2u;
                        least = lesser < least ? lesser : least;
                        last = greater > last ? greater : last;
                    }
                } else if (halves == 2u) {
                    for (n = 0; n < part; n++, w += 4) {
                        uint32_t param = wl_rd_le32(w);

                        indices[at] = param & 0xFFFFu;
                        indices[at + 1u] = param >> 16;
                        at += 2u;
                    }
                } else {
                    for (n = 0; n < part; n++, w += 4) {
                        indices[at++] = wl_rd_le32(w);
                    }
                }
            }
            if (halves == 2u && g_index_bounds) {
                bounds_add(dev, from, at - from, least, last);
            }
            k->raw[method >> 2] = wl_rd_le32(words + 4u * (count - 1u));
            dev->batch_index_count = at;
            return count;
        }
        for (n = 0; n < count; n++) {
            uint32_t param = wl_rd_le32(words + 4u * n);

            k->raw[method >> 2] = param;
            for (h = 0; h < halves; h++) {
                if (at == WL_NV2A_BATCH_INDICES) {
                    dev->batch_index_count = at;
                    batch_flush(dev, false);
                    indices = dev->batch_indices;
                    at = dev->batch_index_count;
                }
                indices[at++] = halves == 2u ? (h == 0 ? param & 0xFFFFu : param >> 16) : param;
            }
        }
        dev->batch_index_count = at;
        return count;
    }
    if (increasing && method - WL_NV2A_K_SET_TRANSFORM_PROGRAM < WINDOWS_SIZE && count != 0 &&
        ((method + 4u * (count - 1u)) ^ method) < 0x80u) {
        // (a run inside one window: its words stored without each one's dispatch)
        window_run(k, method, words, count);
        return count;
    }
    for (n = 0; n < count; n++) {
        uint32_t m = (increasing ? method + 4u * n : method) & (WL_NV2A_K_METHODS - 4u);
        uint32_t param = wl_rd_le32(words + 4u * n), info = g_method_info[m >> 2];

        // (a plain method's value kept here, and an immediate one's taken, as wl_nv2a_kelvin_method does)
        if (info & METHOD_PLAIN) {
            if (k->raw[m >> 2] != param) {
                k->raw[m >> 2] = param;
                k->dirty |= info & METHOD_DIRTY;
            }
            continue;
        }
        if (info & METHOD_IMMEDIATE) {
            k->raw[m >> 2] = param;
            immediate_method(dev, m, param);
            continue;
        }
        if (m - WL_NV2A_K_SET_TRANSFORM_PROGRAM < WINDOWS_SIZE) {
            window_word(k, m, param);
            continue;
        }
        wl_nv2a_kelvin_method(dev, m, param);
        if (ch != NULL && ch->suspended) {
            // (a method stopped the channel: the rest of the run after it)
            return n + 1u;
        }
    }
    return count;
}

static bool g_push_cuts = true;
static uint32_t index_run_fit(WlNv2a *dev, uint32_t method, const uint8_t *words, uint32_t count);

uint32_t wl_nv2a_kelvin_methods(WlNv2a *dev, uint32_t method, bool increasing, const uint8_t *words, uint32_t count) {
    if (g_push_cuts && !increasing && (method & (WL_NV2A_K_METHODS - 4u)) == WL_NV2A_K_ARRAY_ELEMENT16 &&
        index_run_fit(dev, method, words, count) != 0) {
        // (the rest of an index run the pusher's loop began, cut at PUT)
        return count;
    }
    return methods_run(dev, dev->current_channel >= 0 ? &dev->channels[dev->current_channel] : NULL, method,
                       increasing, words, count);
}

// the pusher's header loop as it was before (wl_nv2a_set_old_push: a file "oldpush" in the title's
// folder, for an A/B on the console)
static bool g_old_push;

void wl_nv2a_set_old_push(bool on) {
    g_old_push = on;
}

// (GCC saved and restored the registers the runs take around each header, inside the loop: once, at
// the entry, they cost a tenth of the pusher's instructions less)
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("no-shrink-wrap-separate")))
#endif
static __attribute__((noinline)) uint32_t push_old(WlNv2a *dev, WlNv2aChannel *ch, const uint8_t *words,
                                                   uint32_t avail) {
    uint32_t taken = 0, last_method = 0;
    unsigned last_subchannel = 0;
    bool last_non_increasing = false;

    while (avail - taken >= 2u && !ch->suspended) {
        uint32_t header = wl_rd_le32(words + 4u * taken), type = header & WL_NV2A_CMD_TYPE_MASK;
        uint32_t method, count, run, done;
        unsigned subchannel;
        bool non_increasing;

        if (wl_nv2a_prefetching()) {
            // (the headers and short runs that follow: the line WL_NV2A_PUSH_AHEAD on asked for)
            WL_NV2A_PREFETCH(words + 4u * taken + WL_NV2A_PUSH_AHEAD);
        }

        // (a header: what the pusher takes it for when it is not a jump, a call or a return, which
        // never match these bits)
        if (type != WL_NV2A_CMD_INCREASING && type != WL_NV2A_CMD_NON_INCREASING) {
            break;
        }
        method = header & WL_NV2A_CMD_METHOD_MASK;
        subchannel = (header >> WL_NV2A_CMD_SUBCH_SHIFT) & 7u;
        count = (header >> WL_NV2A_CMD_COUNT_SHIFT) & WL_NV2A_CMD_COUNT_MASK;
        non_increasing = type == WL_NV2A_CMD_NON_INCREASING;
        if (count == 0 || !wl_nv2a_bulk_method(ch->subchannel_class[subchannel], method, non_increasing, count)) {
            break;
        }
        run = avail - taken - 1u < count ? avail - taken - 1u : count;
        done = methods_run(dev, ch, method, !non_increasing, words + 4u * (taken + 1u), run);
        dev->stats.methods += done;
        taken += 1u + done;
        if (done < count) {
            // (a run cut short: the rest left in the header state, for the pusher)
            ch->method = non_increasing ? method : method + 4u * done;
            ch->subchannel = (uint8_t)subchannel;
            ch->count = count - done;
            ch->non_increasing = non_increasing;
            return taken;
        }
        last_method = non_increasing ? method : method + 4u * count;
        last_subchannel = subchannel;
        last_non_increasing = non_increasing;
    }
    if (taken != 0) {
        // (the last header's state when its run is done, as the pusher leaves it)
        ch->method = last_method;
        ch->subchannel = (uint8_t)last_subchannel;
        ch->count = 0;
        ch->non_increasing = last_non_increasing;
    }
    return taken;
}

// an increasing run of `count` words from `method` on, inside the methods' space: a plain method's
// word kept (its dirty groups gathered here and stored once, before any other method's work and at
// the end), any other through what methods_run makes of it; the words taken (fewer when a method
// stops the channel)
static inline __attribute__((always_inline)) uint32_t increasing_run(WlNv2a *dev, const WlNv2aChannel *ch,
                                                                     uint32_t method, const uint8_t *words,
                                                                     uint32_t count) {
    WlNv2aKelvin *k = &dev->kelvin;
    const uint16_t *infos = &g_method_info[method >> 2];
    uint32_t *raw = &k->raw[method >> 2];
    uint32_t dirty = 0, n;

    for (n = 0; n < count; n++) {
        uint32_t param = wl_rd_le32(words + 4u * n), info = infos[n], m;

        if (info & METHOD_PLAIN) {
            if (raw[n] != param) {
                raw[n] = param;
                dirty |= info;
            }
            continue;
        }
        k->dirty |= dirty & METHOD_DIRTY;
        dirty = 0;
        m = method + 4u * n;
        if (info & METHOD_IMMEDIATE) {
            raw[n] = param;
            immediate_method(dev, m, param);
            continue;
        }
        if (info & METHOD_WINDOW) {
            window_word(k, m, param);
            continue;
        }
        wl_nv2a_kelvin_method(dev, m, param);
        if (ch->suspended) {
            // (a method stopped the channel: the rest of the run after it)
            return n + 1u;
        }
    }
    k->dirty |= dirty & METHOD_DIRTY;
    return count;
}
// (as push_old, with the header's tests from the method table, a header of one word to a plain
// method on a path of its own (most of a title's headers: its render states), each kind of run to a
// loop of its own, the dirty groups and the methods gathered here: the channel is looked at for a
// stop only after a method that may stop it, and the subchannels bound to the 3D object once, as no
// method a run takes binds one. The loop as wave 3 left it: wl_nv2a_set_push_cuts(false))
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("no-shrink-wrap-separate")))
#endif
static __attribute__((noinline)) uint32_t push_wave3(WlNv2a *dev, WlNv2aChannel *ch, const uint8_t *words,
                                                     uint32_t avail) {
    WlNv2aKelvin *k = &dev->kelvin;
    uint32_t taken = 0, methods = 0, last_header = 0, dirty = 0, kelvin = 0;
    unsigned s;
    bool prefetch;

    prefetch = wl_nv2a_prefetching();
    for (s = 0; s < WL_NV2A_SUBCHANNELS; s++) {
        kelvin |= (uint32_t)(ch->subchannel_class[s] == WL_NV2A_CLASS_KELVIN) << s;
    }
    while (avail - taken >= 2u) {
        const uint8_t *at = words + 4u * taken;
        uint32_t header = wl_rd_le32(at), method, count, run, done, info;

        if (prefetch) {
            // (the headers and short runs that follow: the line WL_NV2A_PUSH_AHEAD on asked for)
            WL_NV2A_PREFETCH(at + WL_NV2A_PUSH_AHEAD);
        }
        // (a header, increasing or not, to the 3D object: what the pusher takes it for when it is not
        // a jump, a call or a return, which never match these bits)
        if ((header & (WL_NV2A_CMD_TYPE_MASK & ~WL_NV2A_CMD_NON_INCREASING)) != 0 ||
            !((kelvin >> ((header >> WL_NV2A_CMD_SUBCH_SHIFT) & 7u)) & 1u)) {
            break;
        }
        method = header & WL_NV2A_CMD_METHOD_MASK;
        info = g_method_info[method >> 2];
        if ((header & (WL_NV2A_CMD_NON_INCREASING | WL_NV2A_CMD_COUNT_MASK << WL_NV2A_CMD_COUNT_SHIFT)) ==
                1u << WL_NV2A_CMD_COUNT_SHIFT &&
            (info & (METHOD_PLAIN | METHOD_BULK)) == (METHOD_PLAIN | METHOD_BULK)) {
            // (one word to a plain method: its value kept, as increasing_run keeps it)
            uint32_t param = wl_rd_le32(at + 4), *raw = &k->raw[method >> 2];

            if (*raw != param) {
                *raw = param;
                dirty |= info;
            }
            methods++;
            taken += 2u;
            last_header = header;
            continue;
        }
        count = (header >> WL_NV2A_CMD_COUNT_SHIFT) & WL_NV2A_CMD_COUNT_MASK;
        // (wl_nv2a_bulk_method's tests: the method's in the table, a run up to the handles' here)
        if (count == 0 || !(info & METHOD_BULK) ||
            (!(header & WL_NV2A_CMD_NON_INCREASING) && method < WL_NV2A_PULLER_HANDLE_FIRST &&
             method + 4u * count > WL_NV2A_PULLER_HANDLE_FIRST)) {
            break;
        }
        // (the dirty groups stored before any other method's work)
        k->dirty |= dirty & METHOD_DIRTY;
        dirty = 0;
        run = avail - taken - 1u < count ? avail - taken - 1u : count;
        at += 4;
        if (header & WL_NV2A_CMD_NON_INCREASING) {
            done = methods_run(dev, ch, method, false, at, run);
        } else if ((info & METHOD_WINDOW) && ((method + 4u * (run - 1u)) ^ method) < 0x80u) {
            // (a run inside one window: its words stored without each one's dispatch)
            window_run(k, method, at, run);
            done = run;
        } else if (method + 4u * run <= WL_NV2A_K_METHODS) {
            done = increasing_run(dev, ch, method, at, run);
        } else {
            done = methods_run(dev, ch, method, true, at, run); // (past the last method: wrapped)
        }
        methods += done;
        taken += 1u + done;
        if (done < count) {
            // (a run cut short: the rest left in the header state, for the pusher)
            bool non_increasing = (header & WL_NV2A_CMD_NON_INCREASING) != 0;

            dev->stats.methods += methods;
            ch->method = non_increasing ? method : method + 4u * done;
            ch->subchannel = (uint8_t)((header >> WL_NV2A_CMD_SUBCH_SHIFT) & 7u);
            ch->count = count - done;
            ch->non_increasing = non_increasing;
            return taken;
        }
        last_header = header;
        if (ch->suspended) {
            break;
        }
    }
    k->dirty |= dirty & METHOD_DIRTY;
    dev->stats.methods += methods;
    if (taken != 0) {
        // (the last header's state when its run is done, as the pusher leaves it)
        uint32_t method = last_header & WL_NV2A_CMD_METHOD_MASK;
        bool non_increasing = (last_header & WL_NV2A_CMD_NON_INCREASING) != 0;

        ch->method = non_increasing
                         ? method
                         : method + 4u * ((last_header >> WL_NV2A_CMD_COUNT_SHIFT) & WL_NV2A_CMD_COUNT_MASK);
        ch->subchannel = (uint8_t)((last_header >> WL_NV2A_CMD_SUBCH_SHIFT) & 7u);
        ch->count = 0;
        ch->non_increasing = non_increasing;
    }
    return taken;
}

// ---- the pusher's loop of wave 4 ------------------------------------------------------------------
// (wl_nv2a_set_push_cuts(false): the loop as wave 3 left it, push_wave3; a file "oldpush2" in the
// title's folder, WL_XB_OLDPUSH2 on the host and in the replay)

void wl_nv2a_set_push_cuts(bool on) {
    g_push_cuts = on;
}

// a push buffer word read (lwbrx on Broadway: one instruction, where GCC may make a load and three
// rotates of it), and the line `ahead` bytes on from `p` asked for (dcbt with the distance in a
// register: one instruction)
#if (defined(__powerpc__) || defined(__PPC__)) && defined(__BYTE_ORDER__) && \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__ && !defined(WL_NV2A_PREFETCH_RECORD)
static inline __attribute__((always_inline)) uint32_t push_rd(const uint8_t *p) {
    uint32_t value;

    __asm__("lwbrx %0,%y1" : "=r"(value) : "Z"(*(const SlotWord *)(const void *)p));
    return value;
}
#define PUSH_AHEAD_AT(p, ahead) __asm__ volatile("dcbt %0,%1" : : "b"(p), "r"(ahead))
#else
static inline uint32_t push_rd(const uint8_t *p) {
    return wl_rd_le32(p);
}
#define PUSH_AHEAD_AT(p, ahead) WL_NV2A_PREFETCH((p) + (ahead))
#endif

// (methods_run out of line: the runs push_run leaves to it are few, and inlined they took push_run's
// registers)
static __attribute__((noinline)) uint32_t methods_out(WlNv2a *dev, const WlNv2aChannel *ch, uint32_t method,
                                                      bool increasing, const uint8_t *words, uint32_t count) {
    return methods_run(dev, ch, method, increasing, words, count);
}

static __attribute__((noinline)) void immediate_out(WlNv2a *dev, uint32_t method, uint32_t param) {
    immediate_method(dev, method, param);
}

static __attribute__((noinline)) void begin_end_out(WlNv2a *dev, uint32_t param) {
    begin_end(dev, param);
}

// a word of two 16-bit indices into `to`, the least and the greatest kept: the pair's lesser against
// the least, its greater against the greatest
static inline __attribute__((always_inline)) void index_word(uint32_t param, uint32_t *to, uint32_t *least,
                                                             uint32_t *last) {
    uint32_t lo = param & 0xFFFFu, hi = param >> 16;

    to[0] = lo;
    to[1] = hi;
    if (lo < hi) {
        *least = lo < *least ? lo : *least;
        *last = hi > *last ? hi : *last;
    } else {
        *least = hi < *least ? hi : *least;
        *last = lo > *last ? lo : *last;
    }
}

static inline __attribute__((always_inline)) void index_pair(const uint8_t *w, uint32_t *to, uint32_t *least,
                                                             uint32_t *last) {
    index_word(wl_rd_le32(w), to, least, last);
}

// index_run16's lines of eight words (`left` / 8 of them) into `to`, their least and greatest index
// in *least and *last, the line `ahead` on asked for at each line's first word: the words left. On
// Broadway a word is eleven or twelve instructions when the bounds do not move: lwbrx with its offset in
// a register (no address made for each word), its halves stored, the pair's lesser against the least
// and its greater against the greatest (GCC kept both paths' moves in the line and an address a word).
#if (defined(__powerpc__) || defined(__PPC__)) && defined(__BYTE_ORDER__) && \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__ && !defined(WL_NV2A_PREFETCH_RECORD)
#define INDEX_WORD_ASM(load, at)                                                                           \
    "   " load "\n"                                                                                         \
    "   clrlwi %[lo],%[t],16\n"                                                                            \
    "   srwi %[hi],%[t],16\n"                                                                              \
    "   stw %[lo]," #at "(%[to])\n"                                                                        \
    "   stw %[hi]," #at "+4(%[to])\n"                                                                      \
    "   cmplw %[lo],%[hi]\n"                                                                               \
    "   bge 1f\n"                                                                                          \
    "   cmplw %[lo],%[least]\n"                                                                            \
    "   bge 2f\n"                                                                                          \
    "   mr %[least],%[lo]\n"                                                                               \
    "2: cmplw %[hi],%[last]\n"                                                                             \
    "   ble 3f\n"                                                                                          \
    "   mr %[last],%[hi]\n"                                                                                \
    "   b 3f\n"                                                                                            \
    "1: cmplw %[hi],%[least]\n"                                                                            \
    "   bge 2f\n"                                                                                          \
    "   mr %[least],%[hi]\n"                                                                               \
    "2: cmplw %[lo],%[last]\n"                                                                             \
    "   ble 3f\n"                                                                                          \
    "   mr %[last],%[lo]\n"                                                                                \
    "3:\n"
static inline __attribute__((always_inline)) uint32_t index_lines(const uint8_t **wp, uint32_t **top, uint32_t left,
                                                                  uint32_t *least, uint32_t *last) {
    const uint8_t *w = *wp;
    uint32_t *to = *top, lo_bound = *least, hi_bound = *last, ahead = WL_NV2A_PUSH_AHEAD;
    uint32_t o4 = 4, o8 = 8, o12 = 12, o16 = 16, o20 = 20, o24 = 24, o28 = 28, t, lo, hi;

    for (; left >= 8u; left -= 8u, w += 32, to += 16) {
        PUSH_AHEAD_AT(w, ahead);
        __asm__ volatile(INDEX_WORD_ASM("lwbrx %[t],0,%[w]", 0) INDEX_WORD_ASM("lwbrx %[t],%[w],%[o4]", 8)
                             INDEX_WORD_ASM("lwbrx %[t],%[w],%[o8]", 16) INDEX_WORD_ASM("lwbrx %[t],%[w],%[o12]", 24)
                                 INDEX_WORD_ASM("lwbrx %[t],%[w],%[o16]", 32)
                                     INDEX_WORD_ASM("lwbrx %[t],%[w],%[o20]", 40)
                                         INDEX_WORD_ASM("lwbrx %[t],%[w],%[o24]", 48)
                                             INDEX_WORD_ASM("lwbrx %[t],%[w],%[o28]", 56)
                         : [least] "+r"(lo_bound), [last] "+r"(hi_bound), [t] "=&r"(t), [lo] "=&r"(lo), [hi] "=&r"(hi)
                         : [w] "b"(w), [to] "b"(to), [o4] "r"(o4), [o8] "r"(o8), [o12] "r"(o12), [o16] "r"(o16),
                           [o20] "r"(o20), [o24] "r"(o24), [o28] "r"(o28)
                         : "cc", "memory");
    }
    *wp = w;
    *top = to;
    *least = lo_bound;
    *last = hi_bound;
    return left;
}
// (a word alone, the head's and tail's: the same work)
static inline __attribute__((always_inline)) void index_one(const uint8_t *w, uint32_t *to, uint32_t *least,
                                                            uint32_t *last) {
    uint32_t lo_bound = *least, hi_bound = *last, t, lo, hi;

    __asm__ volatile(INDEX_WORD_ASM("lwbrx %[t],0,%[w]", 0)
                     : [least] "+r"(lo_bound), [last] "+r"(hi_bound), [t] "=&r"(t), [lo] "=&r"(lo), [hi] "=&r"(hi)
                     : [w] "b"(w), [to] "b"(to)
                     : "cc", "memory");
    *least = lo_bound;
    *last = hi_bound;
}
#else
static inline __attribute__((always_inline)) uint32_t index_lines(const uint8_t **wp, uint32_t **top, uint32_t left,
                                                                  uint32_t *least, uint32_t *last) {
    const uint8_t *w = *wp;
    uint32_t *to = *top, ahead = WL_NV2A_PUSH_AHEAD;
    unsigned i;

    (void)ahead;
    for (; left >= 8u; left -= 8u, w += 32, to += 16) {
        PUSH_AHEAD_AT(w, ahead);
        for (i = 0; i < 8u; i++) {
            index_pair(w + 4u * i, to + 2u * i, least, last);
        }
    }
    *wp = w;
    *top = to;
    return left;
}
static inline __attribute__((always_inline)) void index_one(const uint8_t *w, uint32_t *to, uint32_t *least,
                                                            uint32_t *last) {
    index_pair(w, to, least, last);
}
#endif

// index_run16's work as wave 4's second part makes it (wl_nv2a_set_push_cuts3): its head, lines and
// tail by index_one and index_lines
static __attribute__((noinline)) void index_run16_cut(WlNv2a *dev, const uint8_t *w, uint32_t count) {
    uint32_t at = dev->batch_index_count, *to = dev->batch_indices + at, left = count;
    uint32_t least = 0xFFFFFFFFu, last = 0;

    if (wl_nv2a_prefetching()) {
        uint32_t head = ((WL_NV2A_CACHE_LINE - ((uint32_t)(uintptr_t)w & (WL_NV2A_CACHE_LINE - 1u))) &
                         (WL_NV2A_CACHE_LINE - 1u)) >> 2;

        head = head < left ? head : left;
        if (head != 0) {
            WL_NV2A_PREFETCH(w + WL_NV2A_PUSH_AHEAD);
            for (left -= head; head != 0; head--, w += 4, to += 2) {
                index_one(w, to, &least, &last);
            }
        }
        left = index_lines(&w, &to, left, &least, &last);
        if (left != 0) {
            WL_NV2A_PREFETCH(w + WL_NV2A_PUSH_AHEAD);
        }
    }
    for (; left != 0; left--, w += 4, to += 2) {
        index_one(w, to, &least, &last);
    }
    bounds_add(dev, at, 2u * count, least, last);
    dev->batch_index_count = at + 2u * count;
}

// a run of `count` words of 16-bit indices into the batch, which has room for them all: what
// methods_run makes of it (the least and greatest index as they come, the line WL_NV2A_PUSH_AHEAD on
// asked for at each line's first word read), a line of eight words a pass
static __attribute__((noinline)) void index_run16(WlNv2a *dev, const uint8_t *w, uint32_t count) {
    uint32_t at = dev->batch_index_count, *to = dev->batch_indices + at, left = count;
    uint32_t least = 0xFFFFFFFFu, last = 0;

    if (wl_nv2a_prefetching()) {
        uint32_t head = ((WL_NV2A_CACHE_LINE - ((uint32_t)(uintptr_t)w & (WL_NV2A_CACHE_LINE - 1u))) &
                         (WL_NV2A_CACHE_LINE - 1u)) >> 2;

        head = head < left ? head : left;
        if (head != 0) {
            WL_NV2A_PREFETCH(w + WL_NV2A_PUSH_AHEAD);
            for (left -= head; head != 0; head--, w += 4, to += 2) {
                index_pair(w, to, &least, &last);
            }
        }
        for (; left >= 8u; left -= 8u, w += 32, to += 16) {
            unsigned i;

            WL_NV2A_PREFETCH(w + WL_NV2A_PUSH_AHEAD);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC unroll 8
#endif
            for (i = 0; i < 8u; i++) {
                index_pair(w + 4u * i, to + 2u * i, &least, &last);
            }
        }
        if (left != 0) {
            WL_NV2A_PREFETCH(w + WL_NV2A_PUSH_AHEAD);
        }
    }
    for (; left != 0; left--, w += 4, to += 2) {
        index_pair(w, to, &least, &last);
    }
    bounds_add(dev, at, 2u * count, least, last);
    dev->batch_index_count = at + 2u * count;
}

// a non-increasing run of `count` (1 or more) words to `method`: index_run16's when they are 16-bit
// indices inside a begin/end with room for them all in the batch (`count`), else none taken (0)
static uint32_t index_run_fit(WlNv2a *dev, uint32_t method, const uint8_t *words, uint32_t count) {
    if (count == 0 || (method & (WL_NV2A_K_METHODS - 4u)) != WL_NV2A_K_ARRAY_ELEMENT16 || !g_index_bounds ||
        dev->kelvin.primitive == 0 || count > (WL_NV2A_BATCH_INDICES - dev->batch_index_count) / 2u ||
        !batch_open(dev, WL_NV2A_SOURCE_ARRAYS)) {
        return 0;
    }
    if (g_push_cuts3) {
        index_run16_cut(dev, words, count);
    } else {
        index_run16(dev, words, count);
    }
    dev->kelvin.raw[WL_NV2A_K_ARRAY_ELEMENT16 >> 2] = wl_rd_le32(words + 4u * (count - 1u));
    return count;
}

// window_run's work, the words read through a pointer that walks (push_rd): the words before the first
// whole slot and after the last one (or past the window's bound) one at a time, the whole slots four
// words at once, their changes counted as window_word counts them
static __attribute__((noinline)) void window_words(WlNv2aKelvin *k, uint32_t method, const uint8_t *w,
                                                   uint32_t count) {
    bool program = method < WL_NV2A_K_SET_TRANSFORM_CONSTANT;
    unsigned word = (method >> 2) & 3u, last = word;
    uint32_t load = program ? k->program_load : k->constant_load;
    uint32_t bound = program ? WL_NV2A_PROGRAM_SLOTS : WL_NV2A_CONSTANTS, changes = 0;
    SlotWord *slots = program ? (SlotWord *)(void *)k->program : (SlotWord *)(void *)k->constants;
    const uint8_t *end = w + 4u * (size_t)count;

    for (;;) {
        if (word == 0) {
            for (; (size_t)(end - w) >= 16u && load < bound; w += 16, load++) {
                SlotWord *s = slots + 4u * load;
                uint32_t a = push_rd(w), b = push_rd(w + 4), c = push_rd(w + 8), d = push_rd(w + 12);

                changes += (uint32_t)(s[0] != a) + (uint32_t)(s[1] != b) + (uint32_t)(s[2] != c) + (uint32_t)(s[3] != d);
                s[0] = a;
                s[1] = b;
                s[2] = c;
                s[3] = d;
                last = 3;
            }
        }
        if (w == end) {
            break;
        }
        if (load < bound) {
            uint32_t param = push_rd(w);

            // (the bits compared, as window_word's memcmp)
            if (slots[4u * load + word] != param) {
                slots[4u * load + word] = param;
                changes++;
            }
        }
        w += 4;
        last = word;
        if (word == 3) {
            load++;
            word = 0;
        } else {
            word++;
        }
    }
    if (program) {
        if (changes != 0) {
            k->program_generation += changes;
            k->dirty |= WL_NV2A_DIRTY_PROGRAM;
        }
        k->program_load = load;
        k->program_word = last;
        return;
    }
    if (changes != 0) {
        k->constants_generation += changes;
        k->dirty |= WL_NV2A_DIRTY_CONSTANTS;
    }
    k->constant_load = load;
    k->constant_word = last;
}

// increasing_run, with the load slots and begin/end taken to their work straight (METHOD_DIRECT: what
// wl_nv2a_kelvin_method does with them, without its dispatch); a window's word to
// wl_nv2a_kelvin_method (window_word, as increasing_run's)
static inline __attribute__((always_inline)) uint32_t increasing_direct(WlNv2a *dev, const WlNv2aChannel *ch,
                                                                        uint32_t method, const uint8_t *words,
                                                                        uint32_t count) {
    WlNv2aKelvin *k = &dev->kelvin;
    const uint16_t *infos = &g_method_info[method >> 2];
    uint32_t *raw = &k->raw[method >> 2];
    uint32_t dirty = 0, n;

    for (n = 0; n < count; n++) {
        uint32_t param = wl_rd_le32(words + 4u * n), info = infos[n], m;

        if (info & METHOD_PLAIN) {
            if (raw[n] != param) {
                raw[n] = param;
                dirty |= info;
            }
            continue;
        }
        k->dirty |= dirty & METHOD_DIRTY;
        dirty = 0;
        m = method + 4u * n;
        if (info & METHOD_IMMEDIATE) {
            raw[n] = param;
            immediate_out(dev, m, param);
            continue;
        }
        if (info & METHOD_DIRECT) {
            raw[n] = param;
            if (m == WL_NV2A_K_SET_TRANSFORM_CONSTANT_LOAD) {
                k->constant_load = param;
                k->constant_word = 0;
                continue;
            }
            if (m == WL_NV2A_K_SET_TRANSFORM_PROGRAM_LOAD) {
                k->program_load = param;
                k->program_word = 0;
                continue;
            }
            begin_end_out(dev, param);
        } else {
            wl_nv2a_kelvin_method(dev, m, param);
        }
        if (ch->suspended) {
            // (a method stopped the channel: the rest of the run after it)
            return n + 1u;
        }
    }
    k->dirty |= dirty & METHOD_DIRTY;
    return count;
}

// a run of `count` words inside one of the viewport's vectors (SET_VIEWPORT_OFFSET or _SCALE, from
// `method` on): what wl_nv2a_kelvin_method does with each, without its dispatch (the word kept; the
// constant it is changed if the word changes it, its generation counted a word)
static __attribute__((noinline)) void viewport_words(WlNv2aKelvin *k, uint32_t method, const uint8_t *w,
                                                      uint32_t count) {
    uint32_t *raw = &k->raw[method >> 2], changes = 0, n;
    float *slot = method >= WL_NV2A_K_SET_VIEWPORT_SCALE
                      ? &k->constants[WL_NV2A_CONSTANT_VIEWPORT_SCALE][(method - WL_NV2A_K_SET_VIEWPORT_SCALE) >> 2]
                      : &k->constants[WL_NV2A_CONSTANT_VIEWPORT_OFFSET][(method - WL_NV2A_K_SET_VIEWPORT_OFFSET) >> 2];

    for (n = 0; n < count; n++) {
        uint32_t param = wl_rd_le32(w + 4u * n), was;

        raw[n] = param;
        memcpy(&was, &slot[n], sizeof was);
        if (was != param) {
            memcpy(&slot[n], &param, sizeof param);
            changes++;
        }
    }
    if (changes != 0) {
        k->constants_generation += changes;
        k->dirty |= WL_NV2A_DIRTY_VIEWPORT | WL_NV2A_DIRTY_CONSTANTS;
    }
}

// a header at `at` that is not one word to a plain method, `left` words from it on (2 or more): its
// run as push_wave3 takes it; the words taken, the header's with them (0: a header the pusher takes,
// not a method header to the 3D object or not a run this loop takes), *cut when the run was cut short
// (its rest left in the header state)
static __attribute__((noinline)) uint32_t push_run(WlNv2a *dev, WlNv2aChannel *ch, uint32_t header,
                                                   const uint8_t *at, uint32_t left, uint32_t kelvin, bool *cut) {
    WlNv2aKelvin *k = &dev->kelvin;
    uint32_t method = header & WL_NV2A_CMD_METHOD_MASK, info = g_method_info[method >> 2];
    uint32_t count = (header >> WL_NV2A_CMD_COUNT_SHIFT) & WL_NV2A_CMD_COUNT_MASK, run, done;
    bool non_increasing = (header & WL_NV2A_CMD_NON_INCREASING) != 0;

    // (a header, increasing or not, to the 3D object: what the pusher takes it for when it is not a
    // jump, a call or a return, which never match these bits; then wl_nv2a_bulk_method's tests: the
    // method's in the table, a run up to the handles' here)
    if ((header & (WL_NV2A_CMD_TYPE_MASK & ~WL_NV2A_CMD_NON_INCREASING)) != 0 ||
        !((kelvin >> ((header >> WL_NV2A_CMD_SUBCH_SHIFT) & 7u)) & 1u) || count == 0 || !(info & METHOD_BULK) ||
        (!non_increasing && method < WL_NV2A_PULLER_HANDLE_FIRST && method + 4u * count > WL_NV2A_PULLER_HANDLE_FIRST)) {
        return 0;
    }
    run = left - 1u < count ? left - 1u : count;
    at += 4;
    if (non_increasing) {
        done = index_run_fit(dev, method, at, run);
        if (done == 0) {
            done = methods_out(dev, ch, method, false, at, run);
        }
    } else if ((info & METHOD_WINDOW) && ((method + 4u * (run - 1u)) ^ method) < 0x80u) {
        // (a run inside one window: its words stored without each one's dispatch)
        window_words(k, method, at, run);
        done = run;
    } else if (g_push_cuts3 &&
               (method - WL_NV2A_K_SET_VIEWPORT_OFFSET < 16u || method - WL_NV2A_K_SET_VIEWPORT_SCALE < 16u) &&
               (method & 0xCu) + 4u * run <= 16u) {
        // (a run inside one of the viewport's vectors: its words to their constants without the dispatch)
        viewport_words(k, method, at, run);
        done = run;
    } else if (method + 4u * run <= WL_NV2A_K_METHODS) {
        done = increasing_direct(dev, ch, method, at, run);
    } else {
        done = methods_out(dev, ch, method, true, at, run); // (past the last method: wrapped)
    }
    if (done < count) {
        // (a run cut short: the rest left in the header state, for the pusher)
        ch->method = non_increasing ? method : method + 4u * done;
        ch->subchannel = (uint8_t)((header >> WL_NV2A_CMD_SUBCH_SHIFT) & 7u);
        ch->count = count - done;
        ch->non_increasing = non_increasing;
        *cut = true;
    }
    return 1u + done;
}

// (as push_wave3, with the header of one word to a plain method alone in the loop: its tests in one
// mask, the method's table entry and value found by the header's bits, a header's place a pointer, the
// methods counted as the words less the headers; any other header to push_run)
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("no-shrink-wrap-separate")))
#endif
uint32_t wl_nv2a_kelvin_push(WlNv2a *dev, WlNv2aChannel *ch, const uint8_t *words, uint32_t avail) {
    WlNv2aKelvin *k;
    const uint8_t *at = words, *stop, *last = NULL, *infos;
    uint8_t *raw;
    uint32_t headers = 0, dirty = 0, kelvin = 0, taken, ahead = WL_NV2A_PUSH_AHEAD, short_header;
    unsigned s;
    bool prefetch, cut = false;
    const uint32_t short_mask = (uint32_t)WL_NV2A_CMD_TYPE_MASK | WL_NV2A_CMD_NON_INCREASING |
                                (uint32_t)WL_NV2A_CMD_COUNT_MASK << WL_NV2A_CMD_COUNT_SHIFT |
                                7u << WL_NV2A_CMD_SUBCH_SHIFT;

    if (g_method_info[0] == 0) {
        method_info_make();
    }
    if (g_old_push) {
        return push_old(dev, ch, words, avail);
    }
    if (!g_push_cuts) {
        return push_wave3(dev, ch, words, avail);
    }
    if (avail < 2u) {
        return 0;
    }
    k = &dev->kelvin;
    stop = words + 4u * (size_t)(avail - 2u); // (the last place a header and a word fit)
    infos = (const uint8_t *)g_method_info;
    raw = (uint8_t *)k->raw;
    prefetch = wl_nv2a_prefetching();
    for (s = 0; s < WL_NV2A_SUBCHANNELS; s++) {
        kelvin |= (uint32_t)(ch->subchannel_class[s] == WL_NV2A_CLASS_KELVIN) << s;
    }
    // (a short header's bits under short_mask: increasing, a count of 1, the first subchannel bound to the
    // 3D object (a header to another one bound to it goes the longer way); none when no subchannel is)
    for (s = 0; s < WL_NV2A_SUBCHANNELS && !((kelvin >> s) & 1u); s++) {
    }
    short_header = s < WL_NV2A_SUBCHANNELS ? 1u << WL_NV2A_CMD_COUNT_SHIFT | s << WL_NV2A_CMD_SUBCH_SHIFT : 0xFFFFFFFFu;
    while (at <= stop) {
        // (the method's table entry: its index, header bits 2-12, times the entry's two bytes)
        uint32_t header = push_rd(at), info = *(const uint16_t *)(const void *)(infos + ((header >> 1) & 0xFFEu));
        uint32_t n;

        (void)ahead;
        if (prefetch) {
            // (the headers and short runs that follow: the line WL_NV2A_PUSH_AHEAD on asked for)
            PUSH_AHEAD_AT(at, ahead);
        }
        // (one word, increasing, to a plain method of the 3D object: not a jump, a call or a return, a
        // count of 1, the subchannel's; its value kept, as increasing_run keeps it)
        if ((header & short_mask) == short_header &&
            (info & (METHOD_PLAIN | METHOD_BULK)) == (METHOD_PLAIN | METHOD_BULK)) {
            uint32_t *slot = (uint32_t *)(void *)(raw + (header & WL_NV2A_CMD_METHOD_MASK)), param = push_rd(at + 4);

            if (*slot != param) {
                *slot = param;
                dirty |= info;
            }
            last = at;
            at += 8;
            headers++;
            continue;
        }
        if ((header & WL_NV2A_CMD_TYPE_MASK) == 0 && ((kelvin >> ((header >> WL_NV2A_CMD_SUBCH_SHIFT) & 7u)) & 1u)) {
            // (an increasing header to the 3D object)
            uint32_t count = (header >> WL_NV2A_CMD_COUNT_SHIFT) & WL_NV2A_CMD_COUNT_MASK;
            uint32_t method = header & WL_NV2A_CMD_METHOD_MASK;

            if (count - 1u < g_plain_run[method >> 2] && count - 1u <= (uint32_t)((size_t)(stop - at) >> 2)) {
                // (a run of plain methods, all of it here: their values kept, as increasing_run keeps them)
                uint32_t *slots = (uint32_t *)(void *)(raw + method);
                const uint16_t *of = &g_method_info[method >> 2];

                for (n = 0; n < count; n++) {
                    uint32_t param = push_rd(at + 4u + 4u * n);

                    if (slots[n] != param) {
                        slots[n] = param;
                        dirty |= of[n];
                    }
                }
                last = at;
                at += 4u * (count + 1u);
                headers++;
                continue;
            }
            if (count == 1u && (info & METHOD_DIRECT)) {
                // (one word to a load slot or begin/end: increasing_direct's work for it)
                uint32_t param = push_rd(at + 4);

                k->dirty |= dirty & METHOD_DIRTY;
                dirty = 0;
                k->raw[method >> 2] = param;
                if (method == WL_NV2A_K_SET_TRANSFORM_CONSTANT_LOAD) {
                    k->constant_load = param;
                    k->constant_word = 0;
                } else if (method == WL_NV2A_K_SET_TRANSFORM_PROGRAM_LOAD) {
                    k->program_load = param;
                    k->program_word = 0;
                } else {
                    begin_end_out(dev, param);
                }
                last = at;
                at += 8;
                headers++;
                if (ch->suspended) {
                    break;
                }
                continue;
            }
        }
        // (the dirty groups stored before any other method's work)
        k->dirty |= dirty & METHOD_DIRTY;
        dirty = 0;
        n = push_run(dev, ch, header, at, avail - (uint32_t)((size_t)(at - words) >> 2), kelvin, &cut);
        if (n == 0) {
            break;
        }
        last = at;
        at += 4u * n;
        headers++;
        if (cut) {
            taken = (uint32_t)((size_t)(at - words) >> 2);
            dev->stats.methods += taken - headers;
            return taken;
        }
        if (ch->suspended) {
            break;
        }
    }
    k->dirty |= dirty & METHOD_DIRTY;
    taken = (uint32_t)((size_t)(at - words) >> 2);
    dev->stats.methods += taken - headers;
    if (last != NULL) {
        // (the last header's state when its run is done, as the pusher leaves it)
        uint32_t header = wl_rd_le32(last), method = header & WL_NV2A_CMD_METHOD_MASK;
        bool non_increasing = (header & WL_NV2A_CMD_NON_INCREASING) != 0;

        ch->method =
            non_increasing ? method : method + 4u * ((header >> WL_NV2A_CMD_COUNT_SHIFT) & WL_NV2A_CMD_COUNT_MASK);
        ch->subchannel = (uint8_t)((header >> WL_NV2A_CMD_SUBCH_SHIFT) & 7u);
        ch->count = 0;
        ch->non_increasing = non_increasing;
    }
    return taken;
}
