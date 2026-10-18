// SPDX-License-Identifier: MIT
// The spill-over heap's portable core (see heap_overflow.h).
#include "wl/platform/heap_overflow.h"
#include "wl/platform/mem_census.h"

#include <string.h>

#define LOG_TAG "heap"
#define HEADER_SIZE 32u
#define BLOCK_ALIGN 32u
#define MAGIC_LIVE 0x4F564642u  // "OVFB"
#define MAGIC_FREED 0x4F564644u // "OVFD"
// Sizes and alignments the header's 32-bit fields and the additions below can hold.
#define LIMIT ((size_t)1 << 30)

typedef struct {
    uint32_t magic;
    uint32_t offset;   // from the block's start (what wl_mem returned) to the user's pointer
    uint32_t size;     // bytes the caller asked for (the latest realloc's)
    uint32_t capacity; // bytes from the user's pointer to the block's end
    uint8_t reserved[16];
} Header;

WL_STATIC_ASSERT(sizeof(Header) == HEADER_SIZE, "the header is 32 bytes");

static bool is_pow2(size_t v) {
    return v != 0u && (v & (v - 1u)) == 0u;
}

WlResult wl_overflow_init(WlOverflow *o, WlMemTag tag) {
    if (o == NULL || tag >= WL_MEM_MAX_TAGS) {
        return WL_ERR_ARG;
    }
    if (o->armed) {
        return WL_ERR_STATE;
    }
    const void *base;
    size_t size;
    if (!wl_mem_tag_range(tag, &base, &size)) {
        WL_LOG_E(LOG_TAG, "overflow: tag %u has no heap (wl_mem not initialised, or budget 0)",
                 (unsigned)tag);
        return WL_ERR_STATE;
    }
    memset(&o->stats, 0, sizeof o->stats);
    o->tag = tag;
    o->lo = (uintptr_t)base;
    o->hi = (uintptr_t)base + size;
    o->announced = false;
    o->armed = true; // last: a malloc on another thread sees a complete state
    return WL_OK;
}

void wl_overflow_stop(WlOverflow *o) {
    o->armed = false;
    o->lo = o->hi = 0;
}

void wl_overflow_set_fail_hook(WlOverflow *o, WlOverflowFailFn fn, void *user) {
    o->on_fail = fn;
    o->fail_user = user;
}

bool wl_overflow_owns(const WlOverflow *o, const void *ptr) {
    uintptr_t a = (uintptr_t)ptr;
    return o->armed && a >= o->lo && a < o->hi;
}

// The header of a live overflow block, or NULL (the pointer is in the range but is not one).
static Header *header_of(const WlOverflow *o, void *ptr) {
    uintptr_t a = (uintptr_t)ptr;
    if (!wl_overflow_owns(o, ptr) || (a & (BLOCK_ALIGN - 1u)) != 0u || a - o->lo < HEADER_SIZE) {
        return NULL;
    }
    Header *h = (Header *)(a - HEADER_SIZE);
    if (h->magic != MAGIC_LIVE || h->offset < HEADER_SIZE || h->offset > a - o->lo ||
        ((a - h->offset) & (BLOCK_ALIGN - 1u)) != 0u) {
        return NULL;
    }
    return h;
}

// The primary's answer, asked again while a reclaimer gives memory back to the heap (mem_census.h):
// memory the program keeps for itself when idle comes before the overflow heap.
static void *primary_alloc(const WlOverflowPrimary *p, size_t align, size_t size) {
    void *ptr = p->alloc(p->ctx, align, size);
    while (ptr == NULL && size != 0u && wl_platform_heap_reclaim(size + align) != 0u) {
        ptr = p->alloc(p->ctx, align, size);
    }
    return ptr;
}

// ---- the overflow heap ------------------------------------------------------------------------

void *wl_overflow_alloc(WlOverflow *o, size_t align, size_t size) {
    if (!o->armed || !wl_mem_ready()) {
        return NULL;
    }
    if (align == 0u) {
        align = 1u;
    }
    if (!is_pow2(align) || align > LIMIT || size > LIMIT) {
        return NULL;
    }
    size_t slack = align > BLOCK_ALIGN ? align : 0u; // room to move the pointer up to its alignment
    size_t total = HEADER_SIZE + size + slack;
    uint8_t *block = (uint8_t *)wl_mem_alloc_at(o->tag, total, BLOCK_ALIGN, __FILE__, __LINE__);
    if (block == NULL) {
        return NULL;
    }
    uintptr_t user = (uintptr_t)block + HEADER_SIZE;
    if (align > BLOCK_ALIGN) {
        user = (user + (align - 1u)) & ~(uintptr_t)(align - 1u);
    }
    Header *h = (Header *)(user - HEADER_SIZE);
    memset(h, 0, sizeof *h);
    h->magic = MAGIC_LIVE;
    h->offset = (uint32_t)(user - (uintptr_t)block);
    h->size = (uint32_t)size;
    h->capacity = (uint32_t)(total - (user - (uintptr_t)block));

    wl_platform_mem_lock();
    bool first = !o->announced;
    o->announced = true;
    o->stats.live++;
    o->stats.allocs++;
    o->stats.bytes += size;
    if (o->stats.bytes > o->stats.peak_bytes) {
        o->stats.peak_bytes = o->stats.bytes;
    }
    wl_platform_mem_unlock();
    if (first) {
        WL_LOG_W(LOG_TAG, "the primary heap is full: allocations go to the %s heap now",
                 wl_mem_tag_name(o->tag));
    }
    return (void *)user;
}

