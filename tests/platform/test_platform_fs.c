// SPDX-License-Identifier: MIT
// The file suite (suite_fs.c) on the host stub: sd:/ and usb:/ are
// temp directories. Run once with SD only (USB absent, as in Dolphin) and once with both
// devices; plus host-only checks of the stub's path rules.
#define _POSIX_C_SOURCE 200809L
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

SUITE(fs_suite) {
    RUN_TEST(host_paths);
    RUN_TEST(fs_sd_only);
    RUN_TEST(fs_sd_and_usb);
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
