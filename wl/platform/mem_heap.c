// SPDX-License-Identifier: MIT
// Tagged-heap core (see mem_heap.h).
#include "wl/platform/mem_heap.h"

#define MAGIC_USED 0xA110C8EDu
#define MAGIC_FREE 0xF4EEB10Cu
#define MIN_BLOCK (2u * WL_HEAP_GRANULE) // header + one granule of payload

struct WlHeapBlock {
    uint32_t size;      // whole block incl. this header; multiple of 32
    uint32_t prev_size; // whole size of the block before this one in address order, 0 if first
    uint32_t magic;
    uint32_t line;
    const char *site;
    uint32_t scene;
    uint32_t req; // requested size
} __attribute__((aligned(WL_HEAP_GRANULE)));

_Static_assert(sizeof(WlHeapBlock) == WL_HEAP_GRANULE, "block header must be one granule");

typedef struct {
    WlHeapBlock *next;
    WlHeapBlock *prev;
} FreeLinks;

static FreeLinks *links(WlHeapBlock *b) {
    return (FreeLinks *)(void *)((uint8_t *)b + sizeof(WlHeapBlock));
}

static uint8_t *heap_end(const WlHeap *h) {
    return h->base + h->size;
}

static WlHeapBlock *next_block(const WlHeap *h, WlHeapBlock *b) {
    uint8_t *n = (uint8_t *)b + b->size;
    return n < heap_end(h) ? (WlHeapBlock *)(void *)n : NULL;
}

static WlHeapBlock *prev_block(WlHeapBlock *b) {
    return b->prev_size != 0 ? (WlHeapBlock *)(void *)((uint8_t *)b - b->prev_size) : NULL;
}

static void free_unlink(WlHeap *h, WlHeapBlock *b) {
    FreeLinks *l = links(b);
    if (l->prev != NULL) {
        links(l->prev)->next = l->next;
    } else {
        h->free_head = l->next;
    }
    if (l->next != NULL) {
        links(l->next)->prev = l->prev;
    }
}

static void free_push(WlHeap *h, WlHeapBlock *b) {
    FreeLinks *l = links(b);
    l->prev = NULL;
    l->next = h->free_head;
    if (h->free_head != NULL) {
        links(h->free_head)->prev = b;
    }
    h->free_head = b;
}

void wl_heap_init(WlHeap *heap, void *memory, size_t size, const char *name) {
    WL_ASSERT(heap != NULL && memory != NULL);
    uintptr_t a = ((uintptr_t)memory + (WL_HEAP_GRANULE - 1)) & ~(uintptr_t)(WL_HEAP_GRANULE - 1);
    size_t lost = (size_t)(a - (uintptr_t)memory);
    WL_ASSERT(size > lost);
    size = (size - lost) & ~(size_t)(WL_HEAP_GRANULE - 1);
    WL_ASSERT(size >= 4u * WL_HEAP_GRANULE && size <= 0xFFFFFFF0u);
    heap->base = (uint8_t *)a;
    heap->size = size;
    heap->name = name;
    heap->used = heap->peak = 0;
    heap->allocs = heap->frees = heap->live = heap->fails = 0;
    heap->best_fit = false;
    WlHeapBlock *b = (WlHeapBlock *)(void *)heap->base;
    b->size = (uint32_t)size;
    b->prev_size = 0;
    b->magic = MAGIC_FREE;
    b->line = 0;
    b->site = NULL;
    b->scene = 0;
    b->req = 0;
    heap->free_head = NULL;
    free_push(heap, b);
}

void *wl_heap_alloc(WlHeap *heap, size_t size, size_t align, const char *site, uint32_t line,
                    uint32_t scene) {
    WL_ASSERT(align != 0 && align <= WL_HEAP_GRANULE && (align & (align - 1)) == 0);
    if (size > heap->size) {
        heap->fails++;
        return NULL;
    }
    size_t payload = (size + (WL_HEAP_GRANULE - 1)) & ~(size_t)(WL_HEAP_GRANULE - 1);
    if (payload == 0) {
        payload = WL_HEAP_GRANULE;
    }
    size_t need = payload + sizeof(WlHeapBlock);
    WlHeapBlock *b = heap->free_head;
    while (b != NULL && b->size < need) {
        b = links(b)->next;
    }
    if (b != NULL && heap->best_fit && b->size != need) {
        for (WlHeapBlock *c = links(b)->next; c != NULL; c = links(c)->next) {
            if (c->size >= need && c->size < b->size) {
                b = c;
                if (c->size == need) {
                    break;
                }
            }
        }
    }
    if (b == NULL) {
        heap->fails++;
        return NULL;
    }
    free_unlink(heap, b);
    if (b->size - need >= MIN_BLOCK) { // split: the tail stays free
        WlHeapBlock *rest = (WlHeapBlock *)(void *)((uint8_t *)b + need);
        rest->size = b->size - (uint32_t)need;
        rest->prev_size = (uint32_t)need;
        rest->magic = MAGIC_FREE;
        rest->line = 0;
        rest->site = NULL;
        rest->scene = 0;
        rest->req = 0;
        b->size = (uint32_t)need;
        WlHeapBlock *after = next_block(heap, rest);
        if (after != NULL) {
            after->prev_size = rest->size;
        }
        free_push(heap, rest);
    }
    b->magic = MAGIC_USED;
    b->site = site;
    b->line = line;
    b->scene = scene;
    b->req = (uint32_t)size;
    heap->used += b->size;
    if (heap->used > heap->peak) {
        heap->peak = heap->used;
    }
    heap->allocs++;
    heap->live++;
    return (uint8_t *)b + sizeof(WlHeapBlock);
}

