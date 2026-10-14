// SPDX-License-Identifier: MIT
// The NV2A as a device (nv2a.h): its MMIO registers, the DMA pusher and the puller.
//
// The registers answer as an idle GPU's would: the FIFO's caches are empty, PGRAPH is not busy, the
// write-combining buffer's flush is done the moment it is asked for, the interrupt status registers
// show what the model raised and are cleared by writing ones. A register without behaviour reads
// back what was last written to it.
//
// A write of a channel's DMA_PUT runs the channel's push buffer at once, from GET to PUT: each
// word a command (a method header and its data words, an old-style or new-style jump, a call of
// one level, a return: envytools' "PFIFO DMA pusher") or the data of the method in progress. The
// state of a method header in progress is kept across kicks, as the pusher's is. The puller binds
// objects to subchannels by their handles (RAMHT in instance memory, searched whole: the hash is not
// needed to find an entry), runs its own methods (the reference counter, semaphores) and hands
// the rest to the object: the Kelvin class to the state tracker (nv2a_kelvin.c), whose context DMA
// handles it looks up first. Nothing waits: a semaphore acquire whose value is not there is
// counted and passed, as a synchronous GPU has nothing to wait for.
//
// A method the title traps on (Kelvin's NO_OPERATION with a parameter, which raises PGRAPH's ERROR
// interrupt for the title's ISR, as nxdk's pbkit uses it) stops the channel when the interrupt is
// enabled, as the GPU would stall, until the ISR clears it; the rest of the push buffer runs then.

#include "wl/xbox/nv2a_internal.h"

#include <stdlib.h>
#include <string.h>

#include "wl/base/endian.h"

// ---- creation -----------------------------------------------------------------------------

WlNv2a *wl_nv2a_create(const WlNv2aConfig *cfg) {
    WlNv2a *dev;

    if (cfg == NULL || cfg->phys_ptr == NULL) {
        return NULL;
    }
    dev = calloc(1, sizeof *dev);
    if (dev == NULL) {
        return NULL;
    }
    dev->cfg = *cfg;
    dev->ram_bytes = cfg->ram_bytes != 0 ? cfg->ram_bytes : 64u * 1024u * 1024u;
    if ((dev->ram_bytes & (dev->ram_bytes - 1u)) != 0 || dev->ram_bytes < 65536u) {
        free(dev);
        return NULL;
    }
    dev->ram_mask = dev->ram_bytes - 1u;
    if (cfg->backend != NULL) {
        dev->backend = *cfg->backend;
        dev->has_backend = true;
    }
    dev->batch_indices = malloc(WL_NV2A_BATCH_INDICES * sizeof *dev->batch_indices);
    dev->batch_inline = malloc(WL_NV2A_BATCH_INLINE_BYTES);
    dev->batch_immediate = malloc(WL_NV2A_BATCH_IMMEDIATE * sizeof *dev->batch_immediate);
    if (dev->batch_indices == NULL || dev->batch_inline == NULL || dev->batch_immediate == NULL) {
        wl_nv2a_destroy(dev);
        return NULL;
    }
    dev->current_channel = -1;
    dev->pgraph_fifo = 1;
    dev->ptimer_numerator = 1;
    dev->ptimer_denominator = 1;
    wl_nv2a_kelvin_init(dev);
    return dev;
}

void wl_nv2a_destroy(WlNv2a *dev) {
    if (dev == NULL) {
        return;
    }
    {
        unsigned i;

        for (i = 0; i < WL_NV2A_PRAMIN_SIZE / WL_NV2A_RAMIN_PAGE; i++) {
            free(dev->ramin[i]);
        }
    }
    free(dev->batch_indices);
    free(dev->batch_inline);
    free(dev->batch_immediate);
    free(dev);
}

void wl_nv2a_set_backend(WlNv2a *dev, const WlNv2aBackend *backend) {
    if (backend != NULL) {
        dev->backend = *backend;
        dev->has_backend = true;
    } else {
        memset(&dev->backend, 0, sizeof dev->backend);
        dev->has_backend = false;
    }
}

const WlNv2aKelvin *wl_nv2a_kelvin(const WlNv2a *dev) {
    return &dev->kelvin;
}

void wl_nv2a_stats(const WlNv2a *dev, WlNv2aStats *out) {
    *out = dev->stats;
}

// ---- guest memory ---------------------------------------------------------------------------

void *wl_nv2a_phys_write(WlNv2a *dev, uint32_t phys, uint32_t bytes) {
    void *host;

    phys &= dev->ram_mask;
    if (bytes == 0 || bytes > dev->ram_bytes - phys) {
        dev->stats.memory_misses++;
        return NULL;
    }
    host = dev->cfg.phys_ptr(dev->cfg.user, phys, bytes);
    if (host == NULL) {
        dev->stats.memory_misses++;
    }
    return host;
}

void *wl_nv2a_phys(WlNv2a *dev, uint32_t phys, uint32_t bytes) {
    void *host = wl_nv2a_phys_write(dev, phys, bytes);

    if (host != NULL && dev->capture != NULL) {
        wl_nv2a_capture_note_memory(dev, phys & dev->ram_mask, host, bytes);
    }
    return host;
}

