// SPDX-License-Identifier: MIT
// The file suite (suite_fs.c) on the host stub: sd:/ and usb:/ are
// temp directories. Run once with SD only (USB absent, as in Dolphin) and once with both
// devices; plus host-only checks of the stub's path rules.
#define _POSIX_C_SOURCE 200809L
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "wl/platform/fs.h"
#include "wl/platform/host/fs_host.h"
#include "tests/platform/check.h"
#include "greatest.h"

static int g_fail;
static int g_pass;

void wlt_report(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

void wlt_check(const char *name, bool ok, const char *detail) {
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
        printf("FAIL %s: %s\n", name, detail ? detail : "-");
    }
}

void wlt_suite_fs(void);

static char g_sd[256];
static char g_usb[256];

static void make_dirs(void) {
    const char *base = getenv("TMPDIR");
    snprintf(g_sd, sizeof g_sd, "%s/wlfs_sd_XXXXXX", base != NULL ? base : "/tmp");
    snprintf(g_usb, sizeof g_usb, "%s/wlfs_usb_XXXXXX", base != NULL ? base : "/tmp");
    if (mkdtemp(g_sd) == NULL || mkdtemp(g_usb) == NULL) {
        perror("mkdtemp");
        exit(2);
    }
}

TEST host_paths(void) {
    WlPlatformFile *f;
    wl_host_fs_set_root(WL_DEVICE_SD, NULL);
    wl_host_fs_set_root(WL_DEVICE_USB, NULL);
    ASSERT_EQ(WL_ERR_NOT_FOUND, wl_platform_fs_mount(WL_DEVICE_SD));
    ASSERT(!wl_platform_fs_mounted(WL_DEVICE_SD));
    ASSERT_EQ(WL_ERR_STATE, wl_platform_fs_open("sd:/x", WL_FILE_READ, &f));
    wl_host_fs_set_root(WL_DEVICE_SD, g_sd);
    ASSERT_EQ(WL_OK, wl_platform_fs_mount(WL_DEVICE_SD));
    ASSERT_EQ(WL_ERR_ARG, wl_platform_fs_open("sd:/../escape", WL_FILE_READ, &f));
    ASSERT_EQ(WL_ERR_ARG, wl_platform_fs_open("SD:/x", WL_FILE_READ, &f));
    ASSERT_EQ(WL_ERR_NOT_FOUND, wl_platform_fs_open("sd:/nothing", WL_FILE_READ, &f));
    wl_platform_fs_unmount(WL_DEVICE_SD);
    ASSERT(!wl_platform_fs_mounted(WL_DEVICE_SD));
    ASSERT_EQ(WL_ERR_STATE, wl_platform_fs_open("sd:/x", WL_FILE_READ, &f));
    uint32_t ios = 0;
    ASSERT_EQ(WL_OK, wl_platform_fs_ios_init(&ios));
    ASSERT_EQ(58u, ios);
    PASS();
}

TEST fs_sd_only(void) {
    wl_host_fs_set_root(WL_DEVICE_SD, g_sd);
    wl_host_fs_set_root(WL_DEVICE_USB, NULL);
    wlt_suite_fs();
    ASSERT(g_pass > 55);
    ASSERT_EQ(0, g_fail);
    PASS();
}

TEST fs_sd_and_usb(void) {
    g_pass = 0;
    wl_host_fs_set_root(WL_DEVICE_SD, g_sd);
    wl_host_fs_set_root(WL_DEVICE_USB, g_usb);
    wlt_suite_fs();
    ASSERT(g_pass > 60);
    ASSERT_EQ(0, g_fail);
    PASS();
}

// Makes `rel` below `root` and every folder on the way, as mkdir -p would.
static void make_tree(const char *root, const char *rel) {
    char path[600];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    for (char *p = path + strlen(root) + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(path, 0777);
            *p = '/';
        }
    }
    mkdir(path, 0777);
}

static void touch(const char *root, const char *rel) {
    char path[600];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    int fd = open(path, O_WRONLY | O_CREAT, 0666);
    if (fd >= 0) {
        close(fd);
    }
}

TEST exists_finds_files_and_directories(void) {
    wl_host_fs_set_root(WL_DEVICE_SD, g_sd);
    wl_host_fs_set_root(WL_DEVICE_USB, NULL);
    ASSERT(!wl_platform_fs_exists("sd:/exists_dir"));
    ASSERT_EQ(WL_OK, wl_platform_fs_mount(WL_DEVICE_SD));
    make_tree(g_sd, "exists_dir");
    touch(g_sd, "exists_dir/file");
    ASSERT(wl_platform_fs_exists("sd:/exists_dir"));
    ASSERT(wl_platform_fs_exists("sd:/exists_dir/file"));
    ASSERT(!wl_platform_fs_exists("sd:/exists_dir/missing"));
    ASSERT(!wl_platform_fs_exists("usb:/exists_dir"));
    ASSERT(!wl_platform_fs_exists("sd:/../etc"));
    ASSERT(!wl_platform_fs_exists("exists_dir"));
    ASSERT(!wl_platform_fs_exists(NULL));
    wl_platform_fs_unmount(WL_DEVICE_SD);
    ASSERT(!wl_platform_fs_exists("sd:/exists_dir"));
    PASS();
}

static void mount_both(void) {
    wl_host_fs_set_root(WL_DEVICE_SD, g_sd);
    wl_host_fs_set_root(WL_DEVICE_USB, g_usb);
    wl_platform_fs_mount(WL_DEVICE_SD);
    wl_platform_fs_mount(WL_DEVICE_USB);
}

