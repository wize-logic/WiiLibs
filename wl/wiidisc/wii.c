// SPDX-License-Identifier: MIT
#define _POSIX_C_SOURCE 200809L
#include "wl/wiidisc/wii.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "wl/base/endian.h"
#include "wl/host/file.h"
#include "wl/host/json.h"
#include "wl/host/proc.h"
#include "wl/host/sha1.h"

#define TAG "wii"
#define FLATPAK_APP "org.DolphinEmu.dolphin-emu"
#define WIN_DEFAULT "/mnt/c/Program Files/Dolphin/DolphinTool.exe"

/* ------------------------------------------------------------------ tool discovery */

static int set_path(char *dst, const char *src) {
    size_t n = strlen(src);
    if (n >= WL_WII_PATH_MAX) {
        return 0;
    }
    memcpy(dst, src, n + 1);
    return 1;
}

static int flatpak_has_dolphin(const char *flatpak) {
    const char *argv[] = {flatpak, "info", FLATPAK_APP, NULL};
    int code = 0;
    return wl_proc_run(argv, NULL, &code) == WL_OK && code == 0;
}

WlResult wl_wii_find_tool(WlWiiTool *t) {
    if (t == NULL) {
        return WL_ERR_ARG;
    }
    memset(t, 0, sizeof *t);
    const char *env = getenv("WL_DOLPHIN_TOOL");
    char buf[WL_WII_PATH_MAX];
    if (env != NULL && env[0] != '\0') {
        if (wl_proc_which(env, buf, sizeof buf) == WL_OK && set_path(t->path, buf)) {
            t->kind = WL_WII_TOOL_NATIVE;
            return WL_OK;
        }
        WL_LOG_W(TAG, "WL_DOLPHIN_TOOL='%s' is not an executable; trying other options", env);
    }
    if (wl_proc_which("dolphin-tool", buf, sizeof buf) == WL_OK && set_path(t->path, buf)) {
        t->kind = WL_WII_TOOL_NATIVE;
        return WL_OK;
    }
    if (wl_proc_which("flatpak", buf, sizeof buf) == WL_OK && flatpak_has_dolphin(buf) &&
        set_path(t->path, buf)) {
        t->kind = WL_WII_TOOL_FLATPAK;
        return WL_OK;
    }
    const char *win = getenv("WL_DOLPHIN_WIN");
    if (win == NULL || win[0] == '\0') {
        win = WIN_DEFAULT;
    }
    char wsl[WL_WII_PATH_MAX];
    if (wl_proc_is_executable(win) && wl_proc_which("wslpath", wsl, sizeof wsl) == WL_OK &&
        set_path(t->path, win) && set_path(t->wslpath, wsl)) {
        t->kind = WL_WII_TOOL_WINDOWS;
        return WL_OK;
    }
    return WL_ERR_NO_TOOL;
}

/* ------------------------------------------------------------------ helpers */

static void hex_of(const uint8_t d[20], char out[41]) {
    wl_sha1_to_hex(d, out);
}

WlResult wl_wii_probe(const char *image, int *is_wii, char game_code[7]) {
    WlFile *f;
    uint64_t sz;
    uint8_t h[0x20];
    if (image == NULL || is_wii == NULL) {
        return WL_ERR_ARG;
    }
    WlResult r = wl_file_open_ro(image, &f, &sz);
    if (r != WL_OK) {
        return r;
    }
    r = sz >= sizeof h ? wl_file_read_at(f, 0, h, sizeof h) : WL_ERR_FORMAT;
    wl_file_close(f);
    if (r != WL_OK) {
        return r;
    }
    *is_wii = wl_rd_be32(h + 0x18) == 0x5D1C9EA3u;
    if (game_code != NULL) {
        memcpy(game_code, h, 4);
        /* Wii game ID is 6 characters: 4-char code and 2-char maker code at 0x04. */
        memcpy(game_code + 4, h + 4, 2);
        game_code[6] = '\0';
        for (int i = 0; i < 6; i++) { /* never trust header bytes in logs or paths */
            unsigned char c = (unsigned char)game_code[i];
            if (c < 0x20 || c > 0x7E) {
                game_code[i] = '?';
            }
        }
    }
    return WL_OK;
}

