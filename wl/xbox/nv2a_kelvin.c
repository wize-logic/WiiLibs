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

void wl_nv2a_attrib_decode(uint8_t type, uint8_t size, const uint8_t *data, float out[4]) {
    unsigned i;

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

void wl_nv2a_prim_info(uint32_t primitive, WlNv2aPrimInfo *out) {
    memset(out, 0, sizeof *out);
    switch (primitive) {
    case WL_NV2A_PRIM_POINTS:
        out->first = 1, out->step = 1, out->points = 1;
        break;
    case WL_NV2A_PRIM_LINES:
        out->first = 2, out->step = 2, out->lines = 1;
        break;
    case WL_NV2A_PRIM_LINE_LOOP:
    case WL_NV2A_PRIM_LINE_STRIP:
        out->first = 2, out->step = 1, out->lines = 1, out->strip = 1;
        break;
    case WL_NV2A_PRIM_TRIANGLES:
        out->first = 3, out->step = 3;
        break;
    case WL_NV2A_PRIM_TRIANGLE_STRIP:
        out->first = 3, out->step = 1, out->strip = 1;
        break;
    case WL_NV2A_PRIM_TRIANGLE_FAN:
    case WL_NV2A_PRIM_POLYGON:
        out->first = 3, out->step = 1, out->fan = 1;
        break;
    case WL_NV2A_PRIM_QUADS:
        out->first = 4, out->step = 4;
        break;
    case WL_NV2A_PRIM_QUAD_STRIP:
        out->first = 4, out->step = 2, out->strip = 1;
        break;
    default:
        break;
    }
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

static uint32_t dirty_group(uint32_t method) {
    if (method >= 0x200 && method <= 0x214) {
        return WL_NV2A_DIRTY_SURFACE;
    }
    if ((method >= 0x260 && method <= 0x28C) || (method >= 0xA60 && method <= 0xADC) ||
        (method >= 0x1E20 && method <= 0x1E24) || (method >= 0x1E40 && method <= 0x1E78) || method == 0x17F8) {
        return WL_NV2A_DIRTY_COMBINERS;
    }
    if (method >= 0x1B00 && method < 0x1C00) {
        return WL_NV2A_DIRTY_TEXTURES;
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
        wl_nv2a_raise_pgraph(dev, WL_NV2A_PGRAPH_INTR_NOTIFY);
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
        uint32_t phys, length;

        memset(&attribs[a], 0, sizeof attribs[a]);
        if (bytes == 0) {
            continue;
        }
        phys = dma->base + (offset & 0x0FFFFFFFu) + least * stride;
        length = (last - least) * stride + bytes;
        attribs[a].data = wl_nv2a_phys(dev, phys, length);
        if (attribs[a].data == NULL) {
            static unsigned logged;

            if (logged++ < 8) {
                WL_LOG_W(WL_NV2A_TAG, "vertex array %u at %08x (%u bytes) not in guest RAM", a, (unsigned)phys,
                         (unsigned)length);
            }
            continue;
        }
        attribs[a].type = type;
        attribs[a].size = size;
        attribs[a].stride = stride;
        streamed |= 1u << a;
    }
    return streamed;
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
    k->dirty = 0;
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
    memset(&draw, 0, sizeof draw);
    draw.primitive = (uint8_t)primitive;
    draw.source = dev->batch_source;
    draw.continues = continues;
    draw.count = count;
    switch (dev->batch_source) {
    case WL_NV2A_SOURCE_ARRAYS: {
        uint32_t least = 0xFFFFFFFFu, last = 0, i;

        for (i = 0; i < count; i++) {
            uint32_t index = dev->batch_indices[i];

            least = index < least ? index : least;
            last = index > last ? index : last;
        }
        draw.indices = dev->batch_indices;
        draw.index_base = least;
        draw.index_last = last;
        draw.streamed = arrays_layout(dev, least, last, draw.attribs);
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

static void immediate_method(WlNv2a *dev, uint32_t method, uint32_t param) {
    WlNv2aKelvin *k = &dev->kelvin;
    float f = word_float(param);

    if (method >= WL_NV2A_K_SET_VERTEX3F && method <= WL_NV2A_K_SET_VERTEX3F + 8) {
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
    } else if (method >= WL_NV2A_K_SET_VERTEX_DATA2F_M && method < WL_NV2A_K_SET_VERTEX_DATA2S) {
        unsigned slot = (method - WL_NV2A_K_SET_VERTEX_DATA2F_M) >> 3, c = ((method - WL_NV2A_K_SET_VERTEX_DATA2F_M) >> 2) & 1u;

        set_current(k, slot, c, f);
        if (c == 1) {
            k->current[slot][2] = 0.0f;
            k->current[slot][3] = 1.0f;
            if (slot == 0) {
                emit_immediate(dev);
            }
        }
    } else if (method >= WL_NV2A_K_SET_VERTEX_DATA2S && method < WL_NV2A_K_SET_VERTEX_DATA4UB) {
        unsigned slot = (method - WL_NV2A_K_SET_VERTEX_DATA2S) >> 2;

        set_current4(k, slot, (float)(int16_t)(param & 0xFFFFu), (float)(int16_t)(param >> 16), 0.0f, 1.0f);
        if (slot == 0) {
            emit_immediate(dev);
        }
    } else if (method >= WL_NV2A_K_SET_VERTEX_DATA4UB && method < WL_NV2A_K_SET_VERTEX_DATA4S_M) {
        unsigned slot = (method - WL_NV2A_K_SET_VERTEX_DATA4UB) >> 2;

        bytes4(k, slot, param);
        if (slot == 0) {
            emit_immediate(dev);
        }
    } else if (method >= WL_NV2A_K_SET_VERTEX_DATA4S_M && method < WL_NV2A_K_SET_VERTEX_DATA4F_M) {
        unsigned slot = (method - WL_NV2A_K_SET_VERTEX_DATA4S_M) >> 3, c = ((method - WL_NV2A_K_SET_VERTEX_DATA4S_M) >> 2) & 1u;

        set_current(k, slot, 2 * c, (float)(int16_t)(param & 0xFFFFu));
        set_current(k, slot, 2 * c + 1, (float)(int16_t)(param >> 16));
        if (c == 1 && slot == 0) {
            emit_immediate(dev);
        }
    } else if (method >= WL_NV2A_K_SET_VERTEX_DATA4F_M && method < WL_NV2A_K_SET_VERTEX_DATA4F_M + 0x100) {
        unsigned slot = (method - WL_NV2A_K_SET_VERTEX_DATA4F_M) >> 4, c = ((method - WL_NV2A_K_SET_VERTEX_DATA4F_M) >> 2) & 3u;

        set_current(k, slot, c, f);
        if (c == 3 && slot == 0) {
            emit_immediate(dev);
        }
    }
}

static bool immediate_range(uint32_t method) {
    return (method >= 0x1500 && method <= 0x16CC && method != WL_NV2A_K_SET_EDGE_FLAG) ||
           (method >= WL_NV2A_K_SET_VERTEX_DATA2F_M && method < WL_NV2A_K_SET_VERTEX_DATA4F_M + 0x100);
}

// ---- the dispatch -----------------------------------------------------------------------------

void wl_nv2a_kelvin_method(WlNv2a *dev, uint32_t method, uint32_t param) {
    WlNv2aKelvin *k = &dev->kelvin;

    method &= WL_NV2A_K_METHODS - 4u;
    if (!known_method(method)) {
        note_unhandled(dev, method, param);
    }

    // the windows: their own storage, the slot moving on after the fourth word
    if (method >= WL_NV2A_K_SET_TRANSFORM_PROGRAM && method < WL_NV2A_K_SET_TRANSFORM_CONSTANT) {
        unsigned word = ((method - WL_NV2A_K_SET_TRANSFORM_PROGRAM) >> 2) & 3u;

        if (k->program_load < WL_NV2A_PROGRAM_SLOTS) {
            k->program[k->program_load][word] = param;
        }
        if (word == 3) {
            k->program_load++;
        }
        k->program_word = word;
        k->program_generation++;
        k->dirty |= WL_NV2A_DIRTY_PROGRAM;
        return;
    }
    if (method >= WL_NV2A_K_SET_TRANSFORM_CONSTANT && method < WL_NV2A_K_SET_TRANSFORM_CONSTANT + 0x80) {
        unsigned word = ((method - WL_NV2A_K_SET_TRANSFORM_CONSTANT) >> 2) & 3u;

        if (k->constant_load < WL_NV2A_CONSTANTS) {
            k->constants[k->constant_load][word] = word_float(param);
        }
        if (word == 3) {
            k->constant_load++;
        }
        k->constant_word = word;
        k->constants_generation++;
        k->dirty |= WL_NV2A_DIRTY_CONSTANTS;
        return;
    }

    k->raw[method >> 2] = param;
    if (immediate_range(method)) {
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
        k->program_generation++;
        k->dirty |= WL_NV2A_DIRTY_PROGRAM;
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
        if (method >= WL_NV2A_K_SET_VIEWPORT_OFFSET && method <= WL_NV2A_K_SET_VIEWPORT_OFFSET + 12) {
            k->constants[WL_NV2A_CONSTANT_VIEWPORT_OFFSET][(method - WL_NV2A_K_SET_VIEWPORT_OFFSET) >> 2] =
                word_float(param);
            k->constants_generation++;
            k->dirty |= WL_NV2A_DIRTY_VIEWPORT | WL_NV2A_DIRTY_CONSTANTS;
        } else if (method >= WL_NV2A_K_SET_VIEWPORT_SCALE && method <= WL_NV2A_K_SET_VIEWPORT_SCALE + 12) {
            k->constants[WL_NV2A_CONSTANT_VIEWPORT_SCALE][(method - WL_NV2A_K_SET_VIEWPORT_SCALE) >> 2] =
                word_float(param);
            k->constants_generation++;
            k->dirty |= WL_NV2A_DIRTY_VIEWPORT | WL_NV2A_DIRTY_CONSTANTS;
        } else {
            k->dirty |= dirty_group(method);
        }
        break;
    }
}
