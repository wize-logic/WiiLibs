// SPDX-License-Identifier: MIT
// A main for a recompiled title that needs nothing of its own (wl_xboxrt_main): on the host the
// title's folder (and optionally the hard disk's folder) as arguments, on the Wii
// sd:/xbox/<WL_XB_TITLE_NAME> (a compile definition, "title" by default).
#define _DEFAULT_SOURCE
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wl/platform/platform.h"
#include "wl/xboxrt/boot.h"

#if !defined(__wii__)
#include "wl/platform/host/fs_host.h"
#endif

#ifndef WL_XB_TITLE_NAME
#define WL_XB_TITLE_NAME "title"
#endif

#if !defined(__wii__)
// A host path as a platform path: the card's root is the file system's root.
static void platform_path(const char *host, char *out, size_t cap) {
    char abs[PATH_MAX];
    if (realpath(host, abs) == NULL) {
        snprintf(abs, sizeof abs, "%s", host);
    }
    snprintf(out, cap, "sd:/%s", abs[0] == '/' ? abs + 1 : abs);
}
#endif

__attribute__((weak)) void wl_xb_title_setup(WlXbTitleConfig *cfg) {
    (void)cfg;
}

int main(int argc, char **argv) {
    WlXbTitleConfig cfg;
    wl_xb_title_config_default(&cfg);
#if defined(__wii__)
    (void)argc;
    (void)argv;
    cfg.title_dir = "sd:/xbox/" WL_XB_TITLE_NAME;
#else
    static char title[512], data[512];
    if (argc < 2) {
        fprintf(stderr, "usage: %s <title folder with default.xbe> [hard disk folder]\n", argv[0]);
        return 2;
    }
    wl_host_fs_set_root(WL_DEVICE_SD, "/");
    platform_path(argv[1], title, sizeof title);
    cfg.title_dir = title;
    if (argc >= 3) {
        platform_path(argv[2], data, sizeof data);
        cfg.data_dir = data;
    }
    if (getenv("WL_XB_TRACE") != NULL) {
        cfg.trace_kernel_calls = true;
    }
#endif
    wl_xb_title_setup(&cfg);
#if !defined(__wii__)
    // the play harness from the environment (play.h): WL_XB_PLAY=script:<host path>|bot:<n>|look:<n>,
    // WL_XB_PLAY_OUT=<host folder> for its screenshots
    static char play[600], play_out[512];
    const char *env = getenv("WL_XB_PLAY");
    if (env != NULL && env[0] != '\0') {
        if (strncmp(env, "script:", 7) == 0) {
            char path[512];
            platform_path(env + 7, path, sizeof path);
            snprintf(play, sizeof play, "script:%s", path);
        } else {
            snprintf(play, sizeof play, "%s", env);
        }
        cfg.play = play;
    }
    env = getenv("WL_XB_PLAY_OUT");
    if (env != NULL && env[0] != '\0') {
        platform_path(env, play_out, sizeof play_out);
        cfg.play_out = play_out;
    }
#endif
    int code = wl_xb_boot_run(&cfg);
#if defined(__wii__)
    // the title has left (or never started): wound down, the log and the card closed, then back
    // to the loader, or off where nothing loaded us (Dolphin booting the DOL itself)
    WL_LOG_I("xbmain", "the title left with %d", code);
    wl_platform_exit(WL_EXIT_LOADER);
#else
    return code < 0 ? 1 : code;
#endif
}
