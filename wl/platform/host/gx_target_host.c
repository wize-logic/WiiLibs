// SPDX-License-Identifier: MIT
// Host stub of the render targets' GPU half (gx_target_host.h).
#include <string.h>

#include "wl/platform/host/gx_target_host.h"

#include "wl/platform/gx_direct.h"
#include "wl/platform/host/gx_renderer_host.h"

static WlHostTargetOp ops[WL_HOST_TARGET_OPS_MAX];
static uint32_t op_count, copies;

uint32_t wl_host_gx_target_copies(void) {
    return copies;
}

const WlHostTargetOp *wl_host_target_ops(uint32_t *count) {
    *count = op_count;
    return ops;
}

void wl_host_target_clear_ops(void) {
    op_count = 0;
}

static WlHostTargetOp *next_op(WlHostTargetOpKind kind) {
    static WlHostTargetOp overflow; // (the operations past the limit are not kept)
    WlHostTargetOp *op = op_count < WL_HOST_TARGET_OPS_MAX ? &ops[op_count++] : &overflow;
    memset(op, 0, sizeof *op);
    op->kind = (uint8_t)kind;
    return op;
}

void wl_gx_target_be_default_size(uint32_t *w, uint32_t *h) {
    WlVideoInfo vi;
    wl_platform_video_info(&vi);
    *w = vi.fb_width;
    *h = vi.efb_height;
}

void wl_gx_target_be_copy_begin(void) {
    (void)next_op(WL_HOST_TARGET_COPY_BEGIN);
}

void wl_gx_target_be_copy(void *dst, uint32_t w, uint32_t h, WlGxTargetCopy what, bool invalidate) {
    (void)invalidate; // (no processor cache to keep in step)
    const WlHostGxRenderer *r = wl_host_gx_renderer();
    WlHostTargetOp *op = next_op(WL_HOST_TARGET_COPY);
    copies++;
    op->what = (uint8_t)what;
    op->w = w;
    op->h = h;
    op->dst = dst;
    if (r != NULL && r->copy != NULL) {
        r->copy(r->user, 0, 0, w, h, dst,
                what == WL_GX_TARGET_COPY_Z24X8 ? WL_HOST_GX_COPY_Z24X8
                : what == WL_GX_TARGET_COPY_RGB565 ? (unsigned)WL_GX_TEX_RGB565
                                                   : (unsigned)WL_GX_TEX_RGBA8,
                false);
        return;
    }
    // (the stub draws nothing: an empty image)
    memset(dst, 0, wl_gx_tex_bytes(what == WL_GX_TARGET_COPY_RGB565 ? WL_GX_TEX_RGB565 : WL_GX_TEX_RGBA8, w, h));
}

// What the console's quads (wii/gx_target_wii.c) leave in GX, so that the state recorded is what
// a draw after one would meet there.
static void quad_state(uint32_t back_w, uint32_t back_h, bool colour, bool alpha, bool z_test, bool z_write) {
    float m[4][4];

    memset(m, 0, sizeof m);
    m[0][0] = 2.0f / (float)back_w;
    m[0][3] = -1.0f;
    m[1][1] = -2.0f / (float)back_h;
    m[1][3] = 1.0f;
    m[2][2] = 1.0f;
    m[3][3] = 1.0f;
    wl_gx_direct_projection((const float (*)[4])m, true);
    wl_gx_direct_viewport(0.0f, 0.0f, (float)back_w, (float)back_h);
    wl_gx_direct_blend(WL_GX_BM_NONE, WL_GX_BL_ONE, WL_GX_BL_ZERO);
    wl_gx_direct_alpha_compare(WL_GX_ALWAYS, 0);
    wl_gx_direct_cull(WL_GX_CULL_NONE);
    wl_gx_direct_color_update(colour, alpha);
    wl_gx_direct_zmode(z_test, WL_GX_ALWAYS, z_write);
    wl_gx_direct_zcompare_early(true);
    // (the console's quad sets GX behind the shadow and then forgets it: counted alike)
    wl_gx_invalidate_shadow();
}

void wl_gx_target_be_copy_end(void) {
    (void)next_op(WL_HOST_TARGET_COPY_END);
}

void wl_gx_target_be_scissor(uint32_t w, uint32_t h) {
    WlHostTargetOp *op = next_op(WL_HOST_TARGET_SCISSOR);
    op->w = w;
    op->h = h;
    // (GX has one scissor: the direct layer's)
    wl_gx_direct_scissor(0, 0, w, h);
    wl_gx_invalidate_shadow(); // (as the console's)
}

void wl_gx_target_be_restore(const void *colour, const void *depth, uint32_t w, uint32_t h, uint32_t back_w,
                             uint32_t back_h) {
    const WlHostGxRenderer *r = wl_host_gx_renderer();
    WlHostTargetOp *op = next_op(WL_HOST_TARGET_RESTORE);
    op->dst = colour;
    op->depth_src = depth;
    op->w = w;
    op->h = h;
    op->back_w = back_w;
    op->back_h = back_h;
    if (r != NULL && r->restore != NULL) {
        r->restore(r->user, colour, depth, w, h);
    }
    quad_state(back_w, back_h, true, true, depth != NULL, depth != NULL);
}

void wl_gx_target_be_clear(bool colour, bool depth, uint32_t rgba, float z, int x, int y, int rw, int rh,
                           uint32_t back_w, uint32_t back_h) {
    const WlHostGxRenderer *r = wl_host_gx_renderer();
    WlHostTargetOp *op = next_op(WL_HOST_TARGET_CLEAR);
    op->colour = colour;
    op->depth = depth;
    op->rgba = rgba;
    op->z = z;
    op->x = x;
    op->y = y;
    op->w = (uint32_t)rw;
    op->h = (uint32_t)rh;
    op->back_w = back_w;
    op->back_h = back_h;
    if (r != NULL && r->clear != NULL) {
        r->clear(r->user, colour, depth, rgba, z, x, y, rw, rh);
    }
    quad_state(back_w, back_h, colour, colour, depth, depth);
}

void wl_gx_target_be_squeeze(void *colour, uint32_t back_w, uint32_t back_h, uint32_t x, uint32_t w,
                             bool invalidate) {
    (void)invalidate;
    const WlHostGxRenderer *r = wl_host_gx_renderer();
    WlHostTargetOp *op = next_op(WL_HOST_TARGET_SQUEEZE);
    copies++;
    op->dst = colour;
    op->back_w = back_w;
    op->back_h = back_h;
    op->x = (int32_t)x;
    op->w = w;
    if (r != NULL && r->squeeze != NULL) {
        r->squeeze(r->user, back_w, back_h, x, w);
    }
    quad_state(back_w, back_h, true, true, false, false);
    wl_gx_direct_scissor(0, 0, back_w, back_h);
}
