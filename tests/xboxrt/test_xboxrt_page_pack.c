// SPDX-License-Identifier: MIT
// A guest page packed small and back (page_pack.c): every kind of page comes back byte for byte
// (zero, one word repeated, a table of records, floats, text, random bytes), a page that does not
// fit the room given is refused, and bytes that are not a packed page (cut short, a distance before
// the page, a length past it, garbage) are refused without writing outside the page.
#include <stdlib.h>
#include <string.h>

#include "greatest.h"
#include "wl/xboxrt/mem_internal.h"

#define PAGE WL_XB_PAGE

static uint8_t g_page[PAGE], g_back[PAGE + 64], g_packed[PAGE + 64];
static uint32_t g_rng = 12345u;

static uint32_t rnd(void) {
    g_rng = g_rng * 1103515245u + 12345u;
    return g_rng >> 8;
}

enum { K_ZERO, K_WORD, K_RECORDS, K_FLOATS, K_TEXT, K_RANDOM, K_SPARSE, K_KINDS };

static void fill(int kind) {
    switch (kind) {
    case K_ZERO:
        memset(g_page, 0, PAGE);
        break;
    case K_WORD:
        for (uint32_t i = 0; i < PAGE; i++) {
            g_page[i] = (uint8_t)(0xa5u + (i & 3u));
        }
        break;
    case K_RECORDS: // 32-byte records: an index, a flag word, pointers into one region
        for (uint32_t i = 0; i < PAGE; i += 32u) {
            uint32_t w[8] = {i / 32u, 0x00010000u, 0x80123400u + (rnd() & 0xff0u), 0, 0xffffffffu,
                             0x80200000u + i * 4u, 0, 0x3f800000u};
            memcpy(g_page + i, w, 32);
        }
        break;
    case K_FLOATS: // vertices: positions drift, normals and colours repeat
        for (uint32_t i = 0; i < PAGE; i += 16u) {
            float v[4] = {(float)(i / 16u) * 0.25f, (float)(rnd() & 63u), 1.0f, 0.0f};
            memcpy(g_page + i, v, 16);
        }
        break;
    case K_TEXT:
        for (uint32_t i = 0; i < PAGE; i++) {
            static const char words[] = "levels\\a10\\a10 scenery\\bridge shader_model object ";
            g_page[i] = (uint8_t)words[(i * 7u + rnd() % 3u) % (sizeof words - 1u)];
        }
        break;
    case K_RANDOM:
        for (uint32_t i = 0; i < PAGE; i++) {
            g_page[i] = (uint8_t)rnd();
        }
        break;
    default: // a few words set in a cleared page
        memset(g_page, 0, PAGE);
        for (uint32_t i = 0; i < 40u; i++) {
            g_page[rnd() % PAGE] = (uint8_t)rnd();
        }
        break;
    }
}

TEST every_kind_comes_back(void) {
    for (int kind = 0; kind < K_KINDS; kind++) {
        for (int round = 0; round < 4; round++) {
            fill(kind);
            size_t n = wl_xb_page_pack(g_page, g_packed, PAGE + 64u);
            ASSERT(n > 0 && n <= PAGE + 64u);
            memset(g_back, 0xee, sizeof g_back);
            ASSERT(wl_xb_page_unpack(g_packed, n, g_back));
            ASSERT_MEM_EQ(g_page, g_back, PAGE);
            ASSERT_EQ(0xee, g_back[PAGE]); // nothing past the page
            if (kind == K_ZERO || kind == K_WORD) {
                ASSERT(n < 40u); // a run of one word is one match
            }
            if (kind == K_RECORDS || kind == K_SPARSE) {
                ASSERT(n < PAGE / 2u);
            }
        }
    }
    PASS();
}

TEST a_page_that_does_not_fit_is_refused(void) {
    fill(K_RANDOM);
    ASSERT_EQ(0u, wl_xb_page_pack(g_page, g_packed, PAGE * 3u / 4u));
    fill(K_RECORDS);
    size_t n = wl_xb_page_pack(g_page, g_packed, PAGE);
    ASSERT(n > 0);
    ASSERT_EQ(0u, wl_xb_page_pack(g_page, g_packed, n - 1u));
    ASSERT_EQ(n, wl_xb_page_pack(g_page, g_packed, n));
    PASS();
}

TEST bad_bytes_are_refused(void) {
    fill(K_TEXT);
    size_t n = wl_xb_page_pack(g_page, g_packed, PAGE + 64u);
    ASSERT(n > 8u);
    for (size_t cut = 0; cut < n; cut++) {
        ASSERT_FALSE(wl_xb_page_unpack(g_packed, cut, g_back)); // short: the page is not whole
    }
    // a match before the page's start
    static const uint8_t before[] = {0x10, 'x', 0x02, 0x00};
    ASSERT_FALSE(wl_xb_page_unpack(before, sizeof before, g_back));
    // a match longer than the page
    static const uint8_t longer[] = {0x1f, 'x', 0x01, 0x00, 255, 255, 255, 255, 255, 255, 255, 255,
                                     255, 255, 255, 255, 255, 255, 255, 255, 255, 0};
    ASSERT_FALSE(wl_xb_page_unpack(longer, sizeof longer, g_back));
    // literals past the input
    static const uint8_t past[] = {0xf0, 200, 'a', 'b'};
    ASSERT_FALSE(wl_xb_page_unpack(past, sizeof past, g_back));
    // garbage: refused or a page, never a write past it
    for (int round = 0; round < 2000; round++) {
        size_t len = 1u + rnd() % 64u;
        for (size_t i = 0; i < len; i++) {
            g_packed[i] = (uint8_t)rnd();
        }
        g_back[PAGE] = 0xee;
        (void)wl_xb_page_unpack(g_packed, len, g_back);
        ASSERT_EQ(0xee, g_back[PAGE]);
    }
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(every_kind_comes_back);
    RUN_TEST(a_page_that_does_not_fit_is_refused);
    RUN_TEST(bad_bytes_are_refused);
    GREATEST_MAIN_END();
}
