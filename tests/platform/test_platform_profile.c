// SPDX-License-Identifier: MIT
// The sampling profiler's ring, jitter and dumps (wl/platform/profile.h) on synthetic samples and a
// synthetic stack, on every host; on the big-endian build also a sample taken from the real stack.
// The sampler itself (the decrementer interrupt) is the Wii's and is checked on the console.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "greatest.h"
#include "wl/platform/profile.h"

// --- a synthetic stack, as the backtrace test's ---------------------------------------------------
#define IMAGE_BASE 0x1000u
#define IMAGE_WORDS 64u

typedef struct {
    uint32_t words[IMAGE_WORDS];
} Image;

static bool image_read(void *user, uint32_t address, uint32_t *value) {
    Image *image = user;
    if ((address & 3u) != 0 || address < IMAGE_BASE || address >= IMAGE_BASE + IMAGE_WORDS * 4u) {
        return false;
    }
    *value = image->words[(address - IMAGE_BASE) / 4u];
    return true;
}

static void put(Image *image, uint32_t address, uint32_t value) {
    image->words[(address - IMAGE_BASE) / 4u] = value;
}

// frames at 0x1000, 0x1040, 0x1080, 0x10c0 (the first); callers A1, A2, A3 up the chain
static void build_chain(Image *image) {
    memset(image, 0, sizeof *image);
    put(image, 0x1000, 0x1040);
    put(image, 0x1040, 0x1080);
    put(image, 0x1044, 0xA0000001u);
    put(image, 0x1080, 0x10c0);
    put(image, 0x1084, 0xA0000002u);
    put(image, 0x10c0, 0);
    put(image, 0x10c4, 0xA0000003u);
}

static uint32_t g_store[64];

TEST init_validates_arguments(void) {
    WlProfileRing ring;
    ASSERT_EQ(WL_ERR_ARG, wl_profile_ring_init(NULL, g_store, 64, 4, WL_PROFILE_STOP_WHEN_FULL));
    ASSERT_EQ(WL_ERR_ARG, wl_profile_ring_init(&ring, NULL, 64, 4, WL_PROFILE_STOP_WHEN_FULL));
    ASSERT_EQ(WL_ERR_ARG, wl_profile_ring_init(&ring, g_store, 64, WL_PROFILE_MAX_DEPTH + 1u, WL_PROFILE_OVERWRITE));
    ASSERT_EQ(WL_ERR_ARG, wl_profile_ring_init(&ring, g_store, 7, 4, WL_PROFILE_OVERWRITE)); // (8 words a sample)
    ASSERT_EQ(WL_ERR_ARG, wl_profile_ring_init(&ring, g_store, 64, 4, (WlProfileMode)7));
    ASSERT_EQ(WL_OK, wl_profile_ring_init(&ring, g_store, 8, 4, WL_PROFILE_OVERWRITE));
    ASSERT_EQ(1u, ring.capacity);
    ASSERT_EQ(8u, ring.stride);
    ASSERT_EQ(WL_OK, wl_profile_ring_init(&ring, g_store, 64, 0, WL_PROFILE_OVERWRITE)); // (no callers at all)
    ASSERT_EQ(16u, ring.capacity);
    PASS();
}

TEST a_sample_holds_the_context_and_the_stack(void) {
    WlProfileRing ring;
    Image image;
    const uint32_t *s;

    build_chain(&image);
    ASSERT_EQ(WL_OK, wl_profile_ring_init(&ring, g_store, 64, 5, WL_PROFILE_STOP_WHEN_FULL));
    ASSERT(wl_profile_ring_record_with(&ring, 1234, 0x80aabbccu, 0x80001000u, 0x80002000u, 0x1000, image_read,
                                       &image));
    ASSERT_EQ(1u, ring.count);
    s = wl_profile_ring_get(&ring, 0);
    ASSERT(s != NULL);
    ASSERT_EQ(1234u, s[0]);
    ASSERT_EQ(0x80aabbccu, s[1]);
    ASSERT_EQ(0x80001000u, s[2]);
    ASSERT_EQ(0x80002000u, s[3]);
    ASSERT_EQ(0xA0000001u, s[4]);
    ASSERT_EQ(0xA0000002u, s[5]);
    ASSERT_EQ(0xA0000003u, s[6]);
    ASSERT_EQ(0u, s[7]); // (the walk found three of five)
    ASSERT_EQ(0u, s[8]);
    ASSERT(wl_profile_ring_get(&ring, 1) == NULL);
    PASS();
}

