// SPDX-License-Identifier: MIT
// Memory system (wl/platform/mem.c) on the host backend: built-in and application tags,
// budgets, frame and scene arenas, leak report, overlay rows, configuration checks.
#include "wl/platform/mem.h"

#include <stdint.h>
#include <string.h>

#include "greatest.h"

// Two application tags on top of the built-in ones.
enum { TAG_SCRIPT = WL_TAG_USER, TAG_SAVE, TAG_COUNT };

static WlMemTagDef g_tags[TAG_COUNT];

static const WlMemConfig *test_config(void) {
    static WlMemConfig cfg;
    memcpy(g_tags, wl_mem_builtin_tags(), WL_TAG_BUILTIN_COUNT * sizeof g_tags[0]);
    g_tags[TAG_SCRIPT] = (WlMemTagDef){"script", WL_MEM1, 2u * 1024 * 1024};
    g_tags[TAG_SAVE] = (WlMemTagDef){"save", WL_MEM2, 4u * 1024 * 1024};
    wl_mem_config_default(&cfg);
    cfg.tags = g_tags;
    cfg.tag_count = TAG_COUNT;
    return &cfg;
}

#define SETUP()                                                                                    \
    do {                                                                                           \
        (void)wl_mem_shutdown();                                                                   \
        ASSERT_EQ(WL_OK, wl_mem_init(test_config()));                                              \
    } while (0)

TEST init_state_and_double_init(void) {
    SETUP();
    ASSERT(wl_mem_ready());
    ASSERT_EQ(WL_ERR_STATE, wl_mem_init(NULL));
    ASSERT_EQ((uint32_t)TAG_COUNT, wl_mem_tag_count());
    ASSERT_STR_EQ("save", wl_mem_tag_name(TAG_SAVE));
    ASSERT_EQ(0u, wl_mem_shutdown());
    ASSERT(!wl_mem_ready());
    PASS();
}

TEST rows_match_the_budget_table(void) {
    SETUP();
    WlMemRow r[WL_MEM_MAX_ROWS];
    ASSERT_EQ(TAG_COUNT + 2u, wl_mem_rows(r, WL_MEM_MAX_ROWS));
    size_t m1 = 0, m2 = 0;
    for (unsigned i = 0; i < TAG_COUNT + 2u; i++) {
        (r[i].region == WL_MEM1 ? &m1 : &m2)[0] += r[i].budget;
    }
    ASSERT_EQ(11u * 1024 * 1024, m1); // 0.5+1+1+1+0.5 built in, 2 script, frames 2, scene 3
    ASSERT_EQ(42u * 1024 * 1024, m2); // 30+6+2 built in, 4 save
    ASSERT_STR_EQ("frame", r[TAG_COUNT].name);
    ASSERT_STR_EQ("asset", r[WL_TAG_ASSET].name);
    ASSERT_EQ((size_t)(30u * 1024 * 1024), r[WL_TAG_ASSET].budget);
    wl_mem_log_budgets();
    wl_mem_log_usage();
    PASS();
}

TEST alloc_free_alignment_and_stats(void) {
    SETUP();
    void *a = WL_MEM_ALLOC(TAG_SCRIPT, 100);
    void *b = WL_MEM_ALLOC_GPU(WL_TAG_ASSET, 4096);
    ASSERT(a && b);
    ASSERT_EQ(0u, (unsigned)((uintptr_t)a & 31u));
    ASSERT_EQ(0u, (unsigned)((uintptr_t)b & 31u));
    wl_gpu_buffer_commit(b, 4096);
    wl_gpu_buffer_invalidate(b, 4096);
    WlMemRow r[WL_MEM_MAX_ROWS];
    wl_mem_rows(r, WL_MEM_MAX_ROWS);
    ASSERT(r[TAG_SCRIPT].used >= 100 && r[TAG_SCRIPT].live == 1);
    ASSERT(r[WL_TAG_ASSET].used >= 4096 && r[WL_TAG_ASSET].live == 1);
    wl_mem_free(a);
    wl_mem_free(b);
    wl_mem_rows(r, WL_MEM_MAX_ROWS);
    ASSERT_EQ(0u, r[TAG_SCRIPT].used);
    ASSERT(r[TAG_SCRIPT].peak >= 100); // high-water mark survives the free
    ASSERT(wl_mem_check_all());
    ASSERT_EQ(0u, wl_mem_shutdown());
    PASS();
}