// ---- instance memory ----------------------------------------------------------------------------
// The device's own megabyte behind PRAMIN (xemu keeps it apart from RAM too: the Xbox's Direct3D
// writes its objects, RAMHT and RAMFC through the window), in 64 KiB pages made at their first write.

uint32_t wl_nv2a_ramin_read32(const WlNv2a *dev, uint32_t offset) {
    const uint8_t *page;

    offset &= (WL_NV2A_PRAMIN_SIZE - 1u) & ~3u;
    page = dev->ramin[offset / WL_NV2A_RAMIN_PAGE];
    return page != NULL ? wl_rd_le32(page + offset % WL_NV2A_RAMIN_PAGE) : 0;
}

uint8_t *wl_nv2a_ramin_page(WlNv2a *dev, uint32_t offset) {
    uint8_t **page = &dev->ramin[(offset & (WL_NV2A_PRAMIN_SIZE - 1u)) / WL_NV2A_RAMIN_PAGE];

    if (*page == NULL) {
        *page = calloc(1, WL_NV2A_RAMIN_PAGE);
    }
    return *page;
}

static uint32_t ramin_word(WlNv2a *dev, uint32_t instance) {
    return wl_nv2a_ramin_read32(dev, instance);
}

void wl_nv2a_dma_load(WlNv2a *dev, uint32_t instance, WlNv2aDma *out) {
    memset(out, 0, sizeof *out);
    out->instance = instance;
    out->limit = 0xFFFFFFFFu;
    if (instance == 0) {
        return;
    }
    {
        uint32_t flags = ramin_word(dev, instance), limit = ramin_word(dev, instance + 4),
                 frame = ramin_word(dev, instance + 8);

        if (flags == 0) {
            WL_LOG_W(WL_NV2A_TAG, "no DMA object at instance %05x: base 0, no limit", (unsigned)instance);
            return;
        }
        out->class_id = flags & WL_NV2A_DMA_CLASS_MASK;
        out->base = (frame & WL_NV2A_DMA_PAGE_MASK) | (flags >> WL_NV2A_DMA_ADJUST_SHIFT);
        out->limit = limit;
        out->valid = 1;
    }
}

// The instance address of a handle's object for a channel, from RAMHT (0: not found). The table
// is searched whole (a valid entry of the handle and the channel; else of the handle alone).
static uint32_t ramht_lookup(WlNv2a *dev, unsigned chid, uint32_t handle, uint32_t *engine) {
    uint32_t base = (dev->pfifo_ramht & 0x1F0u) << 8;
    uint32_t size = 4096u << ((dev->pfifo_ramht >> 16) & 3u);
    uint32_t found = 0, offset;
    bool exact = false;

    *engine = 0;
    for (offset = 0; offset + 8 <= size; offset += 8) {
        uint32_t key = ramin_word(dev, base + offset), context = ramin_word(dev, base + offset + 4);

        if (key != handle || !(context & WL_NV2A_RAMHT_VALID)) {
            continue;
        }
        if (((context >> WL_NV2A_RAMHT_CHID_SHIFT) & 0x1Fu) == chid) {
            found = (context & WL_NV2A_RAMHT_INSTANCE_MASK) << 4;
            *engine = (context >> WL_NV2A_RAMHT_ENGINE_SHIFT) & 3u;
            exact = true;
            break;
        }
        if (found == 0) {
            found = (context & WL_NV2A_RAMHT_INSTANCE_MASK) << 4;
            *engine = (context >> WL_NV2A_RAMHT_ENGINE_SHIFT) & 3u;
        }
    }
    if (found != 0 && !exact) {
        WL_LOG_D(WL_NV2A_TAG, "handle %08x found for another channel than %u", (unsigned)handle, chid);
    }
    return found;
}

// ---- time and interrupts --------------------------------------------------------------------

uint64_t wl_nv2a_time(WlNv2a *dev) {
    if (dev->cfg.time_ns != NULL) {
        dev->time_ns = dev->cfg.time_ns(dev->cfg.user);
    }
    return dev->time_ns;
}

static uint32_t pmc_intr(const WlNv2a *dev) {
    uint32_t bits = dev->pmc_soft;

    if (dev->pfifo_intr & dev->pfifo_intr_en) {
        bits |= WL_NV2A_PMC_INTR_PFIFO;
    }
    if (dev->pgraph_intr & dev->pgraph_intr_en) {
        bits |= WL_NV2A_PMC_INTR_PGRAPH;
    }
    if (dev->ptimer_intr & dev->ptimer_intr_en) {
        bits |= WL_NV2A_PMC_INTR_PTIMER;
    }
    if (dev->pcrtc_intr & dev->pcrtc_intr_en) {
        bits |= WL_NV2A_PMC_INTR_PCRTC;
    }
    return bits;
}

bool wl_nv2a_irq_pending(const WlNv2a *dev) {
    return (dev->pmc_intr_en & 3u) != 0 && pmc_intr(dev) != 0;
}

void wl_nv2a_raise_pgraph(WlNv2a *dev, uint32_t bits) {
    dev->pgraph_intr |= bits;
}

