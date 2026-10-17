// SPDX-License-Identifier: MIT
// One run at a time on a hard disk's folder (wl/xboxrt/boot.h, wl_xb_hdd_lock): a second holder,
// in this process or another, is refused until the first lets go or ends.
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "greatest.h"
#include "wl/xboxrt/boot.h"

static char g_dir[256];

TEST second_holder_is_refused(void) {
    int a = -1, b = -1;
    ASSERT_EQ(WL_OK, wl_xb_hdd_lock(g_dir, &a));
    ASSERT(a >= 0);
    ASSERT_EQ(WL_ERR_DENIED, wl_xb_hdd_lock(g_dir, &b));
    ASSERT_EQ(-1, b);
    wl_xb_hdd_unlock(a);
    ASSERT_EQ(WL_OK, wl_xb_hdd_lock(g_dir, &b));
    wl_xb_hdd_unlock(b);
    PASS();
}

TEST another_process_is_refused_until_it_ends(void) {
    int lock_pipe[2], done_pipe[2];
    ASSERT_EQ(0, pipe(lock_pipe));
    ASSERT_EQ(0, pipe(done_pipe));
    pid_t child = fork();
    ASSERT(child >= 0);
    if (child == 0) {
        int fd = -1;
        char ok = wl_xb_hdd_lock(g_dir, &fd) == WL_OK ? 'y' : 'n';
        (void)!write(lock_pipe[1], &ok, 1);
        char go;
        (void)!read(done_pipe[0], &go, 1); // holds it until told, then ends without unlocking
        _exit(0);
    }
    char ok = 0;
    ASSERT_EQ(1, read(lock_pipe[0], &ok, 1));
    ASSERT_EQ('y', ok);
    int fd = -1;
    ASSERT_EQ(WL_ERR_DENIED, wl_xb_hdd_lock(g_dir, &fd));
    (void)!write(done_pipe[1], "x", 1);
    int status = 0;
    ASSERT_EQ(child, waitpid(child, &status, 0));
    ASSERT_EQ(WL_OK, wl_xb_hdd_lock(g_dir, &fd)); // the process's end let it go
    wl_xb_hdd_unlock(fd);
    close(lock_pipe[0]);
    close(lock_pipe[1]);
    close(done_pipe[0]);
    close(done_pipe[1]);
    PASS();
}

TEST missing_folder_holds_nothing(void) {
    char missing[300];
    snprintf(missing, sizeof missing, "%s/none", g_dir);
    int fd = 7;
    ASSERT_EQ(WL_ERR_NOT_FOUND, wl_xb_hdd_lock(missing, &fd));
    ASSERT_EQ(-1, fd);
    ASSERT_EQ(WL_ERR_ARG, wl_xb_hdd_lock("", &fd));
    PASS();
}

SUITE(hdd_lock_suite) {
    RUN_TEST(second_holder_is_refused);
    RUN_TEST(another_process_is_refused_until_it_ends);
    RUN_TEST(missing_folder_holds_nothing);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    const char *base = getenv("TMPDIR");
    snprintf(g_dir, sizeof g_dir, "%s/wlhdd_XXXXXX", base != NULL ? base : "/tmp");
    if (mkdtemp(g_dir) == NULL) {
        perror("mkdtemp");
        return 2;
    }
    RUN_SUITE(hdd_lock_suite);
    rmdir(g_dir);
    GREATEST_MAIN_END();
}
