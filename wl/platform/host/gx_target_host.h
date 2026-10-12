// SPDX-License-Identifier: MIT
// Host stub of the render targets' GPU half (gx_target.h): the portable bookkeeping runs as on the
// console, and what it would have asked of the GPU is recorded for host tests.
#pragma once

#include "wl/platform/gx_target_internal.h"

typedef enum {
    WL_HOST_TARGET_COPY_BEGIN = 0, // a run of copies out of the EFB starts (the copy filter off)
    WL_HOST_TARGET_COPY,           // one copy: dst, w x h, what
    WL_HOST_TARGET_COPY_END,       // ...and ends (the filter back on, the texture cache invalidated)
    WL_HOST_TARGET_SCISSOR,        // (0, 0, w, h)
    WL_HOST_TARGET_RESTORE,        // the parked colour and depth over the EFB: colour, depth, w x h
    WL_HOST_TARGET_CLEAR           // a clear quad
} WlHostTargetOpKind;

typedef struct {
    uint8_t kind;           // WlHostTargetOpKind
    uint8_t what;           // WlGxTargetCopy of a COPY
    bool colour, depth;     // a CLEAR's
    uint32_t w, h;          // a COPY's, a SCISSOR's and a RESTORE's size; a CLEAR's rectangle size
    int32_t x, y;           // a CLEAR's rectangle origin
    uint32_t back_w, back_h; // a CLEAR's back buffer
    uint32_t rgba;          // a CLEAR's colour
    float z;                // a CLEAR's depth
    const void *dst;        // a COPY's destination; a RESTORE's colour
    const void *depth_src;  // a RESTORE's depth
} WlHostTargetOp;

// The operations since the last wl_host_target_clear_ops (the first WL_HOST_TARGET_OPS_MAX are kept).
#define WL_HOST_TARGET_OPS_MAX 64u
const WlHostTargetOp *wl_host_target_ops(uint32_t *count);
void wl_host_target_clear_ops(void);
