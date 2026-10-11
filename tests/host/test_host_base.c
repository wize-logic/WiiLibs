// SPDX-License-Identifier: MIT
// SHA-1 (FIPS 180 vectors), the JSON reader, and the file shim.
#include <stdlib.h>
#include <string.h>

#include "wl/host/file.h"
#include "wl/host/json.h"
#include "wl/host/sha1.h"
#include "greatest.h"

static void hex_of(const void *data, size_t len, char out[WL_SHA1_HEX_SIZE]) {
    uint8_t d[20];
    wl_sha1_buf(data, len, d);
    wl_sha1_to_hex(d, out);
}

TEST sha1_abc(void) {
    char h[WL_SHA1_HEX_SIZE];
    hex_of("abc", 3, h);
    ASSERT_STR_EQ("a9993e364706816aba3e25717850c26c9cd0d89d", h);
    PASS();
}

TEST sha1_empty(void) {
    char h[WL_SHA1_HEX_SIZE];
    hex_of("", 0, h);
    ASSERT_STR_EQ("da39a3ee5e6b4b0d3255bfef95601890afd80709", h);
    PASS();
}

TEST sha1_448_bit_message(void) {
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    char h[WL_SHA1_HEX_SIZE];
    ASSERT_EQ(56, (int)strlen(m));
    hex_of(m, strlen(m), h);
    ASSERT_STR_EQ("84983e441c3bd26ebaae4aa1f95129e5e54670f1", h);
    PASS();
}

TEST sha1_million_a_streamed(void) {
    WlSha1 c;
    uint8_t d[20], block[1000];
    char h[WL_SHA1_HEX_SIZE];
    memset(block, 'a', sizeof block);
    wl_sha1_init(&c);
    for (int i = 0; i < 1000; i++) {
        wl_sha1_update(&c, block, sizeof block);
    }
    wl_sha1_final(&c, d);
    wl_sha1_to_hex(d, h);
    ASSERT_STR_EQ("34aa973cd4c4daa4f61eeb2bdbad27316534016f", h);
    PASS();
}

TEST sha1_chunking_does_not_matter(void) {
    uint8_t data[1000], d1[20], d2[20];
    for (size_t i = 0; i < sizeof data; i++) {
        data[i] = (uint8_t)(i * 13u + 5u);
    }
    wl_sha1_buf(data, sizeof data, d1);
    WlSha1 c;
    wl_sha1_init(&c);
    size_t off = 0, step = 1;
    while (off < sizeof data) {
        size_t n = step < sizeof data - off ? step : sizeof data - off;
        wl_sha1_update(&c, data + off, n);
        off += n;
        step = step * 3 + 1;
    }
    wl_sha1_final(&c, d2);
    ASSERT_MEM_EQ(d1, d2, 20);
    PASS();
}

TEST sha1_hex_roundtrip(void) {
    uint8_t d[20], e[20];
    char h[WL_SHA1_HEX_SIZE];
    wl_sha1_buf("x", 1, d);
    wl_sha1_to_hex(d, h);
    ASSERT_EQ(WL_OK, wl_sha1_from_hex(h, e));
    ASSERT_MEM_EQ(d, e, 20);
    ASSERT_EQ(WL_ERR_FORMAT, wl_sha1_from_hex("zz", e));
    ASSERT_EQ(WL_ERR_FORMAT, wl_sha1_from_hex("g9993e364706816aba3e25717850c26c9cd0d89d", e));
    PASS();
}

SUITE(sha1_suite) {
    RUN_TEST(sha1_abc);
    RUN_TEST(sha1_empty);
    RUN_TEST(sha1_448_bit_message);
    RUN_TEST(sha1_million_a_streamed);
    RUN_TEST(sha1_chunking_does_not_matter);
    RUN_TEST(sha1_hex_roundtrip);
}

static WlResult parse(WlJson *j, const char *s) {
    return wl_json_parse(j, s, strlen(s));
}

