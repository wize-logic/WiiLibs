// SPDX-License-Identifier: MIT
// pread, pwrite and pause over a descriptor (wl/platform/posix.h): the position is left alone, short
// and failing transfers, and threads sharing one descriptor.
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "greatest.h"
#include "tests/platform/check.h"
#include "wl/platform/posix.h"

#define FILE_SIZE 4096u

static char g_path[] = "/tmp/wl_posix_XXXXXX";
static int g_fd = -1;

static uint8_t pattern(size_t i) {
    return (uint8_t)((i * 131u + (i >> 8) * 7u + 3u) & 0xFFu);
}

static void make_file(void) {
    g_fd = mkstemp(g_path);
    uint8_t data[FILE_SIZE];
    for (size_t i = 0; i < FILE_SIZE; i++) {
        data[i] = pattern(i);
    }
    if (g_fd < 0 || write(g_fd, data, sizeof data) != (ssize_t)sizeof data) {
        fprintf(stderr, "test file: %s\n", strerror(errno));
        exit(1);
    }
    lseek(g_fd, 0, SEEK_SET);
}

// suite_posix.c, shared with the on-console runner
static int g_suite_fail;

void wlt_report(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

void wlt_check(const char *name, bool ok, const char *detail) {
    if (!ok) {
        g_suite_fail++;
        printf("FAIL %s: %s\n", name, detail ? detail : "-");
    }
}

void wlt_suite_posix(const char *path);

TEST libc_names_suite(void) {
    char path[] = "/tmp/wl_posix_suite_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    close(fd);
    wlt_suite_posix(path);
    ASSERT_EQ(0, g_suite_fail);
    PASS();
}

TEST pread_reads_at_the_offset_and_keeps_the_position(void) {
    uint8_t buf[100];
    ASSERT_EQ(10, lseek(g_fd, 10, SEEK_SET));
    ASSERT_EQ((ssize_t)sizeof buf, wl_posix_pread(g_fd, buf, sizeof buf, 1000));
    for (size_t i = 0; i < sizeof buf; i++) {
        ASSERT_EQ(pattern(1000 + i), buf[i]);
    }
    ASSERT_EQ(10, lseek(g_fd, 0, SEEK_CUR));
    // the plain read continues from the position the call left
    uint8_t one;
    ASSERT_EQ(1, read(g_fd, &one, 1));
    ASSERT_EQ(pattern(10), one);
    PASS();
}

TEST pread_short_at_the_end_and_zero_past_it(void) {
    uint8_t buf[64];
    ASSERT_EQ(10, wl_posix_pread(g_fd, buf, sizeof buf, FILE_SIZE - 10));
    ASSERT_EQ(pattern(FILE_SIZE - 1), buf[9]);
    ASSERT_EQ(0, wl_posix_pread(g_fd, buf, sizeof buf, FILE_SIZE));
    ASSERT_EQ(0, wl_posix_pread(g_fd, buf, sizeof buf, FILE_SIZE + 5000));
    ASSERT_EQ(0, wl_posix_pread(g_fd, buf, 0, 12));
    PASS();
}

TEST pwrite_writes_at_the_offset_and_keeps_the_position(void) {
    const uint8_t data[5] = {1, 2, 3, 4, 5};
    uint8_t back[7];
    ASSERT_EQ(20, lseek(g_fd, 20, SEEK_SET));
    ASSERT_EQ(5, wl_posix_pwrite(g_fd, data, sizeof data, 300));
    ASSERT_EQ(20, lseek(g_fd, 0, SEEK_CUR));
    ASSERT_EQ(7, wl_posix_pread(g_fd, back, sizeof back, 299));
    ASSERT_EQ(pattern(299), back[0]);
    ASSERT_MEM_EQ(data, back + 1, 5);
    ASSERT_EQ(pattern(305), back[6]);
    // restore the pattern for the other tests
    uint8_t original[5];
    for (size_t i = 0; i < 5; i++) {
        original[i] = pattern(300 + i);
    }
    ASSERT_EQ(5, wl_posix_pwrite(g_fd, original, 5, 300));
    PASS();
}

TEST pwrite_past_the_end_extends_the_file(void) {
    char path[] = "/tmp/wl_posix_grow_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    ASSERT_EQ(3, wl_posix_pwrite(fd, "abc", 3, 10));
    ASSERT_EQ(13, lseek(fd, 0, SEEK_END));
    char back[3];
    ASSERT_EQ(3, wl_posix_pread(fd, back, 3, 10));
    ASSERT_MEM_EQ("abc", back, 3);
    close(fd);
    unlink(path);
    PASS();
}

TEST errors_set_errno(void) {
    uint8_t buf[8];
    errno = 0;
    ASSERT_EQ(-1, wl_posix_pread(g_fd, buf, sizeof buf, -1));
    ASSERT_EQ(EINVAL, errno);
    errno = 0;
    ASSERT_EQ(-1, wl_posix_pread(g_fd, buf, (size_t)SSIZE_MAX + 1u, 0));
    ASSERT_EQ(EINVAL, errno);
    errno = 0;
    ASSERT_EQ(-1, wl_posix_pread(-1, buf, sizeof buf, 0));
    ASSERT_EQ(EBADF, errno);
    errno = 0;
    ASSERT_EQ(-1, wl_posix_pwrite(-1, buf, sizeof buf, 0));
    ASSERT_EQ(EBADF, errno);
    if (sizeof(off_t) < 8) {
        errno = 0;
        ASSERT_EQ(-1, wl_posix_pread(g_fd, buf, sizeof buf, (int64_t)INT32_MAX + 10));
        ASSERT_EQ(EOVERFLOW, errno);
    }
    // reading a descriptor opened write-only fails in read(), and the position is still restored
    int wo = open(g_path, O_WRONLY);
    ASSERT(wo >= 0);
    ASSERT_EQ(5, lseek(wo, 5, SEEK_SET));
    errno = 0;
    ASSERT_EQ(-1, wl_posix_pread(wo, buf, sizeof buf, 100));
    ASSERT_EQ(EBADF, errno);
    ASSERT_EQ(5, lseek(wo, 0, SEEK_CUR));
    close(wo);
    // a pipe cannot seek
    int fds[2];
    ASSERT_EQ(0, pipe(fds));
    errno = 0;
    ASSERT_EQ(-1, wl_posix_pread(fds[0], buf, sizeof buf, 0));
    ASSERT_EQ(ESPIPE, errno);
    close(fds[0]);
    close(fds[1]);
    PASS();
}

// Threads: each reads and writes its own 256-byte slot of a second file through one shared
// descriptor, checking what it reads, while the descriptor's position stays where main put it.
#define THREADS 4
#define ROUNDS 400
#define SLOT 256u

static int g_shared_fd;
static volatile int g_thread_failures;

static void *worker(void *arg) {
    size_t id = (size_t)(uintptr_t)arg;
    uint8_t out[SLOT], in[SLOT];
    for (int round = 0; round < ROUNDS; round++) {
        for (size_t i = 0; i < SLOT; i++) {
            out[i] = (uint8_t)(id * 40u + (size_t)round + i);
        }
        if (wl_posix_pwrite(g_shared_fd, out, SLOT, (int64_t)(id * SLOT)) != (ssize_t)SLOT ||
            wl_posix_pread(g_shared_fd, in, SLOT, (int64_t)(id * SLOT)) != (ssize_t)SLOT ||
            memcmp(in, out, SLOT) != 0) {
            g_thread_failures++;
        }
    }
    return NULL;
}

TEST threads_do_not_disturb_each_other_or_the_position(void) {
    char path[] = "/tmp/wl_posix_mt_XXXXXX";
    g_shared_fd = mkstemp(path);
    ASSERT(g_shared_fd >= 0);
    ASSERT_EQ(1234, lseek(g_shared_fd, 1234, SEEK_SET));
    pthread_t threads[THREADS];
    for (size_t i = 0; i < THREADS; i++) {
        ASSERT_EQ(0, pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)i));
    }
    for (size_t i = 0; i < THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    ASSERT_EQ(0, g_thread_failures);
    ASSERT_EQ(1234, lseek(g_shared_fd, 0, SEEK_CUR));
    close(g_shared_fd);
    unlink(path);
    PASS();
}

