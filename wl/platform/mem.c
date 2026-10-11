// SPDX-License-Identifier: MIT
// Memory system: tags, budgets, frame and scene arenas, tracking (see mem.h).
#include "wl/platform/mem.h"

#include <string.h>

#include "wl/platform/mem_heap.h"

#define KIB 1024u
#define MIB (1024u * 1024u)
#define LOG_TAG "mem"
#define LEAK_LINES_PER_HEAP 16u

// Default budgets. MEM1: 4 MiB of heaps + 2 x 1 MiB frame arenas + a 3 MiB scene stack = 9 MiB,
// leaving room for the executable, the XFBs and libogc's own allocations. MEM2: 38 MiB of heaps;
// the rest is the reserve (IOS keeps its own heap outside the arena handed to us).
static const WlMemTagDef k_builtin[WL_TAG_BUILTIN_COUNT] = {
    [WL_TAG_STACK] = {"stack", WL_MEM1, 512u * KIB},
    [WL_TAG_GXFIFO] = {"gxfifo", WL_MEM1, 1u * MIB},
    [WL_TAG_HOT] = {"hot", WL_MEM1, 1u * MIB},
    [WL_TAG_AUDIO] = {"audio", WL_MEM1, 1u * MIB},
    [WL_TAG_NET] = {"net", WL_MEM1, 512u * KIB},
    [WL_TAG_ASSET] = {"asset", WL_MEM2, 30u * MIB},
    [WL_TAG_AUDIO_BANK] = {"audiobank", WL_MEM2, 6u * MIB},
    [WL_TAG_IO] = {"io", WL_MEM2, 2u * MIB},
};

typedef struct {
    WlArena arena;
    uint32_t id;
    const char *name;
} Scene;

static struct {
    bool ready;
    WlMemTagDef tags[WL_MEM_MAX_TAGS];
    uint32_t tag_count;
    size_t frame_size;
    size_t scene_size;
    WlHeap heaps[WL_MEM_MAX_TAGS];
    WlArena frame[2];
    uint32_t frame_cur;
    uint32_t frame_count;
    size_t frame_peak;
    bool guard;
    uint32_t violations;
    uint32_t bad_frees;
    uint8_t *scene_base;
    size_t scene_used; // bytes of the scene stack in use (32-byte granules)
    size_t scene_peak;
    Scene scenes[WL_MEM_SCENE_MAX_DEPTH];
    uint32_t scene_depth;
    uint32_t scene_serial;
} g;

static size_t align32(size_t v) {
    return (v + (WL_ARENA_ALIGN_GPU - 1)) & ~(size_t)(WL_ARENA_ALIGN_GPU - 1);
}

bool wl_mem_ready(void) {
    return g.ready;
}

uint32_t wl_mem_tag_count(void) {
    return g.ready ? g.tag_count : 0;
}

const char *wl_mem_tag_name(WlMemTag tag) {
    if (g.ready) {
        return tag < g.tag_count ? g.tags[tag].name : "?";
    }
    return tag < WL_TAG_BUILTIN_COUNT ? k_builtin[tag].name : "?";
}

static bool heap_live(uint32_t tag);

bool wl_mem_tag_range(WlMemTag tag, const void **base, size_t *size) {
    if (!g.ready || tag >= g.tag_count || !heap_live(tag) || base == NULL || size == NULL) {
        return false;
    }
    *base = g.heaps[tag].base;
    *size = g.heaps[tag].size;
    return true;
}

const WlMemTagDef *wl_mem_builtin_tags(void) {
    return k_builtin;
}

void wl_mem_config_default(WlMemConfig *out) {
    out->tags = k_builtin;
    out->tag_count = WL_TAG_BUILTIN_COUNT;
    out->frame_arena_size = WL_MEM_FRAME_ARENA_SIZE;
    out->scene_stack_size = WL_MEM_SCENE_STACK_SIZE;
    out->claim[WL_MEM1] = WL_MEM_CLAIM_ALL;
    out->claim[WL_MEM2] = WL_MEM_CLAIM_ALL;
}

// A disabled tag (budget 0) has no heap memory: every allocation from it fails.
static bool heap_live(uint32_t tag) {
    return g.heaps[tag].base != NULL;
}

static size_t region_need(WlMemRegion r) {
    size_t n = 0;
    for (unsigned i = 0; i < g.tag_count; i++) {
        if (g.tags[i].region == r) {
            n += align32(g.tags[i].budget);
        }
    }
    if (r == WL_MEM1) {
        n += 2u * g.frame_size + g.scene_size;
    }
    return n;
}