void wl_nv2a_vblank(WlNv2a *dev) {
    if (dev->capture != NULL) {
        wl_nv2a_capture_note_vblank(dev);
    }
    dev->pcrtc_intr |= WL_NV2A_PCRTC_INTR_VBLANK;
    dev->stats.vblanks++;
    if (dev->cfg.time_ns == NULL) {
        dev->time_ns += 16683333u;
    }
}

// ---- the misc registers -----------------------------------------------------------------------

static uint32_t reg_slot(const WlNv2a *dev, uint32_t offset, bool *found) {
    uint32_t key = offset | 1u, slot = ((offset >> 2) * 2654435761u) & (WL_NV2A_REGISTER_SLOTS - 1u);
    unsigned probe;

    for (probe = 0; probe < WL_NV2A_REGISTER_SLOTS; probe++) {
        uint32_t at = (slot + probe) & (WL_NV2A_REGISTER_SLOTS - 1u);

        if (dev->reg_keys[at] == key) {
            *found = true;
            return at;
        }
        if (dev->reg_keys[at] == 0) {
            *found = false;
            return at;
        }
    }
    *found = false;
    return WL_NV2A_REGISTER_SLOTS;
}

static uint32_t reg_get(const WlNv2a *dev, uint32_t offset) {
    bool found;
    uint32_t slot = reg_slot(dev, offset, &found);

    return found ? dev->reg_values[slot] : 0;
}

static void reg_put(WlNv2a *dev, uint32_t offset, uint32_t value) {
    bool found;
    uint32_t slot = reg_slot(dev, offset, &found);

    if (slot == WL_NV2A_REGISTER_SLOTS) {
        static bool logged;

        if (!logged) {
            logged = true;
            WL_LOG_W(WL_NV2A_TAG, "register table full: %06x not kept", (unsigned)offset);
        }
        return;
    }
    if (!found) {
        dev->reg_keys[slot] = offset | 1u;
        dev->reg_used++;
    }
    dev->reg_values[slot] = value;
}

// ---- the pusher -----------------------------------------------------------------------------

static void puller(WlNv2a *dev, unsigned chid, unsigned subchannel, uint32_t method, uint32_t param);

static void pusher_error(WlNv2a *dev, unsigned chid, uint32_t at, uint32_t word, const char *what) {
    dev->stats.pusher_errors++;
    if (dev->stats.pusher_errors <= 16) {
        WL_LOG_W(WL_NV2A_TAG, "channel %u: %s at %08x (word %08x)", chid, what, (unsigned)at, (unsigned)word);
    }
}

// whether a run of words to this method may go to the 3D object at once: the 3D class's own methods
// (not the puller's, an object handle the puller looks up, or the trap's NO_OPERATION), a run that
// stays among them
static bool bulk_method(const WlNv2aChannel *ch, unsigned subchannel, uint32_t method) {
    return ch->subchannel_class[subchannel] == WL_NV2A_CLASS_KELVIN && method >= 0x100u &&
           method != WL_NV2A_K_NO_OPERATION &&
           !(method >= WL_NV2A_PULLER_HANDLE_FIRST && method <= WL_NV2A_PULLER_HANDLE_LAST) &&
           (ch->non_increasing || method + 4u * ch->count <= WL_NV2A_PULLER_HANDLE_FIRST ||
            method > WL_NV2A_PULLER_HANDLE_LAST);
}

