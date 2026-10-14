// SPDX-License-Identifier: MIT
// Who holds the host's heap (mem_census.h).
#include "wl/platform/mem_census.h"

#include <stdio.h>
#include <string.h>

static struct {
    const char *name;
    WlMemCensusFn fn;
    void *user;
} g_reporters[WL_MEM_CENSUS_MAX];
static uint32_t g_count;

WlResult wl_mem_census_add(const char *name, WlMemCensusFn fn, void *user) {
    if (name == NULL || fn == NULL) {
        return WL_ERR_ARG;
    }
    uint32_t i = 0;
    while (i < g_count && strcmp(g_reporters[i].name, name) != 0) {
        i++;
    }
    if (i == g_count) {
        if (g_count == WL_MEM_CENSUS_MAX) {
            return WL_ERR_LIMIT;
        }
        g_count++;
    }
    g_reporters[i].name = name;
    g_reporters[i].fn = fn;
    g_reporters[i].user = user;
    return WL_OK;
}

void wl_mem_census_remove(const char *name) {
    for (uint32_t i = 0; name != NULL && i < g_count; i++) {
        if (strcmp(g_reporters[i].name, name) == 0) {
            memmove(&g_reporters[i], &g_reporters[i + 1], (g_count - i - 1u) * sizeof g_reporters[0]);
            g_count--;
            return;
        }
    }
}

void wl_mem_census_take(WlMemCensus *out) {
    memset(out, 0, sizeof *out);
    wl_platform_heap_info(&out->heap);
    for (uint32_t i = 0; i < g_count; i++) {
        WlMemCensusRow *row = &out->rows[out->count++];
        row->name = g_reporters[i].name;
        g_reporters[i].fn(g_reporters[i].user, &row->report);
        row->report.note[WL_MEM_CENSUS_NOTE - 1u] = '\0';
        out->claimed += row->report.bytes;
    }
    // largest first (a handful of rows: insertion sort)
    for (uint32_t i = 1; i < out->count; i++) {
        WlMemCensusRow r = out->rows[i];
        uint32_t j = i;
        while (j > 0 && out->rows[j - 1].report.bytes < r.report.bytes) {
            out->rows[j] = out->rows[j - 1];
            j--;
        }
        out->rows[j] = r;
    }
    out->other = out->heap.known && out->heap.in_use > out->claimed ? out->heap.in_use - out->claimed : 0;
}

static unsigned kb(size_t bytes) {
    return (unsigned)((bytes + 512u) >> 10);
}

void wl_mem_census_heap_text(const WlPlatformHeapInfo *h, char *buf, size_t size) {
    if (!h->known) {
        snprintf(buf, size, "heap: not known on this platform");
        return;
    }
    int n;
    if (h->top == SIZE_MAX) {
        n = snprintf(buf, size, "heap: %u KB in use, %u KB free inside (no limit above)", kb(h->in_use),
                     kb(h->free_inside));
    } else {
        n = snprintf(buf, size, "heap: %u KB in use, %u KB free (%u inside, %u above the top", kb(h->in_use),
                     kb(h->free_total), kb(h->free_inside), kb(h->top));
        if (n > 0 && (size_t)n < size) {
            n += snprintf(buf + n, size - (size_t)n, h->elsewhere != 0 ? ", %u in MEM2)" : ")", kb(h->elsewhere));
        }
    }
    if (h->cap != 0 && n > 0 && (size_t)n < size) {
        snprintf(buf + n, size - (size_t)n, "; cap %u KB: %u used, %u at most, %u refused (largest %u B)", kb(h->cap),
                 kb(h->cap_used), kb(h->cap_peak), (unsigned)h->cap_refused, (unsigned)h->cap_largest_refused);
    }
}

#define LINE 200u

void wl_mem_census_log(const char *tag, bool heap_only) {
    static WlMemCensus c; // (large for a stack; one thread at a time takes the census)
    char line[LINE + 64];
    if (heap_only) {
        WlPlatformHeapInfo h;
        wl_platform_heap_info(&h);
        wl_mem_census_heap_text(&h, line, sizeof line);
        WL_LOG_I(tag, "census: %s", line);
        return;
    }
    wl_mem_census_take(&c);
    wl_mem_census_heap_text(&c.heap, line, sizeof line);
    WL_LOG_I(tag, "census: %s", line);
    size_t used = 0;
    line[0] = '\0';
    for (uint32_t i = 0; i <= c.count; i++) {
        char item[32 + WL_MEM_CENSUS_NOTE + 8];
        if (i < c.count) {
            const WlMemCensusRow *r = &c.rows[i];
            if (r->report.bytes == 0 && r->report.note[0] == '\0') {
                continue; // (nothing held: not listed)
            }
            if (r->report.note[0] != '\0') {
                snprintf(item, sizeof item, "%s %u (%s)", r->name, kb(r->report.bytes), r->report.note);
            } else {
                snprintf(item, sizeof item, "%s %u", r->name, kb(r->report.bytes));
            }
        } else {
            snprintf(item, sizeof item, "other %u", kb(c.other));
        }
        size_t n = strlen(item);
        if (used != 0 && used + 2u + n > LINE) {
            WL_LOG_I(tag, "census KB: %s", line);
            used = 0;
            line[0] = '\0';
        }
        used += (size_t)snprintf(line + used, sizeof line - used, "%s%s", used != 0 ? ", " : "", item);
    }
    WL_LOG_I(tag, "census KB: %s", line);
}
