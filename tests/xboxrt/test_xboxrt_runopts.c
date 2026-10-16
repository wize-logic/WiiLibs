// SPDX-License-Identifier: MIT
// The run's options from the command line and netrun.txt (wl/xboxrt/runopts.h): flag files,
// settings files (read by the profiler's and the counters' loaders, which use stdio), the play
// setting, the remote log's spec, what is refused, and the order: arguments before netrun.txt.
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "greatest.h"
#include "wl/platform/host/fs_host.h"
#include "wl/platform/hwcount.h"
#include "wl/platform/platform.h"
#include "wl/platform/profile_arm.h"
#include "wl/xboxrt/runopts.h"

static char g_dir[256];

static void prepare_fs(void) {
    const char *base = getenv("TMPDIR");
    snprintf(g_dir, sizeof g_dir, "%s/wlro_XXXXXX", base != NULL ? base : "/tmp");
    if (mkdtemp(g_dir) == NULL) {
        perror("mkdtemp");
        exit(2);
    }
    wl_host_fs_set_root(WL_DEVICE_SD, g_dir);
    wl_platform_fs_mount(WL_DEVICE_SD);
}

TEST options_are_recognised(void) {
    ASSERT(wl_xb_run_option_is("play=script:a"));
    ASSERT(wl_xb_run_option_is("switch=noxf"));
    ASSERT(wl_xb_run_option_is("file=x:y"));
    ASSERT(wl_xb_run_option_is("net=1.2.3.4:5"));
    ASSERT(wl_xb_run_option_is("run=a"));
    ASSERT(!wl_xb_run_option_is("/home/someone/hdd"));
    ASSERT(!wl_xb_run_option_is("play"));
    ASSERT(!wl_xb_run_option_is(NULL));
    PASS();
}

TEST switches_are_flag_files(void) {
    WlXbRunOptions o;
    WlPlatformFile *f = NULL;
    char text[16];
    uint32_t got = 0;

    memset(&o, 0, sizeof o);
    ASSERT_EQ(WL_OK, wl_xb_run_option_apply("sd:/xbox/t1", "switch=noxf,novcache,vcache=2048,profile", &o));
    ASSERT_EQ(4u, o.virtual_files);
    ASSERT(wl_platform_fs_exists("sd:/xbox/t1/noxf"));
    ASSERT(wl_platform_fs_exists("sd:/xbox/t1/novcache"));
    ASSERT(wl_platform_fs_exists("sd:/xbox/t1/profile.txt")); // (the name of the file that holds settings)
    ASSERT(!wl_platform_fs_exists("sd:/xbox/t1/hwcount.txt"));
    ASSERT(!wl_platform_fs_exists("sd:/xbox/t1/profile"));
    ASSERT_EQ(WL_OK, wl_platform_fs_open("sd:/xbox/t1/vcache", WL_FILE_READ, &f));
    ASSERT_EQ(WL_OK, wl_platform_fs_read_some_at(f, 0, text, sizeof text - 1u, &got));
    wl_platform_fs_close(f);
    text[got] = '\0';
    ASSERT_STR_EQ("2048", text);
    // the profiler's loader (stdio) sees a virtual file too: an empty one is the defaults
    WlProfileArmSettings s;
    ASSERT_EQ(WL_OK, wl_profile_arm_settings_load(&s, "sd:/xbox/t1/profile.txt", NULL, NULL));
    WlProfileArmSettings d;
    wl_profile_arm_settings_default(&d);
    ASSERT_EQ(d.seconds, s.seconds);
    ASSERT_EQ(WL_ERR_NOT_FOUND, wl_profile_arm_settings_load(&s, "sd:/xbox/t1/none.txt", NULL, NULL));
    PASS();
}

TEST a_file_option_gives_the_text(void) {
    WlXbRunOptions o;
    WlProfileArmSettings s;
    WlHwcountSettings h;

    memset(&o, 0, sizeof o);
    ASSERT_EQ(WL_OK, wl_xb_run_option_apply("sd:/xbox/t2", "file=profile.txt:after 30;seconds 7;repeat 2", &o));
    ASSERT_EQ(WL_OK, wl_xb_run_option_apply("sd:/xbox/t2", "file=hwcount.txt:", &o));
    ASSERT_EQ(WL_OK, wl_profile_arm_settings_load(&s, "sd:/xbox/t2/profile.txt", NULL, NULL));
    ASSERT_EQ(30u, s.after);
    ASSERT_EQ(7u, s.seconds);
    ASSERT_EQ(2u, s.repeat);
    ASSERT_EQ(WL_OK, wl_hwcount_settings_load(&h, "sd:/xbox/t2/hwcount.txt", NULL, NULL));
    PASS();
}

