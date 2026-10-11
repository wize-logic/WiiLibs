// SPDX-License-Identifier: MIT
// FNV-1a 64 and CRC-32 against published check values.
#include "wl/base/hash.h"

#include <string.h>

#include "greatest.h"

TEST fnv1a64_vectors(void) {
    // Vectors from the FNV reference test suite (isthe.com/chongo/tech/comp/fnv).
    ASSERT(wl_fnv1a64("", 0) == 0xcbf29ce484222325ull);
    ASSERT(wl_fnv1a64("a", 1) == 0xaf63dc4c8601ec8cull);
    ASSERT(wl_fnv1a64("foobar", 6) == 0x85944171f73967e8ull);
    PASS();
}

TEST fnv1a64_incremental(void) {
    uint64_t h = WL_FNV1A64_INIT;
    h = wl_fnv1a64_update(h, "foo", 3);
    h = wl_fnv1a64_update(h, "bar", 3);
    ASSERT(h == wl_fnv1a64("foobar", 6));
    PASS();
}

TEST crc32_vectors(void) {
    ASSERT_EQ_FMT(0x00000000u, (unsigned)wl_crc32("", 0), "%#x");
    ASSERT_EQ_FMT(0xCBF43926u, (unsigned)wl_crc32("123456789", 9), "%#x"); // CRC-32 check value
    ASSERT_EQ_FMT(0xE8B7BE43u, (unsigned)wl_crc32("a", 1), "%#x");
    ASSERT_EQ_FMT(0x414FA339u, (unsigned)wl_crc32("The quick brown fox jumps over the lazy dog", 43),
                  "%#x");
    PASS();
}

TEST crc32_incremental(void) {
    uint32_t c = wl_crc32_update(0, "1234", 4);
    c = wl_crc32_update(c, "56789", 5);
    ASSERT_EQ_FMT(0xCBF43926u, (unsigned)c, "%#x");
    PASS();
}

TEST crc32_all_bytes(void) {
    uint8_t buf[256];
    for (size_t i = 0; i < sizeof buf; i++) {
        buf[i] = (uint8_t)i;
    }
    ASSERT_EQ_FMT(0x29058C73u, (unsigned)wl_crc32(buf, sizeof buf), "%#x");
    PASS();
}

SUITE(hash_suite) {
    RUN_TEST(fnv1a64_vectors);
    RUN_TEST(fnv1a64_incremental);
    RUN_TEST(crc32_vectors);
    RUN_TEST(crc32_incremental);
    RUN_TEST(crc32_all_bytes);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(hash_suite);
    GREATEST_MAIN_END();
}