static void run_channel(WlNv2a *dev, unsigned chid) {
    WlNv2aChannel *ch = &dev->channels[chid];
    uint32_t words = 0;

    if (dev->executing || ch->suspended || ch->get == ch->put) {
        return;
    }
    dev->executing = true;
    dev->current_channel = (int)chid;
    dev->stats.kicks++;
    while (ch->get != ch->put && !ch->suspended) {
        // the words from GET up to PUT, or a while past a wrap (a jump ends it)
        uint32_t span = ch->put > ch->get ? ch->put - ch->get : 1024u;
        const uint8_t *p;
        uint32_t i;
        bool redirected = false;

        if (span > 16384u) {
            span = 16384u;
        }
        p = wl_nv2a_phys(dev, ch->get, span);
        if (p == NULL) {
            span = 4;
            p = wl_nv2a_phys(dev, ch->get, 4);
            if (p == NULL) {
                pusher_error(dev, chid, ch->get, 0, "push buffer outside guest RAM");
                dev->pfifo_intr |= WL_NV2A_PFIFO_INTR_DMA_PUSHER;
                ch->get = ch->put;
                break;
            }
        }
        for (i = 0; i < span && ch->get != ch->put && !ch->suspended && !redirected; i += 4) {
            uint32_t word = wl_rd_le32(p + i), at = ch->get;

            ch->get += 4;
            dev->stats.words++;
            if (++words > WL_NV2A_KICK_WORD_LIMIT) {
                pusher_error(dev, chid, at, word, "runaway push buffer (a jump loop?)");
                ch->get = ch->put;
                break;
            }
            if (ch->count != 0) {
                uint32_t method = ch->method, run;

                // (the rest of a header's run to the 3D object at once, as far as this span, PUT and
                // the word limit allow: each word what the puller makes of it, without the pusher's and
                // the puller's tests for each)
                run = (span - i) / 4u;
                if (ch->put >= at && (ch->put - at) / 4u < run) {
                    run = (ch->put - at) / 4u;
                }
                run = run < ch->count ? run : ch->count;
                if (run > WL_NV2A_KICK_WORD_LIMIT - (words - 1u)) {
                    run = WL_NV2A_KICK_WORD_LIMIT - (words - 1u);
                }
                if (run >= 2 && bulk_method(ch, ch->subchannel, method)) {
                    ch->get = at + 4u * run;
                    dev->stats.words += run - 1u;
                    dev->stats.methods += run;
                    words += run - 1u;
                    ch->count -= run;
                    if (!ch->non_increasing) {
                        ch->method += 4u * run;
                    }
                    wl_nv2a_kelvin_methods(dev, method, !ch->non_increasing, p + i, run);
                    i += 4u * (run - 1u);
                    continue;
                }
                if (!ch->non_increasing) {
                    ch->method += 4;
                }
                ch->count--;
                puller(dev, chid, ch->subchannel, method, word);
                continue;
            }
            if ((word & WL_NV2A_CMD_OLD_JUMP_MASK) == WL_NV2A_CMD_OLD_JUMP) {
                ch->get = word & WL_NV2A_CMD_OLD_JUMP_TARGET;
                dev->stats.jumps++;
                redirected = true;
            } else if ((word & 3u) == WL_NV2A_CMD_JUMP) {
                ch->get = word & ~3u;
                dev->stats.jumps++;
                redirected = true;
            } else if ((word & 3u) == WL_NV2A_CMD_CALL) {
                if (ch->subroutine_active) {
                    pusher_error(dev, chid, at, word, "call inside a call");
                    continue;
                }
                ch->subroutine_return = ch->get;
                ch->subroutine_active = 1;
                ch->get = word & ~3u;
                dev->stats.calls++;
                redirected = true;
            } else if (word == WL_NV2A_CMD_RETURN) {
                if (!ch->subroutine_active) {
                    pusher_error(dev, chid, at, word, "return outside a call");
                    continue;
                }
                ch->get = ch->subroutine_return;
                ch->subroutine_active = 0;
                dev->stats.returns++;
                redirected = true;
            } else if ((word & WL_NV2A_CMD_TYPE_MASK) == WL_NV2A_CMD_INCREASING ||
                       (word & WL_NV2A_CMD_TYPE_MASK) == WL_NV2A_CMD_NON_INCREASING) {
                ch->method = word & WL_NV2A_CMD_METHOD_MASK;
                ch->subchannel = (uint8_t)((word >> WL_NV2A_CMD_SUBCH_SHIFT) & 7u);
                ch->count = (word >> WL_NV2A_CMD_COUNT_SHIFT) & WL_NV2A_CMD_COUNT_MASK;
                ch->non_increasing = (word & WL_NV2A_CMD_TYPE_MASK) == WL_NV2A_CMD_NON_INCREASING;
            } else {
                pusher_error(dev, chid, at, word, "invalid command");
            }
        }
    }
    dev->executing = false;
    dev->current_channel = -1;
}

// ---- the puller -------------------------------------------------------------------------------

static void bind_object(WlNv2a *dev, unsigned chid, unsigned subchannel, uint32_t handle) {
    WlNv2aChannel *ch = &dev->channels[chid];
    uint32_t engine, instance = ramht_lookup(dev, chid, handle, &engine), class_id = 0;

    if (instance != 0) {
        class_id = ramin_word(dev, instance) & 0xFFu;
    } else if (subchannel == 0) {
        // (no RAMHT to look in: subchannel 0 is taken for the 3D object, as Direct3D binds it)
        class_id = WL_NV2A_CLASS_KELVIN;
        WL_LOG_W(WL_NV2A_TAG, "object %08x not in RAMHT: subchannel 0 taken for the 3D class", (unsigned)handle);
    } else {
        WL_LOG_W(WL_NV2A_TAG, "object %08x not in RAMHT (subchannel %u)", (unsigned)handle, subchannel);
    }
    ch->subchannel_class[subchannel] = class_id;
    ch->subchannel_instance[subchannel] = instance;
}

void wl_nv2a_bind_class(WlNv2a *dev, unsigned subchannel, uint32_t class_id) {
    unsigned chid;

    for (chid = 0; chid < WL_NV2A_CHANNELS; chid++) {
        dev->channels[chid].subchannel_class[subchannel & 7u] = class_id;
    }
}

static void puller_semaphore_write(WlNv2a *dev, WlNv2aChannel *ch, uint32_t value) {
    uint8_t *p;

    if (ch->semaphore_offset > ch->semaphore_limit) {
        return;
    }
    p = wl_nv2a_phys_write(dev, ch->semaphore_base + ch->semaphore_offset, 4);
    if (p != NULL) {
        wl_wr_le32(p, value);
        dev->stats.semaphores++;
    }
}