TEST json_reads_values(void) {
    WlJson j;
    int64_t n;
    int b;
    ASSERT_EQ(WL_OK, parse(&j, " {\"a\": [1, -2, true, null, \"x\\ty\\u00e9\"], \"b\": {\"c\": false}} "));
    WlJsonRef a = wl_json_get(&j, wl_json_root(&j), "a");
    ASSERT_EQ(WL_JSON_ARRAY, wl_json_type(&j, a));
    ASSERT_EQ(5, (int)wl_json_len(&j, a));
    ASSERT_EQ(WL_OK, wl_json_int(&j, wl_json_at(&j, a, 1), &n));
    ASSERT_EQ(-2, (int)n);
    ASSERT_EQ(WL_OK, wl_json_bool(&j, wl_json_at(&j, a, 2), &b));
    ASSERT_EQ(1, b);
    ASSERT_EQ(WL_JSON_NULL, wl_json_type(&j, wl_json_at(&j, a, 3)));
    ASSERT_STR_EQ("x\ty\xc3\xa9", wl_json_str(&j, wl_json_at(&j, a, 4)));
    WlJsonRef c = wl_json_get(&j, wl_json_get(&j, wl_json_root(&j), "b"), "c");
    ASSERT_EQ(WL_OK, wl_json_bool(&j, c, &b));
    ASSERT_EQ(0, b);
    ASSERT_EQ(WL_JSON_NONE, wl_json_get(&j, wl_json_root(&j), "missing"));
    ASSERT_EQ(WL_JSON_NONE, wl_json_at(&j, a, 5));
    wl_json_free(&j);
    PASS();
}

TEST json_large_integer(void) {
    WlJson j;
    int64_t n;
    ASSERT_EQ(WL_OK, parse(&j, "[4699979776]"));
    ASSERT_EQ(WL_OK, wl_json_int(&j, wl_json_at(&j, wl_json_root(&j), 0), &n));
    ASSERT_EQ(4699979776LL, (long long)n);
    wl_json_free(&j);
    PASS();
}

TEST json_rejects_bad_input(void) {
    static const char *const bad[] = {
        "",        "{",           "[1,]",       "{\"a\" 1}", "{\"a\":1} x", "\"abc",
        "[1.5]",   "[1e3]",       "tru",        "[\"\\ud800\"]", "{'a':1}",  "[\"a\nb\"]",
        "[99999999999999999999]", "{\"a\":1,}", "nul",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        WlJson j;
        if (parse(&j, bad[i]) == WL_OK) {
            wl_json_free(&j);
            FAILm(bad[i]);
        }
    }
    PASS();
}

TEST json_limits_depth(void) {
    char deep[100];
    WlJson j;
    memset(deep, '[', 40);
    memset(deep + 40, ']', 40);
    deep[80] = '\0';
    ASSERT_EQ(WL_ERR_RANGE, parse(&j, deep));
    PASS();
}

SUITE(json_suite) {
    RUN_TEST(json_reads_values);
    RUN_TEST(json_large_integer);
    RUN_TEST(json_rejects_bad_input);
    RUN_TEST(json_limits_depth);
}

TEST file_shim_roundtrip(void) {
    char dir[256], path[300], nested[320];
    uint8_t d1[20], d2[20];
    uint64_t size = 0;
    uint8_t *data;
    size_t len;
    ASSERT_EQ(WL_OK, wl_file_make_temp_dir(dir, sizeof dir, "wiilibs-host-"));
    snprintf(path, sizeof path, "%s/a.bin", dir);
    snprintf(nested, sizeof nested, "%s/x/y/b.bin", dir);
    ASSERT_EQ(WL_OK, wl_file_write_all(path, "hello", 5));
    ASSERT_EQ(WL_OK, wl_file_write_all(nested, "", 0)); /* creates parents */
    ASSERT_EQ(WL_OK, wl_file_sha1(path, d1, &size));
    ASSERT_EQ(5, (int)size);
    wl_sha1_buf("hello", 5, d2);
    ASSERT_MEM_EQ(d1, d2, 20);
    ASSERT_EQ(WL_OK, wl_file_read_all(path, 100, &data, &len));
    ASSERT_EQ(5, (int)len);
    ASSERT_STR_EQ("hello", (char *)data);
    free(data);
    ASSERT_EQ(WL_ERR_RANGE, wl_file_read_all(path, 4, &data, &len));
    ASSERT_EQ(WL_OK, wl_file_truncate(path, 2));
    ASSERT_EQ(WL_OK, wl_file_size(path, &size));
    ASSERT_EQ(2, (int)size);
    char **names;
    size_t n;
    ASSERT_EQ(WL_OK, wl_file_list(dir, &names, &n));
    ASSERT_EQ(1, (int)n); /* only regular files, directories are skipped */
    ASSERT_STR_EQ("a.bin", names[0]);
    wl_file_list_free(names, n);
    ASSERT_EQ(WL_OK, wl_file_remove_tree(dir));
    ASSERT_FALSE(wl_file_exists(dir));
    PASS();
}

SUITE(file_suite) {
    RUN_TEST(file_shim_roundtrip);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(sha1_suite);
    RUN_SUITE(json_suite);
    RUN_SUITE(file_suite);
    GREATEST_MAIN_END();
}