TEST strings_and_refusals(void) {
    WlXbRunOptions o;

    memset(&o, 0, sizeof o);
    ASSERT_EQ(WL_OK, wl_xb_run_option_apply("sd:/xbox/t3", "play=replay:play/a30.cap 5", &o));
    ASSERT_STR_EQ("replay:play/a30.cap 5", o.play);
    ASSERT_EQ(WL_OK, wl_xb_run_option_apply("sd:/xbox/t3", "play=script:other", &o)); // (the first wins)
    ASSERT_STR_EQ("replay:play/a30.cap 5", o.play);
    ASSERT_EQ(WL_OK, wl_xb_run_option_apply("sd:/xbox/t3", "net=192.168.1.9:7777", &o));
    ASSERT_STR_EQ("192.168.1.9:7777", o.net);
    ASSERT_EQ(WL_OK, wl_xb_run_option_apply("sd:/xbox/t3", "run=a30 bench", &o));
    ASSERT_STR_EQ("a30 bench", o.name);
    WlXbRunOptions empty;
    memset(&empty, 0, sizeof empty);
    ASSERT_EQ(WL_ERR_FORMAT, wl_xb_run_option_apply("sd:/xbox/t3", "play=", &empty));
    ASSERT_EQ(WL_ERR_FORMAT, wl_xb_run_option_apply("sd:/xbox/t3", "colour=red", &o));
    ASSERT_EQ(WL_ERR_FORMAT, wl_xb_run_option_apply("sd:/xbox/t3", "switch=../x", &o));
    ASSERT_EQ(WL_ERR_FORMAT, wl_xb_run_option_apply("sd:/xbox/t3", "switch=a/b", &o));
    ASSERT_EQ(WL_ERR_FORMAT, wl_xb_run_option_apply("sd:/xbox/t3", "switch=.hidden", &o));
    ASSERT_EQ(WL_ERR_FORMAT, wl_xb_run_option_apply("sd:/xbox/t3", "switch=a b", &o));
    ASSERT_EQ(WL_ERR_FORMAT, wl_xb_run_option_apply("sd:/xbox/t3", "file=noname", &o));
    ASSERT(!wl_platform_fs_exists("sd:/xbox/t3/x"));
    // a bad name among good ones does not stop the good
    ASSERT_EQ(WL_ERR_FORMAT, wl_xb_run_option_apply("sd:/xbox/t3", "switch=ok1,a/b,ok2", &o));
    ASSERT(wl_platform_fs_exists("sd:/xbox/t3/ok1") && wl_platform_fs_exists("sd:/xbox/t3/ok2"));
    ASSERT_EQ(WL_ERR_ARG, wl_xb_run_option_apply(NULL, "run=a", &o));
    PASS();
}

TEST arguments_come_before_netrun_txt(void) {
    WlXbRunOptions o;
    WlPlatformFile *f = NULL;
    static const char netrun[] = "# a Dolphin test\n"
                                 "net=127.0.0.1:7000\n"
                                 "\n"
                                 "  switch=deterministic,vcache=512\r\n"
                                 "play=script:from_file\n"
                                 "bogus line\n";
    char *args[] = {(char *)"net=listen:7777", (char *)"switch=vcache=4096", (char *)"not an option"};

    ASSERT_EQ(WL_OK, wl_platform_fs_mkdir("sd:/xbox"));
    ASSERT_EQ(WL_OK, wl_platform_fs_mkdir("sd:/xbox/t4"));
    ASSERT_EQ(WL_OK, wl_platform_fs_open("sd:/xbox/t4/netrun.txt", WL_FILE_WRITE_NEW, &f));
    ASSERT_EQ(WL_OK, wl_platform_fs_write(f, netrun, sizeof netrun - 1u));
    wl_platform_fs_close(f);

    wl_xb_run_options_load("sd:/xbox/t4", 3, args, &o);
    ASSERT_STR_EQ("listen:7777", o.net);            // the argument won
    ASSERT_STR_EQ("script:from_file", o.play);      // the file's, nothing else gave one
    ASSERT(wl_platform_fs_exists("sd:/xbox/t4/deterministic"));
    char text[8];
    uint32_t got = 0;
    ASSERT_EQ(WL_OK, wl_platform_fs_open("sd:/xbox/t4/vcache", WL_FILE_READ, &f));
    ASSERT_EQ(WL_OK, wl_platform_fs_read_some_at(f, 0, text, sizeof text - 1u, &got));
    wl_platform_fs_close(f);
    text[got] = '\0';
    ASSERT_STR_EQ("4096", text);
    // no arguments, no file: nothing
    wl_xb_run_options_load("sd:/xbox/none", 0, NULL, &o);
    ASSERT(o.net[0] == '\0' && o.play[0] == '\0' && o.name[0] == '\0' && o.virtual_files == 0u);
    PASS();
}

SUITE(runopts_suite) {
    RUN_TEST(options_are_recognised);
    RUN_TEST(switches_are_flag_files);
    RUN_TEST(a_file_option_gives_the_text);
    RUN_TEST(strings_and_refusals);
    RUN_TEST(arguments_come_before_netrun_txt);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    prepare_fs();
    RUN_SUITE(runopts_suite);
    GREATEST_MAIN_END();
}
