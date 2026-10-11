// SPDX-License-Identifier: MIT
// The overflow heap's portable core (wl/platform/heap_overflow.c) against a fake primary allocator
// (a small wl_heap, with the checks newlib cannot make: it counts any pointer it is given that is
// not its own) and the real wl_mem on the host backend. Byte-order independent: also runs on the
// big-endian build.
#include "wl/platform/heap_overflow.h"

#include <stdint.h>
#include <string.h>

#include "greatest.h"
#include "wl/platform/mem_heap.h"

enum { TAG_SPILL = WL_TAG_USER, TAG_COUNT };

#define PRIMARY_BYTES (64u * 1024u)
#define SPILL_BYTES (256u * 1024u)

static WlMemTagDef g_tags[TAG_COUNT];

// ---- the fake primary -------------------------------------------------------------------------

typedef struct {
    WlHeap heap;
    uint32_t foreign; // frees, reallocs or size queries of a pointer that is not a live block
    uint32_t allocs;
    bool has_realloc; // false: the core emulates realloc
} Fake;

static _Alignas(32) uint8_t g_fake_memory[PRIMARY_BYTES];
static Fake g_fake;

static void *fake_alloc(void *ctx, size_t align, size_t size) {
    Fake *f = (Fake *)ctx;
    f->allocs++;
    if (align > 32u || (align & (align - 1u)) != 0u) {
        return NULL; // this primary does not do large alignments, nor odd ones
    }
    return wl_heap_alloc(&f->heap, size, align == 0u ? 8u : align, "fake", 0u, 0u);
}

static void fake_free(void *ctx, void *ptr) {
    Fake *f = (Fake *)ctx;
    if (!wl_heap_free(&f->heap, ptr)) {
        f->foreign++;
    }
}

static size_t fake_usable(void *ctx, void *ptr) {
    Fake *f = (Fake *)ctx;
    size_t n = wl_heap_usable_size(&f->heap, ptr);
    if (n == 0u) {
        f->foreign++;
    }
    return n;
}

// realloc in place only when it shrinks or the block already holds the size; else "no room"
static void *fake_realloc(void *ctx, void *ptr, size_t size) {
    Fake *f = (Fake *)ctx;
    size_t have = fake_usable(ctx, ptr);
    if (size <= have) {
        return ptr;
    }
    void *n = wl_heap_alloc(&f->heap, size, 8u, "fake", 0u, 0u);
    if (n == NULL) {
        return NULL;
    }
    memcpy(n, ptr, have);
    fake_free(ctx, ptr);
    return n;
}

static void fake_usage(void *ctx, size_t *used, size_t *free_bytes) {
    Fake *f = (Fake *)ctx;
    *used = f->heap.used;
    *free_bytes = f->heap.size - f->heap.used;
}

static WlOverflowPrimary primary(bool with_realloc) {
    WlOverflowPrimary p = {&g_fake,   fake_alloc,  NULL,      with_realloc ? fake_realloc : NULL,
                           fake_free, fake_usable, fake_usage};
    return p;
}

// ---- fixtures ---------------------------------------------------------------------------------

static WlOverflow g_o;
static WlOverflowPrimary g_p;
static struct {
    uint32_t calls;
    char what[16];
    size_t size, align;
} g_fail;

static void on_fail(void *user, const char *what, size_t size, size_t align) {
    (void)user;
    g_fail.calls++;
    strncpy(g_fail.what, what, sizeof g_fail.what - 1u);
    g_fail.size = size;
    g_fail.align = align;
}