TEST app_dir_prefers_sd_and_falls_back_to_usb(void) {
    char out[64];
    mount_both();
    // nothing anywhere: the SD card's folder, where a first run would make it
    ASSERT_EQ(WL_ERR_NOT_FOUND, wl_platform_fs_find_app_dir("game", "maps", out, sizeof out));
    ASSERT_STR_EQ("sd:/apps/game", out);
    // the SD card has the folder but not the marker (a stale or empty folder); USB has both
    make_tree(g_sd, "apps/game");
    make_tree(g_usb, "apps/game/maps");
    ASSERT_EQ(WL_OK, wl_platform_fs_find_app_dir("game", "maps", out, sizeof out));
    ASSERT_STR_EQ("usb:/apps/game", out);
    // without a marker the SD card's folder is enough
    ASSERT_EQ(WL_OK, wl_platform_fs_find_app_dir("game", NULL, out, sizeof out));
    ASSERT_STR_EQ("sd:/apps/game", out);
    ASSERT_EQ(WL_OK, wl_platform_fs_find_app_dir("game", "", out, sizeof out));
    ASSERT_STR_EQ("sd:/apps/game", out);
    // both qualify: the SD card wins
    make_tree(g_sd, "apps/game/maps");
    ASSERT_EQ(WL_OK, wl_platform_fs_find_app_dir("game", "maps", out, sizeof out));
    ASSERT_STR_EQ("sd:/apps/game", out);
    // a marker below the folder, and a file as marker
    make_tree(g_usb, "apps/other/data");
    touch(g_usb, "apps/other/data/game.pak");
    ASSERT_EQ(WL_OK, wl_platform_fs_find_app_dir("other", "data/game.pak", out, sizeof out));
    ASSERT_STR_EQ("usb:/apps/other", out);
    ASSERT_EQ(WL_ERR_NOT_FOUND, wl_platform_fs_find_app_dir("other", "data/missing.pak", out, sizeof out));
    ASSERT_STR_EQ("sd:/apps/other", out);
    PASS();
}

TEST app_dir_skips_an_unmounted_device(void) {
    char out[64];
    mount_both();
    make_tree(g_sd, "apps/lonely/maps");
    make_tree(g_usb, "apps/lonely/maps");
    wl_platform_fs_unmount(WL_DEVICE_SD);
    ASSERT_EQ(WL_OK, wl_platform_fs_find_app_dir("lonely", "maps", out, sizeof out));
    ASSERT_STR_EQ("usb:/apps/lonely", out);
    wl_platform_fs_unmount(WL_DEVICE_USB);
    ASSERT_EQ(WL_ERR_NOT_FOUND, wl_platform_fs_find_app_dir("lonely", "maps", out, sizeof out));
    ASSERT_STR_EQ("sd:/apps/lonely", out);
    PASS();
}

TEST app_dir_rejects_bad_arguments(void) {
    char out[64] = "untouched";
    mount_both();
    ASSERT_EQ(WL_ERR_ARG, wl_platform_fs_find_app_dir(NULL, "maps", out, sizeof out));
    ASSERT_EQ(WL_ERR_ARG, wl_platform_fs_find_app_dir("", "maps", out, sizeof out));
    ASSERT_EQ(WL_ERR_ARG, wl_platform_fs_find_app_dir("a/b", "maps", out, sizeof out));
    ASSERT_EQ(WL_ERR_ARG, wl_platform_fs_find_app_dir("..", "maps", out, sizeof out));
    ASSERT_EQ(WL_ERR_ARG, wl_platform_fs_find_app_dir(".hidden", "maps", out, sizeof out));
    ASSERT_EQ(WL_ERR_ARG, wl_platform_fs_find_app_dir("game", "/maps", out, sizeof out));
    ASSERT_EQ(WL_ERR_ARG, wl_platform_fs_find_app_dir("game", "../maps", out, sizeof out));
    ASSERT_EQ(WL_ERR_ARG, wl_platform_fs_find_app_dir("game", "a/../maps", out, sizeof out));
    ASSERT_EQ(WL_ERR_ARG, wl_platform_fs_find_app_dir("game", "a\\b", out, sizeof out));
    ASSERT_EQ(WL_ERR_ARG, wl_platform_fs_find_app_dir("game", "maps", NULL, sizeof out));
    ASSERT_EQ(WL_ERR_ARG, wl_platform_fs_find_app_dir("game", "maps", out, 0));
    // the answer does not fit: "sd:/apps/game" is 13 characters and its end
    ASSERT_EQ(WL_ERR_ARG, wl_platform_fs_find_app_dir("game", "maps", out, 13));
    char name[WL_FS_PATH_MAX + 8];
    memset(name, 'n', sizeof name - 1);
    name[sizeof name - 1] = '\0';
    ASSERT_EQ(WL_ERR_ARG, wl_platform_fs_find_app_dir(name, "maps", out, sizeof out));
    ASSERT_STR_EQ("untouched", out);
    // exactly fitting
    ASSERT_EQ(WL_ERR_NOT_FOUND, wl_platform_fs_find_app_dir("nowhere", "maps", out, 17));
    ASSERT_STR_EQ("sd:/apps/nowhere", out);
    PASS();
}

SUITE(fs_suite) {
    RUN_TEST(host_paths);
    RUN_TEST(fs_sd_only);
    RUN_TEST(fs_sd_and_usb);
    RUN_TEST(exists_finds_files_and_directories);
    RUN_TEST(app_dir_prefers_sd_and_falls_back_to_usb);
    RUN_TEST(app_dir_skips_an_unmounted_device);
    RUN_TEST(app_dir_rejects_bad_arguments);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    make_dirs();
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(fs_suite);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' '%s'", g_sd, g_usb); // our own mkdtemp names
    int rc = system(cmd);
    (void)rc;
    GREATEST_MAIN_END();
}
