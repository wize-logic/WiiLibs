// SPDX-License-Identifier: MIT
// Captures of the NV2A model's input and their replay (nv2a_capture.h).
//
// The snapshot is one walk over the model's state (walk_state) that writes each field in order or
// reads it back in the same order, so the writer and the reader cannot disagree. A memory block is
// written when its range has not been written with the same bytes before (an FNV-1a hash of its
// bytes kept by range, 4096 ranges; a range pushed out is written again when it comes back).

#include "wl/xbox/nv2a_capture.h"

#include <stdlib.h>
#include <string.h>

#include "wl/base/endian.h"
#include "wl/base/hash.h"
#include "wl/xbox/nv2a_internal.h"

enum { CAPTURE_VERSION = 1, SEEN_SLOTS = 4096 };

static const char capture_magic[8] = {'W', 'L', 'N', 'V', '2', 'A', 'C', 'P'};

struct WlNv2aCapture {
    WlNv2aCaptureSink sink;
    uint32_t frames_left; // 0: until wl_nv2a_capture_end
    bool armed, running;
    WlResult error;
    struct {
        uint32_t phys, bytes;
        uint64_t hash;
    } seen[SEEN_SLOTS];
};

// ---- the state walk ---------------------------------------------------------------------------

typedef struct Walker {
    bool reading;
    // writing: a growing buffer; reading: the snapshot's bytes
    uint8_t *data;
    size_t size, capacity, at;
    bool failed;
} Walker;

static void walk_u32(Walker *w, uint32_t *value) {
    if (w->failed) {
        return;
    }
    if (w->reading) {
        if (w->at + 4 > w->size) {
            w->failed = true;
            return;
        }
        *value = wl_rd_le32(w->data + w->at);
        w->at += 4;
        return;
    }
    if (w->size + 4 > w->capacity) {
        size_t capacity = w->capacity ? w->capacity * 2 : 65536;
        uint8_t *grown = realloc(w->data, capacity);

        if (grown == NULL) {
            w->failed = true;
            return;
        }
        w->data = grown;
        w->capacity = capacity;
    }
    wl_wr_le32(w->data + w->size, *value);
    w->size += 4;
}

static void walk_words(Walker *w, uint32_t *words, size_t count) {
    size_t i;

    for (i = 0; i < count; i++) {
        walk_u32(w, &words[i]);
    }
}

static void walk_floats(Walker *w, float *values, size_t count) {
    size_t i;

    for (i = 0; i < count; i++) {
        uint32_t bits;

        memcpy(&bits, &values[i], 4);
        walk_u32(w, &bits);
        memcpy(&values[i], &bits, 4);
    }
}

static void walk_u8(Walker *w, uint8_t *value) {
    uint32_t word = *value;

    walk_u32(w, &word);
    *value = (uint8_t)word;
}

static void walk_dma(Walker *w, WlNv2aDma *dma) {
    walk_u32(w, &dma->instance);
    walk_u32(w, &dma->base);
    walk_u32(w, &dma->limit);
    walk_u32(w, &dma->class_id);
    walk_u32(w, &dma->valid);
}

