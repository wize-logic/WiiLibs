// SPDX-License-Identifier: MIT
// Wii disc access through Dolphin's own tool. Wii discs are encrypted; decryption is done only by
// `dolphin-tool extract` (this project holds no keys). The wrapper finds the tool,
// extracts the DATA partition into a cache, checks that files really appeared, and optionally
// compares the SHA-1 of sys/main.dol with a reference supplied by the caller (for example one
// looked up with wl_wii_reference_hash in the caller's own known-dump table). All processes
// start from argv arrays. Host only (needs wl_host: files, processes, SHA-1, JSON).
//
// Tool lookup order: env WL_DOLPHIN_TOOL; `dolphin-tool` on PATH; the flatpak
// org.DolphinEmu.dolphin-emu (run with --filesystem=<cache root>, because the sandbox cannot
// see other paths and silently writes nothing); the Windows DolphinTool.exe under WSL
// (env WL_DOLPHIN_WIN, default "/mnt/c/Program Files/Dolphin/DolphinTool.exe"; paths go
// through `wslpath -w`).
//
// Cache: <root>/<sha1-prefix>/ where the prefix is the first 16 hex digits of the image SHA-1;
// root defaults to env WL_WII_CACHE or ~/.cache/wiilibs/wii. A finished extraction is
// moved into place with one rename, so a half-written directory is never used.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "wl/host/base.h" /* WL_ERR_NO_TOOL, WL_ERR_TOOL_FAILED, WL_ERR_EMPTY */

#define WL_WII_PATH_MAX 1024

typedef enum { WL_WII_TOOL_NATIVE = 0, WL_WII_TOOL_FLATPAK, WL_WII_TOOL_WINDOWS } WlWiiToolKind;

typedef struct {
    WlWiiToolKind kind;
    char path[WL_WII_PATH_MAX];    /* the executable to start (flatpak, DolphinTool.exe, ...) */
    char wslpath[WL_WII_PATH_MAX]; /* WINDOWS only: wslpath executable */
} WlWiiTool;

/* WL_ERR_NO_TOOL when nothing is found. */
WlResult wl_wii_find_tool(WlWiiTool *t);

typedef enum {
    WL_WII_DOL_NO_REFERENCE = 0, /* no reference hash was given: check skipped */
    WL_WII_DOL_MATCH,
    WL_WII_DOL_MISMATCH
} WlWiiDolCheck;

typedef struct {
    char root[WL_WII_PATH_MAX];  /* directory holding sys/ and files/ */
    char image_sha1[41];
    char game_code[7];           /* from the image header, e.g. "RPBP01" */
    int from_cache;              /* 1 when no extraction was needed */
    char main_dol_sha1[41];
    WlWiiDolCheck dol_check;
    char dol_expected[41];       /* the reference hash (lower-case hex) when one was given */
    uint32_t files, dirs;        /* inside files/ (the DATA partition file area), root excluded */
    uint64_t bytes;
} WlWiiDisc;

/* Opens a Wii disc image: extracts it when it is not cached and fills d. tool may be NULL
 * (then wl_wii_find_tool runs only when an extraction is needed). cache_root may be NULL.
 * expected_dol_sha1 (40 hex digits, any case) may be NULL to skip the main.dol check; a
 * malformed value is WL_ERR_ARG. A mismatch is reported in d->dol_check, not as an error. */
WlResult wl_wii_open(WlWiiDisc *d, const char *image, const char *cache_root,
                     const WlWiiTool *tool, const char *expected_dol_sha1);

/* Reads the first six header bytes and tells whether the image is a Wii disc (the magic at
 * 0x18). WL_OK with *is_wii set; an unreadable file is an error. */
WlResult wl_wii_probe(const char *image, int *is_wii, char game_code[7]);

/* Helper for callers that keep a known-dump table as JSON:
 *   {"dumps":[{"game_code":"XXXX01","inner":[{"path":"sys/main.dol","sha1":"<hex>",
 *              "verified":true}, ...]}, ...]}
 * Looks up the reference SHA-1 of inner_path for the disc with this game code. Returns 1 and
 * fills hex (lower case) and verified when a non-null hash exists, else 0. */
int wl_wii_reference_hash(const char *json, size_t json_len, const char *game_code,
                          const char *inner_path, char hex[41], int *verified);
