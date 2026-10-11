// SPDX-License-Identifier: MIT
// Endian readers and writers of core/base/endian.h: known byte patterns, round trips and
// unaligned access.
#include "wl/base/endian.h"

#include <string.h>

#include "wl/base/base.h"
#include "greatest.h"

static const uint8_t k_bytes[9] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};

TEST reads_little_endian(void) {
    ASSERT_EQ_FMT(0x0201u, (unsigned)wl_rd_le16(k_bytes + 1), "%#x");
    ASSERT_EQ_FMT(0x030201u, (unsigned)wl_rd_le24(k_bytes + 1), "%#x");
    ASSERT_EQ_FMT(0x04030201u, (unsigned)wl_rd_le32(k_bytes + 1), "%#x");
    ASSERT(wl_rd_le64(k_bytes + 1) == 0x0807060504030201ull);
    PASS();
}

TEST reads_big_endian(void) {
    ASSERT_EQ_FMT(0x0102u, (unsigned)wl_rd_be16(k_bytes + 1), "%#x");
    ASSERT_EQ_FMT(0x010203u, (unsigned)wl_rd_be24(k_bytes + 1), "%#x");
    ASSERT_EQ_FMT(0x01020304u, (unsigned)wl_rd_be32(k_bytes + 1), "%#x");
    ASSERT(wl_rd_be64(k_bytes + 1) == 0x0102030405060708ull);
    PASS();
}

TEST high_bits_survive(void) {
    static const uint8_t ff[8] = {0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 0xF8};
    ASSERT_EQ_FMT(0xFEFFu, (unsigned)wl_rd_le16(ff), "%#x");
    ASSERT_EQ_FMT(0xFFFEu, (unsigned)wl_rd_be16(ff), "%#x");
    ASSERT_EQ_FMT(0xFCFDFEFFu, (unsigned)wl_rd_le32(ff), "%#x");
    ASSERT_EQ_FMT(0xFFFEFDFCu, (unsigned)wl_rd_be32(ff), "%#x");
    ASSERT(wl_rd_le64(ff) == 0xF8F9FAFBFCFDFEFFull);
    ASSERT(wl_rd_be64(ff) == 0xFFFEFDFCFBFAF9F8ull);
    PASS();
}

TEST writes_match_reads(void) {
    uint8_t buf[9];
    memset(buf, 0xAA, sizeof buf);

    wl_wr_le16(buf + 1, 0xBEEF);
    ASSERT_EQ(0xEF, buf[1]);
    ASSERT_EQ(0xBE, buf[2]);
    ASSERT_EQ(0xAA, buf[3]); // no overrun
    wl_wr_be16(buf + 1, 0xBEEF);
    ASSERT_EQ(0xBE, buf[1]);
    ASSERT_EQ(0xEF, buf[2]);

    wl_wr_le32(buf + 1, 0xDEADBEEFu);
    ASSERT_EQ_FMT(0xDEADBEEFu, (unsigned)wl_rd_le32(buf + 1), "%#x");
    ASSERT_EQ(0xEF, buf[1]);
    wl_wr_be32(buf + 1, 0xDEADBEEFu);
    ASSERT_EQ_FMT(0xDEADBEEFu, (unsigned)wl_rd_be32(buf + 1), "%#x");
    ASSERT_EQ(0xDE, buf[1]);

    wl_wr_le64(buf + 1, 0x0123456789ABCDEFull);
    ASSERT(wl_rd_le64(buf + 1) == 0x0123456789ABCDEFull);
    ASSERT_EQ(0xEF, buf[1]);
    ASSERT_EQ(0x01, buf[8]);
    wl_wr_be64(buf + 1, 0x0123456789ABCDEFull);
    ASSERT(wl_rd_be64(buf + 1) == 0x0123456789ABCDEFull);
    ASSERT_EQ(0x01, buf[1]);
    ASSERT_EQ(0xEF, buf[8]);
    ASSERT_EQ(0xAA, buf[0]); // no underrun
    PASS();
}

TEST range_check(void) {
    ASSERT(wl_range_ok(0, 0, 0));
    ASSERT(wl_range_ok(4, 4, 8));
    ASSERT_FALSE(wl_range_ok(5, 4, 8));
    ASSERT_FALSE(wl_range_ok(9, 0, 8));
    ASSERT_FALSE(wl_range_ok(1, UINT64_MAX, 8)); // no wrap-around
    PASS();
}

// Native byte order, probed through memory (no macros), with the expected order for the build:
// the ppc-be build (qemu-ppc) is big-endian, everything else little-endian.
static bool native_is_big(void) {
    const uint32_t probe = 0x01020304u;
    uint8_t b[4];
    memcpy(b, &probe, 4);
    return b[0] == 0x01;
}

// Deliberately wrong helpers: they reinterpret the bytes in native order, as a careless
// `*(uint32_t *)p` would. They must agree with the real helpers on exactly one byte order.
static uint32_t native_rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

TEST native_order_is_the_expected_one(void) {
#ifdef WL_EXPECT_BIG_ENDIAN
    ASSERT(native_is_big());
#else
    ASSERT_FALSE(native_is_big());
#endif
    PASS();
}

TEST helpers_do_not_depend_on_native_order(void) {
    static const uint8_t b[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    // These constants are the same on every host.
    ASSERT_EQ_FMT(0x11223344u, (unsigned)wl_rd_be32(b), "%#x");
    ASSERT_EQ_FMT(0x44332211u, (unsigned)wl_rd_le32(b), "%#x");
    ASSERT(wl_rd_be64(b) == 0x1122334455667788ull);
    ASSERT(wl_rd_le64(b) == 0x8877665544332211ull);
    // Negative self-check: the naive native read equals the right helper only on its own order.
    if (native_is_big()) {
        ASSERT_EQ_FMT(wl_rd_be32(b), native_rd32(b), "%#x");
        ASSERT(native_rd32(b) != wl_rd_le32(b));
    } else {
        ASSERT_EQ_FMT(wl_rd_le32(b), native_rd32(b), "%#x");
        ASSERT(native_rd32(b) != wl_rd_be32(b));
    }
    // A write followed by a byte-wise check: layout must be the same on both hosts.
    uint8_t out[4];
    wl_wr_be32(out, 0xA1B2C3D4u);
    ASSERT(out[0] == 0xA1 && out[3] == 0xD4);
    wl_wr_le32(out, 0xA1B2C3D4u);
    ASSERT(out[0] == 0xD4 && out[3] == 0xA1);
    PASS();
}

SUITE(core_endian) {
    RUN_TEST(native_order_is_the_expected_one);
    RUN_TEST(helpers_do_not_depend_on_native_order);
    RUN_TEST(reads_little_endian);
    RUN_TEST(reads_big_endian);
    RUN_TEST(high_bits_survive);
    RUN_TEST(writes_match_reads);
    RUN_TEST(range_check);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(core_endian);
    GREATEST_MAIN_END();
}
