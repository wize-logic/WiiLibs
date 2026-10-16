// SPDX-License-Identifier: MIT
// The remote log (net_stream.h) over loopback: the sink's lines and the files the run writes reach a
// listener in the protocol's form, a ring that fills drops and says so, a peer's QUIT is heard, the
// program can listen instead of connect, and the files of platform.h that it rests on (virtual
// files, the written hook).
#define _POSIX_C_SOURCE 200809L
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "greatest.h"
#include "wl/platform/host/fs_host.h"
#include "wl/platform/net.h"
#include "wl/platform/net_internal.h"
#include "wl/platform/net_stream.h"
#include "wl/platform/platform.h"

static char g_dir[256];
static atomic_int g_quit;

static void on_quit(void) {
    atomic_store(&g_quit, 1);
}

static void prepare_fs(void) {
    const char *base = getenv("TMPDIR");
    snprintf(g_dir, sizeof g_dir, "%s/wlns_XXXXXX", base != NULL ? base : "/tmp");
    if (mkdtemp(g_dir) == NULL) {
        perror("mkdtemp");
        exit(2);
    }
    wl_host_fs_set_root(WL_DEVICE_SD, g_dir);
    wl_platform_fs_mount(WL_DEVICE_SD);
    wl_platform_log_init(NULL);
}

static void write_file(const char *path, const char *text, size_t n) {
    WlPlatformFile *f = NULL;
    if (wl_platform_fs_open(path, WL_FILE_WRITE_NEW, &f) != WL_OK) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(2);
    }
    wl_platform_fs_write(f, text, (uint32_t)n);
    wl_platform_fs_close(f);
}

// A listener on a free loopback port.
static WlSocket listen_any(uint16_t *port) {
    WlSocket s;
    WlNetAddr a = {.address = WL_NET_ADDR(127, 0, 0, 1), .port = 0}, got;
    if (wl_net_socket(WL_NET_TCP, &s) != WL_OK || wl_net_bind(s, &a) != WL_OK || wl_net_listen(s, 2) != WL_OK ||
        wl_net_local_addr(s, &got) != WL_OK) {
        return WL_SOCKET_INVALID;
    }
    *port = got.port;
    return s;
}

static WlSocket accept_within(WlSocket listener, int ms) {
    WlNetPoll p = {.socket = listener, .events = WL_NET_POLL_READ};
    WlSocket c = WL_SOCKET_INVALID;
    if (wl_net_poll(&p, 1, ms) > 0) {
        wl_net_accept(listener, &c, NULL);
    }
    return c;
}

// Reads until the peer closes or `ms` pass with nothing; returns a malloc'd NUL-terminated copy.
static char *read_all(WlSocket s, size_t *size, int ms) {
    char *buf = malloc(1u << 20);
    size_t n = 0;
    for (;;) {
        WlNetPoll p = {.socket = s, .events = WL_NET_POLL_READ};
        if (wl_net_poll(&p, 1, ms) <= 0) {
            break;
        }
        int32_t got = wl_net_recv(s, buf + n, (uint32_t)((1u << 20) - 1u - n));
        if (got <= 0) {
            break;
        }
        n += (size_t)got;
    }
    buf[n] = '\0';
    *size = n;
    return buf;
}

static const char *find(const char *hay, size_t n, const char *needle, size_t m) {
    for (size_t i = 0; i + m <= n; i++) {
        if (memcmp(hay + i, needle, m) == 0) {
            return hay + i;
        }
    }
    return NULL;
}

// (the stream holds NUL bytes: strstr would stop at the first)
static size_t count_of(const char *text, size_t size, const char *needle) {
    size_t n = 0;
    size_t m = strlen(needle);
    for (const char *p = text; (p = find(p, size - (size_t)(p - text), needle, m)) != NULL; p += m) {
        n++;
    }
    return n;
}