WlResult wl_mem_init(const WlMemConfig *cfg) {
    if (g.ready) {
        return WL_ERR_STATE;
    }
    WlMemConfig def;
    if (cfg == NULL) {
        wl_mem_config_default(&def);
        cfg = &def;
    }
    if (cfg->tags == NULL || cfg->tag_count < WL_TAG_BUILTIN_COUNT || cfg->tag_count > WL_MEM_MAX_TAGS) {
        WL_LOG_E(LOG_TAG, "bad config: %u tags (need %u..%u)", (unsigned)cfg->tag_count,
                 (unsigned)WL_TAG_BUILTIN_COUNT, (unsigned)WL_MEM_MAX_TAGS);
        return WL_ERR_ARG;
    }
    for (uint32_t i = 0; i < cfg->tag_count; i++) {
        const WlMemTagDef *t = &cfg->tags[i];
        if (t->name == NULL || (unsigned)t->region > WL_MEM2 || (t->budget != 0 && t->budget < 128u) ||
            align32(t->budget) < t->budget) {
            WL_LOG_E(LOG_TAG, "bad config: tag %u", (unsigned)i);
            return WL_ERR_ARG;
        }
    }
    memset(&g, 0, sizeof g);
    memcpy(g.tags, cfg->tags, cfg->tag_count * sizeof g.tags[0]);
    g.tag_count = cfg->tag_count;
    g.frame_size = align32(cfg->frame_arena_size);
    g.scene_size = align32(cfg->scene_stack_size);
    for (int r = WL_MEM1; r <= WL_MEM2; r++) {
        void *base = NULL;
        size_t size = 0;
        // a region no budget (nor arena) needs is not claimed: its arena stays libogc's, for
        // malloc (a program whose own data fill MEM1 puts every heap in MEM2)
        size_t need = region_need((WlMemRegion)r);
        if (need == 0) {
            continue;
        }
        size_t claim = cfg->claim[r] == WL_MEM_CLAIM_NEEDED ? need : cfg->claim[r];
        if (claim < need) {
            WL_LOG_E(LOG_TAG, "mem%d claim of %zu bytes is below the budgets' %zu", r + 1, claim, need);
            return WL_ERR_NOMEM;
        }
        if (claim != WL_MEM_CLAIM_ALL) {
            // WL_ERR_STATE: the region is claimed already, and keeps that claim
            (void)wl_platform_mem_region_limit((WlMemRegion)r, claim);
        }
        WlResult res = wl_platform_mem_region((WlMemRegion)r, &base, &size);
        if (res != WL_OK) {
            WL_LOG_E(LOG_TAG, "mem%d region unavailable (%s)", r + 1, wl_result_name(res));
            return res;
        }
        uintptr_t a = align32((size_t)(uintptr_t)base);
        size_t lost = (size_t)(a - (uintptr_t)base);
        size = size > lost ? size - lost : 0;
        if (size < need) {
            WL_LOG_E(LOG_TAG, "mem%d too small: %zu bytes free, budgets need %zu", r + 1, size, need);
            return WL_ERR_NOMEM;
        }
        uint8_t *p = (uint8_t *)a;
        for (unsigned i = 0; i < g.tag_count; i++) {
            if (g.tags[i].region == (WlMemRegion)r && g.tags[i].budget != 0) {
                wl_heap_init(&g.heaps[i], p, g.tags[i].budget, g.tags[i].name);
                p += align32(g.tags[i].budget);
            }
        }
        if (r == WL_MEM1) {
            for (unsigned i = 0; i < 2; i++) {
                wl_arena_init(&g.frame[i], p, g.frame_size, "frame");
                p += g.frame_size;
            }
            g.scene_base = p;
        }
    }
    g.ready = true;
    return WL_OK;
}

uint32_t wl_mem_shutdown(void) {
    if (!g.ready) {
        return 0;
    }
    uint32_t leaks = wl_mem_leak_report();
    memset(&g, 0, sizeof g);
    return leaks;
}

// ---- heaps ------------------------------------------------------------------------------------

void *wl_mem_alloc_at(WlMemTag tag, size_t size, size_t align, const char *file, uint32_t line) {
    WL_ASSERT(g.ready && tag < g.tag_count);
    bool violation = g.guard && tag == WL_TAG_HOT;
    uint32_t scene = g.scene_depth != 0 ? g.scenes[g.scene_depth - 1].id : 0;
    wl_platform_mem_lock();
    void *p = NULL;
    bool failed;
    if (heap_live(tag)) {
        uint32_t fails_before = g.heaps[tag].fails;
        p = wl_heap_alloc(&g.heaps[tag], size, align, file, line, scene);
        failed = g.heaps[tag].fails != fails_before;
    } else {
        g.heaps[tag].fails++;
        failed = true;
    }
    if (violation) {
        g.violations++;
    }
    wl_platform_mem_unlock();
    if (failed) {
        WL_LOG_E(LOG_TAG, "%s heap exhausted: %zu bytes at %s:%u", g.tags[tag].name, size,
                 file != NULL ? file : "?", (unsigned)line);
    }
    if (violation) {
        WL_LOG_E(LOG_TAG, "frame-loop allocation on hot heap at %s:%u: use the frame arena",
                 file != NULL ? file : "?", (unsigned)line);
    }
    return p;
}