static void setup(bool arm, bool with_realloc) {
    (void)wl_mem_shutdown();
    memcpy(g_tags, wl_mem_builtin_tags(), WL_TAG_BUILTIN_COUNT * sizeof g_tags[0]);
    g_tags[TAG_SPILL] = (WlMemTagDef){"spill", WL_MEM2, SPILL_BYTES};
    WlMemConfig cfg;
    wl_mem_config_default(&cfg);
    cfg.tags = g_tags;
    cfg.tag_count = TAG_COUNT;
    if (wl_mem_init(&cfg) != WL_OK) {
        abort();
    }
    memset(g_fake_memory, 0, sizeof g_fake_memory);
    memset(&g_fake, 0, sizeof g_fake);
    wl_heap_init(&g_fake.heap, g_fake_memory, sizeof g_fake_memory, "fake");
    memset(&g_o, 0, sizeof g_o);
    memset(&g_fail, 0, sizeof g_fail);
    g_p = primary(with_realloc);
    if (arm) {
        if (wl_overflow_init(&g_o, TAG_SPILL) != WL_OK) {
            abort();
        }
        wl_overflow_set_fail_hook(&g_o, on_fail, NULL);
    }
}

// Takes blocks of `chunk` bytes, then smaller ones, until the primary has no room for another;
// returns how many.
#define FILL_MAX 256u
static void *g_fill[FILL_MAX];
static unsigned fill_primary(size_t chunk) {
    unsigned n = 0;
    for (; chunk >= 8u; chunk /= 4u) { // then the crumbs, so that nothing at all fits
        for (;;) {
            void *p = g_p.alloc(g_p.ctx, 0u, chunk);
            if (p == NULL || n == FILL_MAX) {
                break;
            }
            g_fill[n++] = p;
        }
    }
    return n;
}

static void pattern_set(void *p, size_t n, unsigned seed) {
    uint8_t *b = (uint8_t *)p;
    for (size_t i = 0; i < n; i++) {
        b[i] = (uint8_t)(seed * 31u + i * 7u + (i >> 8));
    }
}

static bool pattern_ok(const void *p, size_t n, unsigned seed) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) {
        if (b[i] != (uint8_t)(seed * 31u + i * 7u + (i >> 8))) {
            return false;
        }
    }
    return true;
}

// ---- tests ------------------------------------------------------------------------------------

TEST init_needs_wl_mem_and_a_live_tag(void) {
    (void)wl_mem_shutdown();
    WlOverflow o;
    memset(&o, 0, sizeof o);
    ASSERT_EQ(WL_ERR_STATE, wl_overflow_init(&o, WL_TAG_ASSET)); // wl_mem not initialised
    setup(false, true);
    ASSERT_EQ(WL_ERR_ARG, wl_overflow_init(&g_o, WL_MEM_MAX_TAGS));
    ASSERT_EQ(WL_ERR_STATE, wl_overflow_init(&g_o, TAG_COUNT)); // a tag the config does not define
    ASSERT_EQ(WL_OK, wl_overflow_init(&g_o, TAG_SPILL));
    ASSERT_EQ(WL_ERR_STATE, wl_overflow_init(&g_o, TAG_SPILL)); // armed already
    wl_overflow_stop(&g_o);
    ASSERT(!g_o.armed);
    ASSERT(!wl_overflow_owns(&g_o, g_fake_memory));
    ASSERT_EQ(WL_OK, wl_overflow_init(&g_o, TAG_SPILL)); // and again after a stop
    PASS();
}

TEST disarmed_goes_to_the_primary_alone(void) {
    setup(false, true);
    unsigned n = fill_primary(1024u);
    ASSERT(n > 40u);
    ASSERT_EQ(NULL, wl_overflow_malloc(&g_o, &g_p, 1024u)); // full, and nowhere else to go
    ASSERT_EQ(1u, g_o.stats.fails);
    ASSERT_EQ(0u, g_o.stats.allocs);
    void *z = wl_overflow_malloc(&g_o, &g_p, 0u);
    ASSERT_EQ(NULL, z); // a zero-size request never goes to the overflow heap
    ASSERT_EQ(1u, g_o.stats.fails);
    for (unsigned i = 0; i < n; i++) {
        wl_overflow_free(&g_o, &g_p, g_fill[i]);
    }
    void *p = wl_overflow_malloc(&g_o, &g_p, 100u);
    ASSERT(p != NULL && !wl_overflow_owns(&g_o, p));
    wl_overflow_free(&g_o, &g_p, p);
    ASSERT_EQ(0u, g_fake.foreign);
    PASS();
}

