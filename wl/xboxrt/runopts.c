// SPDX-License-Identifier: MIT
// The run's options from the command line and netrun.txt (runopts.h).
#include "wl/xboxrt/runopts.h"

#include <stdio.h>
#include <string.h>

#include "wl/platform/platform.h"

#define TAG "xbrun"
#define NAME_MAX_LEN 40u
#define TEXT_MAX 512u

static bool name_ok(const char *name, size_t len) {
    if (len == 0u || len > NAME_MAX_LEN || name[0] == '.') {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '_' ||
              c == '-')) {
            return false;
        }
    }
    return true;
}

static WlResult make_file(const char *title_dir, const char *name, size_t name_len, const char *text, size_t text_len,
                          WlXbRunOptions *out) {
    char path[WL_FS_PATH_MAX];
    char full[NAME_MAX_LEN + 8u];

    if (!name_ok(name, name_len)) {
        return WL_ERR_FORMAT;
    }
    // (the two files that hold settings may be named without their extension)
    bool bare = name_len == 7u && (memcmp(name, "profile", 7) == 0 || memcmp(name, "hwcount", 7) == 0);
    snprintf(full, sizeof full, "%.*s%s", (int)name_len, name, bare ? ".txt" : "");
    if (snprintf(path, sizeof path, "%s/%s", title_dir, full) >= (int)sizeof path) {
        return WL_ERR_FORMAT;
    }
    WlResult r = wl_platform_fs_virtual_set(path, text, (uint32_t)text_len);
    if (r == WL_ERR_STATE) {
        return WL_OK; // (given twice: the first wins)
    }
    if (r == WL_OK) {
        out->virtual_files++;
    }
    return r;
}

static WlResult set_string(char *dst, size_t cap, const char *value) {
    if (dst[0] != '\0') {
        return WL_OK; // (given twice: the first wins)
    }
    if (value[0] == '\0' || strlen(value) >= cap) {
        return WL_ERR_FORMAT;
    }
    memcpy(dst, value, strlen(value) + 1u);
    return WL_OK;
}

bool wl_xb_run_option_is(const char *arg) {
    static const char *const keys[] = {"play=", "switch=", "file=", "net=", "run="};

    for (size_t i = 0; arg != NULL && i < sizeof keys / sizeof keys[0]; i++) {
        if (strncmp(arg, keys[i], strlen(keys[i])) == 0) {
            return true;
        }
    }
    return false;
}

WlResult wl_xb_run_option_apply(const char *title_dir, const char *option, WlXbRunOptions *out) {
    if (title_dir == NULL || option == NULL || out == NULL) {
        return WL_ERR_ARG;
    }
    if (strncmp(option, "play=", 5) == 0) {
        return set_string(out->play, sizeof out->play, option + 5);
    }
    if (strncmp(option, "net=", 4) == 0) {
        return set_string(out->net, sizeof out->net, option + 4);
    }
    if (strncmp(option, "run=", 4) == 0) {
        return set_string(out->name, sizeof out->name, option + 4);
    }
    if (strncmp(option, "switch=", 7) == 0) {
        const char *p = option + 7;
        WlResult first_error = WL_OK;
        while (*p != '\0') {
            const char *end = strchr(p, ',');
            size_t len = end != NULL ? (size_t)(end - p) : strlen(p);
            const char *eq = memchr(p, '=', len);
            size_t name_len = eq != NULL ? (size_t)(eq - p) : len;
            const char *text = eq != NULL ? eq + 1 : "";
            size_t text_len = eq != NULL ? len - name_len - 1u : 0u;
            WlResult r = make_file(title_dir, p, name_len, text, text_len, out);
            if (r != WL_OK && first_error == WL_OK) {
                first_error = r;
            }
            p += len + (end != NULL ? 1u : 0u);
        }
        return first_error;
    }
    if (strncmp(option, "file=", 5) == 0) {
        const char *colon = strchr(option + 5, ':');
        char text[TEXT_MAX];
        if (colon == NULL || strlen(colon + 1) >= sizeof text - 1u) {
            return WL_ERR_FORMAT;
        }
        size_t n = 0;
        for (const char *c = colon + 1; *c != '\0'; c++) {
            text[n++] = *c == ';' ? '\n' : *c;
        }
        if (n > 0u) {
            text[n++] = '\n';
        }
        return make_file(title_dir, option + 5, (size_t)(colon - (option + 5)), text, n, out);
    }
    return WL_ERR_FORMAT;
}

static void apply_logged(const char *title_dir, const char *option, WlXbRunOptions *out) {
    WlResult r = wl_xb_run_option_apply(title_dir, option, out);
    if (r != WL_OK) {
        WL_LOG_W(TAG, "option \"%.60s\": %s", option, wl_result_name(r));
    }
}

void wl_xb_run_options_load(const char *title_dir, int arg_count, char *const *args, WlXbRunOptions *out) {
    char path[WL_FS_PATH_MAX];
    char text[1024];

    memset(out, 0, sizeof *out);
    for (int i = 0; i < arg_count; i++) {
        if (args[i] != NULL && wl_xb_run_option_is(args[i])) {
            apply_logged(title_dir, args[i], out);
        } else if (args[i] != NULL) {
            WL_LOG_W(TAG, "argument \"%.60s\" is not an option", args[i]);
        }
    }
    WlPlatformFile *file = NULL;
    snprintf(path, sizeof path, "%s/netrun.txt", title_dir);
    if (wl_platform_fs_open(path, WL_FILE_READ, &file) != WL_OK) {
        return;
    }
    uint32_t got = 0;
    (void)wl_platform_fs_read_some_at(file, 0, text, sizeof text - 1u, &got);
    wl_platform_fs_close(file);
    text[got] = '\0';
    for (char *line = text; *line != '\0';) {
        char *eol = line + strcspn(line, "\r\n");
        char saved = *eol;
        *eol = '\0';
        while (*line == ' ' || *line == '\t') {
            line++;
        }
        if (*line != '\0' && *line != '#') {
            apply_logged(title_dir, line, out);
        }
        if (saved == '\0') {
            break;
        }
        line = eol + 1;
    }
}