static volatile sig_atomic_t g_signalled;

static void on_alarm(int sig) {
    (void)sig;
    g_signalled = 1;
}

TEST pause_returns_after_a_signal_handler_ran(void) {
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = on_alarm;
    sigemptyset(&action.sa_mask);
    ASSERT_EQ(0, sigaction(SIGALRM, &action, NULL));
    // A repeating timer: if the first signal arrives before the call is waiting (a slow machine),
    // the next one ends it.
    struct itimerval timer;
    memset(&timer, 0, sizeof timer);
    timer.it_value.tv_usec = 30000;
    timer.it_interval.tv_usec = 30000;
    g_signalled = 0;
    ASSERT_EQ(0, setitimer(ITIMER_REAL, &timer, NULL));
    errno = 0;
    int result = wl_posix_pause();
    int error = errno;
    memset(&timer, 0, sizeof timer);
    setitimer(ITIMER_REAL, &timer, NULL);
    ASSERT_EQ(-1, result);
    ASSERT_EQ(EINTR, error);
    ASSERT_EQ(1, g_signalled);
    PASS();
}

TEST sysconf_answers_for_the_console(void) {
    errno = 0;
    ASSERT_EQ(4096, wl_posix_sysconf(_SC_PAGESIZE, 0));
    ASSERT_EQ((88L << 20) / 4096, wl_posix_sysconf(_SC_PHYS_PAGES, 0));
    // the free memory is counted in whole pages
    ASSERT_EQ(0, wl_posix_sysconf(_SC_AVPHYS_PAGES, 0));
    ASSERT_EQ(0, wl_posix_sysconf(_SC_AVPHYS_PAGES, 4095));
    ASSERT_EQ(1, wl_posix_sysconf(_SC_AVPHYS_PAGES, 4096));
    ASSERT_EQ(5, wl_posix_sysconf(_SC_AVPHYS_PAGES, 5u * 4096u + 100u));
    ASSERT_EQ(1, wl_posix_sysconf(_SC_NPROCESSORS_ONLN, 0));
    ASSERT_EQ(1, wl_posix_sysconf(_SC_NPROCESSORS_CONF, 0));
    ASSERT_EQ(0, errno);
    errno = 0;
    ASSERT_EQ(-1, wl_posix_sysconf(-12345, 0));
    ASSERT_EQ(EINVAL, errno);
    errno = 0;
    ASSERT_EQ(-1, wl_posix_sysconf(_SC_CLK_TCK, 0));
    ASSERT_EQ(EINVAL, errno);
    PASS();
}

