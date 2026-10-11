// SPDX-License-Identifier: MIT
// The Wii's newlib wrap layer (wl/platform/wii/heap_wrap_wii.c, compiled into this test; it
// includes no libogc header) against a fake newlib: __real_* names that are a small wl_heap and
// that call each other the way newlib's objects do (malloc through the wrapped _malloc_r, calloc,
// realloc and memalign through _malloc_r with the assumption that what it returns is theirs).
// The fake counts what newlib would corrupt: a pointer it did not allocate given to it
// ("foreign"), and an overflow block handed to its internal malloc ("violations"). Byte-order
// independent: also runs on the big-endian build.
#include <stdint.h>
#include <string.h>

#include "greatest.h"
#include "wl/platform/heap_overflow.h"
#include "wl/platform/mem_heap.h"

struct _reent;

void *__wrap_malloc(size_t size);
void *__wrap_calloc(size_t count, size_t size);
void *__wrap_realloc(void *pointer, size_t size);
void __wrap_free(void *pointer);
void *__wrap_memalign(size_t alignment, size_t size);
void *__wrap_aligned_alloc(size_t alignment, size_t size);
size_t __wrap_malloc_usable_size(void *pointer);
void *__wrap__malloc_r(struct _reent *reent, size_t size);
void *__wrap__calloc_r(struct _reent *reent, size_t count, size_t size);
void *__wrap__realloc_r(struct _reent *reent, void *pointer, size_t size);
void __wrap__free_r(struct _reent *reent, void *pointer);
void *__wrap__memalign_r(struct _reent *reent, size_t alignment, size_t size);
size_t __wrap__malloc_usable_size_r(struct _reent *reent, void *pointer);

enum { TAG_SPILL = WL_TAG_USER, TAG_COUNT };
#define NEWLIB_BYTES (64u * 1024u)
#define SPILL_BYTES (256u * 1024u)

// ---- the fake newlib --------------------------------------------------------------------------

static _Alignas(32) uint8_t g_memory[NEWLIB_BYTES];
static WlHeap g_heap;
static int g_lock_depth;
static int g_lock_max;
static uint32_t g_foreign;
static uint32_t g_violations;

void __malloc_lock(struct _reent *reent);
void __malloc_unlock(struct _reent *reent);

void __malloc_lock(struct _reent *reent) {
    (void)reent;
    if (++g_lock_depth > g_lock_max) {
        g_lock_max = g_lock_depth;
    }
}

void __malloc_unlock(struct _reent *reent) {
    (void)reent;
    g_lock_depth--;
}

static bool in_newlib(const void *p) {
    return (const uint8_t *)p >= g_memory && (const uint8_t *)p < g_memory + sizeof g_memory;
}

void *__real__malloc_r(struct _reent *reent, size_t size);
void *__real__calloc_r(struct _reent *reent, size_t count, size_t size);
void *__real__realloc_r(struct _reent *reent, void *pointer, size_t size);
void __real__free_r(struct _reent *reent, void *pointer);
void *__real__memalign_r(struct _reent *reent, size_t alignment, size_t size);
size_t __real__malloc_usable_size_r(struct _reent *reent, void *pointer);
void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *pointer, size_t size);
void __real_free(void *pointer);
void *__real_memalign(size_t alignment, size_t size);
size_t __real_malloc_usable_size(void *pointer);

void *__real__malloc_r(struct _reent *reent, size_t size) {
    __malloc_lock(reent);
    void *p = wl_heap_alloc(&g_heap, size, 8u, "newlib", 0u, 0u);
    __malloc_unlock(reent);
    return p;
}

void __real__free_r(struct _reent *reent, void *pointer) {
    __malloc_lock(reent);
    if (!wl_heap_free(&g_heap, pointer)) {
        g_foreign++;
    }
    __malloc_unlock(reent);
}

