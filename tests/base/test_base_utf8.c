// SPDX-License-Identifier: MIT
#include "wl/base/utf8.h"

#include <string.h>

#include "greatest.h"

#define VALID(lit) ASSERT(wl_utf8_valid((lit), sizeof(lit) - 1))
#define INVALID(lit) ASSERT(!wl_utf8_valid((lit), sizeof(lit) - 1))

TEST accepts_valid(void) {
    ASSERT(wl_utf8_valid("", 0));
    VALID("plain ascii");
    VALID("\xC3\xA9");             // U+00E9
    VALID("\xE2\x82\xAC");         // U+20AC
    VALID("\xF0\x9F\x8E\xAE");     // U+1F3AE
    VALID("\xC2\x80");             // lowest 2-byte
    VALID("\xDF\xBF");             // U+07FF
    VALID("\xE0\xA0\x80");         // U+0800
    VALID("\xED\x9F\xBF");         // U+D7FF, just below surrogates
    VALID("\xEE\x80\x80");         // U+E000, just above surrogates
    VALID("\xEF\xBF\xBF");         // U+FFFF
    VALID("\xF0\x90\x80\x80");     // U+10000
    VALID("\xF4\x8F\xBF\xBF");     // U+10FFFF
    VALID("a\0b");                 // embedded NUL
    PASS();
}

TEST rejects_invalid(void) {
    INVALID("\x80");                 // lone continuation
    INVALID("\xBF");
    INVALID("\xC0\x80");             // overlong NUL
    INVALID("\xC1\xBF");             // overlong 2-byte
    INVALID("\xE0\x80\x80");         // overlong 3-byte
    INVALID("\xE0\x9F\xBF");         // overlong 3-byte (U+07FF)
    INVALID("\xF0\x80\x80\x80");     // overlong 4-byte
    INVALID("\xF0\x8F\xBF\xBF");     // overlong 4-byte (U+FFFF)
    INVALID("\xED\xA0\x80");         // U+D800 surrogate
    INVALID("\xED\xBF\xBF");         // U+DFFF surrogate
    INVALID("\xF4\x90\x80\x80");     // U+110000
    INVALID("\xF5\x80\x80\x80");     // lead byte beyond range
    INVALID("\xFF");
    INVALID("\xC3");                 // truncated
    INVALID("\xE2\x82");
    INVALID("\xF0\x9F\x8E");
    INVALID("\xC3\x28");             // bad continuation
    INVALID("\xE2\x28\xA1");
    PASS();
}

TEST decode_reports_size_and_value(void) {
    uint32_t cp = 0;
    ASSERT_EQ(0u, wl_utf8_decode("", 0, &cp));
    ASSERT_EQ(1u, wl_utf8_decode("A", 1, &cp));
    ASSERT_EQ(0x41u, cp);
    ASSERT_EQ(2u, wl_utf8_decode("\xC3\xA9xyz", 5, &cp));
    ASSERT_EQ(0xE9u, cp);
    ASSERT_EQ(3u, wl_utf8_decode("\xE2\x82\xAC", 3, &cp));
    ASSERT_EQ(0x20ACu, cp);
    ASSERT_EQ(4u, wl_utf8_decode("\xF4\x8F\xBF\xBF", 4, &cp));
    ASSERT_EQ(0x10FFFFu, cp);
    ASSERT_EQ(0u, wl_utf8_decode("\xE2\x82\xAC", 2, &cp)); // length cuts the sequence
    PASS();
}

TEST encode_known_and_round_trip(void) {
    char buf[4];
    ASSERT_EQ(1u, wl_utf8_encode(0x7F, buf));
    ASSERT_EQ(0x7F, buf[0]);
    ASSERT_EQ(2u, wl_utf8_encode(0xE9, buf));
    ASSERT(memcmp(buf, "\xC3\xA9", 2) == 0);
    ASSERT_EQ(3u, wl_utf8_encode(0x20AC, buf));
    ASSERT(memcmp(buf, "\xE2\x82\xAC", 3) == 0);
    ASSERT_EQ(4u, wl_utf8_encode(0x1F3AE, buf));
    ASSERT(memcmp(buf, "\xF0\x9F\x8E\xAE", 4) == 0);
    ASSERT_EQ(0u, wl_utf8_encode(0xD800, buf));
    ASSERT_EQ(0u, wl_utf8_encode(0xDFFF, buf));
    ASSERT_EQ(0u, wl_utf8_encode(0x110000, buf));
    for (uint32_t cp = 0; cp <= 0x10FFFF; cp += (cp < 0x800 ? 1 : 0x3F)) {
        size_t n = wl_utf8_encode(cp, buf);
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            ASSERT_EQ(0u, n);
            continue;
        }
        uint32_t back = 0xFFFFFFFFu;
        ASSERT(n >= 1 && n <= 4);
        ASSERT_EQ(n, wl_utf8_decode(buf, n, &back));
        ASSERT_EQ(cp, back);
    }
    PASS();
}

TEST length_counts_code_points(void) {
    size_t n = 99;
    ASSERT_EQ(WL_OK, wl_utf8_length("", 0, &n));
    ASSERT_EQ(0u, n);
    ASSERT_EQ(WL_OK, wl_utf8_length("a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x8E\xAE", 10, &n));
    ASSERT_EQ(4u, n);
    ASSERT_EQ(WL_ERR_FORMAT, wl_utf8_length("a\xC0\x80", 3, &n));
    PASS();
}

SUITE(utf8_suite) {
    RUN_TEST(accepts_valid);
    RUN_TEST(rejects_invalid);
    RUN_TEST(decode_reports_size_and_value);
    RUN_TEST(encode_known_and_round_trip);
    RUN_TEST(length_counts_code_points);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(utf8_suite);
    GREATEST_MAIN_END();
}