static void puller(WlNv2a *dev, unsigned chid, unsigned subchannel, uint32_t method, uint32_t param) {
    WlNv2aChannel *ch = &dev->channels[chid];

    dev->stats.methods++;
    if (method == WL_NV2A_PULLER_OBJECT) {
        bind_object(dev, chid, subchannel, param);
        return;
    }
    if (method < 0x100u) {
        switch (method) {
        case WL_NV2A_PULLER_REF_CNT:
            ch->ref = param;
            break;
        case WL_NV2A_PULLER_DMA_SEMAPHORE: {
            uint32_t engine, instance = ramht_lookup(dev, chid, param, &engine);
            WlNv2aDma dma;

            wl_nv2a_dma_load(dev, instance, &dma);
            ch->semaphore_base = dma.base;
            ch->semaphore_limit = dma.limit;
            break;
        }
        case WL_NV2A_PULLER_SEMAPHORE_OFFSET:
            ch->semaphore_offset = param & 0xFFCu;
            break;
        case WL_NV2A_PULLER_SEMAPHORE_ACQUIRE: {
            const uint8_t *p = wl_nv2a_phys(dev, ch->semaphore_base + ch->semaphore_offset, 4);

            if (p == NULL || wl_rd_le32(p) != param) {
                dev->stats.acquires_unmet++;
            }
            break;
        }
        case WL_NV2A_PULLER_SEMAPHORE_RELEASE:
            puller_semaphore_write(dev, ch, param);
            break;
        default:
            if (dev->stats.unhandled < 64) {
                WL_LOG_W(WL_NV2A_TAG, "puller method %03x (%08x) not handled", (unsigned)method, (unsigned)param);
            }
            dev->stats.unhandled++;
            break;
        }
        return;
    }
    if (method >= WL_NV2A_PULLER_HANDLE_FIRST && method <= WL_NV2A_PULLER_HANDLE_LAST) {
        uint32_t engine;

        param = param != 0 ? ramht_lookup(dev, chid, param, &engine) : 0;
    }
    switch (ch->subchannel_class[subchannel]) {
    case WL_NV2A_CLASS_KELVIN:
        if (method == WL_NV2A_K_NO_OPERATION && param != 0) {
            if (wl_nv2a_trap(dev, method | (uint32_t)subchannel << 13, param)) {
                ch->suspended = 1;
            }
            return;
        }
        wl_nv2a_kelvin_method(dev, method, param);
        break;
    default: {
        uint32_t class_id = ch->subchannel_class[subchannel] & 0xFFu;

        if (!dev->classes_logged[class_id]) {
            dev->classes_logged[class_id] = 1;
            WL_LOG_W(WL_NV2A_TAG, "methods of class %02x (subchannel %u) are not handled; first %04x", (unsigned)class_id,
                     subchannel, (unsigned)method);
        }
        dev->stats.unhandled++;
        break;
    }
    }
}

bool wl_nv2a_trap(WlNv2a *dev, uint32_t method, uint32_t param) {
    unsigned chid = dev->current_channel >= 0 ? (unsigned)dev->current_channel : 0;

    // what pbkit's ISR reads of it: the method and channel, the data, the class, ERROR with a
    // notification as its source
    dev->pgraph_trapped_addr = (uint32_t)chid << 20 | (method & 0xFFFFu);
    dev->pgraph_trapped_data = param;
    dev->pgraph_trapped_class = WL_NV2A_CLASS_KELVIN;
    dev->pgraph_nsource |= 1u;
    dev->pgraph_intr |= WL_NV2A_PGRAPH_INTR_ERROR;
    return (dev->pmc_intr_en & 3u) != 0 && (dev->pgraph_intr_en & WL_NV2A_PGRAPH_INTR_ERROR) != 0;
}

void wl_nv2a_method(WlNv2a *dev, unsigned subchannel, uint32_t method, uint32_t param) {
    int was = dev->current_channel;

    if (was < 0) {
        dev->current_channel = 0;
    }
    puller(dev, (unsigned)dev->current_channel, subchannel & 7u, method & 0x1FFCu, param);
    dev->current_channel = was;
}

// a channel stopped at a trap goes on once the ISR has cleared it
static void resume_trapped(WlNv2a *dev) {
    unsigned chid;

    if (dev->pgraph_intr & WL_NV2A_PGRAPH_INTR_ERROR) {
        return;
    }
    for (chid = 0; chid < WL_NV2A_CHANNELS; chid++) {
        if (dev->channels[chid].suspended) {
            dev->channels[chid].suspended = 0;
            run_channel(dev, chid);
        }
    }
}

// ---- MMIO -------------------------------------------------------------------------------------

static WlNv2aChannel *cache1_channel(WlNv2a *dev) {
    return &dev->channels[dev->pfifo_cache1_push1 & 0x1Fu];
}

