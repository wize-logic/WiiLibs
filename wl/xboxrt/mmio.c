// SPDX-License-Identifier: MIT
// Device registry and dispatch (mmio.h).
#include <stdio.h>
#include <string.h>

#include "wl/xboxrt/crash.h"

#include "wl/xboxrt/mem_internal.h"
#include "wl/xboxrt/mmio.h"
#include "wl/xboxrt/thread.h"
#include "wl/base/base.h"

static WlXbDevice g_devices[WL_XB_MAX_DEVICES];
static uint32_t g_count;
static WlXbMmioStats g_stats;
uint32_t g_xb_mmio_depth;
uint32_t g_xb_mmio_last_write;
static uintptr_t g_fault_pc;
// The accesses by register (a 32-bit word) and by guest site (the faulting host PC and the
// register): where a title keeps going back to, each a fault on the Wii. A small open-addressed
// table each, the first comers kept; a status line every 10 s (wl_xb_mmio_log_window) gives the
// window's hottest a frame, the exit's report the whole run's. The host's only (on the Wii the
// handler does not note its PC, and the table would add to the console's faults).
#if !defined(__wii__)
#define HOT_SLOTS 2048u
#define SITE_SLOTS 2048u
typedef struct {
    uint32_t key; // device index << 24 | (offset >> 2) << 1 | write, + 1 (0: free)
    uint32_t count, last; // last: the count at the window's start
} HotSlot;
static HotSlot g_hot[HOT_SLOTS];
static struct {
    uintptr_t pc;
    uint32_t key; // as g_hot's
    uint32_t count, last;
} g_site[SITE_SLOTS];

static uint32_t hot_key(const WlXbDevice *d, uint32_t off, uint32_t write) {
    return ((uint32_t)(d - g_devices) << 24 | (off >> 2 & 0x3FFFFFu) << 1 | write) + 1u;
}
#endif
static uint32_t g_window_flips;

// The mirrored pages (mmio.h): their VA and the runtime's alias of their bytes (little-endian
// words), published by the count after the entry is whole (the Wii's DSI handler reads them).
typedef struct {
    uint32_t va;
    uint8_t *rw;
    const WlXbMmioFastStore *stores;
    uint32_t n_stores;
} Mirror;
static Mirror g_mirror[WL_XB_MIRROR_PAGES];
static volatile uint32_t g_mirror_n;
static bool g_mirrors_off, g_mirror_check;

static inline const Mirror *mirror_of(uint32_t va) {
    uint32_t page = va & ~(WL_XB_PAGE - 1u);
    for (uint32_t i = 0, n = g_mirror_n; i < n; i++) {
        if (g_mirror[i].va == page) {
            return &g_mirror[i];
        }
    }
    return NULL;
}

