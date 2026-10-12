// SPDX-License-Identifier: MIT
// Crash-safe file replacement and the sweep that repairs a cut one (file_replace.h).
#define _POSIX_C_SOURCE 200809L
#include "wl/platform/file_replace.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int wl_file_replace_aside(const char *temporary, const char *path) {
    char old_path[WL_FILE_PATH_MAX + 8];
    if (temporary == NULL || path == NULL) {
        return EINVAL;
    }
    if (strlen(path) + sizeof(WL_FILE_OLD_SUFFIX) > WL_FILE_PATH_MAX) {
        return ENAMETOOLONG;
    }
    // FAT: the old file goes aside, the copy takes its name, the old file goes.
    snprintf(old_path, sizeof old_path, "%s%s", path, WL_FILE_OLD_SUFFIX);
    unlink(old_path);
    if (rename(path, old_path) != 0) {
        return errno != 0 ? errno : EIO;
    }
    if (rename(temporary, path) != 0) {
        int error = errno != 0 ? errno : EIO;
        rename(old_path, path);
        return error;
    }
    unlink(old_path);
    return 0;
}

int wl_file_replace(const char *temporary, const char *path) {
    struct stat st;
    if (temporary == NULL || path == NULL) {
        return EINVAL;
    }
    if (strlen(path) + sizeof(WL_FILE_OLD_SUFFIX) > WL_FILE_PATH_MAX) {
        return ENAMETOOLONG;
    }
    if (stat(temporary, &st) != 0) {
        return errno != 0 ? errno : ENOENT;
    }
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        return EISDIR;
    }
    if (rename(temporary, path) == 0) {
        return 0;
    }
    if (stat(path, &st) != 0) {
        return errno != 0 ? errno : EIO; // nothing in the way, and the rename still failed
    }
    return wl_file_replace_aside(temporary, path);
}

static bool has_suffix(const char *name, const char *suffix) {
    size_t length = strlen(name);
    size_t suffix_length = strlen(suffix);
    return length > suffix_length && strcmp(name + length - suffix_length, suffix) == 0;
}

// One change in `directory` (or below), 0 when it found nothing to do: the folder is read again after a change,
// since a directory changed under an open stream may be read wrongly on FAT.
static int recover_directory(const char *directory, uint32_t depth, uint32_t max_depth, WlFileRecoverFn fn,
                             void *user) {
    char path[WL_FILE_PATH_MAX + 8];
    char target[WL_FILE_PATH_MAX + 8];
    struct stat st;
    int changed = 0;
    DIR *stream = opendir(directory);
    struct dirent *entry;
    if (stream == NULL) {
        return 0;
    }
    while (!changed && (entry = readdir(stream)) != NULL) {
        const char *name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }
        int n = snprintf(path, sizeof path, "%s/%s", directory, name);
        if (n < 0 || (size_t)n >= sizeof path) {
            continue;
        }
        if (has_suffix(name, WL_FILE_OLD_SUFFIX)) {
            snprintf(target, sizeof target, "%.*s", (int)(strlen(path) - strlen(WL_FILE_OLD_SUFFIX)), path);
            if (stat(target, &st) == 0) {
                changed = unlink(path) == 0;
                if (changed && fn != NULL) {
                    fn(path, 0, user);
                }
            } else {
                changed = rename(path, target) == 0;
                if (changed && fn != NULL) {
                    fn(target, 1, user);
                }
            }
        } else if (has_suffix(name, WL_FILE_TEMPORARY_SUFFIX)) {
            changed = unlink(path) == 0;
            if (changed && fn != NULL) {
                fn(path, 0, user);
            }
        } else if (depth < max_depth && stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            changed = recover_directory(path, depth + 1u, max_depth, fn, user);
        }
    }
    closedir(stream);
    return changed;
}

uint32_t wl_file_recover(const char *root, uint32_t max_depth, WlFileRecoverFn fn, void *user) {
    uint32_t changes = 0;
    if (root == NULL) {
        return 0;
    }
    // (a guard against a folder that cannot be made clean)
    while (changes < 1000u && recover_directory(root, 0, max_depth, fn, user)) {
        changes++;
    }
    return changes;
}