TEST relative_priority_is_main_less_the_steps(void) {
    ASSERT_EQ(64u, WL_POSIX_PRIO_MAIN);
    ASSERT_EQ(64u, wl_posix_priority_above_main(0));
    // a step above main is a smaller number: libogc runs 0 first
    ASSERT_EQ(63u, wl_posix_priority_above_main(1));
    ASSERT_EQ(62u, wl_posix_priority_above_main(2));
    ASSERT_EQ(65u, wl_posix_priority_above_main(-1));
    ASSERT_EQ(66u, wl_posix_priority_above_main(-2));
    // clamped to 15 steps either way
    ASSERT_EQ(49u, wl_posix_priority_above_main(15));
    ASSERT_EQ(49u, wl_posix_priority_above_main(16));
    ASSERT_EQ(49u, wl_posix_priority_above_main(1000000));
    ASSERT_EQ(79u, wl_posix_priority_above_main(-15));
    ASSERT_EQ(79u, wl_posix_priority_above_main(-16));
    ASSERT_EQ(79u, wl_posix_priority_above_main(-1000000));
    ASSERT_EQ(79u, wl_posix_priority_above_main(INT_MIN));
    ASSERT_EQ(49u, wl_posix_priority_above_main(INT_MAX));
    PASS();
}

static void *idle_thread(void *arg) {
    (void)arg;
    return NULL;
}

TEST thread_priority_calls_check_their_numbers(void) {
    pthread_t thread;
    ASSERT_EQ(0, pthread_create(&thread, NULL, idle_thread, NULL));
    ASSERT_EQ(WL_OK, wl_posix_thread_set_priority(thread, 0));
    ASSERT_EQ(WL_OK, wl_posix_thread_set_priority(thread, WL_POSIX_PRIO_MAIN));
    ASSERT_EQ(WL_OK, wl_posix_thread_set_priority(thread, WL_POSIX_PRIO_MIN));
    ASSERT_EQ(WL_ERR_ARG, wl_posix_thread_set_priority(thread, WL_POSIX_PRIO_MIN + 1u));
    ASSERT_EQ(WL_ERR_ARG, wl_posix_thread_set_priority(thread, 0xFFFFFFFFu));
    ASSERT_EQ(WL_OK, wl_posix_thread_set_relative_priority(thread, 0));
    ASSERT_EQ(WL_OK, wl_posix_thread_set_relative_priority(thread, 2));
    ASSERT_EQ(WL_OK, wl_posix_thread_set_relative_priority(thread, -100));
    ASSERT_EQ(WL_OK, wl_posix_thread_set_relative_priority(pthread_self(), 1));
    pthread_join(thread, NULL);
    PASS();
}

SUITE(posix_suite) {
    make_file();
    RUN_TEST(pread_reads_at_the_offset_and_keeps_the_position);
    RUN_TEST(pread_short_at_the_end_and_zero_past_it);
    RUN_TEST(pwrite_writes_at_the_offset_and_keeps_the_position);
    RUN_TEST(pwrite_past_the_end_extends_the_file);
    RUN_TEST(errors_set_errno);
    RUN_TEST(threads_do_not_disturb_each_other_or_the_position);
    RUN_TEST(libc_names_suite);
    RUN_TEST(pause_returns_after_a_signal_handler_ran);
    RUN_TEST(sysconf_answers_for_the_console);
    RUN_TEST(relative_priority_is_main_less_the_steps);
    RUN_TEST(thread_priority_calls_check_their_numbers);
    close(g_fd);
    unlink(g_path);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(posix_suite);
    GREATEST_MAIN_END();
}
