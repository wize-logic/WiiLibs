// SPDX-License-Identifier: MIT
// Budgeted memory system over the console's two RAM regions.
//
// wl_mem_init claims MEM1 and MEM2 through wl_platform_mem_region and carves them into one
// budgeted heap per tag, plus the per-frame arenas and the per-scene arena stack in MEM1.
// Whatever the budgets leave over is the reserve and is never handed out. WlMemConfig.claim can
// instead claim only the budgets' need (or a set size) from the top of a region, leaving the rest
// to libogc's malloc, for programs whose libraries allocate through malloc.
//   - Tagged heaps: wl_mem_alloc / wl_mem_free. Exceeding a tag's budget returns NULL and logs an
//     error (the caller degrades: evict, lower detail). Every block records its allocation site.
//   - Frame arena: two bump arenas swapped by wl_mem_frame_begin; contents die at the next swap.
//   - Scene arenas: a stack; pop must be LIFO and reports hot-heap blocks the scene leaked.
//   - Thread-safe through wl_platform_mem_lock (heaps only; arenas have one owner thread).
// A debug overlay reads wl_mem_rows; wl_mem_log_budgets and wl_mem_log_usage print the same table
// through the log sink.
//
// Tags. The library's own users need a few built-in tags (thread stacks, the GX FIFO, audio
// buffers, IO buffers); an application adds its own from WL_TAG_USER up through WlMemConfig and
// may resize (or disable, budget 0) the built-in ones.
#pragma once

#include "wl/base/arena.h"
#include "wl/base/base.h"
#include "wl/platform/platform.h"

// One heap (and one budget) per tag. The tag also says which region the memory comes from.
typedef uint32_t WlMemTag;
enum {
    // MEM1 (fast, 24 MiB in all)                         default budget
    WL_TAG_STACK = 0, // thread stacks (IO thread, audio thread)       0.5 MiB
    WL_TAG_GXFIFO,    // GX command FIFO (wl_gx_init)                   1.0 MiB
    WL_TAG_HOT,       // general heap: scene graph, UI tree             1.0 MiB
    WL_TAG_AUDIO,     // mix buffers, synth state, stream rings         1.0 MiB
    WL_TAG_NET,       // socket buffers, message queues                 0.5 MiB
    // MEM2 (64 MiB, slower, shared with IOS)
    WL_TAG_ASSET,      // models, textures, animations                 30.0 MiB
    WL_TAG_AUDIO_BANK, // decoded samples, instrument banks              6.0 MiB
    WL_TAG_IO,         // read buffers, decompression scratch            2.0 MiB
    WL_TAG_BUILTIN_COUNT,
    WL_TAG_USER = WL_TAG_BUILTIN_COUNT // the first application-defined tag
};
#define WL_MEM_MAX_TAGS 16u
// Rows of wl_mem_rows: every tag, then "frame" and "scene".
#define WL_MEM_MAX_ROWS (WL_MEM_MAX_TAGS + 2u)

// Default sizes of the two frame arenas (each) and of the scene arena stack, both in MEM1.
#define WL_MEM_FRAME_ARENA_SIZE (1024u * 1024u)
#define WL_MEM_SCENE_STACK_SIZE (3u * 1024u * 1024u)
#define WL_MEM_SCENE_MAX_DEPTH 8u

typedef struct {
    const char *name; // static string, shown in rows and logs
    WlMemRegion region;
    size_t budget; // bytes; 0 = the tag is disabled (every allocation fails)
} WlMemTagDef;

typedef struct {
    // tags[i] defines tag i. The first WL_TAG_BUILTIN_COUNT entries are the built-in tags (their
    // budgets may be changed); application tags follow. tag_count is WL_TAG_BUILTIN_COUNT ..
    // WL_MEM_MAX_TAGS. The table is copied by wl_mem_init; the name strings must stay valid.
    const WlMemTagDef *tags;
    uint32_t tag_count;
    size_t frame_arena_size; // each of the two (multiple of 32 bytes is used)
    size_t scene_stack_size;
    // How much of each region (claim[WL_MEM1], claim[WL_MEM2]) wl_mem takes, from the top of its
    // free arena; the rest stays libogc's for malloc. WL_MEM_CLAIM_ALL (the default): the whole
    // free region, the budgets' leftover is the reserve. WL_MEM_CLAIM_NEEDED: exactly the
    // region's budgets (plus the arenas, in MEM1). Any other value: that many bytes, at least
    // the budgets' need (WL_ERR_NOMEM otherwise). Ignored for a region that was already claimed
    // (an earlier wl_mem_init or wl_platform_mem_region keeps its claim).
    size_t claim[2];
} WlMemConfig;

#define WL_MEM_CLAIM_ALL SIZE_MAX
#define WL_MEM_CLAIM_NEEDED 0u