static void walk_state(WlNv2a *dev, Walker *w) {
    WlNv2aKelvin *k = &dev->kelvin;
    uint32_t time_lo = (uint32_t)dev->time_ns, time_hi = (uint32_t)(dev->time_ns >> 32);
    unsigned i;

    walk_u32(w, &dev->pmc_intr_en);
    walk_u32(w, &dev->pmc_soft);
    walk_u32(w, &dev->pfifo_intr);
    walk_u32(w, &dev->pfifo_intr_en);
    walk_u32(w, &dev->pfifo_ramht);
    walk_u32(w, &dev->pfifo_cache1_push1);
    walk_u32(w, &dev->pfifo_dma_instance);
    walk_u32(w, &dev->pgraph_intr);
    walk_u32(w, &dev->pgraph_intr_en);
    walk_u32(w, &dev->pgraph_nsource);
    walk_u32(w, &dev->pgraph_trapped_addr);
    walk_u32(w, &dev->pgraph_trapped_data);
    walk_u32(w, &dev->pgraph_trapped_class);
    walk_u32(w, &dev->pgraph_fifo);
    walk_u32(w, &dev->pcrtc_intr);
    walk_u32(w, &dev->pcrtc_intr_en);
    walk_u32(w, &dev->pcrtc_start);
    walk_u32(w, &dev->ptimer_intr);
    walk_u32(w, &dev->ptimer_intr_en);
    walk_u32(w, &dev->ptimer_numerator);
    walk_u32(w, &dev->ptimer_denominator);
    walk_u32(w, &dev->ptimer_alarm);
    walk_u32(w, &time_lo);
    walk_u32(w, &time_hi);
    dev->time_ns = (uint64_t)time_hi << 32 | time_lo;
    walk_u32(w, &dev->pfb_wbc);
    walk_u8(w, &dev->crtc_index);
    for (i = 0; i < 256; i++) {
        walk_u8(w, &dev->crtc[i]);
    }
    // instance memory: each page, if it was written
    for (i = 0; i < WL_NV2A_PRAMIN_SIZE / WL_NV2A_RAMIN_PAGE; i++) {
        uint32_t present = dev->ramin[i] != NULL, word;
        unsigned j;

        walk_u32(w, &present);
        if (!present || w->failed) {
            continue;
        }
        if (w->reading && wl_nv2a_ramin_page(dev, i * WL_NV2A_RAMIN_PAGE) == NULL) {
            w->failed = true;
            return;
        }
        for (j = 0; j < WL_NV2A_RAMIN_PAGE; j += 4) {
            word = wl_rd_le32(dev->ramin[i] + j);
            walk_u32(w, &word);
            wl_wr_le32(dev->ramin[i] + j, word);
        }
    }
    walk_u32(w, &dev->reg_used);
    walk_words(w, dev->reg_keys, WL_NV2A_REGISTER_SLOTS);
    walk_words(w, dev->reg_values, WL_NV2A_REGISTER_SLOTS);
    for (i = 0; i < WL_NV2A_CHANNELS; i++) {
        WlNv2aChannel *ch = &dev->channels[i];

        walk_u32(w, &ch->put);
        walk_u32(w, &ch->get);
        walk_u32(w, &ch->ref);
        walk_u32(w, &ch->subroutine_return);
        walk_u8(w, &ch->subroutine_active);
        walk_u32(w, &ch->method);
        walk_u32(w, &ch->count);
        walk_u8(w, &ch->subchannel);
        walk_u8(w, &ch->non_increasing);
        walk_u8(w, &ch->suspended);
        walk_words(w, ch->subchannel_class, WL_NV2A_SUBCHANNELS);
        walk_words(w, ch->subchannel_instance, WL_NV2A_SUBCHANNELS);
        walk_u32(w, &ch->semaphore_base);
        walk_u32(w, &ch->semaphore_limit);
        walk_u32(w, &ch->semaphore_offset);
    }
    walk_words(w, k->raw, WL_NV2A_K_METHODS / 4);
    walk_dma(w, &k->dma_notifies);
    walk_dma(w, &k->dma_a);
    walk_dma(w, &k->dma_b);
    walk_dma(w, &k->dma_state);
    walk_dma(w, &k->dma_color);
    walk_dma(w, &k->dma_zeta);
    walk_dma(w, &k->dma_vertex_a);
    walk_dma(w, &k->dma_vertex_b);
    walk_dma(w, &k->dma_semaphore);
    walk_dma(w, &k->dma_report);
    walk_words(w, &k->program[0][0], WL_NV2A_PROGRAM_SLOTS * 4);
    walk_floats(w, &k->constants[0][0], WL_NV2A_CONSTANTS * 4);
    walk_u32(w, &k->program_load);
    walk_u32(w, &k->program_word);
    walk_u32(w, &k->constant_load);
    walk_u32(w, &k->constant_word);
    walk_floats(w, &k->current[0][0], WL_NV2A_ATTRIBS * 4);
    walk_u32(w, &k->zpass_pixels);
    walk_u32(w, &k->flip_read);
    walk_u32(w, &k->flip_write);
    walk_u32(w, &k->flip_modulo);
    walk_u32(w, &k->draw_serial);
    walk_u32(w, &k->frames);
    walk_u32(w, &k->program_generation);
    walk_u32(w, &k->constants_generation);
    walk_u32(w, &k->primitive);
    walk_u32(w, &k->dirty);
}

// ---- writing ----------------------------------------------------------------------------------

static void emit(WlNv2a *dev, const void *data, uint32_t bytes) {
    WlNv2aCapture *c = dev->capture;

    if (c == NULL || c->error != WL_OK) {
        return;
    }
    c->error = c->sink.write(c->sink.user, data, bytes);
    if (c->error != WL_OK) {
        WL_LOG_E(WL_NV2A_TAG, "capture: the sink failed (%s); the capture ends", wl_result_name(c->error));
    }
}

static void emit_tag(WlNv2a *dev, char tag) {
    emit(dev, &tag, 1);
}