TEST lines_and_files_reach_the_listener(void) {
    uint16_t port;
    WlSocket l = listen_any(&port);
    char spec[64];

    ASSERT(l != WL_SOCKET_INVALID);
    snprintf(spec, sizeof spec, "127.0.0.1:%u", (unsigned)port);
    ASSERT_EQ(WL_ERR_STATE, wl_net_stream_send_file("sd:/x", "x")); // (not open yet)
    ASSERT_EQ(WL_ERR_FORMAT, wl_net_stream_open("nonsense", "t"));
    ASSERT_EQ(WL_ERR_FORMAT, wl_net_stream_open("1.2.3.4:0", "t"));
    ASSERT_EQ(WL_ERR_FORMAT, wl_net_stream_open("listen:99999", "t"));
    ASSERT_EQ(WL_OK, wl_net_stream_open(spec, "bench a30"));
    ASSERT_EQ(WL_ERR_STATE, wl_net_stream_open(spec, "again"));
    ASSERT(wl_net_stream_active());
    ASSERT_EQ(WL_OK, wl_net_stream_watch_files("sd:/run", "play,x"));

    WL_LOG_I("t", "first line");
    WL_LOG_W("t", "FILE looks like the protocol");
    wl_net_stream_log_line("FILE x 1 (a sink that does not format)", 38);
    wl_net_stream_log_line("END", 3);
    wl_net_stream_log_line("QUIT", 4);
    wl_net_stream_log_line("two lines\nwere one", 18);
    WlSocket c = accept_within(l, 5000);
    ASSERT(c != WL_SOCKET_INVALID);

    wl_platform_fs_mkdir("sd:/run");
    wl_platform_fs_mkdir("sd:/run/play");
    wl_platform_fs_mkdir("sd:/run/hdd");
    wl_platform_fs_mkdir("sd:/run/play/deep");
    write_file("sd:/run/profile1.txt", "profile text\n", 13);
    write_file("sd:/run/play/shot.bin", "\x00\x01\n\xff" "END\nFILE a 1\n", 17);
    write_file("sd:/run/hdd/pagefile.sys", "not sent", 8);          // a folder not listed
    write_file("sd:/run/play/deep/c.txt", "not sent", 8);           // too deep
    write_file("sd:/elsewhere.txt", "not sent", 8);                 // not under the root
    write_file("sd:/run/empty.txt", "", 0);                          // nothing to send
    write_file("sd:/run/tmp.part", "renamed in", 10);
    ASSERT_EQ(WL_OK, wl_platform_fs_rename("sd:/run/tmp.part", "sd:/run/final.txt"));
    WL_LOG_I("t", "last line");

    wl_net_stream_close(3000);
    ASSERT(!wl_net_stream_active());
    size_t size;
    char *text = read_all(c, &size, 2000);
    ASSERT(strncmp(text, "HELLO WLNET1 bench_a30\n", 23) == 0);
    ASSERT(strstr(text, "[I][t] first line\n") != NULL);
    ASSERT(strstr(text, "\n[W][t] FILE looks like the protocol\n") != NULL);
    ASSERT(strstr(text, "\n FILE x 1 (a sink that does not format)\n") != NULL); // (a space before it)
    ASSERT(strstr(text, "\n END\n") != NULL);
    ASSERT(strstr(text, "\n QUIT\n") != NULL);
    ASSERT(strstr(text, "\ntwo lines were one\n") != NULL); // (a newline inside a line is a space)
    ASSERT(strstr(text, "[I][t] last line\n") != NULL);
    ASSERT(strstr(text, "FILE profile1.txt 13\nprofile text\n") != NULL);
    ASSERT(find(text, size, "FILE play/shot.bin 17\n\x00\x01\n\xff" "END\nFILE a 1\n", 22 + 17) != NULL);
    ASSERT(find(text, size, "FILE final.txt 10\nrenamed in", 28) != NULL);
    ASSERT_EQ(0u, count_of(text, size, "pagefile") + count_of(text, size, "c.txt") + count_of(text, size, "elsewhere") +
                      count_of(text, size, "empty.txt"));
    ASSERT_EQ(0, memcmp(text + size - 4u, "END\n", 4)); // (after a file, which has no end of line of its own)
    free(text);
    wl_net_close(c);
    wl_net_close(l);
    PASS();
}

TEST a_full_ring_drops_and_says_so(void) {
    uint16_t port;
    WlSocket probe = listen_any(&port);
    char spec[64];

    ASSERT(probe != WL_SOCKET_INVALID);
    wl_net_close(probe); // (nothing listens on `port` now: the connection is refused, the ring fills)
    snprintf(spec, sizeof spec, "127.0.0.1:%u", (unsigned)port);
    ASSERT_EQ(WL_OK, wl_net_stream_open(spec, "full"));
    for (int i = 0; i < 1500; i++) {
        WL_LOG_I("flood", "line %04d ................................................................", i);
    }
    WlNetStreamStats st;
    wl_net_stream_stats(&st);
    ASSERT(st.lines_dropped > 500u);
    ASSERT_EQ(0u, st.lines_sent);
    ASSERT(!st.connected);

    // now there is a listener: the thread's next try (within a second or two) connects, and the
    // first lines are the oldest that fitted, then the note
    WlSocket l;
    WlNetAddr a = {.address = WL_NET_ADDR(127, 0, 0, 1), .port = port};
    ASSERT_EQ(WL_OK, wl_net_socket(WL_NET_TCP, &l));
    wl_net_set_option(l, WL_NET_OPT_REUSEADDR, 1);
    ASSERT_EQ(WL_OK, wl_net_bind(l, &a));
    ASSERT_EQ(WL_OK, wl_net_listen(l, 1));
    WlSocket c = accept_within(l, 6000);
    ASSERT(c != WL_SOCKET_INVALID);
    WL_LOG_I("flood", "after");
    wl_net_stream_close(3000);
    size_t size;
    char *text = read_all(c, &size, 2000);
    ASSERT(strstr(text, "[I][flood] line 0000 ") != NULL);
    ASSERT(strstr(text, "[I][flood] after\n") != NULL);
    ASSERT(strstr(text, " lines dropped\n") != NULL);
    ASSERT(strstr(text, "[I][flood] line 1499 ") == NULL); // (dropped)
    free(text);
    wl_net_close(c);
    wl_net_close(l);
    PASS();
}

