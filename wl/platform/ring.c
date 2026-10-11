// SPDX-License-Identifier: MIT
// SPSC ring buffer. Contract and memory-ordering notes in ring.h.
#include "wl/platform/ring.h"

#include <string.h>

WlResult wl_ring_init(WlSpscRing *ring, void *storage, uint32_t item_size, uint32_t capacity) {
    if (ring == NULL || storage == NULL || item_size == 0 || item_size > 65536u || capacity < 2u ||
        capacity > (1u << 24) || (capacity & (capacity - 1u)) != 0 ||
        ((uintptr_t)storage & 3u) != 0) {
        return WL_ERR_ARG;
    }
    ring->storage = storage;
    ring->item_size = item_size;
    ring->capacity = capacity;
    ring->mask = capacity - 1u;
    atomic_init(&ring->head, 0u);
    atomic_init(&ring->tail, 0u);
    return WL_OK;
}

bool wl_ring_push(WlSpscRing *ring, const void *item) {
    uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_relaxed); // ours
    uint32_t head = atomic_load_explicit(&ring->head, memory_order_acquire);
    if (tail - head >= ring->capacity) {
        return false;
    }
    memcpy(ring->storage + (size_t)(tail & ring->mask) * ring->item_size, item, ring->item_size);
    atomic_store_explicit(&ring->tail, tail + 1u, memory_order_release); // publish the item
    return true;
}

const void *wl_ring_peek(WlSpscRing *ring) {
    uint32_t head = atomic_load_explicit(&ring->head, memory_order_relaxed); // ours
    uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_acquire);
    if (head == tail) {
        return NULL;
    }
    return ring->storage + (size_t)(head & ring->mask) * ring->item_size;
}

void wl_ring_discard(WlSpscRing *ring) {
    uint32_t head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    WL_ASSERT(head != atomic_load_explicit(&ring->tail, memory_order_relaxed));
    atomic_store_explicit(&ring->head, head + 1u, memory_order_release); // slot is reusable
}

bool wl_ring_pop(WlSpscRing *ring, void *out) {
    const void *p = wl_ring_peek(ring);
    if (p == NULL) {
        return false;
    }
    memcpy(out, p, ring->item_size);
    wl_ring_discard(ring);
    return true;
}

uint32_t wl_ring_count(const WlSpscRing *ring) {
    // Load head first: both only grow, so tail - head cannot underflow past capacity.
    uint32_t head = atomic_load_explicit(&ring->head, memory_order_acquire);
    uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_acquire);
    uint32_t n = tail - head;
    return n > ring->capacity ? ring->capacity : n; // the other side moved between the loads
}

uint32_t wl_ring_free_slots(const WlSpscRing *ring) {
    return ring->capacity - wl_ring_count(ring);
}