bool wl_overflow_free_owned(WlOverflow *o, void *ptr) {
    if (!wl_overflow_owns(o, ptr)) {
        return false;
    }
    Header *h = header_of(o, ptr);
    if (h == NULL) {
        wl_platform_mem_lock();
        o->stats.bad_frees++;
        wl_platform_mem_unlock();
        WL_LOG_E(LOG_TAG, "free of %p: in the %s heap but not a live block (double free?)", ptr,
                 wl_mem_tag_name(o->tag));
        return true;
    }
    void *block = (uint8_t *)ptr - h->offset;
    size_t size = h->size;
    h->magic = MAGIC_FREED;
    wl_platform_mem_lock();
    o->stats.live--;
    o->stats.frees++;
    o->stats.bytes -= size;
    wl_platform_mem_unlock();
    wl_mem_free(block);
    return true;
}

void wl_overflow_failed(WlOverflow *o, const char *what, size_t size, size_t align) {
    wl_platform_mem_lock();
    o->stats.fails++;
    bool report = !o->failing;
    o->failing = true; // (the report may allocate, and fail, and must not report that)
    wl_platform_mem_unlock();
    if (!report) {
        return;
    }
    if (o->on_fail != NULL) {
        o->on_fail(o->fail_user, what, size, align);
    } else {
        WL_LOG_E(LOG_TAG, "%s(%zu) failed: %s", what, size,
                 o->armed ? "the primary heap and the overflow heap are full"
                          : "the primary heap is full");
    }
    wl_platform_mem_lock();
    o->failing = false;
    wl_platform_mem_unlock();
}

// ---- the entry points -------------------------------------------------------------------------

void *wl_overflow_malloc(WlOverflow *o, const WlOverflowPrimary *p, size_t size) {
    void *ptr = primary_alloc(p, 0u, size);
    if (ptr == NULL && size != 0u) {
        ptr = wl_overflow_alloc(o, 0u, size);
        if (ptr == NULL) {
            wl_overflow_failed(o, "malloc", size, 0u);
        }
    }
    return ptr;
}

void *wl_overflow_calloc(WlOverflow *o, const WlOverflowPrimary *p, size_t count, size_t size) {
    if (count != 0u && size > SIZE_MAX / count) {
        return NULL;
    }
    size_t total = count * size;
    void *ptr;
    if (p->calloc != NULL) {
        ptr = p->calloc(p->ctx, count, size);
        while (ptr == NULL && total != 0u && wl_platform_heap_reclaim(total) != 0u) {
            ptr = p->calloc(p->ctx, count, size);
        }
    } else {
        ptr = primary_alloc(p, 0u, total);
        if (ptr != NULL) {
            memset(ptr, 0, total);
        }
    }
    if (ptr == NULL && total != 0u) {
        ptr = wl_overflow_alloc(o, 0u, total);
        if (ptr != NULL) {
            memset(ptr, 0, total);
        } else {
            wl_overflow_failed(o, "calloc", total, 0u);
        }
    }
    return ptr;
}

void *wl_overflow_memalign(WlOverflow *o, const WlOverflowPrimary *p, size_t align, size_t size) {
    if (align == 0u) {
        return wl_overflow_malloc(o, p, size);
    }
    void *ptr = primary_alloc(p, align, size);
    if (ptr == NULL && size != 0u) {
        ptr = wl_overflow_alloc(o, align, size);
        if (ptr == NULL) {
            wl_overflow_failed(o, "memalign", size, align);
        }
    }
    return ptr;
}

void wl_overflow_free(WlOverflow *o, const WlOverflowPrimary *p, void *ptr) {
    if (ptr == NULL || wl_overflow_free_owned(o, ptr)) {
        return;
    }
    p->free(p->ctx, ptr);
}

size_t wl_overflow_usable_size(const WlOverflow *o, const WlOverflowPrimary *p, void *ptr) {
    if (ptr == NULL) {
        return 0u;
    }
    if (wl_overflow_owns(o, ptr)) {
        const Header *h = header_of(o, ptr);
        return h != NULL ? h->capacity : 0u;
    }
    return p != NULL ? p->usable_size(p->ctx, ptr) : 0u;
}

