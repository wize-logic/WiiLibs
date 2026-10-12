// SPDX-License-Identifier: MIT
// Replacing a file with a copy written beside it, so that a reset, a power-off or a pulled card leaves the
// old file or the new one and never a torn one. On POSIX paths (any path the C library takes, "sd:/..." under
// libfat too), so it works wherever the program's own file code does; no platform calls beyond the C library.
//
// The scheme, for a program that saves several writes into one file:
//   1. write the new contents to <file>.wtmp (WL_FILE_TEMPORARY_SUFFIX) and fsync it;
//   2. wl_file_replace("<file>.wtmp", "<file>");
//   3. at the next start, wl_file_recover on the folders that hold saves.
// FAT cannot rename over a file, so the replace first tries rename() (which works where the volume allows it:
// a host's file system) and otherwise sets the old file aside as <file>.wold (WL_FILE_OLD_SUFFIX), renames the
// copy into place and removes the old one. A cut between the two renames leaves <file>.wold and the copy;
// wl_file_recover puts the old file back where <file> is missing and drops it where it is not, and drops
// half-written copies.
#pragma once

#include <stdint.h>

#define WL_FILE_TEMPORARY_SUFFIX ".wtmp"
#define WL_FILE_OLD_SUFFIX ".wold"
// The longest path (with its suffix) the calls take.
#define WL_FILE_PATH_MAX 1100u

// Puts `temporary` in `path`'s place. Returns 0, or the errno that stopped it: the old file is then as it was
// (a failed second rename puts it back). A path that is a directory, or too long for the suffix, is refused
// (EISDIR, ENAMETOOLONG); a missing `temporary` is ENOENT.
int wl_file_replace(const char *temporary, const char *path);

// The form for a volume that cannot rename over a file, which wl_file_replace falls back to when its rename
// fails with `path` in the way: `path` is renamed to <path>.wold, `temporary` to `path`, and the old file is
// removed; a failed second rename puts the old file back. Same results. Called directly it is the way to
// test the FAT sequence on a host whose rename replaces.
int wl_file_replace_aside(const char *temporary, const char *path);

// Called for each file wl_file_recover put back (`restored` 1, `path` is the file) or dropped (0, `path` is
// the leftover), in the order it found them.
typedef void (*WlFileRecoverFn)(const char *path, int restored, void *user);

// What an interrupted write left in `root` and the folders below it (to `max_depth` levels, 0 for `root`
// alone): <file>.wold comes back where <file> is missing and goes where it is not, and a half-written copy
// (<file>.wtmp) goes. `fn` may be NULL. Returns the number of changes made (0 for a clean tree or a root that
// cannot be read).
uint32_t wl_file_recover(const char *root, uint32_t max_depth, WlFileRecoverFn fn, void *user);