static uint32_t read32(WlNv2a *dev, uint32_t offset) {
    switch (offset) {
    case WL_NV2A_PMC_BOOT_0:
        return 0x02A000A1u;
    case WL_NV2A_PMC_INTR_0:
        return pmc_intr(dev);
    case WL_NV2A_PMC_INTR_EN_0:
        return dev->pmc_intr_en;
    case WL_NV2A_PBUS_PCI_NV_0:
        return WL_NV2A_PCI_ID;
    case WL_NV2A_PFIFO_INTR_0:
        return dev->pfifo_intr;
    case WL_NV2A_PFIFO_INTR_EN_0:
        return dev->pfifo_intr_en;
    case WL_NV2A_PFIFO_RAMHT:
        return dev->pfifo_ramht;
    case WL_NV2A_PFIFO_RUNOUT_STATUS:
        return WL_NV2A_PFIFO_RUNOUT_LOW_MARK_EMPTY;
    case WL_NV2A_PFIFO_CACHE1_PUSH1:
        return dev->pfifo_cache1_push1;
    case WL_NV2A_PFIFO_CACHE1_STATUS:
        return WL_NV2A_PFIFO_CACHE1_STATUS_LOW_MARK_EMPTY;
    case WL_NV2A_PFIFO_CACHE1_DMA_PUSH:
        // not busy, the buffer empty, not suspended
        return (reg_get(dev, offset) & ~((uint32_t)WL_NV2A_PFIFO_CACHE1_DMA_PUSH_STATE_BUSY | 0x1000u)) | 0x100u;
    case WL_NV2A_PFIFO_CACHE1_DMA_STATE:
        return 0;
    case WL_NV2A_PFIFO_CACHE1_DMA_INSTANCE:
        return dev->pfifo_dma_instance;
    case WL_NV2A_PFIFO_CACHE1_DMA_PUT:
        return cache1_channel(dev)->put;
    case WL_NV2A_PFIFO_CACHE1_DMA_GET:
        return cache1_channel(dev)->get;
    case WL_NV2A_PFIFO_CACHE1_REF:
        return cache1_channel(dev)->ref;
    case WL_NV2A_PFIFO_CACHE1_DMA_SUBROUTINE: {
        WlNv2aChannel *ch = cache1_channel(dev);

        return ch->subroutine_return | ch->subroutine_active;
    }
    case WL_NV2A_PFIFO_CACHE1_PULL0:
        return reg_get(dev, offset) & ~(uint32_t)WL_NV2A_PFIFO_CACHE1_PULL0_HASH_STATE_BUSY;
    case WL_NV2A_PFIFO_CACHE1_GET:
        // the cache empty: GET at PUT
        return reg_get(dev, WL_NV2A_PFIFO_CACHE1_PUT);
    case WL_NV2A_PTIMER_INTR_0:
        return dev->ptimer_intr;
    case WL_NV2A_PTIMER_INTR_EN_0:
        return dev->ptimer_intr_en;
    case WL_NV2A_PTIMER_NUMERATOR:
        return dev->ptimer_numerator;
    case WL_NV2A_PTIMER_DENOMINATOR:
        return dev->ptimer_denominator;
    case WL_NV2A_PTIMER_TIME_0: {
        uint64_t t = wl_nv2a_time(dev);

        if (dev->cfg.time_ns == NULL) {
            dev->time_ns += 1000u;
        }
        return (uint32_t)t & ~0x1Fu;
    }
    case WL_NV2A_PTIMER_TIME_1:
        return (uint32_t)(wl_nv2a_time(dev) >> 32) & 0x1FFFFFFFu;
    case WL_NV2A_PTIMER_ALARM_0:
        return dev->ptimer_alarm;
    case WL_NV2A_PFB_CFG0: {
        bool found;
        uint32_t slot = reg_slot(dev, offset, &found);

        // (four memory partitions unless the title set it)
        return found ? dev->reg_values[slot] : 3u;
    }
    case WL_NV2A_PFB_CSTATUS:
        return dev->ram_bytes;
    case WL_NV2A_PFB_WBC:
        return dev->pfb_wbc;
    case WL_NV2A_PGRAPH_INTR:
        return dev->pgraph_intr;
    case WL_NV2A_PGRAPH_NSOURCE:
        return dev->pgraph_nsource;
    case WL_NV2A_PGRAPH_INTR_EN:
        return dev->pgraph_intr_en;
    case 0x40014Cu: // CTX_SWITCH1: the class of the object that trapped
        return (reg_get(dev, offset) & ~0xFFu) | dev->pgraph_trapped_class;
    case WL_NV2A_PGRAPH_STATUS:
        return 0;
    case 0x400704u: // TRAPPED_ADDR
        return dev->pgraph_trapped_addr;
    case 0x400708u: // TRAPPED_DATA_LOW
        return dev->pgraph_trapped_data;
    case WL_NV2A_PGRAPH_FIFO:
        return dev->pgraph_fifo;
    case 0x401A88u: // PARAMETER_A: the 3D class's 0x1D8C [pbkit]
        return wl_nv2a_k(&dev->kelvin, WL_NV2A_K_SET_ZSTENCIL_CLEAR_VALUE);
    case 0x40186Cu: // PARAMETER_B: its 0x1D90 [pbkit]
        return wl_nv2a_k(&dev->kelvin, WL_NV2A_K_SET_COLOR_CLEAR_VALUE);
    case WL_NV2A_PCRTC_INTR_0:
        return dev->pcrtc_intr;
    case WL_NV2A_PCRTC_INTR_EN_0:
        return dev->pcrtc_intr_en;
    case WL_NV2A_PCRTC_START:
        return dev->pcrtc_start;
    case WL_NV2A_PCRTC_RASTER:
        // (a line that moves, so a wait for a line ends)
        dev->raster_reads++;
        return (dev->raster_reads * 7u) % 525u;
    default:
        return reg_get(dev, offset);
    }
}

