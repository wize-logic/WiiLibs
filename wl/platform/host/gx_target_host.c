// SPDX-License-Identifier: MIT
// Host stub of the render targets' GPU half (gx_target_host.h).
#include <string.h>

#include "wl/platform/host/gx_target_host.h"

static WlHostTargetOp ops[WL_HOST_TARGET_OPS_MAX];
static uint32_t op_count;

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

void wl_gx_target_be_copy(void *dst, uint32_t w, uint32_t h, WlGxTargetCopy what) {
    WlHostTargetOp *op = next_op(WL_HOST_TARGET_COPY);
    op->what = (uint8_t)what;
    op->w = w;
    op->h = h;
    op->dst = dst;
    // (the stub draws nothing: an empty image)
    memset(dst, 0, wl_gx_tex_bytes(what == WL_GX_TARGET_COPY_RGB565 ? WL_GX_TEX_RGB565 : WL_GX_TEX_RGBA8, w, h));
}

void wl_gx_target_be_copy_end(void) {
    (void)next_op(WL_HOST_TARGET_COPY_END);
}

void wl_gx_target_be_scissor(uint32_t w, uint32_t h) {
    WlHostTargetOp *op = next_op(WL_HOST_TARGET_SCISSOR);
    op->w = w;
    op->h = h;
}

void wl_gx_target_be_restore(const void *colour, const void *depth, uint32_t w, uint32_t h) {
    WlHostTargetOp *op = next_op(WL_HOST_TARGET_RESTORE);
    op->dst = colour;
    op->depth_src = depth;
    op->w = w;
    op->h = h;
}

void wl_gx_target_be_clear(bool colour, bool depth, uint32_t rgba, float z, int x, int y, int rw, int rh,
                           uint32_t back_w, uint32_t back_h) {
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
}
