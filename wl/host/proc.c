// SPDX-License-Identifier: MIT
#define _POSIX_C_SOURCE 200809L
#include "wl/host/proc.h"

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

int wl_proc_is_executable(const char *path) {
    struct stat st;
    return path != NULL && stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

WlResult wl_proc_which(const char *name, char *out, size_t cap) {
    if (name == NULL || out == NULL || cap == 0 || name[0] == '\0') {
        return WL_ERR_ARG;
    }
    if (strchr(name, '/') != NULL) {
        if (!wl_proc_is_executable(name) || strlen(name) >= cap) {
            return WL_ERR_NO_TOOL;
        }
        memcpy(out, name, strlen(name) + 1);
        return WL_OK;
    }
    const char *path = getenv("PATH");
    if (path == NULL) {
        return WL_ERR_NO_TOOL;
    }
    while (*path != '\0') {
        const char *end = strchr(path, ':');
        size_t dl = end ? (size_t)(end - path) : strlen(path);
        if (dl > 0) {
            int n = snprintf(out, cap, "%.*s/%s", (int)dl, path, name);
            if (n > 0 && (size_t)n < cap && wl_proc_is_executable(out)) {
                return WL_OK;
            }
        }
        if (end == NULL) {
            break;
        }
        path = end + 1;
    }
    return WL_ERR_NO_TOOL;
}

WlResult wl_proc_run(const char *const argv[], const char *stdout_path, int *exit_code) {
    if (argv == NULL || argv[0] == NULL || exit_code == NULL) {
        return WL_ERR_ARG;
    }
    posix_spawn_file_actions_t fa;
    if (posix_spawn_file_actions_init(&fa) != 0) {
        return WL_ERR_NOMEM;
    }
    const char *target = stdout_path ? stdout_path : "/dev/null";
    int flags = O_WRONLY | O_CREAT | (stdout_path ? O_TRUNC : 0);
    WlResult res = WL_OK;
    if (posix_spawn_file_actions_addopen(&fa, 1, target, flags, 0644) != 0) {
        res = WL_ERR_IO;
    }
    pid_t pid = 0;
    if (res == WL_OK &&
        posix_spawn(&pid, argv[0], &fa, NULL, (char *const *)argv, environ) != 0) {
        res = WL_ERR_IO;
    }
    (void)posix_spawn_file_actions_destroy(&fa);
    if (res != WL_OK) {
        return res;
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return WL_ERR_IO;
        }
    }
    if (WIFEXITED(status)) {
        *exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        *exit_code = 128 + WTERMSIG(status);
    } else {
        *exit_code = 255;
    }
    return WL_OK;
}