TEST a_shallow_depth_cuts_the_stack_and_an_unreadable_stack_leaves_none(void) {
    WlProfileRing ring;
    Image image;
    const uint32_t *s;

    build_chain(&image);
    ASSERT_EQ(WL_OK, wl_profile_ring_init(&ring, g_store, 64, 2, WL_PROFILE_STOP_WHEN_FULL));
    ASSERT(wl_profile_ring_record_with(&ring, 1, 2, 3, 4, 0x1000, image_read, &image));
    ASSERT(wl_profile_ring_record_with(&ring, 5, 6, 7, 8, 0x9000, image_read, &image));
    s = wl_profile_ring_get(&ring, 0);
    ASSERT_EQ(0xA0000001u, s[4]);
    ASSERT_EQ(0xA0000002u, s[5]);
    ASSERT_EQ(0u, wl_profile_ring_get(&ring, 1)[4]);
    ASSERT_EQ(7u, wl_profile_ring_get(&ring, 1)[2]);
    PASS();
}

TEST stale_callers_in_a_reused_slot_are_cleared(void) {
    WlProfileRing ring;
    Image image;

    build_chain(&image);
    memset(g_store, 0xff, sizeof g_store);
    ASSERT_EQ(WL_OK, wl_profile_ring_init(&ring, g_store, 6, 2, WL_PROFILE_OVERWRITE));
    ASSERT(wl_profile_ring_record_with(&ring, 1, 2, 3, 4, 0x9000, image_read, &image));
    ASSERT_EQ(0u, wl_profile_ring_get(&ring, 0)[4]);
    ASSERT_EQ(0u, wl_profile_ring_get(&ring, 0)[5]);
    PASS();
}

TEST a_full_ring_in_stop_mode_keeps_the_first_samples(void) {
    WlProfileRing ring;
    ASSERT_EQ(WL_OK, wl_profile_ring_init(&ring, g_store, 12, 2, WL_PROFILE_STOP_WHEN_FULL)); // 2 samples
    ASSERT_EQ(2u, ring.capacity);
    ASSERT(wl_profile_ring_record_with(&ring, 10, 1, 0x100, 0, 0, image_read, NULL));
    ASSERT(wl_profile_ring_record_with(&ring, 20, 1, 0x200, 0, 0, image_read, NULL));
    ASSERT(!wl_profile_ring_record_with(&ring, 30, 1, 0x300, 0, 0, image_read, NULL));
    ASSERT(!wl_profile_ring_record_with(&ring, 40, 1, 0x400, 0, 0, image_read, NULL));
    ASSERT_EQ(2u, ring.count);
    ASSERT_EQ(4u, ring.total);
    ASSERT_EQ(2u, ring.dropped);
    ASSERT_EQ(0x100u, wl_profile_ring_get(&ring, 0)[2]);
    ASSERT_EQ(0x200u, wl_profile_ring_get(&ring, 1)[2]);
    PASS();
}

TEST an_overwriting_ring_keeps_the_latest_in_order(void) {
    WlProfileRing ring;
    ASSERT_EQ(WL_OK, wl_profile_ring_init(&ring, g_store, 18, 2, WL_PROFILE_OVERWRITE)); // 3 samples
    ASSERT_EQ(3u, ring.capacity);
    for (uint32_t i = 1; i <= 7; i++) {
        ASSERT(wl_profile_ring_record_with(&ring, i * 10, 1, i, 0, 0, image_read, NULL));
    }
    ASSERT_EQ(3u, ring.count);
    ASSERT_EQ(7u, ring.total);
    ASSERT_EQ(0u, ring.dropped);
    ASSERT_EQ(5u, wl_profile_ring_get(&ring, 0)[2]);
    ASSERT_EQ(6u, wl_profile_ring_get(&ring, 1)[2]);
    ASSERT_EQ(7u, wl_profile_ring_get(&ring, 2)[2]);
    ASSERT(wl_profile_ring_get(&ring, 3) == NULL);
    PASS();
}

TEST reset_empties_the_ring_for_another_run(void) {
    WlProfileRing ring;
    ASSERT_EQ(WL_OK, wl_profile_ring_init(&ring, g_store, 12, 2, WL_PROFILE_STOP_WHEN_FULL));
    wl_profile_ring_record_with(&ring, 1, 1, 1, 0, 0, image_read, NULL);
    wl_profile_ring_record_with(&ring, 2, 1, 2, 0, 0, image_read, NULL);
    wl_profile_ring_record_with(&ring, 3, 1, 3, 0, 0, image_read, NULL);
    wl_profile_ring_reset(&ring);
    ASSERT_EQ(0u, ring.count);
    ASSERT_EQ(0u, ring.total);
    ASSERT_EQ(0u, ring.dropped);
    ASSERT(wl_profile_ring_get(&ring, 0) == NULL);
    ASSERT(wl_profile_ring_record_with(&ring, 4, 1, 4, 0, 0, image_read, NULL));
    ASSERT_EQ(4u, wl_profile_ring_get(&ring, 0)[2]);
    PASS();
}