static void emit_u32(WlNv2a *dev, uint32_t value) {
    uint8_t bytes[4];

    wl_wr_le32(bytes, value);
    emit(dev, bytes, 4);
}

static void capture_start(WlNv2a *dev) {
    WlNv2aCapture *c = dev->capture;
    Walker w;

    memset(&w, 0, sizeof w);
    walk_state(dev, &w);
    emit(dev, capture_magic, 8);
    emit_u32(dev, CAPTURE_VERSION);
    emit_u32(dev, dev->ram_bytes);
    if (w.failed) {
        c->error = WL_ERR_NOMEM;
    } else {
        emit_tag(dev, 'S');
        emit_u32(dev, (uint32_t)w.size);
        emit(dev, w.data, (uint32_t)w.size);
    }
    free(w.data);
    if (dev->batch_index_count || dev->batch_inline_bytes || dev->batch_immediate_count) {
        WL_LOG_W(WL_NV2A_TAG, "capture: began inside a begin/end; its vertices so far are not in it");
    }
    c->armed = false;
    c->running = true;
    WL_LOG_I(WL_NV2A_TAG, "capture: begun (%u flips)", (unsigned)c->frames_left);
}

static WlResult capture_new(WlNv2a *dev, const WlNv2aCaptureSink *sink, uint32_t frames, bool armed) {
    WlNv2aCapture *c;

    if (sink == NULL || sink->write == NULL) {
        return WL_ERR_ARG;
    }
    if (dev->capture != NULL) {
        return WL_ERR_STATE;
    }
    c = calloc(1, sizeof *c);
    if (c == NULL) {
        return WL_ERR_NOMEM;
    }
    c->sink = *sink;
    c->frames_left = frames;
    c->armed = armed;
    dev->capture = c;
    if (!armed) {
        capture_start(dev);
    }
    return dev->capture != NULL && c->error == WL_OK ? WL_OK : c->error;
}

WlResult wl_nv2a_capture_frames(WlNv2a *dev, const WlNv2aCaptureSink *sink, uint32_t frames) {
    return capture_new(dev, sink, frames != 0 ? frames : 1, true);
}

WlResult wl_nv2a_capture_begin(WlNv2a *dev, const WlNv2aCaptureSink *sink) {
    return capture_new(dev, sink, 0, false);
}

void wl_nv2a_capture_end(WlNv2a *dev) {
    WlNv2aCapture *c = dev->capture;

    if (c == NULL) {
        return;
    }
    if (c->running) {
        emit_tag(dev, 'E');
        WL_LOG_I(WL_NV2A_TAG, "capture: ended%s", c->error != WL_OK ? " (with a sink error)" : "");
    }
    dev->capture = NULL;
    free(c);
}

bool wl_nv2a_capture_active(const WlNv2a *dev) {
    return dev->capture != NULL;
}

bool wl_nv2a_capture_running(const WlNv2a *dev) {
    return dev->capture != NULL && dev->capture->running;
}

void wl_nv2a_capture_note_mmio(WlNv2a *dev, uint32_t offset, uint32_t value, unsigned size) {
    uint8_t record[10];

    if (!dev->capture->running) {
        return;
    }
    record[0] = 'W';
    wl_wr_le32(record + 1, offset);
    wl_wr_le32(record + 5, value);
    record[9] = (uint8_t)size;
    emit(dev, record, sizeof record);
}

void wl_nv2a_capture_note_vblank(WlNv2a *dev) {
    if (dev->capture->running) {
        emit_tag(dev, 'V');
    }
}

void wl_nv2a_capture_note_memory(WlNv2a *dev, uint32_t phys, const void *data, uint32_t bytes) {
    WlNv2aCapture *c = dev->capture;
    uint64_t hash;
    uint32_t slot;
    uint8_t header[9];

    if (!c->running) {
        return;
    }
    hash = wl_fnv1a64(data, bytes);
    slot = ((phys >> 2) * 2654435761u ^ bytes * 40503u) & (SEEN_SLOTS - 1u);
    if (c->seen[slot].phys == phys && c->seen[slot].bytes == bytes && c->seen[slot].hash == hash) {
        return;
    }
    c->seen[slot].phys = phys;
    c->seen[slot].bytes = bytes;
    c->seen[slot].hash = hash;
    header[0] = 'M';
    wl_wr_le32(header + 1, phys);
    wl_wr_le32(header + 5, bytes);
    emit(dev, header, sizeof header);
    emit(dev, data, bytes);
}

void wl_nv2a_capture_note_flip(WlNv2a *dev) {
    WlNv2aCapture *c = dev->capture;

    if (c->armed) {
        capture_start(dev);
        return;
    }
    emit_tag(dev, 'F');
    if (c->frames_left != 0 && --c->frames_left == 0) {
        wl_nv2a_capture_end(dev);
    }
}