static void write32(WlNv2a *dev, uint32_t offset, uint32_t value) {
    switch (offset) {
    case WL_NV2A_PMC_INTR_0:
        dev->pmc_soft = value & 0x80000000u;
        break;
    case WL_NV2A_PMC_INTR_EN_0:
        dev->pmc_intr_en = value;
        break;
    case WL_NV2A_PFIFO_INTR_0:
        dev->pfifo_intr &= ~value;
        break;
    case WL_NV2A_PFIFO_INTR_EN_0:
        dev->pfifo_intr_en = value;
        break;
    case WL_NV2A_PFIFO_RAMHT:
        dev->pfifo_ramht = value;
        break;
    case WL_NV2A_PFIFO_CACHE1_PUSH1:
        dev->pfifo_cache1_push1 = value;
        break;
    case WL_NV2A_PFIFO_CACHE1_DMA_INSTANCE:
        dev->pfifo_dma_instance = value;
        break;
    case WL_NV2A_PFIFO_CACHE1_DMA_PUT:
        cache1_channel(dev)->put = value & 0x1FFFFFFCu;
        break;
    case WL_NV2A_PFIFO_CACHE1_DMA_GET:
        cache1_channel(dev)->get = value & 0x1FFFFFFCu;
        break;
    case WL_NV2A_PFIFO_CACHE1_REF:
        cache1_channel(dev)->ref = value;
        break;
    case WL_NV2A_PFIFO_CACHE1_DMA_SUBROUTINE: {
        WlNv2aChannel *ch = cache1_channel(dev);

        ch->subroutine_return = value & 0x0FFFFFFCu;
        ch->subroutine_active = (uint8_t)(value & 1u);
        break;
    }
    case WL_NV2A_PTIMER_INTR_0:
        dev->ptimer_intr &= ~value;
        break;
    case WL_NV2A_PTIMER_INTR_EN_0:
        dev->ptimer_intr_en = value;
        break;
    case WL_NV2A_PTIMER_NUMERATOR:
        dev->ptimer_numerator = value;
        break;
    case WL_NV2A_PTIMER_DENOMINATOR:
        dev->ptimer_denominator = value;
        break;
    case WL_NV2A_PTIMER_ALARM_0:
        dev->ptimer_alarm = value;
        break;
    case WL_NV2A_PTIMER_TIME_0:
        dev->time_ns = (dev->time_ns & 0xFFFFFFFF00000000ull) | (value & ~0x1Fu);
        break;
    case WL_NV2A_PTIMER_TIME_1:
        dev->time_ns = (dev->time_ns & 0xFFFFFFFFull) | (uint64_t)(value & 0x1FFFFFFFu) << 32;
        break;
    case WL_NV2A_PFB_WBC:
        // the flush is done at once (everything the title wrote is in memory already)
        dev->pfb_wbc = value & ~(uint32_t)WL_NV2A_PFB_WBC_FLUSH;
        break;
    case WL_NV2A_PGRAPH_INTR:
        dev->pgraph_intr &= ~value;
        if (value & WL_NV2A_PGRAPH_INTR_ERROR) {
            dev->pgraph_nsource = 0;
        }
        resume_trapped(dev);
        break;
    case WL_NV2A_PGRAPH_NSOURCE:
        dev->pgraph_nsource &= ~value;
        break;
    case WL_NV2A_PGRAPH_INTR_EN:
        dev->pgraph_intr_en = value;
        break;
    case WL_NV2A_PGRAPH_FIFO:
        dev->pgraph_fifo = value;
        break;
    case WL_NV2A_PCRTC_INTR_0:
        dev->pcrtc_intr &= ~value;
        break;
    case WL_NV2A_PCRTC_INTR_EN_0:
        dev->pcrtc_intr_en = value;
        break;
    case WL_NV2A_PCRTC_START:
        dev->pcrtc_start = value;
        dev->stats.flips++;
        if (dev->has_backend && dev->backend.flip != NULL) {
            dev->backend.flip(dev->backend.user, &dev->kelvin, value & dev->ram_mask);
        }
        // (a capture notes the flip after the write itself: wl_nv2a_mmio_write)
        dev->capture_flip = true;
        break;
    default:
        reg_put(dev, offset, value);
        break;
    }
}

static uint32_t lane_mask(unsigned size, unsigned shift) {
    uint32_t mask = size >= 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu;

    return mask << shift;
}