// The built-in tags with their default budgets (static table), default arena sizes.
void wl_mem_config_default(WlMemConfig *out);
// The built-in tag table (WL_TAG_BUILTIN_COUNT entries), for building a larger table on top.
const WlMemTagDef *wl_mem_builtin_tags(void);

// Claims the regions and carves the heaps; a region no budget or arena needs is left unclaimed. `cfg` NULL = wl_mem_config_default. WL_ERR_NOMEM
// (logged) if a region cannot hold its budgets; WL_ERR_ARG for a bad config; WL_ERR_STATE if
// already initialised.
WlResult wl_mem_init(const WlMemConfig *cfg);
// Logs a leak report, releases state; returns the number of leaked heap blocks. The regions
// stay claimed (the platform owns them).
uint32_t wl_mem_shutdown(void);
bool wl_mem_ready(void);
// Tags defined by the current configuration (0 when not initialised).
uint32_t wl_mem_tag_count(void);

// ---- tagged heaps -----------------------------------------------------------------------------

// `align` 1..32. Results are always 32-byte aligned. NULL on exhaustion or over budget.
void *wl_mem_alloc_at(WlMemTag tag, size_t size, size_t align, const char *file, uint32_t line);
#define WL_MEM_ALLOC(tag, size) wl_mem_alloc_at((tag), (size), WL_ARENA_ALIGN, __FILE__, __LINE__)
// GPU/DMA-visible buffer: 32-byte aligned; call wl_gpu_buffer_commit after writing it.
#define WL_MEM_ALLOC_GPU(tag, size)                                                                \
    wl_mem_alloc_at((tag), (size), WL_ARENA_ALIGN_GPU, __FILE__, __LINE__)

// Finds the heap from the pointer. NULL is a no-op. A pointer that is not a live heap block
// (foreign, double free) is logged, counted in wl_mem_bad_frees and ignored.
void wl_mem_free(void *ptr);
uint32_t wl_mem_bad_frees(void);

const char *wl_mem_tag_name(WlMemTag tag);
// The address range of a tag's heap (false if not initialised, an unknown tag, or a disabled
// one). Lets a layer that mixes this heap with another's decide from a pointer which it came from.
bool wl_mem_tag_range(WlMemTag tag, const void **base, size_t *size);

// ---- frame arena ------------------------------------------------------------------------------

// Swaps to the other frame arena and resets it. Call once at the start of each frame. Anything
// allocated from the arena in the previous frame stays valid through this frame.
void wl_mem_frame_begin(void);
WlArena *wl_mem_frame_arena(void);
uint32_t wl_mem_frame_count(void);

// Frame-loop guard: while on, a WL_TAG_HOT heap allocation is a violation (logged, counted):
// per-frame data belongs in the frame arena. The main thread sets it around the frame body.
void wl_mem_frame_guard(bool on);
uint32_t wl_mem_frame_violations(void);

// ---- scene arenas -----------------------------------------------------------------------------

// Pushes a scene arena of `size` bytes (32-byte granules) from the scene stack; NULL if the
// stack or depth is exhausted. Allocations made on WL_TAG_HOT while it is on top are stamped
// with its scene id.
WlArena *wl_mem_scene_push(const char *name, size_t size);
// `arena` must be the top. Logs its peak and reports hot-heap blocks allocated during the scene
// that are still live; returns that count (the blocks are left alone: free them or leak them).
uint32_t wl_mem_scene_pop(WlArena *arena);
uint32_t wl_mem_scene_depth(void);

// ---- statistics, leak report, overlay API -----------------------------------------------------

typedef struct {
    const char *name;
    WlMemRegion region;
    size_t budget; // bytes
    size_t used;   // heaps: live bytes incl. block headers; arenas: bytes bumped now
    size_t peak;   // high-water mark since init
    uint32_t live; // live blocks (heaps) or arenas (scene stack)
    uint32_t fails; // refused requests (heaps)
} WlMemRow;

// Rows in tag order (wl_mem_tag_count), then "frame" (both arenas combined: peak is the larger)
// and "scene". Returns the number written (tag count + 2 when max allows; WL_MEM_MAX_ROWS is
// always enough). The strings are static.
uint32_t wl_mem_rows(WlMemRow *rows, uint32_t max);

// One INFO line per row plus a summary: budgets only, then budget/used/peak.
void wl_mem_log_budgets(void);
void wl_mem_log_usage(void);

// Logs every live heap block (site, size, tag; first 16 per heap) and returns the total.
uint32_t wl_mem_leak_report(void);
// Live block count of one heap.
uint32_t wl_mem_live_blocks(WlMemTag tag);
// Walks every heap checking structure; false if any is corrupt (logged).
bool wl_mem_check_all(void);

// ---- hooks the platform backend provides (mem_wii.c, mem_host.c) ------------------------------

// Recursive lock around heap operations (host stub: a recursive pthread mutex).
void wl_platform_mem_lock(void);
void wl_platform_mem_unlock(void);