static int default_cache(char *out, size_t cap) {
    const char *env = getenv("WL_WII_CACHE");
    if (env != NULL && env[0] != '\0') {
        return snprintf(out, cap, "%s", env) < (int)cap;
    }
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        return 0;
    }
    return snprintf(out, cap, "%s/.cache/wiilibs/wii", home) < (int)cap;
}

static uint32_t fnv(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        h = (h ^ (uint8_t)*s) * 16777619u;
    }
    return h;
}

/* The image hash is memoised by (path, size, mtime) so a cached disc is not hashed again. */
static WlResult image_key(const char *image, const char *root, char hex[41]) {
    struct stat st;
    if (stat(image, &st) != 0 || !S_ISREG(st.st_mode)) {
        return WL_ERR_IO;
    }
    char memo[WL_WII_PATH_MAX];
    if (snprintf(memo, sizeof memo, "%s/.stat/%08x-%llu-%lld", root, (unsigned)fnv(image),
                 (unsigned long long)st.st_size, (long long)st.st_mtime) >= (int)sizeof memo) {
        return WL_ERR_RANGE;
    }
    uint8_t *data;
    size_t len;
    if (wl_file_read_all(memo, 64, &data, &len) == WL_OK) {
        int ok = len == 40 && wl_sha1_from_hex((const char *)data, (uint8_t[20]){0}) == WL_OK;
        if (ok) {
            memcpy(hex, data, 40);
            hex[40] = '\0';
            free(data);
            return WL_OK;
        }
        free(data);
    }
    uint8_t d[20];
    uint64_t sz;
    WL_LOG_I(TAG, "hashing '%s' (one time, memoised)", image);
    WlResult r = wl_file_sha1(image, d, &sz);
    if (r != WL_OK) {
        return r;
    }
    hex_of(d, hex);
    if (wl_file_mkdirs_for(memo) == WL_OK) {
        (void)wl_file_write_all(memo, hex, 40);
    }
    return WL_OK;
}

/* ------------------------------------------------------------------ extraction */

static WlResult run_extract(const WlWiiTool *t, const char *cache_root, const char *image,
                            const char *out) {
    const char *argv[16];
    int n = 0;
    char fsarg[WL_WII_PATH_MAX + 16];
    char wimg[WL_WII_PATH_MAX], wout[WL_WII_PATH_MAX];
    const char *img = image, *dst = out;
    argv[n++] = t->path;
    if (t->kind == WL_WII_TOOL_FLATPAK) {
        if (snprintf(fsarg, sizeof fsarg, "--filesystem=%s", cache_root) >= (int)sizeof fsarg) {
            return WL_ERR_RANGE;
        }
        argv[n++] = "run";
        argv[n++] = fsarg;
        argv[n++] = "--command=dolphin-tool";
        argv[n++] = FLATPAK_APP;
    } else if (t->kind == WL_WII_TOOL_WINDOWS) {
        /* wslpath -w converts a Linux path to the Windows (UNC) form DolphinTool.exe expects. */
        const char *pairs[2][2] = {{image, NULL}, {out, NULL}};
        char *dsts[2] = {wimg, wout};
        for (int i = 0; i < 2; i++) {
            const char *wa[] = {t->wslpath, "-w", pairs[i][0], NULL};
            char tmp[WL_WII_PATH_MAX];
            if (snprintf(tmp, sizeof tmp, "%s.wslpath", out) >= (int)sizeof tmp) {
                return WL_ERR_RANGE;
            }
            int code = 0;
            if (wl_proc_run(wa, tmp, &code) != WL_OK || code != 0) {
                (void)remove(tmp);
                return WL_ERR_TOOL_FAILED;
            }
            uint8_t *txt;
            size_t len;
            WlResult r = wl_file_read_all(tmp, WL_WII_PATH_MAX - 1, &txt, &len);
            (void)remove(tmp);
            if (r != WL_OK) {
                return r;
            }
            while (len > 0 && (txt[len - 1] == '\n' || txt[len - 1] == '\r')) {
                len--;
            }
            memcpy(dsts[i], txt, len);
            dsts[i][len] = '\0';
            free(txt);
        }
        img = wimg;
        dst = wout;
    }
    argv[n++] = "extract";
    argv[n++] = "-i";
    argv[n++] = img;
    argv[n++] = "-o";
    argv[n++] = dst;
    argv[n++] = "-g";
    argv[n++] = "-q";
    argv[n] = NULL;
    int code = 0;
    WlResult r = wl_proc_run(argv, NULL, &code);
    if (r != WL_OK) {
        WL_LOG_E(TAG, "cannot start '%s'", t->path);
        return WL_ERR_TOOL_FAILED;
    }
    if (code != 0) {
        WL_LOG_E(TAG, "dolphin-tool exited with status %d", code);
        return WL_ERR_TOOL_FAILED;
    }
    return WL_OK;
}

