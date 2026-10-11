// SPDX-License-Identifier: MIT
// Wii wrapper with fake tools: discovery order (env, PATH, flatpak, Windows exe), a successful
// extraction, a failing tool, and a tool that reports success but writes nothing.
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "wl/base/endian.h"
#include "wl/host/file.h"
#include "wl/host/sha1.h"
#include "wl/wiidisc/wii.h"
#include "greatest.h"

static char g_dir[512];

static void setup(void) {
    if (g_dir[0] == '\0') {
        (void)wl_file_make_temp_dir(g_dir, sizeof g_dir, "wl-wiidisc-");
    }
}

static void put(const char *name, const char *text, int exec, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s", g_dir, name);
    (void)wl_file_mkdirs_for(out);
    (void)wl_file_write_all(out, text, strlen(text));
    if (exec) {
        (void)chmod(out, 0755);
    }
}

/* Fake dolphin-tool body: parses -o DIR, then runs $WL_FAKE_MODE (ok | empty | fail). */
#define FAKE_BODY \
    "out=\n" \
    "while [ $# -gt 0 ]; do case \"$1\" in -o) out=\"$2\"; shift;; esac; shift; done\n" \
    "case \"$WL_FAKE_MODE\" in\n" \
    "  fail) exit 3;;\n" \
    "  empty) exit 0;;\n" \
    "  *) /bin/mkdir -p \"$out/DATA/sys\" \"$out/DATA/files/sub\" && printf 'dol' > \"$out/DATA/sys/main.dol\" &&\n" \
    "     printf 'aa' > \"$out/DATA/files/a.bin\" && printf 'bbb' > \"$out/DATA/files/sub/b.bin\";;\n" \
    "esac\n"

static void make_image(const char *name, const char *code, char *out, size_t cap) {
    uint8_t h[0x100];
    memset(h, 0, sizeof h);
    memcpy(h, code, 6);
    wl_wr_be32(h + 0x18, 0x5D1C9EA3u);
    snprintf(out, cap, "%s/%s", g_dir, name);
    (void)wl_file_write_all(out, h, sizeof h);
}

static void clear_env(void) {
    unsetenv("WL_DOLPHIN_TOOL");
    unsetenv("WL_DOLPHIN_WIN");
    unsetenv("WL_FAKE_MODE");
    unsetenv("WL_FAKE_LOG");
}

TEST discovery_order(void) {
    setup();
    clear_env();
    char bin[1100], dirA[1100], dirB[1100], p[1300];
    snprintf(bin, sizeof bin, "%s/bin", g_dir);
    put("bin/dolphin-tool", "#!/bin/sh\n" FAKE_BODY, 1, p, sizeof p);
    put("empty/placeholder", "x", 0, p, sizeof p);
    snprintf(dirA, sizeof dirA, "%s/empty", g_dir);
    WlWiiTool t;

    /* nothing anywhere */
    setenv("PATH", dirA, 1);
    setenv("WL_DOLPHIN_WIN", "/nonexistent/DolphinTool.exe", 1);
    ASSERT_EQ(WL_ERR_NO_TOOL, wl_wii_find_tool(&t));

    /* dolphin-tool on PATH */
    setenv("PATH", bin, 1);
    ASSERT_EQ(WL_OK, wl_wii_find_tool(&t));
    ASSERT_EQ(WL_WII_TOOL_NATIVE, t.kind);
    snprintf(p, sizeof p, "%s/dolphin-tool", bin);
    ASSERT_STR_EQ(p, t.path);

    /* env WL_DOLPHIN_TOOL wins over PATH */
    char envtool[1100];
    put("envtool", "#!/bin/sh\n" FAKE_BODY, 1, envtool, sizeof envtool);
    setenv("WL_DOLPHIN_TOOL", envtool, 1);
    ASSERT_EQ(WL_OK, wl_wii_find_tool(&t));
    ASSERT_STR_EQ(envtool, t.path);
    /* a bad env value falls through to PATH */
    setenv("WL_DOLPHIN_TOOL", "/nonexistent/tool", 1);
    ASSERT_EQ(WL_OK, wl_wii_find_tool(&t));
    ASSERT_STR_EQ(p, t.path);
    unsetenv("WL_DOLPHIN_TOOL");

    /* flatpak when it knows the app (and no dolphin-tool on PATH) */
    put("fp/flatpak", "#!/bin/sh\n[ \"$1\" = info ] && exit 0\n" FAKE_BODY, 1, p, sizeof p);
    snprintf(dirB, sizeof dirB, "%s/fp", g_dir);
    setenv("PATH", dirB, 1);
    ASSERT_EQ(WL_OK, wl_wii_find_tool(&t));
    ASSERT_EQ(WL_WII_TOOL_FLATPAK, t.kind);
    /* flatpak that does not know the app is not used */
    put("fp2/flatpak", "#!/bin/sh\nexit 1\n", 1, p, sizeof p);
    snprintf(dirB, sizeof dirB, "%s/fp2", g_dir);
    setenv("PATH", dirB, 1);
    ASSERT_EQ(WL_ERR_NO_TOOL, wl_wii_find_tool(&t));

    /* Windows exe plus wslpath */
    char win[1100];
    put("win/DolphinTool.exe", "#!/bin/sh\n" FAKE_BODY, 1, win, sizeof win);
    put("fp2/wslpath", "#!/bin/sh\nprintf '%s\\n' \"$2\"\n", 1, p, sizeof p);
    setenv("WL_DOLPHIN_WIN", win, 1);
    ASSERT_EQ(WL_OK, wl_wii_find_tool(&t));
    ASSERT_EQ(WL_WII_TOOL_WINDOWS, t.kind);
    ASSERT_STR_EQ(win, t.path);
    clear_env();
    PASS();
}