// ---- replay -----------------------------------------------------------------------------------

typedef struct ReplayMemory {
    uint8_t *bytes;
    uint32_t size;
} ReplayMemory;

static void *replay_phys(void *user, uint32_t phys, uint32_t bytes) {
    ReplayMemory *memory = user;

    if (phys >= memory->size || bytes > memory->size - phys) {
        return NULL;
    }
    return memory->bytes + phys;
}

WlResult wl_nv2a_replay(const uint8_t *data, size_t bytes, const WlNv2aBackend *backend,
                        const WlNv2aReplayHooks *hooks, WlNv2aReplayInfo *info) {
    ReplayMemory memory;
    WlNv2aConfig cfg;
    WlNv2a *dev = NULL;
    WlNv2aReplayInfo local;
    size_t at;
    WlResult result = WL_OK;
    bool ended = false;

    if (info == NULL) {
        info = &local;
    }
    memset(info, 0, sizeof *info);
    if (data == NULL || bytes < 16 || memcmp(data, capture_magic, 8) != 0) {
        return WL_ERR_FORMAT;
    }
    if (wl_rd_le32(data + 8) != CAPTURE_VERSION) {
        return WL_ERR_VERSION;
    }
    memory.size = wl_rd_le32(data + 12);
    info->ram_bytes = memory.size;
    if (memory.size == 0 || (memory.size & (memory.size - 1u)) != 0) {
        return WL_ERR_FORMAT;
    }
    memory.bytes = calloc(1, memory.size);
    if (memory.bytes == NULL) {
        return WL_ERR_NOMEM;
    }
    memset(&cfg, 0, sizeof cfg);
    cfg.phys_ptr = replay_phys;
    cfg.user = &memory;
    cfg.backend = backend;
    cfg.ram_bytes = memory.size;
    dev = wl_nv2a_create(&cfg);
    if (dev == NULL) {
        free(memory.bytes);
        return WL_ERR_NOMEM;
    }
    if (hooks != NULL && hooks->created != NULL) {
        hooks->created(hooks->user, dev);
    }
    at = 16;
    while (at < bytes && !ended && result == WL_OK) {
        char tag = (char)data[at++];

        switch (tag) {
        case 'S': {
            Walker w;
            uint32_t size;

            if (at + 4 > bytes) {
                result = WL_ERR_FORMAT;
                break;
            }
            size = wl_rd_le32(data + at);
            at += 4;
            if (size > bytes - at) {
                result = WL_ERR_FORMAT;
                break;
            }
            memset(&w, 0, sizeof w);
            w.reading = true;
            w.data = (uint8_t *)(uintptr_t)(data + at);
            w.size = size;
            walk_state(dev, &w);
            if (w.failed) {
                result = WL_ERR_FORMAT;
            }
            at += size;
            break;
        }
        case 'W': {
            uint32_t offset, value;
            unsigned size;

            if (at + 9 > bytes) {
                result = WL_ERR_FORMAT;
                break;
            }
            offset = wl_rd_le32(data + at);
            value = wl_rd_le32(data + at + 4);
            size = data[at + 8];
            at += 9;
            if (hooks != NULL && hooks->mmio != NULL) {
                hooks->mmio(hooks->user, offset, value, size);
            }
            wl_nv2a_mmio_write(dev, offset, value, size);
            info->writes++;
            break;
        }
        case 'V':
            wl_nv2a_vblank(dev);
            info->vblanks++;
            break;
        case 'M': {
            uint32_t phys, size;

            if (at + 8 > bytes) {
                result = WL_ERR_FORMAT;
                break;
            }
            phys = wl_rd_le32(data + at);
            size = wl_rd_le32(data + at + 4);
            at += 8;
            if (size > bytes - at || phys >= memory.size || size > memory.size - phys) {
                result = WL_ERR_FORMAT;
                break;
            }
            memcpy(memory.bytes + phys, data + at, size);
            at += size;
            info->blocks++;
            info->block_bytes += size;
            break;
        }
        case 'F':
            if (hooks != NULL && hooks->flip != NULL) {
                hooks->flip(hooks->user, dev, info->flips);
            }
            info->flips++;
            break;
        case 'E':
            ended = true;
            break;
        default:
            result = WL_ERR_FORMAT;
            break;
        }
    }
    wl_nv2a_stats(dev, &info->stats);
    wl_nv2a_destroy(dev);
    free(memory.bytes);
    return result;
}