static inline uint32_t mirror_get(const Mirror *m, uint32_t off, unsigned size) {
    const uint8_t *b = m->rw + off;
    uint32_t v = b[0];
    if (size >= 2) {
        v |= (uint32_t)b[1] << 8;
    }
    if (size == 4) {
        v |= (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    }
    return v;
}

static inline void mirror_put(const Mirror *m, uint32_t off, uint32_t v) {
    uint8_t *b = m->rw + (off & ~3u);
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
}

// After the device's write at `va` (`size` bytes): the words it covers read again into the page.
static void mirror_written(const WlXbDevice *d, uint32_t va, unsigned size) {
    for (uint32_t w = va & ~3u; w < va + size; w += 4u) {
        const Mirror *m = mirror_of(w);
        if (m != NULL) {
            mirror_put(m, w & (WL_XB_PAGE - 1u), d->read(d->user, w - d->base, 4));
        }
    }
}

static void note(const WlXbDevice *d, uint32_t off, uint32_t write) {
#if !defined(__wii__)
    uint32_t key = hot_key(d, off, write);
    uint32_t h = (key * 2654435761u) >> 21;
    // (a full probe run gives its least used slot to the newcomer: a boot's thousands of words
    // written once, the NV2A's instance memory, do not keep the hot registers out)
    HotSlot *least = NULL;
    for (uint32_t i = 0; i < 16u; i++) {
        HotSlot *e = &g_hot[(h + i) % HOT_SLOTS];
        if (e->key == key || e->key == 0u) {
            least = e;
            break;
        }
        if (least == NULL || e->count < least->count) {
            least = e;
        }
    }
    if (least->key != key) {
        least->key = key;
        least->count = least->last = 0;
    }
    least->count++;
    uintptr_t pc = g_fault_pc;
    uint32_t hs = ((uint32_t)pc * 2654435761u ^ key * 40503u) >> 21, at = SITE_SLOTS;
    for (uint32_t i = 0; i < 16u; i++) {
        uint32_t j = (hs + i) % SITE_SLOTS;
        if ((g_site[j].pc == pc && g_site[j].key == key) || g_site[j].key == 0u) {
            at = j;
            break;
        }
        if (at == SITE_SLOTS || g_site[j].count < g_site[at].count) {
            at = j;
        }
    }
    if (g_site[at].pc != pc || g_site[at].key != key) {
        g_site[at].pc = pc;
        g_site[at].key = key;
        g_site[at].count = g_site[at].last = 0;
    }
    g_site[at].count++;
#else
    (void)d; // (the host's report: nothing added to the console's faults)
    (void)off;
    (void)write;
#endif
}

WlResult wl_xb_mmio_register(const WlXbDevice *dev) {
    if (dev == NULL || dev->base < WL_XB_MMIO_BASE || dev->size == 0 ||
        (uint64_t)dev->base + dev->size > 0x100000000ull || dev->read == NULL || dev->write == NULL) {
        return WL_ERR_ARG;
    }
    if (g_count == WL_XB_MAX_DEVICES) {
        return WL_ERR_LIMIT;
    }
    // newest first, so a later registration over a range wins
    memmove(&g_devices[1], &g_devices[0], g_count * sizeof g_devices[0]);
    g_devices[0] = *dev;
    g_count++;
    return WL_OK;
}

void wl_xb_mmio_reset(void) {
    g_count = 0;
    g_mirror_n = 0;
    memset(&g_stats, 0, sizeof g_stats);
}

const WlXbDevice *wl_xb_mmio_find(uint32_t va) {
    for (uint32_t i = 0; i < g_count; i++) {
        if (va - g_devices[i].base < g_devices[i].size) {
            return &g_devices[i];
        }
    }
    return NULL;
}

bool wl_xb_mmio_read(uint32_t va, unsigned size, uint64_t *value) {
    const WlXbDevice *d = wl_xb_mmio_find(va);
    if (d == NULL) {
        g_stats.unclaimed++;
        return false;
    }
    g_stats.reads++;
    g_xb_mmio_last_write = 0;
    note(d, va - d->base, 0u);
    g_xb_mmio_depth++;
    if (size == 8) {
        uint64_t lo = d->read(d->user, va - d->base, 4);
        uint64_t hi = d->read(d->user, va + 4u - d->base, 4);
        *value = (lo & 0xFFFFFFFFu) | (hi << 32);
    } else {
        uint32_t v = d->read(d->user, va - d->base, size);
        *value = size == 4 ? v : size == 2 ? (v & 0xFFFFu) : (v & 0xFFu);
    }
    g_xb_mmio_depth--;
    if (g_mirror_n != 0) {
        const Mirror *m = mirror_of(va);
        if (m != NULL) {
            // (the check: what the page holds against what the device said)
            unsigned n = size == 8 ? 4u : size;
            uint32_t want = (uint32_t)*value & (n == 4 ? 0xFFFFFFFFu : n == 2 ? 0xFFFFu : 0xFFu);
            g_stats.check_reads++;
            if (mirror_get(m, va & (WL_XB_PAGE - 1u), n) != want) {
                if (g_stats.check_misses++ < 8u) {
                    WL_LOG_W("xbmmio", "mirror check: %s+0x%x read 0x%x, the page holds 0x%x", d->name,
                             (unsigned)(va - d->base), (unsigned)want, (unsigned)mirror_get(m, va & (WL_XB_PAGE - 1u), n));
                }
            }
        }
    }
    return true;
}

bool wl_xb_mmio_write(uint32_t va, uint64_t value, unsigned size) {
    const WlXbDevice *d = wl_xb_mmio_find(va);
    if (d == NULL) {
        g_stats.unclaimed++;
        return false;
    }
    g_stats.writes++;
    g_xb_mmio_last_write = 1;
    note(d, va - d->base, 1u);
    g_xb_mmio_depth++;
    if (size == 8) {
        d->write(d->user, va - d->base, (uint32_t)value, 4);
        d->write(d->user, va + 4u - d->base, (uint32_t)(value >> 32), 4);
    } else {
        uint32_t mask = size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu;
        d->write(d->user, va - d->base, (uint32_t)value & mask, size);
    }
    g_xb_mmio_depth--;
    if (g_mirror_n != 0) {
        mirror_written(d, va, size);
    }
    return true;
}

void wl_xb_mmio_set_mirrors(bool on, bool check) {
    g_mirrors_off = !on;
    g_mirror_check = check;
}

WlResult wl_xb_mmio_mirror(uint32_t va, const WlXbMmioFastStore *stores, uint32_t n_stores) {
    if (g_mirrors_off || (wl_xb_deterministic() && !g_mirror_check)) {
        return WL_OK;
    }
    const WlXbDevice *d = wl_xb_mmio_find(va);
    if ((va & (WL_XB_PAGE - 1u)) != 0 || d == NULL || va + WL_XB_PAGE - d->base > d->size || mirror_of(va) != NULL) {
        return WL_ERR_ARG;
    }
    if (g_mirror_n == WL_XB_MIRROR_PAGES) {
        return WL_ERR_LIMIT;
    }
    uint8_t *rw = wl_xb_backend_mirror(WL_XB_EA(va), !g_mirror_check);
    if (rw == NULL) {
        return WL_ERR_UNSUPPORTED;
    }
    Mirror *m = &g_mirror[g_mirror_n];
    m->va = va;
    m->rw = rw;
    m->stores = stores;
    m->n_stores = n_stores;
    for (uint32_t off = 0; off < WL_XB_PAGE; off += 4u) {
        mirror_put(m, off, d->read(d->user, va + off - d->base, 4));
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    g_mirror_n++;
    WL_LOG_I("xbmmio", "%s+0x%x..+0x%x mirrored: %s", d->name, (unsigned)(va - d->base), (unsigned)(va - d->base + WL_XB_PAGE - 1u),
             g_mirror_check ? "the check (every read still the device's, compared)" : "reads from memory");
    return WL_OK;
}

void wl_xb_mmio_mirror_refresh(uint32_t va) {
    const WlXbDevice *d = g_mirror_n != 0 ? wl_xb_mmio_find(va) : NULL;
    if (d != NULL) {
        mirror_written(d, va & ~3u, 4u);
    }
}

bool wl_xb_mmio_fast_read(uint32_t va, unsigned size, uint32_t *value) {
    if (g_mirror_n == 0 || g_mirror_check || g_xb_attention != 0 || (size != 1 && size != 2 && size != 4) ||
        (va & (size - 1u)) != 0) {
        return false;
    }
    const Mirror *m = mirror_of(va);
    if (m == NULL) {
        return false;
    }
    *value = mirror_get(m, va & (WL_XB_PAGE - 1u), size);
    g_stats.fast_reads++;
    return true;
}

bool wl_xb_mmio_fast_write(uint32_t va, uint32_t value, unsigned size) {
    if (g_mirror_n == 0 || g_mirror_check || g_xb_attention != 0 || size != 4 || (va & 3u) != 0) {
        return false;
    }
    const Mirror *m = mirror_of(va);
    if (m == NULL) {
        return false;
    }
    uint32_t off = va & (WL_XB_PAGE - 1u);
    for (uint32_t i = 0; i < m->n_stores; i++) {
        const WlXbMmioFastStore *st = &m->stores[i];
        if (st->offset == off) {
            uint32_t kept = value & st->keep;
            *st->model = kept;
            mirror_put(m, off, kept);
            g_stats.fast_writes++;
            return true;
        }
    }
    return false;
}

void wl_xb_mmio_set_fault_pc(uintptr_t pc) {
    g_fault_pc = pc;
}

uintptr_t wl_xb_mmio_fault_pc(void) {
    return g_fault_pc;
}

void wl_xb_mmio_stats(WlXbMmioStats *out) {
    *out = g_stats;
}

#if !defined(__wii__)
static void name_key(uint32_t key, char *out, size_t cap) {
    key -= 1u;
    uint32_t dev = key >> 24;
    snprintf(out, cap, "%s+0x%06x %s", dev < g_count ? g_devices[dev].name : "?", (unsigned)(((key >> 1) & 0x3FFFFFu) << 2),
             (key & 1u) ? "written" : "read");
}
#endif

void wl_xb_mmio_log_hot(const char *tag, uint32_t top) {
#if defined(__wii__)
    (void)tag;
    (void)top;
#else
    static bool used[HOT_SLOTS];
    memset(used, 0, sizeof used);
    for (uint32_t n = 0; n < top; n++) {
        uint32_t best = HOT_SLOTS;
        for (uint32_t i = 0; i < HOT_SLOTS; i++) {
            if (!used[i] && g_hot[i].key != 0u && (best == HOT_SLOTS || g_hot[i].count > g_hot[best].count)) {
                best = i;
            }
        }
        if (best == HOT_SLOTS) {
            break;
        }
        used[best] = true;
        char name[48];
        name_key(g_hot[best].key, name, sizeof name);
        WL_LOG_I(tag, "device accesses: %s %u times", name, (unsigned)g_hot[best].count);
    }
#endif
}

void wl_xb_mmio_log_window(const char *tag, uint32_t flips, uint32_t top) {
    uint32_t frames = flips - g_window_flips;
    g_window_flips = flips;
    WlXbMmioStats st = g_stats;
    static WlXbMmioStats last;
    uint32_t total = st.reads + st.writes - last.reads - last.writes;
    uint32_t fast = st.fast_reads + st.fast_writes - last.fast_reads - last.fast_writes;
    WL_LOG_I(tag, "status: device faults %u in %u frames (%u.%u a frame: %u reads, %u writes to the devices; %u.%u a frame "
             "answered from a mirror: %u reads, %u writes)",
             (unsigned)(total + fast), (unsigned)frames, (unsigned)((total + fast) / (frames ? frames : 1u)),
             (unsigned)((total + fast) * 10u / (frames ? frames : 1u) % 10u), (unsigned)(st.reads - last.reads),
             (unsigned)(st.writes - last.writes), (unsigned)(fast / (frames ? frames : 1u)),
             (unsigned)(fast * 10u / (frames ? frames : 1u) % 10u), (unsigned)(st.fast_reads - last.fast_reads),
             (unsigned)(st.fast_writes - last.fast_writes));
    if (st.check_reads != last.check_reads) {
        WL_LOG_I(tag, "status: mirror check %u reads, %u differed (%u in the run)", (unsigned)(st.check_reads - last.check_reads),
                 (unsigned)(st.check_misses - last.check_misses), (unsigned)st.check_misses);
    }
    last = st;
#if !defined(__wii__)
    static bool used[SITE_SLOTS];
    uint32_t div = frames != 0 ? frames : 1u;
    memset(used, 0, sizeof used);
    for (uint32_t n = 0; n < top; n++) {
        uint32_t best = HOT_SLOTS;
        for (uint32_t i = 0; i < HOT_SLOTS; i++) {
            if (!used[i] && g_hot[i].count != g_hot[i].last &&
                (best == HOT_SLOTS || g_hot[i].count - g_hot[i].last > g_hot[best].count - g_hot[best].last)) {
                best = i;
            }
        }
        if (best == HOT_SLOTS) {
            break;
        }
        used[best] = true;
        char name[48];
        uint32_t c = g_hot[best].count - g_hot[best].last;
        name_key(g_hot[best].key, name, sizeof name);
        WL_LOG_I(tag, "status: device register %s %u.%u a frame", name, (unsigned)(c / div), (unsigned)(c * 10u / div % 10u));
    }
    memset(used, 0, sizeof used);
    for (uint32_t n = 0; n < top; n++) {
        uint32_t best = SITE_SLOTS;
        for (uint32_t i = 0; i < SITE_SLOTS; i++) {
            if (!used[i] && g_site[i].count != g_site[i].last &&
                (best == SITE_SLOTS || g_site[i].count - g_site[i].last > g_site[best].count - g_site[best].last)) {
                best = i;
            }
        }
        if (best == SITE_SLOTS) {
            break;
        }
        used[best] = true;
        char name[48];
        uint32_t c = g_site[best].count - g_site[best].last;
        name_key(g_site[best].key, name, sizeof name);
        WlXbSymbol sym;
        if (g_site[best].pc != 0 && wl_xb_symbolize(g_site[best].pc, &sym)) {
            WL_LOG_I(tag, "status: device site sub_%08X+0x%x %s %u.%u a frame", (unsigned)sym.guest_va,
                     (unsigned)(g_site[best].pc - sym.host), name, (unsigned)(c / div), (unsigned)(c * 10u / div % 10u));
        } else {
            WL_LOG_I(tag, "status: device site host %p %s %u.%u a frame", (void *)g_site[best].pc, name, (unsigned)(c / div),
                     (unsigned)(c * 10u / div % 10u));
        }
    }
    for (uint32_t i = 0; i < HOT_SLOTS; i++) {
        g_hot[i].last = g_hot[i].count;
    }
    for (uint32_t i = 0; i < SITE_SLOTS; i++) {
        g_site[i].last = g_site[i].count;
    }
#else
    (void)top;
#endif
}
