// SPDX-License-Identifier: MIT
// The kernel's set-up, its export table (exports.inc: every ordinal, the implemented ones bound to
// their k_ functions by weak references), the import resolver, and small shared helpers.
#include <stdio.h>
#include <stdlib.h>

#include "wl/xboxkrnl/kernel_internal.h"

WlXkConfig g_xk_cfg;
WlXkStats g_xk_stats;
static bool g_inited;

// Every export's k_ function, weakly: the ones a group file defines are bound, the rest are NULL.
#define WL_XK_EXPORT(ord, name, bytes, kind) extern void k_##name(void) __attribute__((weak));
#include "wl/xboxkrnl/exports.inc"
#undef WL_XK_EXPORT

typedef struct {
    uint16_t ordinal;
    uint8_t arg_bytes;
    char kind;
    const char *name;
    void (*fn)(void);
} Export;

#define K_F 'F'
#define K_R 'R'
#define K_C 'C'
#define K_D 'D'
#define WL_XK_EXPORT(ord, name, bytes, kind) {ord, bytes, K_##kind, #name, k_##name},
static const Export k_exports[] = {
#include "wl/xboxkrnl/exports.inc"
};
#undef WL_XK_EXPORT

static const Export *find_export(uint32_t ordinal) {
    // ordered by ordinal with gaps: binary search
    uint32_t lo = 0, hi = WL_ARRAY_COUNT(k_exports);
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2u;
        if (k_exports[mid].ordinal < ordinal) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return lo < WL_ARRAY_COUNT(k_exports) && k_exports[lo].ordinal == ordinal ? &k_exports[lo] : NULL;
}

const char *wl_xk_export_name(uint32_t ordinal) {
    const Export *e = find_export(ordinal);
    return e != NULL ? e->name : "Unknown";
}

bool wl_xk_export_info(uint32_t ordinal, uint32_t *arg_bytes, bool *data, bool *implemented) {
    const Export *e = find_export(ordinal);
    if (e == NULL) {
        return false;
    }
    if (arg_bytes != NULL) {
        *arg_bytes = e->arg_bytes;
    }
    if (data != NULL) {
        *data = e->kind == 'D';
    }
    if (implemented != NULL) {
        *implemented = e->kind == 'D' ? xk_kdata_va(ordinal) != 0 : e->fn != NULL;
    }
    return true;
}

bool wl_xk_resolve(void *user, uint32_t ordinal, WlXbImport *out) {
    (void)user;
    const Export *e = find_export(ordinal);
    if (e == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);
    out->name = e->name;
    if (e->kind == 'D') {
        out->data = true;
        out->data_va = xk_kdata_va(ordinal);
        if (out->data_va == 0) {
            WL_LOG_W(XK_TAG, "data export %u (%s) has no storage: the slot reads 0", (unsigned)ordinal, e->name);
        }
        return true;
    }
    out->fn = e->fn;
    out->arg_bytes = e->arg_bytes;
    if (e->fn == NULL) {
        WL_LOG_W(XK_TAG, "import %u (%s) has no implementation: its calls return 0", (unsigned)ordinal, e->name);
    }
    return true;
}

void wl_xk_config_default(WlXkConfig *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->title_dir = "sd:/xbox/title";
    cfg->data_dir = "sd:/xbox/title/hdd";
    cfg->language = 1;
    cfg->game_region = 1;
    cfg->av_region = 0x00400100u;
    cfg->dvd_region = 1;
    cfg->av_pack = 1;
    cfg->gpu_instance_end = true;
}

static void thread_ended(WlXbThread *t) {
    xk_thread_abandon_mutants(t);
    if (t->kthread != 0) {
        xk_object_signaled(t->kthread);
    }
}