TEST extract_ok_and_cache(void) {
    setup();
    clear_env();
    char bin[1100], p[1100], img[1100], cache[1100], log[1100];
    snprintf(bin, sizeof bin, "%s/bin", g_dir);
    snprintf(cache, sizeof cache, "%s/cache", g_dir);
    snprintf(log, sizeof log, "%s/fp.log", g_dir);
    put("fpok/flatpak",
        "#!/bin/sh\n[ \"$1\" = info ] && exit 0\necho \"$@\" > \"$WL_FAKE_LOG\"\n" FAKE_BODY, 1, p,
        sizeof p);
    snprintf(bin, sizeof bin, "%s/fpok", g_dir);
    setenv("PATH", bin, 1);
    setenv("WL_FAKE_LOG", log, 1);
    setenv("WL_DOLPHIN_WIN", "/nonexistent/DolphinTool.exe", 1);
    make_image("t.iso", "TEST01", img, sizeof img);
    WlWiiDisc d;
    ASSERT_EQ(WL_OK, wl_wii_open(&d, img, cache, NULL, NULL));
    ASSERT_EQ(0, d.from_cache);
    ASSERT_EQ(2u, d.files);
    ASSERT_EQ(1u, d.dirs);
    ASSERT_EQ(5u, (unsigned)d.bytes);
    ASSERT_STR_EQ("TEST01", d.game_code);
    ASSERT_EQ(WL_WII_DOL_NO_REFERENCE, d.dol_check); /* no reference hash given */
    uint8_t dig[20];
    char hex[41];
    wl_sha1_buf("dol", 3, dig);
    wl_sha1_to_hex(dig, hex);
    ASSERT_STR_EQ(hex, d.main_dol_sha1);
    /* the flatpak got the cache root as a sandbox filesystem and the dolphin-tool command */
    uint8_t *txt;
    size_t len;
    ASSERT_EQ(WL_OK, wl_file_read_all(log, 4096, &txt, &len));
    char want[1200];
    snprintf(want, sizeof want, "--filesystem=%s", cache);
    ASSERT(strstr((const char *)txt, want) != NULL);
    ASSERT(strstr((const char *)txt, "--command=dolphin-tool org.DolphinEmu.dolphin-emu extract") !=
           NULL);
    ASSERT(strstr((const char *)txt, " -g") != NULL);
    free(txt);
    /* second open is served from the cache without any tool on PATH */
    char empty[1100];
    snprintf(empty, sizeof empty, "%s/none", g_dir);
    setenv("PATH", empty, 1);
    WlWiiDisc d2;
    ASSERT_EQ(WL_OK, wl_wii_open(&d2, img, cache, NULL, NULL));
    ASSERT_EQ(1, d2.from_cache);
    ASSERT_STR_EQ(d.root, d2.root);
    ASSERT_EQ(d.files, d2.files);
    /* the caller-supplied main.dol reference: match (any case), mismatch, malformed */
    char upper[41];
    for (int i = 0; i < 41; i++) {
        upper[i] = (hex[i] >= 'a' && hex[i] <= 'f') ? (char)(hex[i] - 'a' + 'A') : hex[i];
    }
    ASSERT_EQ(WL_OK, wl_wii_open(&d2, img, cache, NULL, upper));
    ASSERT_EQ(WL_WII_DOL_MATCH, d2.dol_check);
    ASSERT_STR_EQ(hex, d2.dol_expected);
    ASSERT_EQ(WL_OK,
              wl_wii_open(&d2, img, cache, NULL, "0000000000000000000000000000000000000000"));
    ASSERT_EQ(WL_WII_DOL_MISMATCH, d2.dol_check);
    ASSERT_EQ(WL_ERR_ARG, wl_wii_open(&d2, img, cache, NULL, "not-a-hash"));
    clear_env();
    PASS();
}