TEST the_period_varies_within_a_sixteenth_and_is_never_zero(void) {
    uint32_t state = 0;
    uint32_t low = 0xffffffffu, high = 0, sum_first = 0;
    const uint32_t base = 60750u; // 1 kHz

    for (int i = 0; i < 10000; i++) {
        uint32_t p = wl_profile_jitter(&state, base);
        ASSERT(p >= base - base / 16u - 1u && p <= base + base / 16u + 1u);
        if (p < low) low = p;
        if (p > high) high = p;
        sum_first += (p > base);
    }
    ASSERT(low < base - base / 20u);   // it does vary,
    ASSERT(high > base + base / 20u);  // to both sides,
    ASSERT(sum_first > 4000u && sum_first < 6000u); // evenly
    // tiny and zero bases still give a period
    state = 0;
    for (uint32_t b = 0; b < 40; b++) {
        ASSERT(wl_profile_jitter(&state, b) >= 1u);
    }
    // the same state gives the same sequence
    uint32_t a = 77, c = 77;
    ASSERT_EQ(wl_profile_jitter(&a, base), wl_profile_jitter(&c, base));
    PASS();
}

typedef struct {
    char lines[16][256];
    uint32_t count;
} Lines;

static void collect(void *user, const char *line) {
    Lines *l = user;
    if (l->count < 16u) {
        snprintf(l->lines[l->count], sizeof l->lines[0], "%s", line);
    }
    l->count++;
}

TEST the_dump_is_a_header_and_a_line_per_sample_oldest_first(void) {
    WlProfileRing ring;
    Image image;
    Lines l = {.count = 0};
    WlProfileDumpInfo info = {.hz = 1000, .timebase_hz = 60750000u, .main_thread = 0x8123, .note = "a10\nplaying"};

    build_chain(&image);
    ASSERT_EQ(WL_OK, wl_profile_ring_init(&ring, g_store, 24, 4, WL_PROFILE_OVERWRITE)); // 3 samples
    for (uint32_t i = 1; i <= 4; i++) {
        wl_profile_ring_record_with(&ring, 0x100u * i, 0x80000000u + i, 0x80010000u + i, 0x80020000u + i,
                                    i == 2 ? 0x1000u : 0x9000u, image_read, &image);
    }
    ASSERT_EQ(4u, wl_profile_dump(&ring, &info, collect, &l));
    ASSERT_EQ(4u, l.count);
    ASSERT_STR_EQ("WLPROF 1 hz=1000 tbhz=60750000 depth=4 samples=3 total=4 dropped=0 thread=8123 note=a10 playing",
                  l.lines[0]);
    ASSERT_STR_EQ("WLPS 200 80000002 80010002 80020002 a0000001 a0000002 a0000003", l.lines[1]); // (trailing zeros left out)
    ASSERT_STR_EQ("WLPS 300 80000003 80010003 80020003", l.lines[2]);
    ASSERT_STR_EQ("WLPS 400 80000004 80010004 80020004", l.lines[3]);
    PASS();
}

TEST the_longest_line_fits_the_log(void) {
    static uint32_t big[(WL_PROFILE_HEADER_WORDS + WL_PROFILE_MAX_DEPTH) * 2];
    WlProfileRing ring;
    Lines l = {.count = 0};
    WlProfileDumpInfo info = {.hz = 10000, .main_thread = 0xffffffffu,
                              .note = "a note that is much longer than the header has room for, so it is cut short"};
    uint32_t i;

    ASSERT_EQ(WL_OK, wl_profile_ring_init(&ring, big, (uint32_t)(sizeof big / sizeof big[0]), WL_PROFILE_MAX_DEPTH,
                                          WL_PROFILE_STOP_WHEN_FULL));
    wl_profile_ring_record_with(&ring, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0, image_read, NULL);
    for (i = 0; i < WL_PROFILE_MAX_DEPTH; i++) {
        ring.words[WL_PROFILE_HEADER_WORDS + i] = 0xffffffffu;
    }
    wl_profile_dump(&ring, &info, collect, &l);
    ASSERT_EQ(2u, l.count);
    ASSERT(strlen(l.lines[0]) < 200u);
    ASSERT(strlen(l.lines[1]) < 200u);
    ASSERT(strstr(l.lines[1], " ffffffff ffffffff ffffffff ffffffff ffffffff ffffffff") != NULL);
    PASS();
}

