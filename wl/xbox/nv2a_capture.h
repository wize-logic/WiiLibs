// SPDX-License-Identifier: MIT
// Captures of what the NV2A model was given (nv2a_capture.c): a frame or a few, as a file that
// replays through the same model and any backend on the host (tools/xbox_nv2a_replay.c).
//
// A capture starts with a snapshot of the model's state (its registers, channels and the 3D
// state), then records, in order, every MMIO write and vertical blank the model receives and every
// block of guest memory it or its backend reads through wl_nv2a_phys (push buffers, vertex and
// index data, textures, instance memory), as it was when it was read; a block read again unchanged
// is not written again. Replayed, the writes run the model over a guest memory the blocks fill in
// as they come: the same push buffers, the same draws.
//
// The file (all numbers little-endian): "WLNV2ACP", a u32 version (1), a u32 RAM size, then
// records of a byte and its fields:
//   'S' u32 bytes, the snapshot          'W' u32 offset, u32 value, u8 size: an MMIO write
//   'V' a vertical blank                  'M' u32 phys, u32 bytes, the bytes: guest memory
//   'F' a flip (after its PCRTC_START write)                  'E' the end
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wl/xbox/nv2a.h"

// where a capture goes: write() is called with each piece of the file in order (a WL_OK result
// or an error, which ends the capture)
typedef struct WlNv2aCaptureSink {
    void *user;
    WlResult (*write)(void *user, const void *data, uint32_t bytes);
} WlNv2aCaptureSink;

// A capture from the next flip (PCRTC_START write) to `frames` flips after it (the sink is
// copied). WL_ERR_STATE when one is already armed or running, WL_ERR_NOMEM.
WlResult wl_nv2a_capture_frames(WlNv2a *dev, const WlNv2aCaptureSink *sink, uint32_t frames);
// A capture from now until wl_nv2a_capture_end.
WlResult wl_nv2a_capture_begin(WlNv2a *dev, const WlNv2aCaptureSink *sink);
// Ends the capture (its 'E' record); nothing when none runs.
void wl_nv2a_capture_end(WlNv2a *dev);
// armed (waiting for its first flip) or running
bool wl_nv2a_capture_active(const WlNv2a *dev);

// ---- replay -----------------------------------------------------------------------------------
typedef struct WlNv2aReplayHooks {
    void *user;
    // the model made, before the first record (a backend that reads guest memory attaches to it
    // here); may be NULL
    void (*created)(void *user, WlNv2a *dev);
    // after each flip record: the model with its state then
    void (*flip)(void *user, WlNv2a *dev, uint32_t flip_index);
    // each MMIO write record before it runs (may be NULL)
    void (*mmio)(void *user, uint32_t offset, uint32_t value, unsigned size);
    // a memory record that changed guest memory, once its bytes are in (may be NULL): the span
    // from its first byte that differs to its last (what the title wrote since the capture last
    // read it, as far as the capture saw)
    void (*memory)(void *user, uint32_t phys, uint32_t bytes);
} WlNv2aReplayHooks;

typedef struct WlNv2aReplayInfo {
    uint32_t ram_bytes;
    uint32_t writes, vblanks, blocks, flips;
    uint64_t block_bytes;
    WlNv2aStats stats; // the model's, at the end
} WlNv2aReplayInfo;

// Replays a capture (the file's bytes) through a new model with `backend` (NULL: none), over a
// guest memory of its own (calloc of the RAM size). WL_ERR_FORMAT for a file that is not a capture
// or ends inside a record, WL_ERR_VERSION, WL_ERR_NOMEM.
WlResult wl_nv2a_replay(const uint8_t *data, size_t bytes, const WlNv2aBackend *backend,
                        const WlNv2aReplayHooks *hooks, WlNv2aReplayInfo *info);