size_t __real__malloc_usable_size_r(struct _reent *reent, void *pointer) {
    (void)reent;
    size_t n = wl_heap_usable_size(&g_heap, pointer);
    if (n == 0u) {
        g_foreign++;
    }
    return n;
}

// (newlib's callocr.c: _malloc_r through the wrapped symbol, then it clears what it was given)
void *__real__calloc_r(struct _reent *reent, size_t count, size_t size) {
    __malloc_lock(reent);
    void *p = __wrap__malloc_r(reent, count * size);
    if (p != NULL) {
        if (!in_newlib(p)) {
            g_violations++;
        }
        memset(p, 0, count * size);
    }
    __malloc_unlock(reent);
    return p;
}

// (reallocr.c: grows in place if it can, else _malloc_r, copy, _free_r)
void *__real__realloc_r(struct _reent *reent, void *pointer, size_t size) {
    __malloc_lock(reent);
    void *result = NULL;
    if (!in_newlib(pointer)) {
        g_foreign++;
    } else {
        size_t have = wl_heap_usable_size(&g_heap, pointer);
        if (size <= have) {
            result = pointer;
        } else {
            void *n = __wrap__malloc_r(reent, size);
            if (n != NULL) {
                if (!in_newlib(n)) {
                    g_violations++;
                }
                memcpy(n, pointer, have);
                __wrap__free_r(reent, pointer);
                result = n;
            }
        }
    }
    __malloc_unlock(reent);
    return result;
}

// (mallocr.c's memalign: _malloc_r of more than asked, then it cuts the chunk to fit)
void *__real__memalign_r(struct _reent *reent, size_t alignment, size_t size) {
    __malloc_lock(reent);
    void *probe = alignment != 0u && (alignment & (alignment - 1u)) == 0u
                      ? __wrap__malloc_r(reent, size + alignment)
                      : NULL;
    void *result = NULL;
    if (probe != NULL) {
        if (!in_newlib(probe)) {
            g_violations++;
        }
        __wrap__free_r(reent, probe);
        if (alignment <= 32u && (alignment & (alignment - 1u)) == 0u) {
            result = wl_heap_alloc(&g_heap, size, alignment, "newlib", 0u, 0u);
        }
    }
    __malloc_unlock(reent);
    return result;
}

// (malloc.o and the others: the plain names call the reentrant ones, wrapped)
void *__real_malloc(size_t size) {
    return __wrap__malloc_r(NULL, size);
}

void *__real_calloc(size_t count, size_t size) {
    return __wrap__calloc_r(NULL, count, size);
}

void *__real_realloc(void *pointer, size_t size) {
    return __wrap__realloc_r(NULL, pointer, size);
}

void __real_free(void *pointer) {
    __wrap__free_r(NULL, pointer);
}

void *__real_memalign(size_t alignment, size_t size) {
    return __wrap__memalign_r(NULL, alignment, size);
}

size_t __real_malloc_usable_size(void *pointer) {
    return __wrap__malloc_usable_size_r(NULL, pointer);
}

// ---- fixtures ---------------------------------------------------------------------------------

static WlMemTagDef g_tags[TAG_COUNT];

static void setup(bool arm) {
    wl_heap_overflow_disable();
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
    memset(g_memory, 0, sizeof g_memory);
    wl_heap_init(&g_heap, g_memory, sizeof g_memory, "newlib");
    g_lock_depth = g_lock_max = 0;
    g_foreign = g_violations = 0;
    if (arm && wl_heap_overflow_enable(TAG_SPILL) != WL_OK) {
        abort();
    }
}

#define FILL_MAX 256u
static void *g_fill[FILL_MAX];
// Takes the fake newlib's whole heap with blocks of `chunk`, then smaller ones.
static unsigned fill_newlib(size_t chunk) {
    unsigned n = 0;
    for (; chunk >= 8u; chunk /= 4u) {
        while (n < FILL_MAX) {
            void *p = wl_heap_alloc(&g_heap, chunk, 8u, "fill", 0u, 0u);
            if (p == NULL) {
                break;
            }
            g_fill[n++] = p;
        }
    }
    return n;
}