TEST the_peer_can_send_quit(void) {
    uint16_t port;
    WlSocket l = listen_any(&port);
    char spec[64];

    ASSERT(l != WL_SOCKET_INVALID);
    snprintf(spec, sizeof spec, "127.0.0.1:%u", (unsigned)port);
    atomic_store(&g_quit, 0);
    wl_net_stream_set_quit(on_quit);
    ASSERT_EQ(WL_OK, wl_net_stream_open(spec, "q"));
    WlSocket c = accept_within(l, 5000);
    ASSERT(c != WL_SOCKET_INVALID);
    ASSERT_EQ(5, wl_net_send(c, "QUIT\n", 5));
    for (int i = 0; i < 200 && atomic_load(&g_quit) == 0; i++) {
        wl_platform_thread_sleep_ms(10);
    }
    ASSERT_EQ(1, atomic_load(&g_quit));
    wl_net_stream_set_quit(NULL);
    wl_net_stream_close(2000);
    wl_net_close(c);
    wl_net_close(l);
    PASS();
}

TEST the_program_can_listen_instead(void) {
    uint16_t port;
    WlSocket probe = listen_any(&port);
    char spec[64];

    ASSERT(probe != WL_SOCKET_INVALID);
    wl_net_close(probe);
    snprintf(spec, sizeof spec, "listen:%u", (unsigned)port);
    ASSERT_EQ(WL_OK, wl_net_stream_open(spec, "dial"));
    WL_LOG_I("t", "before the peer");
    WlSocket c;
    WlNetAddr a = {.address = WL_NET_ADDR(127, 0, 0, 1), .port = port};
    ASSERT_EQ(WL_OK, wl_net_socket(WL_NET_TCP, &c));
    WlResult r = WL_ERR_NET_REFUSED;
    for (int i = 0; i < 100 && r != WL_OK; i++) { // (the thread binds a moment after open)
        r = wl_net_connect(c, &a);
        if (r != WL_OK) {
            wl_net_close(c);
            wl_net_socket(WL_NET_TCP, &c);
            wl_platform_thread_sleep_ms(20);
        }
    }
    ASSERT_EQ(WL_OK, r);
    WL_LOG_I("t", "after the peer");
    wl_net_stream_close(3000);
    size_t size;
    char *text = read_all(c, &size, 2000);
    ASSERT(strncmp(text, "HELLO WLNET1 dial\n", 18) == 0);
    ASSERT(strstr(text, "before the peer") != NULL && strstr(text, "after the peer") != NULL);
    ASSERT_EQ(0, strcmp(text + size - 5u, "\nEND\n"));
    free(text);
    wl_net_close(c);
    PASS();
}