void wl_mem_free(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    WL_ASSERT(g.ready);
    bool ok = false;
    wl_platform_mem_lock();
    for (unsigned i = 0; i < g.tag_count; i++) {
        if (heap_live(i) && wl_heap_owns(&g.heaps[i], ptr)) {
            ok = wl_heap_free(&g.heaps[i], ptr);
            break;
        }
    }
    if (!ok) {
        g.bad_frees++;
    }
    wl_platform_mem_unlock();
    if (!ok) {
        WL_LOG_E(LOG_TAG, "free of a pointer that is not a live heap block (double free?)");
    }
}

uint32_t wl_mem_bad_frees(void) {
    return g.bad_frees;
}

// ---- frame arena ------------------------------------------------------------------------------

void wl_mem_frame_begin(void) {
    WL_ASSERT(g.ready);
    if (g.frame[g.frame_cur].peak > g.frame_peak) {
        g.frame_peak = g.frame[g.frame_cur].peak;
    }
    g.frame_cur ^= 1u;
    WlArena *a = &g.frame[g.frame_cur];
    if (a->peak > g.frame_peak) {
        g.frame_peak = a->peak;
    }
#ifndef NDEBUG
    memset(a->base, 0xDD, a->used); // poison: a stale pointer reads garbage, not old data
#endif
    wl_arena_reset(a);
    g.frame_count++;
}

WlArena *wl_mem_frame_arena(void) {
    WL_ASSERT(g.ready);
    return &g.frame[g.frame_cur];
}

uint32_t wl_mem_frame_count(void) {
    return g.frame_count;
}

void wl_mem_frame_guard(bool on) {
    g.guard = on;
}

uint32_t wl_mem_frame_violations(void) {
    return g.violations;
}

// ---- scene arenas -----------------------------------------------------------------------------

WlArena *wl_mem_scene_push(const char *name, size_t size) {
    WL_ASSERT(g.ready);
    size_t sz = align32(size);
    if (g.scene_depth >= WL_MEM_SCENE_MAX_DEPTH || sz < size || sz > g.scene_size ||
        g.scene_used > g.scene_size - sz) {
        WL_LOG_E(LOG_TAG, "scene arena '%s' (%zu bytes) does not fit: depth %u, %zu of %zu used",
                 name != NULL ? name : "?", size, (unsigned)g.scene_depth, g.scene_used,
                 g.scene_size);
        return NULL;
    }
    Scene *s = &g.scenes[g.scene_depth++];
    s->id = ++g.scene_serial;
    s->name = name != NULL ? name : "scene";
    wl_arena_init(&s->arena, g.scene_base + g.scene_used, sz, s->name);
    g.scene_used += sz;
    if (g.scene_used > g.scene_peak) {
        g.scene_peak = g.scene_used;
    }
    return &s->arena;
}

typedef struct {
    uint32_t scene;
    uint32_t count;
} SceneLeaks;

static bool count_scene(void *user, const WlHeapLive *b) {
    SceneLeaks *s = (SceneLeaks *)user;
    if (b->scene == s->scene) {
        if (s->count < LEAK_LINES_PER_HEAP) {
            WL_LOG_W(LOG_TAG, "scene leak: %zu bytes from %s:%u", b->size,
                     b->site != NULL ? b->site : "?", (unsigned)b->line);
        }
        s->count++;
    }
    return true;
}

uint32_t wl_mem_scene_pop(WlArena *arena) {
    WL_ASSERT(g.ready && g.scene_depth > 0 && arena == &g.scenes[g.scene_depth - 1].arena);
    Scene *s = &g.scenes[g.scene_depth - 1];
    WL_LOG_I(LOG_TAG, "scene '%s' pop: peak %zu of %zu bytes", s->name, s->arena.peak, s->arena.size);
    SceneLeaks leaks = {s->id, 0};
    wl_platform_mem_lock();
    if (heap_live(WL_TAG_HOT)) {
        wl_heap_walk_live(&g.heaps[WL_TAG_HOT], count_scene, &leaks);
    }
    wl_platform_mem_unlock();
    if (leaks.count != 0) {
        WL_LOG_W(LOG_TAG, "scene '%s' leaked %u hot-heap blocks", s->name, (unsigned)leaks.count);
    }
#ifndef NDEBUG
    memset(s->arena.base, 0xDD, s->arena.used);
#endif
    g.scene_used -= s->arena.size;
    g.scene_depth--;
    return leaks.count;
}