static void release_fill(unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        if (g_fill[i] != NULL) {
            wl_heap_free(&g_heap, g_fill[i]);
            g_fill[i] = NULL;
        }
    }
}

#define CLEAN()                                                                                    \
    do {                                                                                           \
        ASSERT_EQ(0u, g_foreign);                                                                  \
        ASSERT_EQ(0u, g_violations);                                                               \
        ASSERT_EQ(0, g_lock_depth);                                                                \
        ASSERT(wl_mem_check_all());                                                                \
    } while (0)

// ---- tests ------------------------------------------------------------------------------------

TEST nothing_changes_until_it_is_enabled(void) {
    setup(false);
    ASSERT_EQ(WL_ERR_STATE, wl_heap_overflow_enable(WL_MEM_MAX_TAGS - 1u)); // no such heap
    unsigned n = fill_newlib(1024u);
    ASSERT(__wrap_malloc(1000u) == NULL); // full, and the overflow is not armed
    ASSERT(__wrap__malloc_r(NULL, 1000u) == NULL);
    release_fill(n);
    void *p = __wrap_malloc(1000u);
    ASSERT(p != NULL && in_newlib(p));
    __wrap_free(p);
    ASSERT_EQ(WL_OK, wl_heap_overflow_enable(TAG_SPILL));
    ASSERT_EQ(WL_ERR_STATE, wl_heap_overflow_enable(TAG_SPILL));
    wl_heap_overflow_disable();
    ASSERT(!wl_heap_overflow_owns(p));
    CLEAN();
    PASS();
}

TEST plain_names_spill_and_free_by_address(void) {
    setup(true);
    unsigned n = fill_newlib(1024u);
    uint8_t *a = __wrap_malloc(3000u);
    ASSERT(a != NULL && wl_heap_overflow_owns(a));
    memset(a, 0xA5, 3000u);
    uint8_t *c = __wrap_calloc(10u, 100u);
    ASSERT(c != NULL && wl_heap_overflow_owns(c));
    for (unsigned i = 0; i < 1000u; i++) {
        ASSERT_EQ(0u, c[i]);
    }
    uint8_t *m = __wrap_memalign(64u, 500u);
    ASSERT(m != NULL && wl_heap_overflow_owns(m) && ((uintptr_t)m & 63u) == 0u);
    uint8_t *al = __wrap_aligned_alloc(32u, 500u);
    ASSERT(al != NULL && wl_heap_overflow_owns(al) && ((uintptr_t)al & 31u) == 0u);
    ASSERT(__wrap_malloc_usable_size(a) >= 3000u);
    uint8_t *g = __wrap_realloc(a, 9000u); // grows: a new overflow block, contents kept
    ASSERT(g != NULL && wl_heap_overflow_owns(g));
    for (unsigned i = 0; i < 3000u; i++) {
        ASSERT_EQ(0xA5u, g[i]);
    }
    ASSERT(__wrap_realloc(g, 0u) == NULL);
    __wrap_free(c);
    __wrap_free(m);
    __wrap_free(al);
    __wrap_free(NULL);
    ASSERT_EQ(0u, wl_mem_live_blocks(TAG_SPILL));
    release_fill(n);
    // with room again, malloc is newlib's
    void *p = __wrap_malloc(2000u);
    ASSERT(p != NULL && in_newlib(p) && !wl_heap_overflow_owns(p));
    ASSERT(__wrap_malloc_usable_size(p) >= 2000u);
    __wrap_free(p);
    CLEAN();
    PASS();
}