TEST malloc_spills_when_the_primary_is_full(void) {
    setup(true, true);
    unsigned n = fill_primary(1024u);
    uint8_t *a = wl_overflow_malloc(&g_o, &g_p, 5000u);
    ASSERT(a != NULL);
    ASSERT(wl_overflow_owns(&g_o, a));
    ASSERT_EQ(0u, (unsigned)((uintptr_t)a & 31u));
    ASSERT(wl_overflow_usable_size(&g_o, &g_p, a) >= 5000u);
    pattern_set(a, 5000u, 1u);
    uint8_t *b = wl_overflow_malloc(&g_o, &g_p, 17u);
    ASSERT(b != NULL && wl_overflow_owns(&g_o, b));
    ASSERT(b + 17u <= a || a + 5000u <= b); // disjoint
    ASSERT(pattern_ok(a, 5000u, 1u));
    WlOverflowStats st;
    wl_overflow_stats(&g_o, &st);
    ASSERT_EQ(2u, st.live);
    ASSERT_EQ(2u, st.allocs);
    ASSERT_EQ(5017u, st.bytes);
    ASSERT_EQ(0u, st.fails);
    ASSERT_EQ(2u, wl_mem_live_blocks(TAG_SPILL));
    wl_overflow_free(&g_o, &g_p, a);
    wl_overflow_free(&g_o, &g_p, b);
    wl_overflow_stats(&g_o, &st);
    ASSERT_EQ(0u, st.live);
    ASSERT_EQ(2u, st.frees);
    ASSERT_EQ(0u, st.bytes);
    ASSERT_EQ(5017u, st.peak_bytes);
    ASSERT_EQ(0u, wl_mem_live_blocks(TAG_SPILL));
    for (unsigned i = 0; i < n; i++) {
        wl_overflow_free(&g_o, &g_p, g_fill[i]);
    }
    ASSERT_EQ(0u, g_fake.foreign);
    ASSERT(wl_mem_check_all());
    PASS();
}

TEST memory_freed_in_the_primary_is_used_first(void) {
    setup(true, true);
    unsigned n = fill_primary(1024u);
    void *spilled = wl_overflow_malloc(&g_o, &g_p, 1024u);
    ASSERT(wl_overflow_owns(&g_o, spilled));
    wl_overflow_free(&g_o, &g_p, g_fill[0]); // a hole in the primary
    void *p = wl_overflow_malloc(&g_o, &g_p, 1024u);
    ASSERT(p != NULL && !wl_overflow_owns(&g_o, p));
    g_fill[0] = p;
    wl_overflow_free(&g_o, &g_p, spilled);
    for (unsigned i = 0; i < n; i++) {
        wl_overflow_free(&g_o, &g_p, g_fill[i]);
    }
    ASSERT_EQ(0u, g_fake.foreign);
    PASS();
}

TEST calloc_zeroes_overflow_blocks_and_checks_the_product(void) {
    setup(true, true);
    // dirty the spill heap first, so a block that is not cleared shows
    void *dirt = wl_mem_alloc_at(TAG_SPILL, SPILL_BYTES / 2u, 32u, __FILE__, __LINE__);
    ASSERT(dirt != NULL);
    memset(dirt, 0xEE, SPILL_BYTES / 2u);
    wl_mem_free(dirt);
    unsigned n = fill_primary(1024u);
    uint8_t *z = wl_overflow_calloc(&g_o, &g_p, 100u, 40u);
    ASSERT(z != NULL && wl_overflow_owns(&g_o, z));
    for (unsigned i = 0; i < 4000u; i++) {
        ASSERT_EQ(0u, z[i]);
    }
    ASSERT_EQ(NULL, wl_overflow_calloc(&g_o, &g_p, SIZE_MAX / 2u, 4u)); // product overflows
    ASSERT_EQ(NULL, wl_overflow_calloc(&g_o, &g_p, 3u, SIZE_MAX / 2u));
    wl_overflow_free(&g_o, &g_p, z);
    for (unsigned i = 0; i < n; i++) {
        wl_overflow_free(&g_o, &g_p, g_fill[i]);
    }
    // primary calloc through the emulation (this primary has no calloc)
    uint8_t *c = wl_overflow_calloc(&g_o, &g_p, 8u, 8u);
    ASSERT(c != NULL && !wl_overflow_owns(&g_o, c));
    for (unsigned i = 0; i < 64u; i++) {
        ASSERT_EQ(0u, c[i]);
    }
    wl_overflow_free(&g_o, &g_p, c);
    PASS();
}