uint32_t wl_mem_scene_depth(void) {
    return g.scene_depth;
}

// ---- statistics -------------------------------------------------------------------------------

uint32_t wl_mem_rows(WlMemRow *rows, uint32_t max) {
    uint32_t n = 0;
    wl_platform_mem_lock();
    for (unsigned i = 0; i < g.tag_count && n < max; i++, n++) {
        const WlHeap *h = &g.heaps[i];
        rows[n] = (WlMemRow){g.tags[i].name, g.tags[i].region, g.tags[i].budget,
                             h->used,        h->peak,          h->live,
                             h->fails};
    }
    wl_platform_mem_unlock();
    if (n < max) {
        size_t peak = g.frame_peak;
        for (unsigned i = 0; i < 2; i++) {
            if (g.frame[i].peak > peak) {
                peak = g.frame[i].peak;
            }
        }
        rows[n++] = (WlMemRow){"frame", WL_MEM1, 2u * g.frame_size,
                               g.frame[g.frame_cur].used, peak, 2, 0};
    }
    if (n < max) {
        rows[n++] = (WlMemRow){"scene", WL_MEM1, g.scene_size, g.scene_used,
                               g.scene_peak, g.scene_depth, 0};
    }
    return n;
}

static void log_rows(bool usage) {
    WlMemRow rows[WL_MEM_MAX_ROWS];
    uint32_t n = wl_mem_rows(rows, WL_ARRAY_COUNT(rows));
    size_t total[2] = {0, 0};
    for (uint32_t i = 0; i < n; i++) {
        total[rows[i].region] += rows[i].budget;
        if (usage) {
            WL_LOG_I(LOG_TAG, "mem%d %-9s %8zu / %8zu KiB  peak %8zu KiB  live %u fails %u",
                     rows[i].region + 1, rows[i].name, rows[i].used / KIB, rows[i].budget / KIB,
                     rows[i].peak / KIB, (unsigned)rows[i].live, (unsigned)rows[i].fails);
        } else {
            WL_LOG_I(LOG_TAG, "mem%d %-9s budget %8zu KiB", rows[i].region + 1, rows[i].name,
                     rows[i].budget / KIB);
        }
    }
    WL_LOG_I(LOG_TAG, "carved: mem1 %zu KiB (of 24576, the rest stays free), mem2 %zu KiB (of 65536)",
             total[0] / KIB, total[1] / KIB);
}

void wl_mem_log_budgets(void) {
    log_rows(false);
}

void wl_mem_log_usage(void) {
    log_rows(true);
}

typedef struct {
    const char *heap;
    uint32_t count;
} LeakCtx;

static bool log_leak(void *user, const WlHeapLive *b) {
    LeakCtx *c = (LeakCtx *)user;
    if (c->count < LEAK_LINES_PER_HEAP) {
        WL_LOG_W(LOG_TAG, "leak: %s %zu bytes from %s:%u", c->heap, b->size,
                 b->site != NULL ? b->site : "?", (unsigned)b->line);
    }
    c->count++;
    return true;
}

uint32_t wl_mem_leak_report(void) {
    uint32_t total = 0;
    if (!g.ready) {
        return 0;
    }
    for (unsigned i = 0; i < g.tag_count; i++) {
        if (!heap_live(i)) {
            continue;
        }
        LeakCtx c = {g.tags[i].name, 0};
        wl_platform_mem_lock();
        wl_heap_walk_live(&g.heaps[i], log_leak, &c);
        wl_platform_mem_unlock();
        if (c.count > LEAK_LINES_PER_HEAP) {
            WL_LOG_W(LOG_TAG, "leak: %s ... and %u more", c.heap,
                     (unsigned)(c.count - LEAK_LINES_PER_HEAP));
        }
        total += c.count;
    }
    if (g.scene_depth != 0) {
        WL_LOG_W(LOG_TAG, "leak: %u scene arenas never popped", (unsigned)g.scene_depth);
    }
    WL_LOG_I(LOG_TAG, "leak report: %u live blocks", (unsigned)total);
    return total;
}

uint32_t wl_mem_live_blocks(WlMemTag tag) {
    return tag < g.tag_count ? g.heaps[tag].live : 0;
}

bool wl_mem_check_all(void) {
    bool ok = true;
    wl_platform_mem_lock();
    for (unsigned i = 0; i < g.tag_count; i++) {
        if (heap_live(i) && !wl_heap_check(&g.heaps[i])) {
            ok = false;
        }
    }
    wl_platform_mem_unlock();
    if (!ok) {
        WL_LOG_E(LOG_TAG, "heap check failed: corruption");
    }
    return ok;
}
