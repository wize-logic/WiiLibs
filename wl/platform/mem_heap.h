// SPDX-License-Identifier: MIT
// Platform-independent tagged-heap core: first-fit with immediate coalescing over
// a caller-supplied block. Integer only, no globals, no locking (wl/platform/mem.c locks),
// deterministic: the same call sequence always yields the same addresses. Every block carries a
// 32-byte header, so every returned pointer is 32-byte aligned (GPU/DMA safe).
// Free blocks sit on a LIFO free list; the header records the allocation site for leak reports.
#pragma once

#include "wl/base/base.h"

#define WL_HEAP_GRANULE 32u // block sizes and user pointers are multiples of / aligned to this

typedef struct WlHeapBlock WlHeapBlock;

typedef struct WlHeap {
    uint8_t *base; // 32-byte aligned
    size_t size;   // multiple of WL_HEAP_GRANULE
    const char *name;
    WlHeapBlock *free_head;
    size_t used; // bytes in live blocks, headers included (what the budget counts)
    size_t peak;
    uint32_t allocs; // successful allocations ever
    uint32_t frees;
    uint32_t live;  // allocs - frees
    uint32_t fails; // allocation requests that returned NULL
} WlHeap;

// `memory` may be unaligned: the heap trims it to 32-byte alignment. Needs >= 128 bytes.
void wl_heap_init(WlHeap *heap, void *memory, size_t size, const char *name);

// `align` is 1..32 (a power of two); every result is 32-byte aligned regardless. `site` is a
// static string (usually __FILE__) kept for leak reports; `scene` is an opaque stamp.
// Returns NULL when no free block fits (counts a fail). size 0 allocates the minimum block.
void *wl_heap_alloc(WlHeap *heap, size_t size, size_t align, const char *site, uint32_t line,
                    uint32_t scene);

// False (and nothing changes) when `ptr` is not a live block of this heap: foreign pointer,
// double free or corrupted header. NULL is a successful no-op.
bool wl_heap_free(WlHeap *heap, void *ptr);

bool wl_heap_owns(const WlHeap *heap, const void *ptr);

// Usable bytes of a live block (>= the size requested), 0 if not a live block.
size_t wl_heap_usable_size(const WlHeap *heap, const void *ptr);

// Largest single allocation that would succeed now (payload bytes).
size_t wl_heap_largest_free(const WlHeap *heap);

// Walks all blocks verifying sizes, back links, magic and the free list. Returns true if sound.
bool wl_heap_check(const WlHeap *heap);

typedef struct {
    const void *ptr;
    size_t size; // payload bytes
    const char *site;
    uint32_t line;
    uint32_t scene;
} WlHeapLive;

// Calls `fn` for each live block in address order; stops early when it returns false.
typedef bool (*WlHeapWalkFn)(void *user, const WlHeapLive *block);
void wl_heap_walk_live(const WlHeap *heap, WlHeapWalkFn fn, void *user);
