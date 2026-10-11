// SPDX-License-Identifier: MIT
// Linear arena (see arena.h).
#include "wl/base/arena.h"

#include <string.h>

void wl_arena_init(WlArena *arena, void *memory, size_t size, const char *tag) {
    WL_ASSERT(arena != NULL);
    arena->base = (uint8_t *)memory;
    arena->size = memory != NULL ? size : 0;
    arena->used = 0;
    arena->peak = 0;
    arena->tag = tag;
}

void *wl_arena_alloc(WlArena *arena, size_t size, size_t align) {
    WL_ASSERT(align != 0 && (align & (align - 1)) == 0);
    // Align the absolute address, not the offset: the block itself may be unaligned.
    uintptr_t addr = (uintptr_t)arena->base + arena->used;
    size_t pad = (size_t)(((addr + (align - 1)) & ~(uintptr_t)(align - 1)) - addr);
    if (pad > arena->size - arena->used || size > arena->size - arena->used - pad) {
        return NULL;
    }
    uint8_t *p = arena->base + arena->used + pad;
    arena->used += pad + size;
    if (arena->used > arena->peak) {
        arena->peak = arena->used;
    }
    return p;
}

void *wl_arena_alloc_zero(WlArena *arena, size_t size, size_t align) {
    void *p = wl_arena_alloc(arena, size, align);
    if (p != NULL && size != 0) {
        memset(p, 0, size);
    }
    return p;
}

WlArenaMark wl_arena_mark(const WlArena *arena) {
    return arena->used;
}

void wl_arena_reset_to(WlArena *arena, WlArenaMark mark) {
    WL_ASSERT(mark <= arena->used);
    arena->used = mark;
}

void wl_arena_reset(WlArena *arena) {
    arena->used = 0;
}

size_t wl_arena_remaining(const WlArena *arena) {
    return arena->size - arena->used;
}