TEST extract_failures(void) {
    setup();
    clear_env();
    char bin[1100], p[1100], img[1100], cache[1100];
    snprintf(cache, sizeof cache, "%s/cache2", g_dir);
    put("tool/dolphin-tool", "#!/bin/sh\n" FAKE_BODY, 1, p, sizeof p);
    snprintf(bin, sizeof bin, "%s/tool", g_dir);
    setenv("PATH", bin, 1);
    make_image("f.iso", "TEST01", img, sizeof img);
    WlWiiDisc d;

    setenv("WL_FAKE_MODE", "fail", 1);
    ASSERT_EQ(WL_ERR_TOOL_FAILED, wl_wii_open(&d, img, cache, NULL, NULL));
    /* a failed run leaves no usable cache entry */
    setenv("WL_FAKE_MODE", "empty", 1);
    ASSERT_EQ(WL_ERR_EMPTY, wl_wii_open(&d, img, cache, NULL, NULL));
    setenv("WL_FAKE_MODE", "ok", 1);
    ASSERT_EQ(WL_OK, wl_wii_open(&d, img, cache, NULL, NULL)); /* recovers once the tool works */
    ASSERT_EQ(0, d.from_cache);

    /* no tool at all */
    snprintf(cache, sizeof cache, "%s/cache3", g_dir);
    snprintf(bin, sizeof bin, "%s/none", g_dir);
    setenv("PATH", bin, 1);
    setenv("WL_DOLPHIN_WIN", "/nonexistent/DolphinTool.exe", 1);
    ASSERT_EQ(WL_ERR_NO_TOOL, wl_wii_open(&d, img, cache, NULL, NULL));

    /* a GameCube-style image is not a Wii disc */
    uint8_t h[0x100] = {0};
    char gc[1100];
    snprintf(gc, sizeof gc, "%s/gc.iso", g_dir);
    wl_wr_be32(h + 0x1C, 0xC2339F3Du);
    ASSERT_EQ(WL_OK, wl_file_write_all(gc, h, sizeof h));
    ASSERT_EQ(WL_ERR_FORMAT, wl_wii_open(&d, gc, cache, NULL, NULL));
    clear_env();
    PASS();
}

TEST windows_tool_extracts(void) {
    setup();
    clear_env();
    char bin[1100], p[1100], win[1100], img[1100], cache[1100];
    snprintf(cache, sizeof cache, "%s/cache4", g_dir);
    put("wbin/wslpath", "#!/bin/sh\nprintf '%s\\n' \"$2\"\n", 1, p, sizeof p);
    put("wbin/DolphinTool.exe", "#!/bin/sh\n" FAKE_BODY, 1, win, sizeof win);
    snprintf(bin, sizeof bin, "%s/wbin:%s/none", g_dir, g_dir);
    setenv("PATH", bin, 1);
    setenv("WL_DOLPHIN_WIN", win, 1);
    make_image("w.iso", "TEST01", img, sizeof img);
    WlWiiDisc d;
    ASSERT_EQ(WL_OK, wl_wii_open(&d, img, cache, NULL, NULL));
    ASSERT_EQ(2u, d.files);
    clear_env();
    PASS();
}

TEST reference_hash_lookup(void) {
    static const char J[] =
        "{\"format\":1,\"dumps\":[{\"game_code\":\"AAAA01\",\"inner\":[{\"path\":\"sys/main.dol\","
        "\"sha1\":\"ffcd539d239722b8bb27e45c9ad4aacc818c8933\",\"verified\":true}]},"
        "{\"game_code\":\"BBBB01\",\"inner\":[{\"path\":\"sys/main.dol\",\"sha1\":null,"
        "\"verified\":false}]},{\"game_code\":\"CCCC01\"}]}";
    char hex[41];
    int ver = 0;
    ASSERT_EQ(1, wl_wii_reference_hash(J, sizeof J - 1, "AAAA01", "sys/main.dol", hex, &ver));
    ASSERT_STR_EQ("ffcd539d239722b8bb27e45c9ad4aacc818c8933", hex);
    ASSERT_EQ(1, ver);
    ASSERT_EQ(0, wl_wii_reference_hash(J, sizeof J - 1, "BBBB01", "sys/main.dol", hex, &ver));
    ASSERT_EQ(0, wl_wii_reference_hash(J, sizeof J - 1, "CCCC01", "sys/main.dol", hex, &ver));
    ASSERT_EQ(0, wl_wii_reference_hash(J, sizeof J - 1, "AAAA01", "sys/other", hex, &ver));
    ASSERT_EQ(0, wl_wii_reference_hash(J, sizeof J - 1, "ZZZZ99", "sys/main.dol", hex, &ver));
    ASSERT_EQ(0, wl_wii_reference_hash("{not json", 9, "AAAA01", "sys/main.dol", hex, &ver));
    PASS();
}

SUITE(wii_suite) {
    RUN_TEST(discovery_order);
    RUN_TEST(extract_ok_and_cache);
    RUN_TEST(extract_failures);
    RUN_TEST(windows_tool_extracts);
    RUN_TEST(reference_hash_lookup);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(wii_suite);
    if (g_dir[0] != '\0') {
        (void)wl_file_remove_tree(g_dir);
    }
    GREATEST_MAIN_END();
}
