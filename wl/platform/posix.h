// SPDX-License-Identifier: MIT
// POSIX calls that devkitPPC's newlib declares and does not define (pread, pwrite, pause), as
// portable functions over a file descriptor, and the C library answers it lacks (sysconf's memory,
// a pthread's priority). Every platform builds them, so a program tests the code the console runs
// on the host; on the Wii wii/posix_wii.c also defines the C library's names over them (weak, so a
// newlib that gains the calls wins).
#pragma once

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "wl/base/base.h"

// Reads up to `count` bytes at `offset` of `fd` into `buf`, and leaves the descriptor's own
// position where it was (a seek to `offset`, the read and a seek back, under one lock shared by
// all descriptors, so threads using these calls on one descriptor do not disturb each other).
// Returns what read() returns, or -1 with errno set: EINVAL for a negative offset or a count over
// SSIZE_MAX, EOVERFLOW for an offset that does not fit off_t, ESPIPE for a descriptor that cannot
// seek, EBADF and the rest from lseek and read. A count of 0 returns 0. A read at or past the end
// returns 0.
// Not atomic against a plain read, write or seek on the same descriptor from another thread
// (those do not take the lock): do not mix them on a descriptor that two threads use.
ssize_t wl_posix_pread(int fd, void *buf, size_t count, int64_t offset);
// The same for write(): the bytes land at `offset`, the position is restored. A write past the
// end extends the file as lseek and write do.
ssize_t wl_posix_pwrite(int fd, const void *buf, size_t count, int64_t offset);

// Blocks the calling thread until a signal handler has run, then returns -1 with errno EINTR
// (pause()). The Wii has no signals: there it blocks for ever, sleeping rather than spinning, and
// a thread that must end waits on something else. For a thread that has nothing left to do.
int wl_posix_pause(void);

// ---- Memory questions ---------------------------------------------------------------------------

// The memory a program is told the console has (sysconf(_SC_PHYS_PAGES)): the Wii's MEM1 and MEM2
// together, 88 MB, and the page size it counts them in.
#define WL_POSIX_PHYS_BYTES (88u << 20)
#define WL_POSIX_PAGE_SIZE 4096u

// sysconf's answers for the console, for `name` one of the C library's _SC_ constants:
// _SC_PAGESIZE is WL_POSIX_PAGE_SIZE, _SC_PHYS_PAGES is WL_POSIX_PHYS_BYTES in pages,
// _SC_AVPHYS_PAGES is `free_bytes` (what a program can still allocate) in whole pages, and
// _SC_NPROCESSORS_CONF and _SC_NPROCESSORS_ONLN (where the C library defines them) are 1. Any
// other name is -1 with errno EINVAL. wii/posix_wii.c defines sysconf over it (weak) with
// `free_bytes` from wl_heap_overflow_usage; the function is portable so that a host test checks it.
long wl_posix_sysconf(int name, size_t free_bytes);

// ---- Thread priorities --------------------------------------------------------------------------
//
// devkitPPC's pthread_create starts every thread at libogc's lowest priority (127) and takes none
// from its attributes. libogc's scheduler is strict: such a thread never runs while a thread above
// it is ready, so a program that makes threads with pthread_create and waits on them (or yields to
// them) hangs. These set a pthread's priority afterwards. The numbers are libogc's: 0 runs first,
// 127 last; the program's main thread runs at WL_POSIX_PRIO_MAIN (the same value as
// WL_THREAD_PRIO_MAIN's, wii/thread_wii.c). On the host the scheduler is the C library's and the
// numbers are checked and then ignored (wl_platform_thread_priorities_real() says which).

#define WL_POSIX_PRIO_MAIN 64u
#define WL_POSIX_PRIO_MIN 127u // the lowest priority: where pthread_create leaves every thread
#define WL_POSIX_PRIO_STEPS 15 // how far set_relative_priority goes above or below main

// The priority number `steps` levels above main (below it when negative): WL_POSIX_PRIO_MAIN less
// `steps`, which is clamped to -WL_POSIX_PRIO_STEPS..WL_POSIX_PRIO_STEPS. A Win32 thread priority
// (THREAD_PRIORITY_NORMAL 0, ABOVE_NORMAL 1, HIGHEST 2, ... BELOW_NORMAL -1) is such a step count.
uint32_t wl_posix_priority_above_main(int steps);

// Sets `thread`'s priority to `priority` (0..WL_POSIX_PRIO_MIN). On the Wii a pthread_t is
// libogc's thread (the block pthread_create allocates begins with it), which the call confirms
// before it changes anything. WL_ERR_ARG for a number out of range, WL_ERR_STATE when `thread` is
// not a live thread (the Wii; the host accepts any thread and ignores the number).
WlResult wl_posix_thread_set_priority(pthread_t thread, uint32_t priority);
// The same with the priority `steps` above main: wl_posix_priority_above_main(steps).
WlResult wl_posix_thread_set_relative_priority(pthread_t thread, int steps);