TEST memalign_serves_any_power_of_two(void) {
    setup(true, true);
    static const size_t aligns[] = {1, 2, 8, 16, 32, 64, 128, 4096};
    for (size_t i = 0; i < WL_ARRAY_COUNT(aligns); i++) {
        // with room in the primary, up to 32 comes from it; above, the fake refuses and it spills
        void *p = wl_overflow_memalign(&g_o, &g_p, aligns[i], 200u);
        ASSERT(p != NULL);
        ASSERT_EQ(0u, (unsigned)((uintptr_t)p & (aligns[i] - 1u)));
        ASSERT_EQ(aligns[i] > 32u, wl_overflow_owns(&g_o, p));
        pattern_set(p, 200u, (unsigned)i);
        ASSERT(wl_overflow_usable_size(&g_o, &g_p, p) >= 200u);
        wl_overflow_free(&g_o, &g_p, p);
    }
    // a full primary spills small alignments too, and they come out 32-aligned at least
    unsigned n = fill_primary(1024u);
    for (size_t i = 0; i < WL_ARRAY_COUNT(aligns); i++) {
        uint8_t *p = wl_overflow_memalign(&g_o, &g_p, aligns[i], 300u);
        ASSERT(p != NULL && wl_overflow_owns(&g_o, p));
        ASSERT_EQ(0u, (unsigned)((uintptr_t)p & (aligns[i] - 1u)));
        ASSERT_EQ(0u, (unsigned)((uintptr_t)p & 31u));
        pattern_set(p, 300u, (unsigned)i);
        ASSERT(pattern_ok(p, 300u, (unsigned)i));
        ASSERT(wl_overflow_usable_size(&g_o, &g_p, p) >= 300u);
        wl_overflow_free(&g_o, &g_p, p);
    }
    ASSERT_EQ(NULL, wl_overflow_memalign(&g_o, &g_p, 48u, 100u)); // not a power of two
    ASSERT_EQ(NULL, wl_overflow_memalign(&g_o, &g_p, 3u, 100u));
    ASSERT_EQ(0u, wl_mem_live_blocks(TAG_SPILL));
    for (unsigned i = 0; i < n; i++) {
        wl_overflow_free(&g_o, &g_p, g_fill[i]);
    }
    ASSERT_EQ(0u, g_fake.foreign);
    ASSERT(wl_mem_check_all());
    PASS();
}