typedef struct {
    uint32_t files, dirs;
    uint64_t bytes;
} Tally;

static WlResult tally_cb(void *ud, const char *rel, int is_dir, uint64_t size) {
    Tally *t = (Tally *)ud;
    (void)rel;
    if (is_dir) {
        t->dirs++;
    } else {
        t->files++;
        t->bytes += size;
    }
    return WL_OK;
}

/* Finds the directory that holds sys/main.dol and files/ (the layout puts it in DATA/ for a
 * Wii extraction; accept the output directory itself too). */
static WlResult find_root(const char *out, char *root, size_t cap) {
    static const char *const cands[] = {"DATA", ""};
    for (size_t i = 0; i < 2; i++) {
        char p[WL_WII_PATH_MAX];
        if (snprintf(p, sizeof p, "%s%s%s/sys/main.dol", out, cands[i][0] ? "/" : "", cands[i]) >=
            (int)sizeof p) {
            return WL_ERR_RANGE;
        }
        if (wl_file_exists(p)) {
            if (snprintf(root, cap, "%s%s%s", out, cands[i][0] ? "/" : "", cands[i]) >= (int)cap) {
                return WL_ERR_RANGE;
            }
            return WL_OK;
        }
    }
    return WL_ERR_EMPTY;
}

int wl_wii_reference_hash(const char *json, size_t json_len, const char *game_code,
                          const char *inner_path, char hex[41], int *verified) {
    WlJson j;
    int found = 0;
    if (wl_json_parse(&j, json, json_len) != WL_OK) {
        return 0;
    }
    WlJsonRef dumps = wl_json_get(&j, wl_json_root(&j), "dumps");
    for (size_t i = 0; !found && i < wl_json_len(&j, dumps); i++) {
        WlJsonRef d = wl_json_at(&j, dumps, i);
        const char *gc = wl_json_str(&j, wl_json_get(&j, d, "game_code"));
        if (gc == NULL || strcmp(gc, game_code) != 0) {
            continue;
        }
        WlJsonRef inner = wl_json_get(&j, d, "inner");
        for (size_t k = 0; k < wl_json_len(&j, inner); k++) {
            WlJsonRef e = wl_json_at(&j, inner, k);
            const char *p = wl_json_str(&j, wl_json_get(&j, e, "path"));
            const char *h = wl_json_str(&j, wl_json_get(&j, e, "sha1"));
            uint8_t raw[20];
            if (p != NULL && strcmp(p, inner_path) == 0 && h != NULL &&
                wl_sha1_from_hex(h, raw) == WL_OK) {
                hex_of(raw, hex);
                int v = 0;
                (void)wl_json_bool(&j, wl_json_get(&j, e, "verified"), &v);
                *verified = v;
                found = 1;
                break;
            }
        }
    }
    wl_json_free(&j);
    return found;
}