// The live block whose payload starts at `ptr`, or NULL.
static WlHeapBlock *live_block(const WlHeap *heap, const void *ptr) {
    if (!wl_heap_owns(heap, ptr)) {
        return NULL;
    }
    WlHeapBlock *b = (WlHeapBlock *)(void *)((uint8_t *)ptr - sizeof(WlHeapBlock));
    return b->magic == MAGIC_USED ? b : NULL;
}

bool wl_heap_free(WlHeap *heap, void *ptr) {
    if (ptr == NULL) {
        return true;
    }
    WlHeapBlock *b = live_block(heap, ptr);
    if (b == NULL || b->size < MIN_BLOCK || (uint8_t *)b + b->size > heap_end(heap)) {
        return false;
    }
    heap->used -= b->size;
    heap->frees++;
    heap->live--;
    b->magic = MAGIC_FREE;
    b->site = NULL;
    b->line = 0;
    b->scene = 0;
    b->req = 0;
    WlHeapBlock *n = next_block(heap, b);
    if (n != NULL && n->magic == MAGIC_FREE) {
        free_unlink(heap, n);
        b->size += n->size;
        n = next_block(heap, b);
        if (n != NULL) {
            n->prev_size = b->size;
        }
    }
    WlHeapBlock *p = prev_block(b);
    if (p != NULL && p->magic == MAGIC_FREE) {
        free_unlink(heap, p);
        p->size += b->size;
        n = next_block(heap, p);
        if (n != NULL) {
            n->prev_size = p->size;
        }
        b = p;
    }
    free_push(heap, b);
    return true;
}

bool wl_heap_owns(const WlHeap *heap, const void *ptr) {
    const uint8_t *p = (const uint8_t *)ptr;
    return p >= heap->base + sizeof(WlHeapBlock) && p < heap_end(heap) &&
           ((uintptr_t)p & (WL_HEAP_GRANULE - 1)) == 0;
}

size_t wl_heap_usable_size(const WlHeap *heap, const void *ptr) {
    WlHeapBlock *b = live_block(heap, ptr);
    return b != NULL ? b->size - sizeof(WlHeapBlock) : 0;
}

size_t wl_heap_largest_free(const WlHeap *heap) {
    size_t best = 0;
    for (WlHeapBlock *b = heap->free_head; b != NULL; b = links(b)->next) {
        if (b->size > best) {
            best = b->size;
        }
    }
    return best > sizeof(WlHeapBlock) ? best - sizeof(WlHeapBlock) : 0;
}

bool wl_heap_check(const WlHeap *heap) {
    size_t total = 0, used = 0, free_blocks = 0;
    uint32_t live = 0;
    uint32_t prev = 0;
    bool prev_free = false;
    for (WlHeapBlock *b = (WlHeapBlock *)(void *)heap->base; b != NULL; b = next_block(heap, b)) {
        if (b->size < MIN_BLOCK || (b->size & (WL_HEAP_GRANULE - 1)) != 0 ||
            (uint8_t *)b + b->size > heap_end(heap) || b->prev_size != prev) {
            return false;
        }
        bool is_free = b->magic == MAGIC_FREE;
        if (!is_free && b->magic != MAGIC_USED) {
            return false;
        }
        if (is_free && prev_free) {
            return false; // two adjacent free blocks: coalescing failed
        }
        if (is_free) {
            free_blocks++;
        } else {
            used += b->size;
            live++;
        }
        prev_free = is_free;
        prev = b->size;
        total += b->size;
    }
    if (total != heap->size || used != heap->used || live != heap->live) {
        return false;
    }
    size_t listed = 0;
    for (WlHeapBlock *b = heap->free_head; b != NULL; b = links(b)->next) {
        if (b->magic != MAGIC_FREE || (uint8_t *)b < heap->base || (uint8_t *)b >= heap_end(heap) ||
            ++listed > free_blocks) {
            return false;
        }
    }
    return listed == free_blocks;
}

void wl_heap_walk_live(const WlHeap *heap, WlHeapWalkFn fn, void *user) {
    for (WlHeapBlock *b = (WlHeapBlock *)(void *)heap->base; b != NULL; b = next_block(heap, b)) {
        if (b->magic == MAGIC_USED) {
            WlHeapLive l = {(uint8_t *)b + sizeof(WlHeapBlock), b->req, b->site, b->line, b->scene};
            if (!fn(user, &l)) {
                return;
            }
        }
    }
}