static int realloc_cases(bool with_realloc) {
    setup(true, with_realloc);
    // NULL: malloc; size 0: free and NULL
    uint8_t *p = wl_overflow_realloc(&g_o, &g_p, NULL, 100u);
    ASSERT(p != NULL && !wl_overflow_owns(&g_o, p));
    pattern_set(p, 100u, 3u);
    p = wl_overflow_realloc(&g_o, &g_p, p, 300u); // grows in the primary
    ASSERT(p != NULL && !wl_overflow_owns(&g_o, p));
    ASSERT(pattern_ok(p, 100u, 3u));
    ASSERT_EQ(NULL, wl_overflow_realloc(&g_o, &g_p, p, 0u));
    ASSERT_EQ(0u, g_fake.heap.live);

    // a primary block that cannot grow moves to the overflow heap, contents kept
    p = wl_overflow_malloc(&g_o, &g_p, 200u);
    ASSERT(p != NULL);
    pattern_set(p, 200u, 4u);
    unsigned n = fill_primary(512u);
    uint8_t *q = wl_overflow_realloc(&g_o, &g_p, p, 20000u);
    ASSERT(q != NULL && wl_overflow_owns(&g_o, q));
    ASSERT(pattern_ok(q, 200u, 4u));
    WlOverflowStats st;
    wl_overflow_stats(&g_o, &st);
    ASSERT_EQ(1u, st.moved_in);
    ASSERT_EQ(1u, st.live);

    // an overflow block keeps its pointer while the size fits, in either direction
    uint8_t *r = wl_overflow_realloc(&g_o, &g_p, q, 100u);
    ASSERT_EQ(q, r);
    ASSERT(pattern_ok(r, 100u, 4u));
    ASSERT_EQ(r, wl_overflow_realloc(&g_o, &g_p, r, 20000u));
    wl_overflow_stats(&g_o, &st);
    ASSERT_EQ(20000u, st.bytes);

    // one that must grow moves to the overflow heap while the primary is full ...
    pattern_set(r, 20000u, 5u);
    uint8_t *s = wl_overflow_realloc(&g_o, &g_p, r, 60000u);
    ASSERT(s != NULL && wl_overflow_owns(&g_o, s));
    ASSERT(pattern_ok(s, 20000u, 5u));
    wl_overflow_stats(&g_o, &st);
    ASSERT_EQ(1u, st.moved_out);
    ASSERT_EQ(1u, st.live);
    ASSERT_EQ(60000u, st.bytes);

    // ... and back to the primary when it has room again
    for (unsigned i = 0; i < n; i++) {
        wl_overflow_free(&g_o, &g_p, g_fill[i]);
    }
    uint8_t *t = wl_overflow_realloc(&g_o, &g_p, s, 62000u);
    ASSERT(t != NULL);
    // (the fake primary holds 64 KiB: 62000 fits only if the heap is empty, which it is now)
    ASSERT(!wl_overflow_owns(&g_o, t));
    ASSERT(pattern_ok(t, 20000u, 5u));
    wl_overflow_stats(&g_o, &st);
    ASSERT_EQ(0u, st.live);
    ASSERT_EQ(2u, st.moved_out);
    wl_overflow_free(&g_o, &g_p, t);

    // both heaps full: NULL, and the block is untouched
    uint8_t *u = wl_overflow_malloc(&g_o, &g_p, 100u);
    pattern_set(u, 100u, 6u);
    n = fill_primary(512u);
    uint8_t *big = wl_overflow_malloc(&g_o, &g_p, SPILL_BYTES - 4096u);
    ASSERT(big != NULL && wl_overflow_owns(&g_o, big));
    uint32_t calls = g_fail.calls;
    ASSERT_EQ(NULL, wl_overflow_realloc(&g_o, &g_p, u, 100000u));
    ASSERT_EQ(calls + 1u, g_fail.calls);
    ASSERT_STR_EQ("realloc", g_fail.what);
    ASSERT_EQ(100000u, g_fail.size);
    ASSERT(pattern_ok(u, 100u, 6u));
    ASSERT_EQ(NULL, wl_overflow_realloc(&g_o, &g_p, big, SPILL_BYTES * 2u));
    wl_overflow_free(&g_o, &g_p, big);
    wl_overflow_free(&g_o, &g_p, u);
    for (unsigned i = 0; i < n; i++) {
        wl_overflow_free(&g_o, &g_p, g_fill[i]);
    }
    ASSERT_EQ(0u, g_fake.foreign);
    ASSERT_EQ(0u, g_fake.heap.live);
    ASSERT_EQ(0u, wl_mem_live_blocks(TAG_SPILL));
    ASSERT(wl_mem_check_all());
    return GREATEST_TEST_RES_PASS;
}

TEST realloc_moves_between_heaps(void) {
    int r = realloc_cases(true);
    if (r != GREATEST_TEST_RES_PASS) {
        return r;
    }
    PASS();
}

TEST realloc_is_emulated_for_a_primary_without_one(void) {
    int r = realloc_cases(false);
    if (r != GREATEST_TEST_RES_PASS) {
        return r;
    }
    PASS();
}