TEST reentrant_names_spill_too(void) {
    setup(true);
    unsigned n = fill_newlib(1024u);
    // what libc itself allocates (fopen, strdup, ...) calls these directly
    uint8_t *a = __wrap__malloc_r(NULL, 2000u);
    ASSERT(a != NULL && wl_heap_overflow_owns(a));
    memset(a, 0x3C, 2000u);
    uint8_t *c = __wrap__calloc_r(NULL, 4u, 250u);
    ASSERT(c != NULL && wl_heap_overflow_owns(c));
    for (unsigned i = 0; i < 1000u; i++) {
        ASSERT_EQ(0u, c[i]);
    }
    uint8_t *m = __wrap__memalign_r(NULL, 128u, 100u);
    ASSERT(m != NULL && wl_heap_overflow_owns(m) && ((uintptr_t)m & 127u) == 0u);
    ASSERT(__wrap__malloc_usable_size_r(NULL, a) >= 2000u);
    uint8_t *g = __wrap__realloc_r(NULL, a, 7000u);
    ASSERT(g != NULL && wl_heap_overflow_owns(g));
    for (unsigned i = 0; i < 2000u; i++) {
        ASSERT_EQ(0x3Cu, g[i]);
    }
    __wrap__free_r(NULL, g);
    __wrap__free_r(NULL, c);
    __wrap__free_r(NULL, m);
    ASSERT_EQ(0u, wl_mem_live_blocks(TAG_SPILL));
    release_fill(n);
    CLEAN();
    PASS();
}

TEST newlibs_own_calloc_realloc_and_memalign_never_get_an_overflow_block(void) {
    setup(true);
    // leave room for a small block but not for what calloc, realloc and memalign ask internally
    unsigned n = fill_newlib(1024u);
    wl_heap_free(&g_heap, g_fill[0]);
    g_fill[0] = NULL;
    uint8_t *small = __wrap_malloc(200u);
    ASSERT(small != NULL && in_newlib(small));
    memset(small, 0x77, 200u);
    uint8_t *c = __wrap_calloc(5u, 1000u); // newlib's calloc fails inside; the wrapper then spills
    ASSERT(c != NULL && wl_heap_overflow_owns(c));
    for (unsigned i = 0; i < 5000u; i++) {
        ASSERT_EQ(0u, c[i]);
    }
    uint8_t *r = __wrap_realloc(small, 5000u); // newlib's realloc fails inside; the block moves
    ASSERT(r != NULL && wl_heap_overflow_owns(r));
    for (unsigned i = 0; i < 200u; i++) {
        ASSERT_EQ(0x77u, r[i]);
    }
    uint8_t *m = __wrap_memalign(16u, 3000u);
    ASSERT(m != NULL && wl_heap_overflow_owns(m));
    ASSERT_EQ(0u, g_violations); // newlib never saw an overflow block as its own
    ASSERT_EQ(0, g_lock_depth);
    __wrap_free(c);
    __wrap_free(r);
    __wrap_free(m);
    release_fill(n);
    ASSERT_EQ(0u, wl_mem_live_blocks(TAG_SPILL));
    CLEAN();
    PASS();
}

TEST a_newlib_realloc_that_fits_stays_in_newlib(void) {
    setup(true);
    uint8_t *p = __wrap_malloc(100u);
    ASSERT(p != NULL && in_newlib(p));
    memset(p, 0x42, 100u);
    uint8_t *q = __wrap_realloc(p, 30000u); // newlib can grow it: no overflow block needed
    ASSERT(q != NULL && in_newlib(q));
    for (unsigned i = 0; i < 100u; i++) {
        ASSERT_EQ(0x42u, q[i]);
    }
    WlOverflowStats st;
    wl_heap_overflow_stats(&st);
    ASSERT_EQ(0u, st.allocs);
    uint8_t *r = __wrap_realloc(q, 200000u); // nothing can: it spills
    ASSERT(r != NULL && wl_heap_overflow_owns(r));
    wl_heap_overflow_stats(&st);
    ASSERT_EQ(1u, st.moved_in);
    __wrap_free(r);
    CLEAN();
    PASS();
}

