// SPDX-License-Identifier: MIT
// The GX command FIFO's size (platform.h, wl_gx_fifo_bytes): what WL_TAG_GXFIFO's heap holds.
#include "wl/platform/mem.h"
#include "wl/platform/platform.h"

uint32_t wl_gx_fifo_bytes(void) {
    const void *base;
    size_t size;
    if (!wl_mem_tag_range(WL_TAG_GXFIFO, &base, &size) || size <= 32u) {
        return 0;
    }
    // the heap keeps a 32-byte header in front of its one block
    size_t bytes = (size - 32u) & ~(size_t)31u;
    if (bytes > WL_GX_FIFO_BYTES) {
        bytes = WL_GX_FIFO_BYTES;
    }
    return bytes < WL_GX_FIFO_MIN_BYTES ? 0u : (uint32_t)bytes;
}