WlResult wl_xk_init(const WlXkConfig *cfg) {
    if (g_inited) {
        return WL_ERR_STATE;
    }
    if (cfg == NULL || !wl_xb_mem_ready() || !wl_xb_threads_ready()) {
        return WL_ERR_ARG;
    }
    g_xk_cfg = *cfg;
    xk_link_ke();
    xk_link_ke_sync();
    xk_link_nt_sync();
    xk_link_ex();
    xk_link_mm();
    xk_link_nt();
    xk_link_ps();
    xk_link_rtl();
    xk_link_hal();
    xk_link_io();
    xk_link_xc();
    xk_kdata_init(cfg);
    xk_files_init(cfg);
    xk_irq_init();
    wl_xb_thread_set_exit_hook(thread_ended);
    wl_xb_service_set_tick_va(xk_kdata_tick_va());
    wl_xb_thunk_trace(cfg->trace_calls, false);
    uint32_t implemented = 0;
    for (size_t i = 0; i < WL_ARRAY_COUNT(k_exports); i++) {
        implemented += k_exports[i].kind == 'D' ? xk_kdata_va(k_exports[i].ordinal) != 0 : k_exports[i].fn != NULL;
    }
    WL_LOG_I(XK_TAG, "kernel up: %u of %u exports implemented", (unsigned)implemented, (unsigned)WL_ARRAY_COUNT(k_exports));
    g_inited = true;
    return WL_OK;
}

WlResult wl_xk_adopt_thread(void) {
    WlXbThread *t = wl_xb_current();
    if (t == NULL) {
        return WL_ERR_STATE;
    }
    return xk_thread_object(t) != NULL ? WL_OK : WL_ERR_NOMEM;
}

void wl_xk_stats(WlXkStats *out) {
    *out = g_xk_stats;
}

void xk_stub(const char *name) {
    static const char *seen[128];
    static uint32_t count;
    for (uint32_t i = 0; i < count; i++) {
        if (seen[i] == name) {
            return;
        }
    }
    if (count < WL_ARRAY_COUNT(seen)) {
        seen[count++] = name;
    }
    WL_LOG_W(XK_TAG, "stub: %s (from 0x%08x)", name, (unsigned)wl_xb_kernel_caller());
}

bool xk_read_ansi(uint32_t str, char *out, uint32_t cap) {
    out[0] = '\0';
    if (str == 0 || !wl_xb_range_ok(str, 8)) {
        return false;
    }
    uint32_t len = wl_xb_rd16(str);
    uint32_t buf = wl_xb_rd32(str + 4u);
    if (len >= cap || (len > 0 && !wl_xb_range_ok(buf, len))) {
        return false;
    }
    wl_xb_copy_out(out, buf, len);
    out[len] = '\0';
    return true;
}

uint32_t xk_write_ansi(uint32_t str, const char *text) {
    if (str == 0 || !wl_xb_range_ok(str, 8)) {
        return STATUS_INVALID_PARAMETER;
    }
    uint32_t len = (uint32_t)strlen(text);
    uint32_t max = wl_xb_rd16(str + 2u);
    uint32_t buf = wl_xb_rd32(str + 4u);
    if (len > max || buf == 0 || !wl_xb_range_ok(buf, len)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    wl_xb_copy_in(buf, text, len);
    if (len < max) {
        wl_xb_wr8(buf + len, 0);
    }
    wl_xb_wr16(str, (uint16_t)len);
    return STATUS_SUCCESS;
}

void xk_header_init(uint32_t obj, uint8_t type, uint8_t size_dwords, int32_t signal) {
    wl_xb_wr8(obj + DISP_TYPE, type);
    wl_xb_wr8(obj + 1u, 0);
    wl_xb_wr8(obj + DISP_SIZE, size_dwords);
    wl_xb_wr8(obj + 3u, 0);
    wl_xb_wr32(obj + DISP_SIGNAL, (uint32_t)signal);
    wl_xb_wr32(obj + DISP_WAITLIST, obj + DISP_WAITLIST);
    wl_xb_wr32(obj + DISP_WAITLIST + 4u, obj + DISP_WAITLIST);
}