TEST failure_is_counted_and_blocks_stay_intact(void) {
    setup(true);
    unsigned n = fill_newlib(1024u);
    void *big = __wrap_malloc(SPILL_BYTES - 1024u);
    ASSERT(big != NULL && wl_heap_overflow_owns(big));
    ASSERT(__wrap_malloc(100000u) == NULL);
    ASSERT(__wrap_calloc(100u, 1000u) == NULL);
    ASSERT(__wrap_memalign(256u, 100000u) == NULL);
    ASSERT(__wrap__malloc_r(NULL, 100000u) == NULL);
    ASSERT(__wrap_memalign(48u, 100u) == NULL); // not a power of two
    WlOverflowStats st;
    wl_heap_overflow_stats(&st);
    ASSERT(st.fails >= 5u);
    WlOverflowUsage u;
    wl_heap_overflow_usage(&u);
    ASSERT(u.primary_known && u.overflow_armed);
    ASSERT(u.overflow_used > SPILL_BYTES - 2048u);
    wl_heap_overflow_log_usage();
    __wrap_free(big);
    release_fill(n);
    CLEAN();
    PASS();
}

TEST random_mixed_calls_keep_every_block_intact(void) {
    setup(true);
    typedef struct {
        uint8_t *ptr;
        size_t size;
        uint8_t fill;
    } Slot;
    enum { SLOTS = 40 };
    static Slot slots[SLOTS];
    memset(slots, 0, sizeof slots);
    uint32_t rng = 777u;
#define NEXT() (rng = rng * 1664525u + 1013904223u, rng >> 8)
    for (unsigned step = 0; step < 5000u; step++) {
        Slot *s = &slots[NEXT() % SLOTS];
        bool reent = NEXT() % 2u == 0u;
        size_t size = 1u + NEXT() % 5000u;
        if (s->ptr != NULL) {
            for (size_t i = 0; i < s->size; i++) {
                ASSERT_EQ(s->fill, s->ptr[i]);
            }
        }
        switch (NEXT() % 6u) {
        case 0:
            if (s->ptr == NULL) {
                s->ptr = reent ? __wrap__malloc_r(NULL, size) : __wrap_malloc(size);
                s->size = s->ptr != NULL ? size : 0u;
            }
            break;
        case 1:
            if (s->ptr == NULL) {
                s->ptr = reent ? __wrap__calloc_r(NULL, 1u, size) : __wrap_calloc(1u, size);
                s->size = s->ptr != NULL ? size : 0u;
                if (s->ptr != NULL) {
                    memset(s->ptr, 0, size);
                    s->fill = 0u;
                }
            }
            break;
        case 2: {
            size_t keep = s->ptr != NULL && s->size < size ? s->size : (s->ptr != NULL ? size : 0u);
            uint8_t *n =
                reent ? __wrap__realloc_r(NULL, s->ptr, size) : __wrap_realloc(s->ptr, size);
            if (n != NULL) {
                for (size_t i = 0; i < keep; i++) {
                    ASSERT_EQ(s->fill, n[i]);
                }
                s->ptr = n;
                s->size = size;
            }
            break;
        }
        case 3:
            if (s->ptr == NULL) {
                size_t align = (size_t)16 << (NEXT() % 4u);
                s->ptr =
                    reent ? __wrap__memalign_r(NULL, align, size) : __wrap_memalign(align, size);
                if (s->ptr != NULL) {
                    ASSERT_EQ(0u, (unsigned)((uintptr_t)s->ptr & (align - 1u)));
                }
                s->size = s->ptr != NULL ? size : 0u;
            }
            break;
        default:
            if (reent) {
                __wrap__free_r(NULL, s->ptr);
            } else {
                __wrap_free(s->ptr);
            }
            s->ptr = NULL;
            s->size = 0u;
            break;
        }
        if (s->ptr != NULL) {
            s->fill = (uint8_t)step;
            memset(s->ptr, s->fill, s->size);
            ASSERT(__wrap_malloc_usable_size(s->ptr) >= s->size);
        }
    }
#undef NEXT
    WlOverflowStats st;
    wl_heap_overflow_stats(&st);
    ASSERT(st.allocs > 20u);
    for (unsigned i = 0; i < SLOTS; i++) {
        __wrap_free(slots[i].ptr);
    }
    ASSERT_EQ(0u, wl_mem_live_blocks(TAG_SPILL));
    ASSERT_EQ(0u, g_heap.live);
    ASSERT(wl_heap_check(&g_heap));
    CLEAN();
    PASS();
}