void *wl_overflow_realloc(WlOverflow *o, const WlOverflowPrimary *p, void *ptr, size_t size) {
    if (ptr == NULL) {
        return wl_overflow_malloc(o, p, size);
    }
    if (size == 0u) {
        wl_overflow_free(o, p, ptr);
        return NULL;
    }
    size_t old_size;
    void *moved;
    if (wl_overflow_owns(o, ptr)) {
        Header *h = header_of(o, ptr);
        if (h == NULL) {
            wl_platform_mem_lock();
            o->stats.bad_frees++;
            wl_platform_mem_unlock();
            WL_LOG_E(LOG_TAG, "realloc of %p: in the %s heap but not a live block", ptr,
                     wl_mem_tag_name(o->tag));
            return NULL;
        }
        if (size <= h->capacity) { // fits: the same pointer, whichever way the size goes
            wl_platform_mem_lock();
            o->stats.bytes = o->stats.bytes - h->size + size;
            if (o->stats.bytes > o->stats.peak_bytes) {
                o->stats.peak_bytes = o->stats.bytes;
            }
            wl_platform_mem_unlock();
            h->size = (uint32_t)size;
            return ptr;
        }
        old_size = h->size;
        // it must grow: the primary heap first (memory freed there is used again), then this one
        moved = primary_alloc(p, 0u, size);
        if (moved == NULL) {
            moved = wl_overflow_alloc(o, 0u, size);
        }
        if (moved == NULL) {
            wl_overflow_failed(o, "realloc", size, 0u);
            return NULL;
        }
        memcpy(moved, ptr, old_size < size ? old_size : size);
        (void)wl_overflow_free_owned(o, ptr);
        wl_platform_mem_lock();
        o->stats.moved_out++;
        wl_platform_mem_unlock();
        return moved;
    }
    if (p->realloc != NULL) {
        moved = p->realloc(p->ctx, ptr, size);
        while (moved == NULL && wl_platform_heap_reclaim(size) != 0u) {
            moved = p->realloc(p->ctx, ptr, size);
        }
        if (moved != NULL) {
            return moved;
        }
    } else {
        moved = primary_alloc(p, 0u, size);
        if (moved != NULL) {
            size_t have = p->usable_size(p->ctx, ptr);
            memcpy(moved, ptr, have < size ? have : size);
            p->free(p->ctx, ptr);
            return moved;
        }
    }
    // the primary heap cannot hold the new size: the block moves to the overflow heap
    old_size = p->usable_size(p->ctx, ptr);
    moved = wl_overflow_alloc(o, 0u, size);
    if (moved == NULL) {
        wl_overflow_failed(o, "realloc", size, 0u);
        return NULL;
    }
    memcpy(moved, ptr, old_size < size ? old_size : size);
    p->free(p->ctx, ptr);
    wl_platform_mem_lock();
    o->stats.moved_in++;
    wl_platform_mem_unlock();
    return moved;
}

// ---- statistics -------------------------------------------------------------------------------

void wl_overflow_stats(const WlOverflow *o, WlOverflowStats *out) {
    wl_platform_mem_lock();
    *out = o->stats;
    wl_platform_mem_unlock();
}

void wl_overflow_usage(const WlOverflow *o, const WlOverflowPrimary *p, WlOverflowUsage *out) {
    memset(out, 0, sizeof *out);
    if (p != NULL && p->usage != NULL) {
        p->usage(p->ctx, &out->primary_used, &out->primary_free);
        out->primary_known = true;
    }
    out->overflow_armed = o->armed;
    if (o->armed && wl_mem_ready()) {
        WlMemRow rows[WL_MEM_MAX_ROWS];
        uint32_t n = wl_mem_rows(rows, WL_ARRAY_COUNT(rows));
        if (o->tag < n) {
            const WlMemRow *r = &rows[o->tag];
            out->overflow_used = r->used;
            out->overflow_peak = r->peak;
            out->overflow_budget = r->budget;
            out->overflow_free = r->budget > r->used ? r->budget - r->used : 0u;
            out->overflow_live = r->live;
        }
    }
    out->free_total = out->primary_free + out->overflow_free;
}

void wl_overflow_log_usage(const WlOverflow *o, const WlOverflowPrimary *p) {
    WlOverflowUsage u;
    wl_overflow_usage(o, p, &u);
    if (u.primary_known) {
        WL_LOG_I(LOG_TAG, "primary heap %zu KiB used, %zu KiB free", u.primary_used / 1024u,
                 u.primary_free / 1024u);
    }
    if (u.overflow_armed) {
        WL_LOG_I(LOG_TAG,
                 "overflow heap (%s) %zu KiB used of %zu, peak %zu, %u blocks; %zu KiB free in all",
                 wl_mem_tag_name(o->tag), u.overflow_used / 1024u, u.overflow_budget / 1024u,
                 u.overflow_peak / 1024u, (unsigned)u.overflow_live, u.free_total / 1024u);
    }
}
