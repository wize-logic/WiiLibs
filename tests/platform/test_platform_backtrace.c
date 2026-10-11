// SPDX-License-Identifier: MIT
// Backtraces (wl/platform/backtrace.h). The walker runs against a synthetic stack image on every
// host; the walk of the real stack runs where the frames are PowerPC's (the ppc-be build under qemu,
// the Wii's byte order and frame layout) and checks the addresses against __builtin_return_address.
#include <stdint.h>
#include <string.h>

#include "greatest.h"
#include "wl/platform/backtrace.h"
#include "wl/platform/platform.h"

// A stack image: words at 0x1000 up, as addresses of the 32-bit world the walker reads.
#define IMAGE_BASE 0x1000u
#define IMAGE_WORDS 64u

typedef struct {
    uint32_t words[IMAGE_WORDS];
    uint32_t reads;
} Image;

static bool image_read(void *user, uint32_t address, uint32_t *value) {
    Image *image = user;
    if ((address & 3u) != 0 || address < IMAGE_BASE || address >= IMAGE_BASE + IMAGE_WORDS * 4u) {
        return false;
    }
    image->reads++;
    *value = image->words[(address - IMAGE_BASE) / 4u];
    return true;
}

static void put(Image *image, uint32_t address, uint32_t value) {
    image->words[(address - IMAGE_BASE) / 4u] = value;
}

// Frames at 0x1000 (innermost), 0x1040, 0x1080 and 0x10c0 (the first frame, back chain 0). A
// function saves its return address in its caller's frame, so the word at 0x1044 is the address in
// the function owning frame 0x1040 that the function owning frame 0x1000 returns to.
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

TEST walk_follows_the_chain(void) {
    Image image;
    uint32_t out[8];
    build_chain(&image);
    ASSERT_EQ(3u, wl_backtrace_walk(image_read, &image, 0x1000, 0, 0, out, 8));
    ASSERT_EQ(0xA0000001u, out[0]);
    ASSERT_EQ(0xA0000002u, out[1]);
    ASSERT_EQ(0xA0000003u, out[2]); // (the first frame's own function; its back chain, 0, ends the walk)
    PASS();
}

TEST walk_ends_at_a_zero_return_address(void) {
    Image image;
    uint32_t out[8];
    build_chain(&image);
    put(&image, 0x10c4, 0); // the first function was called by nobody
    ASSERT_EQ(2u, wl_backtrace_walk(image_read, &image, 0x1000, 0, 0, out, 8));
    ASSERT_EQ(0xA0000002u, out[1]);
    PASS();
}

TEST walk_reports_the_link_register_first(void) {
    Image image;
    uint32_t out[8];
    build_chain(&image);
    ASSERT_EQ(4u, wl_backtrace_walk(image_read, &image, 0x1000, 0xB0000000u, 0, out, 8));
    ASSERT_EQ(0xB0000000u, out[0]);
    ASSERT_EQ(0xA0000001u, out[1]);
    ASSERT_EQ(0xA0000002u, out[2]);
    ASSERT_EQ(0xA0000003u, out[3]);
    PASS();
}

TEST walk_skips_and_stops_at_max(void) {
    Image image;
    uint32_t out[8];
    build_chain(&image);
    ASSERT_EQ(2u, wl_backtrace_walk(image_read, &image, 0x1000, 0xB0000000u, 2, out, 8));
    ASSERT_EQ(0xA0000002u, out[0]);
    ASSERT_EQ(0xA0000003u, out[1]);
    ASSERT_EQ(0u, wl_backtrace_walk(image_read, &image, 0x1000, 0xB0000000u, 4, out, 8));
    ASSERT_EQ(1u, wl_backtrace_walk(image_read, &image, 0x1000, 0, 0, out, 1));
    ASSERT_EQ(0xA0000001u, out[0]);
    ASSERT_EQ(0u, wl_backtrace_walk(image_read, &image, 0x1000, 0, 0, out, 0));
    PASS();
}

TEST walk_survives_a_damaged_chain(void) {
    Image image;
    uint32_t out[8];

    // a back chain that does not rise (a loop), one out of the image, a misaligned frame
    build_chain(&image);
    put(&image, 0x1040, 0x1000);
    ASSERT_EQ(1u, wl_backtrace_walk(image_read, &image, 0x1000, 0, 0, out, 8));
    put(&image, 0x1040, 0x9000);
    ASSERT_EQ(1u, wl_backtrace_walk(image_read, &image, 0x1000, 0, 0, out, 8));
    put(&image, 0x1040, 0x1082);
    ASSERT_EQ(1u, wl_backtrace_walk(image_read, &image, 0x1000, 0, 0, out, 8));
    put(&image, 0x1040, 0xfffffffcu);
    ASSERT_EQ(1u, wl_backtrace_walk(image_read, &image, 0x1000, 0, 0, out, 8));
    build_chain(&image);
    ASSERT_EQ(0u, wl_backtrace_walk(image_read, &image, 0x1002, 0, 0, out, 8)); // misaligned start
    ASSERT_EQ(0u, wl_backtrace_walk(image_read, &image, 0x8000, 0, 0, out, 8)); // unreadable start
    ASSERT_EQ(0u, wl_backtrace_walk(NULL, &image, 0x1000, 0, 0, out, 8));
    ASSERT_EQ(0u, wl_backtrace_walk(image_read, &image, 0x1000, 0, 0, NULL, 8));
    PASS();
}