static WlResult finish(WlWiiDisc *d, const char *expected) {
    char dolp[WL_WII_PATH_MAX], filesp[WL_WII_PATH_MAX];
    if (snprintf(dolp, sizeof dolp, "%s/sys/main.dol", d->root) >= (int)sizeof dolp ||
        snprintf(filesp, sizeof filesp, "%s/files", d->root) >= (int)sizeof filesp) {
        return WL_ERR_RANGE;
    }
    uint8_t dig[20];
    uint64_t sz;
    WlResult r = wl_file_sha1(dolp, dig, &sz);
    if (r != WL_OK) {
        return r;
    }
    hex_of(dig, d->main_dol_sha1);
    d->dol_check = WL_WII_DOL_NO_REFERENCE;
    if (expected != NULL) {
        uint8_t raw[20];
        if (wl_sha1_from_hex(expected, raw) != WL_OK) {
            return WL_ERR_ARG;
        }
        hex_of(raw, d->dol_expected);
        d->dol_check = strcmp(d->dol_expected, d->main_dol_sha1) == 0 ? WL_WII_DOL_MATCH
                                                                      : WL_WII_DOL_MISMATCH;
    }
    Tally t = {0, 0, 0};
    r = wl_file_walk(filesp, tally_cb, &t);
    if (r != WL_OK) {
        return r;
    }
    d->files = t.files;
    d->dirs = t.dirs;
    d->bytes = t.bytes;
    return WL_OK;
}

WlResult wl_wii_open(WlWiiDisc *d, const char *image, const char *cache_root,
                     const WlWiiTool *tool, const char *expected_dol_sha1) {
    if (d == NULL || image == NULL) {
        return WL_ERR_ARG;
    }
    memset(d, 0, sizeof *d);
    int is_wii = 0;
    WlResult r = wl_wii_probe(image, &is_wii, d->game_code);
    if (r != WL_OK) {
        return r;
    }
    if (!is_wii) {
        return WL_ERR_FORMAT;
    }
    char root[WL_WII_PATH_MAX];
    if (cache_root != NULL) {
        if (snprintf(root, sizeof root, "%s", cache_root) >= (int)sizeof root) {
            return WL_ERR_RANGE;
        }
    } else if (!default_cache(root, sizeof root)) {
        return WL_ERR_ARG;
    }
    r = wl_file_mkdirs(root);
    if (r != WL_OK) {
        return r;
    }
    r = image_key(image, root, d->image_sha1);
    if (r != WL_OK) {
        return r;
    }
    char dir[WL_WII_PATH_MAX], part[WL_WII_PATH_MAX];
    if (snprintf(dir, sizeof dir, "%s/%.16s", root, d->image_sha1) >= (int)sizeof dir ||
        snprintf(part, sizeof part, "%s/%.16s.part", root, d->image_sha1) >= (int)sizeof part) {
        return WL_ERR_RANGE;
    }
    if (wl_file_is_dir(dir) && find_root(dir, d->root, sizeof d->root) == WL_OK) {
        d->from_cache = 1;
        return finish(d, expected_dol_sha1);
    }
    WlWiiTool found;
    if (tool == NULL) {
        r = wl_wii_find_tool(&found);
        if (r != WL_OK) {
            WL_LOG_E(TAG, "dolphin-tool not found (set WL_DOLPHIN_TOOL, install the Dolphin "
                          "flatpak, or set WL_DOLPHIN_WIN)");
            return r;
        }
        tool = &found;
    }
    (void)wl_file_remove_tree(part);
    (void)wl_file_remove_tree(dir);
    r = wl_file_mkdirs(part);
    if (r != WL_OK) {
        return r;
    }
    WL_LOG_I(TAG, "extracting the DATA partition of '%s' into '%s'", image, dir);
    r = run_extract(tool, root, image, part);
    if (r != WL_OK) {
        (void)wl_file_remove_tree(part);
        return r;
    }
    /* The flatpak sandbox exits 0 even when it wrote nothing: look for the files. */
    char found_root[WL_WII_PATH_MAX];
    r = find_root(part, found_root, sizeof found_root);
    char filesp[WL_WII_PATH_MAX];
    if (r == WL_OK &&
        (snprintf(filesp, sizeof filesp, "%s/files", found_root) >= (int)sizeof filesp ||
         !wl_file_is_dir(filesp))) {
        r = WL_ERR_EMPTY;
    }
    if (r != WL_OK) {
        WL_LOG_E(TAG, "dolphin-tool reported success but extracted no disc files into '%s'", part);
        (void)wl_file_remove_tree(part);
        return WL_ERR_EMPTY;
    }
    r = wl_file_rename(part, dir);
    if (r != WL_OK) {
        return r;
    }
    r = find_root(dir, d->root, sizeof d->root);
    if (r != WL_OK) {
        return r;
    }
    return finish(d, expected_dol_sha1);
}
