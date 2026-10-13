// SPDX-License-Identifier: MIT
// A vertex's bytes as GX_Begin sends it, and the descriptor GX misreads (gx_direct.h): no hardware,
// so the Wii's flush (wii/gx_flush_wii.c) and the tests share them. Moved from OpenCE-Wii's
// d3d8_gx.c (CC0: vertex_format, vertex_bytes, vertex_shape_check).
#include "wl/platform/gx_direct.h"

uint8_t wl_gx_attribute_bytes(uint32_t attribute, uint32_t count, uint32_t type) {
    // (GX_U8, GX_S8, GX_U16, GX_S16, GX_F32; GX_RGB565 to GX_RGBA8)
    static const uint8_t component_bytes[5] = {1, 1, 2, 2, 4};
    static const uint8_t color_bytes[6] = {2, 3, 4, 2, 3, 4};

    if (attribute == WL_GX_VA_CLR0 || attribute == WL_GX_VA_CLR1) {
        return type < 6 ? color_bytes[type] : 0;
    }
    if (type >= 5) {
        return 0;
    }
    if (attribute == WL_GX_VA_POS) {
        return (uint8_t)((count == WL_GX_COUNT_POS_XY ? 2 : 3) * component_bytes[type]);
    }
    if (attribute == WL_GX_VA_NRM) {
        return count == WL_GX_COUNT_NRM_XYZ ? (uint8_t)(3 * component_bytes[type]) : 0;
    }
    if (attribute >= WL_GX_VA_TEX0 && attribute <= WL_GX_VA_TEX7) {
        return (uint8_t)((count == WL_GX_COUNT_TEX_S ? 1 : 2) * component_bytes[type]);
    }
    return 0;
}

uint32_t wl_gx_vertex_bytes(const WlGxVtxDesc *desc, unsigned count,
                            const uint8_t bytes[WL_GX_VA_ATTRIBUTES]) {
    uint32_t total = 0;
    unsigned i;

    for (i = 0; i < count && desc[i].attr != WL_GX_VA_END; i++) {
        uint8_t attribute = desc[i].attr, type = desc[i].type;

        if (type == WL_GX_IN_NONE) {
            continue;
        }
        // (a matrix index is a byte; an index one or two)
        if (attribute <= WL_GX_VA_TEX7MTXIDX || type == WL_GX_IN_INDEX8) {
            total += 1;
        } else if (type == WL_GX_IN_INDEX16) {
            total += 2;
        } else if (attribute < WL_GX_VA_ATTRIBUTES && bytes[attribute] != 0) {
            total += bytes[attribute];
        } else {
            return 0;
        }
    }
    return total;
}

bool wl_gx_vertex_shape_ok(const WlGxVtxDesc *desc, unsigned count) {
    unsigned i, others = 0;
    bool position = false, matrices = false;

    for (i = 0; i < count && desc[i].attr != WL_GX_VA_END; i++) {
        if (desc[i].type == WL_GX_IN_NONE) {
            continue;
        }
        if (desc[i].attr <= WL_GX_VA_TEX7MTXIDX) {
            matrices = true;
        } else if (desc[i].attr == WL_GX_VA_POS) {
            position = true;
        } else {
            others++;
        }
    }
    return !position || !matrices || others != 0;
}
