// SPDX-License-Identifier: MIT
// Process helpers for host tools (host only): run an external tool with an argv array (never a shell
// string) and look an executable up on PATH. The on-console installer does not provide these.
#pragma once

#include <stddef.h>

#include "wl/host/base.h"

/* Runs argv[0] (a path; no PATH search) with the given NULL-terminated argv, waits, and stores
 * the exit status (128+signal when killed). stdout goes to stdout_path (truncated) or to
 * /dev/null when NULL; stderr is inherited. Returns WL_ERR_IO when the process cannot be
 * started. */
WlResult wl_proc_run(const char *const argv[], const char *stdout_path, int *exit_code);

/* Finds an executable called name in the directories of the current PATH (read at call time).
 * A name containing '/' is checked as given. WL_ERR_NO_TOOL when not found. */
WlResult wl_proc_which(const char *name, char *out, size_t cap);

/* True when path is an executable regular file. */
int wl_proc_is_executable(const char *path);
