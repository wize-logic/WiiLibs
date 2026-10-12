// SPDX-License-Identifier: MIT
// The bring-up text console and the stop screen: what a program shows when it cannot go on.
//
// The console is a text console (libogc's) drawn into one of the video framebuffers, so a
// program in bring-up, before its renderer works, can say what it does. The stop screen puts a
// message on it, writes it to the log, and waits for the user to leave (HOME on a Remote, START
// on a GameCube controller, or the console's reset or power button): a failure to start, or a
// function the program does not provide yet (tools/link_stubs.py stubs those with
// wl_platform_missing_symbol).
//
// wl/platform/wii implements it over libogc; wl/platform/host prints to stdout and stderr, and a
// stop ends the program with status 1 (or goes to the hook of host/stop_host.h, for tests).
#pragma once

#include "wl/base/base.h"

// Starts the text console on the video's back buffer (wl_platform_video_init is called if it has
// not been: the console takes one of the video's framebuffers, not a framebuffer of its own, and
// the program's frames cover the text once they start). Text written to stdout then appears on
// the screen. Idempotent. An error when the video cannot start, in which case the program can go
// on without a screen. Host: WL_OK, and stdout is the terminal.
WlResult wl_platform_console_start(void);

// The framebuffer the console draws into, or NULL while it is not started (and on the host).
void *wl_platform_console_framebuffer(void);

// Prints one formatted message to stdout (the console, once started) and the log (info level,
// `tag`). A message longer than 255 characters is cut.
void wl_platform_console_report(const char *tag, const char *fmt, ...) WL_PRINTF_FMT(2, 3);

// Reports the message, logs it as an error and flushes the log, puts the console's framebuffer
// back on the screen (after the last frame presented, which would otherwise cover the text), and
// waits for HOME on a Remote or START on a GameCube controller (or a reset or power event) to
// leave for the loader (wl_platform_exit with WL_EXIT_LOADER). Starts the console if it is not
// started. The first stop wins: a second one (a function reached while stopping) only waits.
// Host: prints to stderr and exits with status 1, unless a hook is set (host/stop_host.h).
WL_NORETURN void wl_platform_stop_screen(const char *fmt, ...) WL_PRINTF_FMT(1, 2);

// The stop of a function the program does not provide: what tools/link_stubs.py defines every
// unresolved symbol to call, with the symbol's name.
WL_NORETURN void wl_platform_missing_symbol(const char *name);