uint32_t wl_nv2a_mmio_read(WlNv2a *dev, uint32_t offset, unsigned size) {
    unsigned shift = (offset & 3u) * 8u;
    uint32_t word;

    offset &= WL_NV2A_MMIO_SIZE - 1u;
    if (size != 1 && size != 2) {
        size = 4;
    }
    if (offset >= WL_NV2A_PRAMIN && offset < WL_NV2A_PRAMIN + WL_NV2A_PRAMIN_SIZE) {
        word = wl_nv2a_ramin_read32(dev, offset - WL_NV2A_PRAMIN);
        return (word >> shift) & lane_mask(size, 0);
    }
    if (offset >= WL_NV2A_USER) {
        unsigned chid = (offset - WL_NV2A_USER) / WL_NV2A_USER_STRIDE;
        WlNv2aChannel *ch;

        if (chid >= WL_NV2A_CHANNELS) {
            return 0;
        }
        ch = &dev->channels[chid];
        switch (offset & 0x1FFCu) {
        case WL_NV2A_USER_DMA_PUT:
            word = ch->put;
            break;
        case WL_NV2A_USER_DMA_GET:
            word = ch->get;
            break;
        case WL_NV2A_USER_REF:
            word = ch->ref;
            break;
        default:
            word = 0;
            break;
        }
        return (word >> shift) & lane_mask(size, 0);
    }
    switch (offset) {
    case WL_NV2A_PRMCIO_CRX:
        return dev->crtc_index;
    case WL_NV2A_PRMCIO_CR:
        return dev->crtc[dev->crtc_index];
    case WL_NV2A_PRMCIO_INP0:
        // the retrace and display-enable bits change from read to read, so a wait for either ends
        dev->status_reads++;
        return (dev->status_reads & 2u) ? 0x09u : 0x00u;
    default:
        break;
    }
    word = read32(dev, offset & ~3u);
    return (word >> shift) & lane_mask(size, 0);
}

static void mmio_write(WlNv2a *dev, uint32_t offset, uint32_t value, unsigned size);

void wl_nv2a_mmio_write(WlNv2a *dev, uint32_t offset, uint32_t value, unsigned size) {
    bool capturing;

    offset &= WL_NV2A_MMIO_SIZE - 1u;
    if (size != 1 && size != 2) {
        size = 4;
    }
    // a capture records the write after it ran: the memory it read comes before it in the file
    capturing = dev->capture != NULL && wl_nv2a_capture_running(dev);
    dev->capture_flip = false;
    mmio_write(dev, offset, value, size);
    if (dev->capture != NULL) {
        if (capturing) {
            wl_nv2a_capture_note_mmio(dev, offset, value, size);
        }
        if (dev->capture_flip) {
            wl_nv2a_capture_note_flip(dev);
        }
    }
    dev->capture_flip = false;
}

static void mmio_write(WlNv2a *dev, uint32_t offset, uint32_t value, unsigned size) {
    unsigned shift = (offset & 3u) * 8u;
    uint32_t mask;

    if (offset >= WL_NV2A_PRAMIN && offset < WL_NV2A_PRAMIN + WL_NV2A_PRAMIN_SIZE) {
        uint32_t at = offset - WL_NV2A_PRAMIN;
        uint8_t *page = wl_nv2a_ramin_page(dev, at);

        if (page != NULL) {
            uint8_t *p = page + at % WL_NV2A_RAMIN_PAGE;

            if (size == 4 && (at % WL_NV2A_RAMIN_PAGE) + 4u <= WL_NV2A_RAMIN_PAGE) {
                wl_wr_le32(p, value);
            } else if (size == 2 && (at % WL_NV2A_RAMIN_PAGE) + 2u <= WL_NV2A_RAMIN_PAGE) {
                wl_wr_le16(p, (uint16_t)value);
            } else {
                p[0] = (uint8_t)value;
            }
        }
        return;
    }
    if (offset >= WL_NV2A_USER) {
        unsigned chid = (offset - WL_NV2A_USER) / WL_NV2A_USER_STRIDE;
        WlNv2aChannel *ch;

        if (chid >= WL_NV2A_CHANNELS || size != 4) {
            return;
        }
        ch = &dev->channels[chid];
        if ((offset & 0x1FFCu) == WL_NV2A_USER_DMA_PUT) {
            ch->put = value & 0x1FFFFFFCu;
            run_channel(dev, chid);
        }
        return;
    }
    switch (offset) {
    case WL_NV2A_PRMCIO_CRX:
        dev->crtc_index = (uint8_t)value;
        return;
    case WL_NV2A_PRMCIO_CR:
        dev->crtc[dev->crtc_index] = (uint8_t)value;
        return;
    default:
        break;
    }
    mask = lane_mask(size, shift);
    if (mask != 0xFFFFFFFFu) {
        // a part of a register: the rest as it reads (the status registers' clear-by-one bits
        // are cleared only where this part has ones)
        uint32_t old = 0;

        switch (offset & ~3u) {
        case WL_NV2A_PFIFO_INTR_0:
        case WL_NV2A_PTIMER_INTR_0:
        case WL_NV2A_PGRAPH_INTR:
        case WL_NV2A_PGRAPH_NSOURCE:
        case WL_NV2A_PCRTC_INTR_0:
            break;
        default:
            old = read32(dev, offset & ~3u) & ~mask;
            break;
        }
        value = old | ((value << shift) & mask);
    }
    write32(dev, offset & ~3u, value);
}
