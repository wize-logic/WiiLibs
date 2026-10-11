// SPDX-License-Identifier: MIT
// Single-producer / single-consumer ring buffer: how the main, io,
// audio and net threads talk. Platform-independent; storage is caller-owned.
//
// Contract: exactly one thread calls wl_ring_push (and wl_ring_free_slots), exactly one thread
// calls wl_ring_pop / wl_ring_peek / wl_ring_discard; wl_ring_count is safe from either side
// (it may be stale by the time it returns). Never blocks: a full push and an empty pop return
// false, and the caller decides how to wait (semaphore, see io.h; or yield).
//
// Memory ordering (C11 atomics, no locks):
//   producer: write the item, then store `tail` with release;
//   consumer: load `tail` with acquire, read the item, then store `head` with release;
//   producer: load `head` with acquire before reusing the slot.
// On the Wii, devkitPPC GCC 16 (-mcpu=750) lowers the release store to `lwsync` and the acquire
// load to a load followed by `isync` (checked in the disassembly of ring.o). `lwsync` is encoded
// as `sync` with L=1; the 750-family cores (Broadway) ignore the L field and execute a full
// `sync`, which is stronger than needed, so the ordering holds. Broadway is single-core: the
// barriers order the compiler, interrupt-driven thread switches and DMA; the same code is also
// correct on the multicore host the tests run on (clean under ThreadSanitizer). Not verifiable
// in Dolphin beyond "the instruction decodes".
#pragma once

#include <stdatomic.h>

#include "wl/base/base.h"

typedef struct WlSpscRing {
    uint8_t *storage;
    uint32_t item_size;
    uint32_t capacity; // items, a power of two
    uint32_t mask;
    // Free-running indices (wrap at 2^32; capacity divides 2^32). Each is written by one side
    // only, on its own cache line (32 bytes on the Wii, 64 on common hosts: pad for the larger).
    _Alignas(64) atomic_uint head; // next item to pop (consumer-owned)
    _Alignas(64) atomic_uint tail; // next slot to fill (producer-owned)
} WlSpscRing;

// `capacity` must be a power of two in 2..2^24, `item_size` 1..65536, and `storage` must hold
// capacity * item_size bytes (4-byte aligned at least). Returns WL_ERR_ARG otherwise.
WlResult wl_ring_init(WlSpscRing *ring, void *storage, uint32_t item_size, uint32_t capacity);

// Producer: copies one item in. false when full.
bool wl_ring_push(WlSpscRing *ring, const void *item);
// Consumer: copies the oldest item out. false when empty.
bool wl_ring_pop(WlSpscRing *ring, void *out);
// Consumer: pointer to the oldest item (valid until wl_ring_discard), NULL when empty.
const void *wl_ring_peek(WlSpscRing *ring);
// Consumer: drops the item returned by wl_ring_peek.
void wl_ring_discard(WlSpscRing *ring);

// Items currently queued (either side; a snapshot).
uint32_t wl_ring_count(const WlSpscRing *ring);
// Free slots (producer side; a lower bound, since the consumer may free more at any time).
uint32_t wl_ring_free_slots(const WlSpscRing *ring);