TEST virtual_files_answer_like_files(void) {
    WlPlatformFile *f = NULL;
    WlPlatformStat st;
    char buf[16];
    uint32_t got = 99;
    const void *data;
    uint32_t n;

    ASSERT(!wl_platform_fs_exists("sd:/v/flag"));
    ASSERT_EQ(WL_ERR_ARG, wl_platform_fs_virtual_set("flag", "x", 1));
    ASSERT_EQ(WL_OK, wl_platform_fs_virtual_set("sd:/v/flag", "", 0));
    ASSERT_EQ(WL_OK, wl_platform_fs_virtual_set("sd:/v/text", "1024", 4));
    ASSERT_EQ(WL_ERR_STATE, wl_platform_fs_virtual_set("sd:/v/text", "9", 1));
    ASSERT(wl_platform_fs_exists("sd:/v/flag"));
    ASSERT(wl_platform_fs_virtual_get("sd:/v/text", &data, &n) && n == 4u && memcmp(data, "1024", 4) == 0);
    ASSERT(!wl_platform_fs_virtual_get("sd:/v/other", &data, &n));
    ASSERT_EQ(WL_OK, wl_platform_fs_stat("sd:/v/text", &st));
    ASSERT(st.size == 4u && st.read_only && !st.is_dir);
    ASSERT_EQ(WL_OK, wl_platform_fs_open("sd:/v/text", WL_FILE_READ, &f));
    ASSERT_EQ(4u, wl_platform_fs_size(f));
    ASSERT_EQ(WL_OK, wl_platform_fs_read_some_at(f, 1, buf, sizeof buf, &got));
    ASSERT(got == 3u && memcmp(buf, "024", 3) == 0);
    ASSERT_EQ(WL_ERR_RANGE, wl_platform_fs_read_at(f, 2, buf, 3));
    ASSERT_EQ(WL_OK, wl_platform_fs_read_at(f, 0, buf, 4));
    ASSERT_EQ(WL_ERR_DENIED, wl_platform_fs_write(f, "x", 1));
    ASSERT_EQ(WL_ERR_DENIED, wl_platform_fs_truncate(f, 0));
    wl_platform_fs_close(f);
    ASSERT_EQ(WL_OK, wl_platform_fs_open("sd:/v/flag", WL_FILE_READ, &f)); // (an empty one)
    ASSERT_EQ(0u, wl_platform_fs_size(f));
    wl_platform_fs_close(f);
    // a real file under the same path is shadowed
    wl_platform_fs_mkdir("sd:/v");
    write_file("sd:/v/text", "real", 4);
    ASSERT_EQ(WL_OK, wl_platform_fs_open("sd:/v/text", WL_FILE_READ, &f));
    ASSERT_EQ(WL_OK, wl_platform_fs_read_at(f, 0, buf, 4));
    ASSERT_EQ(0, memcmp(buf, "1024", 4));
    wl_platform_fs_close(f);
    for (unsigned i = 0; i < WL_FS_VIRTUAL_MAX; i++) {
        char p[32];
        snprintf(p, sizeof p, "sd:/v/n%u", i);
        WlResult r = wl_platform_fs_virtual_set(p, "", 0);
        if (r == WL_ERR_LIMIT) {
            PASS();
        }
    }
    FAIL();
}

static unsigned g_written;
static char g_written_last[WL_FS_PATH_MAX];

static void written(void *user, const char *path) {
    (void)user;
    g_written++;
    snprintf(g_written_last, sizeof g_written_last, "%s", path);
}

TEST the_written_hook_sees_closed_files(void) {
    WlPlatformFile *f = NULL;

    wl_platform_fs_mkdir("sd:/w");
    write_file("sd:/w/before", "x", 1);
    wl_platform_fs_set_written_hook(written, NULL);
    g_written = 0;
    write_file("sd:/w/a", "abc", 3);
    ASSERT_EQ(1u, g_written);
    ASSERT_STR_EQ("sd:/w/a", g_written_last);
    ASSERT_EQ(WL_OK, wl_platform_fs_open("sd:/w/a", WL_FILE_READ, &f)); // read only: not reported
    wl_platform_fs_close(f);
    ASSERT_EQ(WL_OK, wl_platform_fs_open("sd:/w/a", WL_FILE_READ_WRITE, &f)); // opened and not written
    wl_platform_fs_close(f);
    ASSERT_EQ(1u, g_written);
    ASSERT_EQ(WL_OK, wl_platform_fs_open("sd:/w/a", WL_FILE_READ_WRITE, &f));
    ASSERT_EQ(WL_OK, wl_platform_fs_write_at(f, 1, "Z", 1));
    ASSERT_EQ(1u, g_written); // (not before it is closed)
    wl_platform_fs_close(f);
    ASSERT_EQ(2u, g_written);
    ASSERT_EQ(WL_OK, wl_platform_fs_rename("sd:/w/a", "sd:/w/b"));
    ASSERT_EQ(3u, g_written);
    ASSERT_STR_EQ("sd:/w/b", g_written_last);
    wl_platform_fs_notify_written("sd:/w/stdio");
    ASSERT_EQ(4u, g_written);
    wl_platform_fs_set_written_hook(NULL, NULL);
    write_file("sd:/w/c", "abc", 3);
    ASSERT_EQ(4u, g_written);
    PASS();
}

SUITE(net_stream_suite) {
    RUN_TEST(virtual_files_answer_like_files);
    RUN_TEST(the_written_hook_sees_closed_files);
    RUN_TEST(lines_and_files_reach_the_listener);
    RUN_TEST(a_full_ring_drops_and_says_so);
    RUN_TEST(the_peer_can_send_quit);
    RUN_TEST(the_program_can_listen_instead);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    prepare_fs();
    RUN_SUITE(net_stream_suite);
    GREATEST_MAIN_END();
}
