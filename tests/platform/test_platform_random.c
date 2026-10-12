// SPDX-License-Identifier: MIT
// wl_platform_random_bytes on the host (getrandom or /dev/urandom): the buffer is filled to its
// last byte and no further, calls differ, the bytes are spread over all values.
#include <string.h>

#include "greatest.h"
#include "wl/platform/platform.h"

TEST fills_exactly_the_buffer(void) {
    unsigned char buf[64 + 16];
    for (size_t size = 0; size <= 64; size += (size < 9 ? 1u : 11u)) {
        memset(buf, 0xA5, sizeof buf);
        ASSERT_EQ(WL_OK, wl_platform_random_bytes(buf + 8, size));
        for (size_t i = 0; i < 8; i++) {
            ASSERT_EQ(0xA5, buf[i]);
        }
        for (size_t i = 8 + size; i < sizeof buf; i++) {
            ASSERT_EQ(0xA5, buf[i]);
        }
    }
    PASS();
}

TEST zero_bytes_need_no_buffer(void) {
    ASSERT_EQ(WL_OK, wl_platform_random_bytes(NULL, 0));
    ASSERT_EQ(WL_ERR_ARG, wl_platform_random_bytes(NULL, 1));
    PASS();
}

TEST calls_differ(void) {
    unsigned char a[32], b[32];
    ASSERT_EQ(WL_OK, wl_platform_random_bytes(a, sizeof a));
    ASSERT_EQ(WL_OK, wl_platform_random_bytes(b, sizeof b));
    ASSERT(memcmp(a, b, sizeof a) != 0);
    PASS();
}

TEST bytes_cover_the_range(void) {
    // 64 KiB: every one of the 256 values turns up (a miss has a probability under 1e-100), and no
    // value takes more than twice its share
    static unsigned char buf[65536];
    unsigned counts[256] = {0};
    ASSERT_EQ(WL_OK, wl_platform_random_bytes(buf, sizeof buf));
    for (size_t i = 0; i < sizeof buf; i++) {
        counts[buf[i]]++;
    }
    for (int v = 0; v < 256; v++) {
        ASSERT(counts[v] > 0);
        ASSERT(counts[v] < 2u * 256u);
    }
    PASS();
}

SUITE(random_suite) {
    RUN_TEST(fills_exactly_the_buffer);
    RUN_TEST(zero_bytes_need_no_buffer);
    RUN_TEST(calls_differ);
    RUN_TEST(bytes_cover_the_range);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(random_suite);
    GREATEST_MAIN_END();
}