TEST budget_is_enforced_per_tag(void) {
    SETUP();
    ASSERT(WL_MEM_ALLOC(WL_TAG_NET, 600u * 1024) == NULL); // net budget is 512 KiB
    void *p = WL_MEM_ALLOC(WL_TAG_NET, 400u * 1024);
    ASSERT(p != NULL);
    ASSERT(WL_MEM_ALLOC(WL_TAG_NET, 200u * 1024) == NULL);
    ASSERT(WL_MEM_ALLOC(WL_TAG_IO, 200u * 1024) != NULL); // another tag is unaffected
    WlMemRow r[WL_MEM_MAX_ROWS];
    wl_mem_rows(r, WL_MEM_MAX_ROWS);
    ASSERT_EQ(2u, r[WL_TAG_NET].fails);
    wl_mem_free(p);
    (void)wl_mem_shutdown();
    PASS();
}

TEST bad_and_double_free_are_counted(void) {
    SETUP();
    uint8_t local[64];
    void *p = WL_MEM_ALLOC(WL_TAG_HOT, 64);
    wl_mem_free(p);
    wl_mem_free(p);
    wl_mem_free(local);
    wl_mem_free(NULL);
    ASSERT_EQ(2u, wl_mem_bad_frees());
    ASSERT(wl_mem_check_all());
    PASS();
}

TEST frame_arena_swaps_and_resets(void) {
    SETUP();
    wl_mem_frame_begin();
    WlArena *f0 = wl_mem_frame_arena();
    uint8_t *a = wl_arena_alloc(f0, 1000, 32);
    ASSERT(a != NULL);
    memset(a, 0x11, 1000);
    wl_mem_frame_begin();
    WlArena *f1 = wl_mem_frame_arena();
    ASSERT(f1 != f0);
    ASSERT_EQ(0u, f1->used);
    ASSERT_EQ(0x11, a[0]); // previous frame's data is still valid for one frame
    wl_mem_frame_begin();
    ASSERT(wl_mem_frame_arena() == f0);
    ASSERT_EQ(0u, f0->used);
    ASSERT_EQ(3u, wl_mem_frame_count());
    ASSERT(wl_arena_alloc(f0, WL_MEM_FRAME_ARENA_SIZE + 1, 8) == NULL);
    WlMemRow r[WL_MEM_MAX_ROWS];
    wl_mem_rows(r, WL_MEM_MAX_ROWS);
    ASSERT(r[TAG_COUNT].peak >= 1000);
    PASS();
}

TEST frame_guard_flags_hot_heap_allocs(void) {
    SETUP();
    void *p = WL_MEM_ALLOC(WL_TAG_HOT, 32);
    ASSERT_EQ(0u, wl_mem_frame_violations());
    wl_mem_frame_guard(true);
    void *q = WL_MEM_ALLOC(WL_TAG_HOT, 32);
    void *r = WL_MEM_ALLOC(WL_TAG_IO, 32); // other tags are not frame-loop violations
    wl_mem_frame_guard(false);
    ASSERT_EQ(1u, wl_mem_frame_violations());
    wl_mem_free(p);
    wl_mem_free(q);
    wl_mem_free(r);
    PASS();
}

TEST scene_arenas_are_a_stack(void) {
    SETUP();
    WlArena *a = wl_mem_scene_push("overworld", 1000u * 1024);
    WlArena *b = wl_mem_scene_push("battle", 1500u * 1024);
    ASSERT(a && b && a != b);
    ASSERT_EQ(2u, wl_mem_scene_depth());
    ASSERT(b->base == a->base + a->size);
    ASSERT_EQ(0u, (unsigned)((uintptr_t)b->base & 31u));
    ASSERT(wl_mem_scene_push("too_big", 1024u * 1024) == NULL); // 3 MiB stack: 2.5 used
    ASSERT(wl_arena_alloc(b, 100, 8) != NULL);
    ASSERT_EQ(0u, wl_mem_scene_pop(b));
    WlArena *c = wl_mem_scene_push("menu", 1024u * 1024); // space came back
    ASSERT(c != NULL && c->base == b->base);
    ASSERT_EQ(0u, wl_mem_scene_pop(c));
    ASSERT_EQ(0u, wl_mem_scene_pop(a));
    ASSERT_EQ(0u, wl_mem_scene_depth());
    PASS();
}

TEST scene_depth_is_limited(void) {
    SETUP();
    WlArena *s[WL_MEM_SCENE_MAX_DEPTH];
    for (unsigned i = 0; i < WL_MEM_SCENE_MAX_DEPTH; i++) {
        s[i] = wl_mem_scene_push("s", 1024);
        ASSERT(s[i] != NULL);
    }
    ASSERT(wl_mem_scene_push("s", 1024) == NULL);
    for (unsigned i = WL_MEM_SCENE_MAX_DEPTH; i-- > 0;) {
        ASSERT_EQ(0u, wl_mem_scene_pop(s[i]));
    }
    PASS();
}