TEST an_aligned_overflow_block_reallocs_in_place_within_its_capacity(void) {
    setup(true, true);
    uint8_t *p = wl_overflow_alloc(&g_o, 4096u, 100u);
    ASSERT(p != NULL && ((uintptr_t)p & 4095u) == 0u);
    size_t cap = wl_overflow_usable_size(&g_o, &g_p, p);
    ASSERT(cap >= 100u);
    pattern_set(p, 100u, 9u);
    ASSERT_EQ(p, wl_overflow_realloc(&g_o, &g_p, p, cap)); // the slack counts as capacity
    ASSERT(pattern_ok(p, 100u, 9u));
    uint8_t *q = wl_overflow_realloc(&g_o, &g_p, p, cap + 1u); // moves; malloc's alignment only
    ASSERT(q != NULL && q != p);
    ASSERT(pattern_ok(q, 100u, 9u));
    wl_overflow_free(&g_o, &g_p, q);
    ASSERT_EQ(0u, wl_mem_live_blocks(TAG_SPILL));
    PASS();
}

TEST bad_frees_are_refused_and_counted(void) {
    setup(true, true);
    unsigned n = fill_primary(1024u);
    uint8_t *a = wl_overflow_malloc(&g_o, &g_p, 1000u);
    ASSERT(wl_overflow_owns(&g_o, a));
    wl_overflow_free(&g_o, &g_p, a + 64u); // inside a block
    ASSERT_EQ(1u, g_o.stats.bad_frees);
    wl_overflow_free(&g_o, &g_p, a);
    ASSERT_EQ(0u, g_o.stats.live);
    wl_overflow_free(&g_o, &g_p, a); // double free
    ASSERT_EQ(2u, g_o.stats.bad_frees);
    ASSERT_EQ(NULL, wl_overflow_realloc(&g_o, &g_p, a, 10u)); // realloc of a freed block
    ASSERT_EQ(3u, g_o.stats.bad_frees);
    ASSERT_EQ(0u, wl_overflow_usable_size(&g_o, &g_p, a));
    wl_overflow_free(&g_o, &g_p, NULL); // no-op
    ASSERT_EQ(3u, g_o.stats.bad_frees);
    ASSERT_EQ(0u, wl_mem_bad_frees()); // wl_mem was never handed a bad pointer
    ASSERT_EQ(0u, g_fake.foreign);     // nor was the primary
    ASSERT(wl_mem_check_all());
    for (unsigned i = 0; i < n; i++) {
        wl_overflow_free(&g_o, &g_p, g_fill[i]);
    }
    PASS();
}

TEST exhaustion_reports_through_the_hook_once_per_request(void) {
    setup(true, true);
    unsigned n = fill_primary(1024u);
    void *big = wl_overflow_malloc(&g_o, &g_p, SPILL_BYTES - 1024u);
    ASSERT(big != NULL);
    ASSERT_EQ(0u, g_fail.calls);
    ASSERT_EQ(NULL, wl_overflow_malloc(&g_o, &g_p, 4096u));
    ASSERT_EQ(1u, g_fail.calls);
    ASSERT_STR_EQ("malloc", g_fail.what);
    ASSERT_EQ(4096u, g_fail.size);
    ASSERT_EQ(NULL, wl_overflow_calloc(&g_o, &g_p, 4u, 1024u));
    ASSERT_STR_EQ("calloc", g_fail.what);
    ASSERT_EQ(NULL, wl_overflow_memalign(&g_o, &g_p, 64u, 4096u));
    ASSERT_STR_EQ("memalign", g_fail.what);
    ASSERT_EQ(64u, g_fail.align);
    ASSERT_EQ(3u, g_fail.calls);
    ASSERT_EQ(3u, g_o.stats.fails);
    ASSERT_EQ(NULL, wl_overflow_malloc(&g_o, &g_p, SIZE_MAX)); // absurd sizes just fail
    wl_overflow_free(&g_o, &g_p, big);
    for (unsigned i = 0; i < n; i++) {
        wl_overflow_free(&g_o, &g_p, g_fill[i]);
    }
    // (the failure path may allocate: a hook that mallocs does not recurse)
    PASS();
}