TEST the_file_dump_round_trips(void) {
    WlProfileRing ring;
    WlProfileDumpInfo info = {.hz = 500};
    char path[64];
    char line[256];
    FILE *file;
    int lines = 0;

    snprintf(path, sizeof path, "wl_profile_test_%d.txt", 1);
    ASSERT_EQ(WL_OK, wl_profile_ring_init(&ring, g_store, 24, 1, WL_PROFILE_STOP_WHEN_FULL));
    wl_profile_ring_record_with(&ring, 1, 2, 3, 4, 0, image_read, NULL);
    wl_profile_ring_record_with(&ring, 5, 6, 7, 8, 0, image_read, NULL);
    ASSERT_EQ(WL_OK, wl_profile_dump_file(&ring, &info, path));
    file = fopen(path, "r");
    ASSERT(file != NULL);
    while (fgets(line, sizeof line, file) != NULL) {
        lines++;
        if (lines == 1) {
            ASSERT(strncmp(line, "WLPROF 1 hz=500 ", 16) == 0);
        } else if (lines == 3) {
            ASSERT_STR_EQ("WLPS 5 6 7 8\n", line);
        }
    }
    fclose(file);
    remove(path);
    ASSERT_EQ(3, lines);
    ASSERT_EQ(WL_ERR_IO, wl_profile_dump_file(&ring, &info, "/nonexistent-directory/profile.txt"));
    ASSERT_EQ(WL_ERR_ARG, wl_profile_dump_file(&ring, &info, NULL));
    PASS();
}

TEST the_sampler_is_the_consoles(void) {
    WlProfileRing ring;
    ASSERT_EQ(WL_OK, wl_profile_ring_init(&ring, g_store, 64, 2, WL_PROFILE_STOP_WHEN_FULL));
    ASSERT_EQ(WL_ERR_ARG, wl_profile_start(NULL, 1000));
    ASSERT_EQ(WL_ERR_ARG, wl_profile_start(&ring, 0));
    ASSERT_EQ(WL_ERR_ARG, wl_profile_start(&ring, 10001));
    ASSERT_EQ(WL_ERR_UNSUPPORTED, wl_profile_start(&ring, 1000));
    ASSERT(!wl_profile_running());
    wl_profile_stop(); // harmless
    ASSERT_EQ(60750000u, wl_profile_timebase_hz());
    PASS();
}

#if defined(__powerpc__) && !defined(__powerpc64__)

// A sample "taken" in a function that has called another, as the interrupt would find it: pc in the
// function, the link register stale (inside the function), r1 its frame. The stack walk must give
// the caller's call site and then its caller's.
static uint32_t g_outer_returns_to;
static int g_real_ok;

__attribute__((noinline)) static uint32_t middle(WlProfileRing *ring) {
    uint32_t sp;

    __asm__ volatile("mr %0, 1" : "=r"(sp));
    wl_profile_ring_record(ring, 99, 0x80001234u, (uint32_t)(uintptr_t)&middle, (uint32_t)(uintptr_t)&middle + 8u, sp);
    const uint32_t *s = wl_profile_ring_get(ring, 0);
    g_real_ok = s != NULL && s[WL_PROFILE_HEADER_WORDS + 0] == (uint32_t)(uintptr_t)__builtin_return_address(0) &&
                s[WL_PROFILE_HEADER_WORDS + 1] == g_outer_returns_to;
    return (uint32_t)g_real_ok;
}

__attribute__((noinline)) static uint32_t outer(WlProfileRing *ring) {
    g_outer_returns_to = (uint32_t)(uintptr_t)__builtin_return_address(0);
    uint32_t r = middle(ring);
    __asm__ volatile("" : "+r"(r));
    return r;
}

TEST a_sample_from_the_real_stack_has_the_real_callers(void) {
    WlProfileRing ring;
    ASSERT_EQ(WL_OK, wl_profile_ring_init(&ring, g_store, 64, 4, WL_PROFILE_STOP_WHEN_FULL));
    outer(&ring);
    ASSERT(g_real_ok);
    PASS();
}

#endif

SUITE(profile_suite) {
    RUN_TEST(init_validates_arguments);
    RUN_TEST(a_sample_holds_the_context_and_the_stack);
    RUN_TEST(a_shallow_depth_cuts_the_stack_and_an_unreadable_stack_leaves_none);
    RUN_TEST(stale_callers_in_a_reused_slot_are_cleared);
    RUN_TEST(a_full_ring_in_stop_mode_keeps_the_first_samples);
    RUN_TEST(an_overwriting_ring_keeps_the_latest_in_order);
    RUN_TEST(reset_empties_the_ring_for_another_run);
    RUN_TEST(the_period_varies_within_a_sixteenth_and_is_never_zero);
    RUN_TEST(the_dump_is_a_header_and_a_line_per_sample_oldest_first);
    RUN_TEST(the_longest_line_fits_the_log);
    RUN_TEST(the_file_dump_round_trips);
    RUN_TEST(the_sampler_is_the_consoles);
#if defined(__powerpc__) && !defined(__powerpc64__)
    RUN_TEST(a_sample_from_the_real_stack_has_the_real_callers);
#endif
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(profile_suite);
    GREATEST_MAIN_END();
}
