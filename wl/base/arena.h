// SPDX-License-Identifier: MIT
// Linear (bump) arena allocator.
//
// The caller owns the memory block; an arena never calls malloc. Objects created in an arena
// live until the arena (or a mark below them) is reset, so most arena-backed objects need no destroy
// function. Not thread-safe: one owner thread.
#pragma once

#include "wl/base/base.h"

// Public so callers can embed arenas by value (stack, scene structs). Treat fields as
// read-only outside wl/base/arena.c.
typedef struct WlArena {
    uint8_t *base;
    size_t size;
    size_t used;
    size_t peak;     // high-water mark, for budget reports
    const char *tag; // budget tag for debug tracking; may be NULL
} WlArena;

// Position to roll back to; valid only for the arena that produced it.
typedef size_t WlArenaMark;

// Default alignment of wl_arena_alloc: enough for any scalar on both targets.
#define WL_ARENA_ALIGN 8u
// Alignment the Wii GPU and DMA need (32 bytes).
#define WL_ARENA_ALIGN_GPU 32u

void wl_arena_init(WlArena *arena, void *memory, size_t size, const char *tag);

// Returns NULL when the arena is exhausted (callers turn that into WL_ERR_NOMEM). `align` is
// a power of two. Memory is not zeroed.
void *wl_arena_alloc(WlArena *arena, size_t size, size_t align);

// As wl_arena_alloc, but zero-filled.
void *wl_arena_alloc_zero(WlArena *arena, size_t size, size_t align);

WlArenaMark wl_arena_mark(const WlArena *arena);
void wl_arena_reset_to(WlArena *arena, WlArenaMark mark);
void wl_arena_reset(WlArena *arena);

size_t wl_arena_remaining(const WlArena *arena);

// Typed helpers.
#define WL_ARENA_NEW(arena, T) ((T *)wl_arena_alloc_zero((arena), sizeof(T), _Alignof(T)))
#define WL_ARENA_ARRAY(arena, T, n)                                                                \
    ((T *)wl_arena_alloc_zero((arena), sizeof(T) * (size_t)(n), _Alignof(T)))
