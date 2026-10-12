// SPDX-License-Identifier: MIT
// What the NV2A model's files share (nv2a.c, nv2a_kelvin.c, nv2a_capture.c). Not for applications.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "wl/xbox/nv2a.h"
#include "wl/xbox/nv2a_kelvin.h"

#define WL_NV2A_TAG "nv2a"

enum {
    // the misc registers kept (read back as written): an open-addressed table
    WL_NV2A_REGISTER_SLOTS = 1024,
    WL_NV2A_SUBCHANNELS = 8,
    // a begin/end's batch, kept until it is handed over (in parts when longer)
    WL_NV2A_BATCH_INDICES = 8192,
    WL_NV2A_BATCH_INLINE_BYTES = 65536,
    WL_NV2A_BATCH_IMMEDIATE = 512,
    // push buffer words a kick may read before it is taken for a loop
    WL_NV2A_KICK_WORD_LIMIT = 64u * 1024u * 1024u
};

typedef struct WlNv2aChannel {
    uint32_t put, get, ref;
    uint32_t subroutine_return;
    uint8_t subroutine_active;
    // the method header in progress: its method, subchannel, words left, non-increasing
    uint32_t method, count;
    uint8_t subchannel, non_increasing;
    // stopped after a trapped method until the title's ISR acknowledges it
    uint8_t suspended;
    uint32_t subchannel_class[WL_NV2A_SUBCHANNELS];
    uint32_t subchannel_instance[WL_NV2A_SUBCHANNELS];
    // the puller's semaphore (its DMA object's base and limit, the offset)
    uint32_t semaphore_base, semaphore_limit, semaphore_offset;
} WlNv2aChannel;

typedef struct WlNv2aCapture WlNv2aCapture;

struct WlNv2a {
    WlNv2aConfig cfg;
    WlNv2aBackend backend;
    bool has_backend;
    uint32_t ram_bytes, ram_mask;
    WlNv2aStats stats;

    // registers with behaviour
    uint32_t pmc_intr_en, pmc_soft;
    uint32_t pfifo_intr, pfifo_intr_en, pfifo_ramht, pfifo_cache1_push1, pfifo_dma_instance;
    uint32_t pgraph_intr, pgraph_intr_en, pgraph_nsource, pgraph_trapped_addr, pgraph_trapped_data,
        pgraph_trapped_class, pgraph_fifo;
    uint32_t pcrtc_intr, pcrtc_intr_en, pcrtc_start;
    uint32_t ptimer_intr, ptimer_intr_en, ptimer_numerator, ptimer_denominator, ptimer_alarm;
    uint64_t time_ns;
    uint32_t pfb_wbc;
    uint8_t crtc_index, crtc[256];
    uint32_t raster_reads, status_reads;
    // every other register written, read back as written
    uint32_t reg_keys[WL_NV2A_REGISTER_SLOTS];
    uint32_t reg_values[WL_NV2A_REGISTER_SLOTS];
    uint32_t reg_used;

    WlNv2aChannel channels[WL_NV2A_CHANNELS];
    // the channel whose push buffer is running (for the puller's methods); -1 none
    int current_channel;
    // a kick is running (a resumed one does not start another inside it)
    bool executing;

    WlNv2aKelvin kelvin;
    // the begin/end batch (nv2a_kelvin.c)
    uint32_t *batch_indices;
    uint32_t batch_index_count;
    uint8_t *batch_inline;
    uint32_t batch_inline_bytes;
    float (*batch_immediate)[WL_NV2A_ATTRIBS][4];
    uint32_t batch_immediate_count;
    uint8_t batch_source;   // WL_NV2A_SOURCE_* of the batch (the first kind of vertex it got)
    uint8_t batch_mixed;    // vertices of another kind came too (logged, dropped)
    uint8_t batch_parts;    // parts already handed over
    uint32_t batch_loop_first[1]; // a line loop's first vertex (its slot), when it was handed in parts
    // the Kelvin methods logged once as not interpreted (bit by method / 4)
    uint8_t unhandled_logged[WL_NV2A_K_METHODS / 32];
    uint8_t classes_logged[256];

    WlNv2aCapture *capture;
    // a PCRTC_START write just ran (the capture notes it after the write)
    bool capture_flip;
};

// guest memory for the model's own writes (semaphores, notifiers, reports): not captured
void *wl_nv2a_phys_write(WlNv2a *dev, uint32_t phys, uint32_t bytes);
// a DMA object at an instance address, from instance memory
void wl_nv2a_dma_load(WlNv2a *dev, uint32_t instance, WlNv2aDma *out);
// the time PTIMER shows now
uint64_t wl_nv2a_time(WlNv2a *dev);

// the state tracker (nv2a_kelvin.c)
void wl_nv2a_kelvin_init(WlNv2a *dev);
void wl_nv2a_kelvin_method(WlNv2a *dev, uint32_t method, uint32_t param);
// a trap the title's ISR must see (NO_OPERATION with a parameter): true when the pusher stops
bool wl_nv2a_trap(WlNv2a *dev, uint32_t method, uint32_t param);
// the PGRAPH interrupt bits raised by a method
void wl_nv2a_raise_pgraph(WlNv2a *dev, uint32_t bits);

// the capture (nv2a_capture.c)
void wl_nv2a_capture_note_mmio(WlNv2a *dev, uint32_t offset, uint32_t value, unsigned size);
void wl_nv2a_capture_note_vblank(WlNv2a *dev);
void wl_nv2a_capture_note_memory(WlNv2a *dev, uint32_t phys, const void *data, uint32_t bytes);
void wl_nv2a_capture_note_flip(WlNv2a *dev);
bool wl_nv2a_capture_running(const WlNv2a *dev);