static WlOverflow *g_reenter_o;
static const WlOverflowPrimary *g_reenter_p;
static uint32_t g_reenter_depth;
static void on_fail_reenters(void *user, const char *what, size_t size, size_t align) {
    (void)user;
    (void)what;
    (void)size;
    (void)align;
    g_reenter_depth++;
    (void)wl_overflow_malloc(g_reenter_o, g_reenter_p, SPILL_BYTES * 4u); // fails again
}

TEST a_failing_report_does_not_report_itself(void) {
    setup(true, true);
    unsigned n = fill_primary(1024u);
    g_reenter_o = &g_o;
    g_reenter_p = &g_p;
    g_reenter_depth = 0;
    wl_overflow_set_fail_hook(&g_o, on_fail_reenters, NULL);
    ASSERT_EQ(NULL, wl_overflow_malloc(&g_o, &g_p, SPILL_BYTES * 2u));
    ASSERT_EQ(1u, g_reenter_depth);
    ASSERT_EQ(2u, g_o.stats.fails);                                    // both counted, one reported
    ASSERT_EQ(NULL, wl_overflow_malloc(&g_o, &g_p, SPILL_BYTES * 2u)); // reports again afterwards
    ASSERT_EQ(2u, g_reenter_depth);
    for (unsigned i = 0; i < n; i++) {
        wl_overflow_free(&g_o, &g_p, g_fill[i]);
    }
    PASS();
}

TEST usage_report_combines_both_heaps(void) {
    setup(true, true);
    WlOverflowUsage u;
    wl_overflow_usage(&g_o, &g_p, &u);
    ASSERT(u.primary_known && u.overflow_armed);
    ASSERT_EQ(0u, u.primary_used);
    ASSERT_EQ((size_t)PRIMARY_BYTES, u.primary_free);
    ASSERT_EQ((size_t)SPILL_BYTES, u.overflow_budget);
    ASSERT_EQ(0u, u.overflow_used);
    ASSERT_EQ(u.primary_free + (size_t)SPILL_BYTES, u.free_total);
    unsigned n = fill_primary(1024u);
    void *a = wl_overflow_malloc(&g_o, &g_p, 10000u);
    ASSERT(wl_overflow_owns(&g_o, a));
    wl_overflow_usage(&g_o, &g_p, &u);
    ASSERT(u.primary_used > 40u * 1024u);
    ASSERT(u.overflow_used >= 10000u + 64u && u.overflow_used < 10000u + 256u);
    ASSERT_EQ(1u, u.overflow_live);
    ASSERT_EQ(u.overflow_budget - u.overflow_used, u.overflow_free);
    wl_overflow_log_usage(&g_o, &g_p);
    wl_overflow_free(&g_o, &g_p, a);
    wl_overflow_usage(&g_o, &g_p, &u);
    ASSERT_EQ(0u, u.overflow_used);
    ASSERT(u.overflow_peak >= 10000u);
    for (unsigned i = 0; i < n; i++) {
        wl_overflow_free(&g_o, &g_p, g_fill[i]);
    }
    WlOverflow off;
    memset(&off, 0, sizeof off);
    wl_overflow_usage(&off, NULL, &u); // disarmed, no primary report
    ASSERT(!u.primary_known && !u.overflow_armed);
    PASS();
}

typedef struct {
    uint8_t *ptr;
    size_t size;
    unsigned seed;
} Slot;