#if defined(__powerpc__) && !defined(__powerpc64__)

// Three levels of calls that stay calls: each checks that the addresses wl_platform_backtrace
// gives line up with what the compiler says its callers are.
static uint32_t g_level2_returns_to; // __builtin_return_address(0) of level2
static uint32_t g_depth_found;
static uint32_t g_returns_match;

__attribute__((noinline)) static uint32_t level3(void) {
    uint32_t out[4] = {0, 0, 0, 0};
    uint32_t count = wl_platform_backtrace(out, 4, 0);
    // out[0] is in this function; out[1] is where it returns to, out[2] where its caller does
    g_depth_found = count;
    g_returns_match = out[1] == (uint32_t)(uintptr_t)__builtin_return_address(0) &&
                      out[2] == g_level2_returns_to;
    // (and `skip` drops from the front)
    uint32_t skipped[2] = {0, 0};
    uint32_t skipped_count = wl_platform_backtrace(skipped, 2, 1);
    g_returns_match = g_returns_match && skipped_count == 2 && skipped[0] == out[1] && skipped[1] == out[2];
    return count;
}

__attribute__((noinline)) static uint32_t level2(void) {
    g_level2_returns_to = (uint32_t)(uintptr_t)__builtin_return_address(0);
    uint32_t count = level3();
    __asm__ volatile("" : "+r"(count)); // (not a tail call)
    return count;
}

__attribute__((noinline)) static uint32_t level1(void) {
    uint32_t count = level2();
    __asm__ volatile("" : "+r"(count));
    return count;
}

TEST real_stack_matches_the_compilers_callers(void) {
    uint32_t count = level1();
    ASSERT(count >= 3u);
    ASSERT_EQ(4u, g_depth_found);
    ASSERT(g_returns_match);
    PASS();
}

TEST real_stack_ends_at_the_threads_first_frame(void) {
    uint32_t out[256];
    uint32_t count = wl_platform_backtrace(out, 256, 0);
    ASSERT(count >= 2u && count < 256u);
    for (uint32_t i = 0; i < count; i++) {
        ASSERT(out[i] != 0 && (out[i] & 3u) == 0);
    }
    PASS();
}

TEST backtrace_from_a_context_starts_at_the_link_register(void) {
    uint32_t sp, out[4];
    __asm__ volatile("mr %0, 1" : "=r"(sp));
    uint32_t count = wl_platform_backtrace_from(sp, 0x12345678u, out, 4);
    ASSERT(count >= 2u);
    ASSERT_EQ(0x12345678u, out[0]);
    PASS();
}

__attribute__((noinline)) static void log_it(void) {
    wl_platform_log_backtrace("trace", "exit(7)", 0);
    __asm__ volatile("");
}

TEST log_line_lists_the_callers(void) {
    wl_platform_log_init(NULL);
    log_it();
    const char *lines[1];
    ASSERT_EQ(1u, wl_platform_log_tail(lines, 1));
    ASSERT(strstr(lines[0], "[E][trace] exit(7) from ") == lines[0]);
    // 8 hex digits per address after "from"
    const char *from = strstr(lines[0], "from ") + 5;
    ASSERT(strlen(from) >= 8u);
    ASSERT(strspn(from, "0123456789abcdef ") == strlen(from));
    PASS();
}

#else

TEST no_backtrace_off_powerpc(void) {
    uint32_t out[4];
    ASSERT_EQ(0u, wl_platform_backtrace(out, 4, 0));
    ASSERT_EQ(0u, wl_platform_backtrace_from(0x1000, 0x2000, out, 4));
    wl_platform_log_init(NULL);
    wl_platform_log_backtrace("trace", "abort(0)", 0);
    const char *lines[1];
    ASSERT_EQ(1u, wl_platform_log_tail(lines, 1));
    ASSERT(strstr(lines[0], "abort(0) from (no backtrace") != NULL);
    PASS();
}

#endif

SUITE(backtrace_suite) {
    RUN_TEST(walk_follows_the_chain);
    RUN_TEST(walk_ends_at_a_zero_return_address);
    RUN_TEST(walk_reports_the_link_register_first);
    RUN_TEST(walk_skips_and_stops_at_max);
    RUN_TEST(walk_survives_a_damaged_chain);
#if defined(__powerpc__) && !defined(__powerpc64__)
    RUN_TEST(real_stack_matches_the_compilers_callers);
    RUN_TEST(real_stack_ends_at_the_threads_first_frame);
    RUN_TEST(backtrace_from_a_context_starts_at_the_link_register);
    RUN_TEST(log_line_lists_the_callers);
#else
    RUN_TEST(no_backtrace_off_powerpc);
#endif
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(backtrace_suite);
    GREATEST_MAIN_END();
}