TEST scene_pop_reports_leaked_hot_blocks(void) {
    SETUP();
    void *before = WL_MEM_ALLOC(WL_TAG_HOT, 64); // not part of the scene
    WlArena *s = wl_mem_scene_push("leaky", 4096);
    void *kept = WL_MEM_ALLOC(WL_TAG_HOT, 64);
    void *leak = WL_MEM_ALLOC(WL_TAG_HOT, 64);
    wl_mem_free(kept);
    ASSERT_EQ(1u, wl_mem_scene_pop(s));
    wl_mem_free(leak);
    wl_mem_free(before);
    ASSERT_EQ(0u, wl_mem_shutdown());
    PASS();
}

TEST leak_report_finds_a_deliberate_leak(void) {
    SETUP();
    ASSERT_EQ(0u, wl_mem_leak_report());
    void *leak = WL_MEM_ALLOC(TAG_SAVE, 128);
    void *ok = WL_MEM_ALLOC(WL_TAG_AUDIO, 128);
    wl_mem_free(ok);
    ASSERT(leak != NULL);
    ASSERT_EQ(1u, wl_mem_leak_report());
    ASSERT_EQ(1u, wl_mem_live_blocks(TAG_SAVE));
    ASSERT_EQ(1u, wl_mem_shutdown()); // shutdown reports it too
    PASS();
}

TEST default_config_has_the_builtin_tags(void) {
    (void)wl_mem_shutdown();
    ASSERT_EQ(WL_OK, wl_mem_init(NULL));
    ASSERT_EQ((uint32_t)WL_TAG_BUILTIN_COUNT, wl_mem_tag_count());
    ASSERT_STR_EQ("gxfifo", wl_mem_tag_name(WL_TAG_GXFIFO));
    ASSERT_STR_EQ("?", wl_mem_tag_name(WL_TAG_USER));
    void *p = WL_MEM_ALLOC_GPU(WL_TAG_GXFIFO, 1024u * 1024u - 32u); // the GX FIFO fits exactly
    ASSERT(p != NULL);
    wl_mem_free(p);
    ASSERT_EQ(0u, wl_mem_shutdown());
    PASS();
}

TEST disabled_tag_and_bad_configs(void) {
    (void)wl_mem_shutdown();
    WlMemTagDef tags[WL_TAG_BUILTIN_COUNT];
    memcpy(tags, wl_mem_builtin_tags(), sizeof tags);
    tags[WL_TAG_NET].budget = 0; // no networking: the tag is disabled
    WlMemConfig cfg;
    wl_mem_config_default(&cfg);
    cfg.tags = tags;
    ASSERT_EQ(WL_OK, wl_mem_init(&cfg));
    ASSERT(WL_MEM_ALLOC(WL_TAG_NET, 32) == NULL);
    WlMemRow r[WL_MEM_MAX_ROWS];
    wl_mem_rows(r, WL_MEM_MAX_ROWS);
    ASSERT_EQ(1u, r[WL_TAG_NET].fails);
    ASSERT(wl_mem_check_all());
    ASSERT_EQ(0u, wl_mem_shutdown());

    cfg.tag_count = WL_TAG_BUILTIN_COUNT - 1u; // a built-in tag is missing
    ASSERT_EQ(WL_ERR_ARG, wl_mem_init(&cfg));
    cfg.tag_count = WL_MEM_MAX_TAGS + 1u;
    ASSERT_EQ(WL_ERR_ARG, wl_mem_init(&cfg));
    cfg.tag_count = WL_TAG_BUILTIN_COUNT;
    tags[WL_TAG_ASSET].budget = 1024u * 1024u * 1024u; // more than MEM2 holds
    ASSERT_EQ(WL_ERR_NOMEM, wl_mem_init(&cfg));
    ASSERT(!wl_mem_ready());
    PASS();
}

SUITE(mem_suite) {
    RUN_TEST(default_config_has_the_builtin_tags);
    RUN_TEST(disabled_tag_and_bad_configs);
    RUN_TEST(init_state_and_double_init);
    RUN_TEST(rows_match_the_budget_table);
    RUN_TEST(alloc_free_alignment_and_stats);
    RUN_TEST(budget_is_enforced_per_tag);
    RUN_TEST(bad_and_double_free_are_counted);
    RUN_TEST(frame_arena_swaps_and_resets);
    RUN_TEST(frame_guard_flags_hot_heap_allocs);
    RUN_TEST(scene_arenas_are_a_stack);
    RUN_TEST(scene_depth_is_limited);
    RUN_TEST(scene_pop_reports_leaked_hot_blocks);
    RUN_TEST(leak_report_finds_a_deliberate_leak);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(mem_suite);
    GREATEST_MAIN_END();
}