TEST random_operations_keep_every_block_intact(void) {
    setup(true, true);
    enum { SLOTS = 48 };
    static Slot slots[SLOTS];
    memset(slots, 0, sizeof slots);
    uint32_t rng = 12345u;
#define NEXT() (rng = rng * 1664525u + 1013904223u, rng >> 8)
    for (unsigned step = 0; step < 6000u; step++) {
        Slot *s = &slots[NEXT() % SLOTS];
        unsigned op = NEXT() % 8u;
        size_t size = 1u + NEXT() % 6000u;
        if (NEXT() % 10u == 0u) {
            size += 8000u; // an occasional big one
        }
        if (s->ptr != NULL) {
            ASSERT(pattern_ok(s->ptr, s->size, s->seed));
        }
        if (op < 3u) { // malloc into an empty slot
            if (s->ptr == NULL) {
                s->ptr = wl_overflow_malloc(&g_o, &g_p, size);
                s->size = s->ptr != NULL ? size : 0u;
            }
        } else if (op < 5u) { // realloc
            uint8_t *n = wl_overflow_realloc(&g_o, &g_p, s->ptr, size);
            if (n != NULL) {
                size_t keep =
                    s->ptr != NULL && s->size < size ? s->size : (s->ptr != NULL ? size : 0u);
                if (s->ptr != NULL) {
                    ASSERT(pattern_ok(n, keep, s->seed));
                }
                s->ptr = n;
                s->size = size;
                s->seed = step;
                pattern_set(n, size, step);
                continue;
            }
        } else if (op == 5u) { // memalign
            if (s->ptr == NULL) {
                size_t align = (size_t)1 << (NEXT() % 9u);
                s->ptr = wl_overflow_memalign(&g_o, &g_p, align, size);
                if (s->ptr != NULL) {
                    ASSERT_EQ(0u, (unsigned)((uintptr_t)s->ptr & (align - 1u)));
                }
                s->size = s->ptr != NULL ? size : 0u;
            }
        } else if (op == 6u) { // calloc
            if (s->ptr == NULL) {
                s->ptr = wl_overflow_calloc(&g_o, &g_p, 1u, size);
                if (s->ptr != NULL) {
                    for (size_t i = 0; i < size; i++) {
                        ASSERT_EQ(0u, s->ptr[i]);
                    }
                }
                s->size = s->ptr != NULL ? size : 0u;
            }
        } else { // free
            wl_overflow_free(&g_o, &g_p, s->ptr);
            s->ptr = NULL;
            s->size = 0u;
        }
        if (s->ptr != NULL && op != 4u && op != 3u) {
            s->seed = step;
            pattern_set(s->ptr, s->size, s->seed);
        }
    }
#undef NEXT
    WlOverflowStats st;
    wl_overflow_stats(&g_o, &st);
    ASSERT(st.allocs > 50u); // the run did spill
    ASSERT(g_o.stats.moved_in + g_o.stats.moved_out > 0u);
    for (unsigned i = 0; i < SLOTS; i++) {
        if (slots[i].ptr != NULL) {
            ASSERT(pattern_ok(slots[i].ptr, slots[i].size, slots[i].seed));
            wl_overflow_free(&g_o, &g_p, slots[i].ptr);
        }
    }
    wl_overflow_stats(&g_o, &st);
    ASSERT_EQ(0u, st.live);
    ASSERT_EQ(0u, st.bytes);
    ASSERT_EQ(0u, st.bad_frees);
    ASSERT_EQ(0u, g_fake.foreign);
    ASSERT_EQ(0u, g_fake.heap.live);
    ASSERT_EQ(0u, wl_mem_live_blocks(TAG_SPILL));
    ASSERT(wl_heap_check(&g_fake.heap));
    ASSERT(wl_mem_check_all());
    PASS();
}

SUITE(heap_overflow_suite) {
    RUN_TEST(init_needs_wl_mem_and_a_live_tag);
    RUN_TEST(disarmed_goes_to_the_primary_alone);
    RUN_TEST(malloc_spills_when_the_primary_is_full);
    RUN_TEST(memory_freed_in_the_primary_is_used_first);
    RUN_TEST(calloc_zeroes_overflow_blocks_and_checks_the_product);
    RUN_TEST(memalign_serves_any_power_of_two);
    RUN_TEST(realloc_moves_between_heaps);
    RUN_TEST(realloc_is_emulated_for_a_primary_without_one);
    RUN_TEST(an_aligned_overflow_block_reallocs_in_place_within_its_capacity);
    RUN_TEST(bad_frees_are_refused_and_counted);
    RUN_TEST(exhaustion_reports_through_the_hook_once_per_request);
    RUN_TEST(a_failing_report_does_not_report_itself);
    RUN_TEST(usage_report_combines_both_heaps);
    RUN_TEST(random_operations_keep_every_block_intact);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(heap_overflow_suite);
    GREATEST_MAIN_END();
}
