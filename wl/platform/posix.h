// SPDX-License-Identifier: MIT
// POSIX calls that devkitPPC's newlib declares and does not define (pread, pwrite, pause), as
// portable functions over a file descriptor. Every platform builds them, so a program tests the
// code the console runs on the host; on the Wii wii/posix_wii.c also defines the C library's
// names over them (weak, so a newlib that gains the calls wins).
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

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