// (last: once on, the accounting stays on for the program's life)
TEST the_accounting_by_site_follows_every_name(void) {
    static _Alignas(8) uint8_t storage[16u * 1024u];
    WlHeapSitesTotals t;
    setup(true);
    ASSERT_EQ(WL_OK, wl_heap_overflow_track_sites(storage, sizeof storage, 32u, 512u));
    ASSERT_EQ(WL_ERR_STATE, wl_heap_overflow_track_sites(storage, sizeof storage, 32u, 512u));
    unsigned n = fill_newlib(4096u); // what follows spills, so both heaps' blocks are counted
    uint8_t *a = __wrap_malloc(3000u);
    uint8_t *b = __wrap__malloc_r(NULL, 100u);
    uint8_t *c = __wrap_calloc(10u, 200u);
    uint8_t *d = __wrap__memalign_r(NULL, 64u, 5000u);
    uint8_t *e = __wrap_aligned_alloc(32u, 40u);
    ASSERT(a != NULL && b != NULL && c != NULL && d != NULL && e != NULL);
    wl_heap_overflow_site_totals(&t);
    ASSERT_EQ(3u, t.tracked_blocks);
    ASSERT(t.tracked_bytes >= 3000u + 2000u + 5000u);
    ASSERT_EQ(2u, t.small_blocks[0] + t.small_blocks[1]);
    a = __wrap_realloc(a, 9000u);
    b = __wrap__realloc_r(NULL, b, 2000u); // small to tracked
    ASSERT(a != NULL && b != NULL);
    ASSERT(__wrap_realloc(c, 1u << 30) == NULL); // fails: c stays, and stays counted
    wl_heap_overflow_site_totals(&t);
    ASSERT_EQ(4u, t.tracked_blocks);
    ASSERT(t.tracked_bytes >= 9000u + 2000u + 2000u + 5000u);
    wl_heap_overflow_log_sites(4u);
    __wrap_free(a);
    __wrap__free_r(NULL, b);
    __wrap_free(c);
    __wrap_free(d);
    __wrap_free(e);
    wl_heap_overflow_site_totals(&t);
    ASSERT_EQ(0u, t.tracked_blocks);
    ASSERT_EQ(0u, t.tracked_bytes);
    ASSERT_EQ(0u, t.small_blocks[0] + t.small_blocks[1] + t.small_blocks[2] + t.small_blocks[3]);
    ASSERT_EQ(0u, t.untracked);
    release_fill(n);
    CLEAN();
    PASS();
}

SUITE(heap_wrap_suite) {
    RUN_TEST(nothing_changes_until_it_is_enabled);
    RUN_TEST(plain_names_spill_and_free_by_address);
    RUN_TEST(reentrant_names_spill_too);
    RUN_TEST(newlibs_own_calloc_realloc_and_memalign_never_get_an_overflow_block);
    RUN_TEST(a_newlib_realloc_that_fits_stays_in_newlib);
    RUN_TEST(failure_is_counted_and_blocks_stay_intact);
    RUN_TEST(random_mixed_calls_keep_every_block_intact);
    RUN_TEST(the_accounting_by_site_follows_every_name);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(heap_wrap_suite);
    GREATEST_MAIN_END();
}
