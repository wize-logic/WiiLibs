// SPDX-License-Identifier: MIT
// XBE parsing, loading and thunk resolution (xbe.h). Header and certificate offsets are the XBE
// format's as public descriptions (xboxdevwiki "Xbe") give them; the same fields xboxrecomp's
// tools/xbe_parser reads.
#include <stdlib.h>
#include <string.h>

#include "wl/base/endian.h"
#include "wl/xboxrt/page_census.h"
#include "wl/xboxrt/mem.h"
#include "wl/xboxrt/thread.h"
#include "wl/xboxrt/thunk.h"
#include "wl/xboxrt/xbe.h"

#define TAG "xbe"
#define BOUNCE 65536u
#define MAX_THUNKS 400u

static bool in_image(const WlXbe *x, uint32_t va) {
    return va >= x->base && va - x->base < x->image_size;
}

WlResult wl_xbe_parse(const uint8_t *h, uint32_t n, WlXbe *out) {
    memset(out, 0, sizeof *out);
    if (n < 0x178u || memcmp(h, "XBEH", 4) != 0) {
        return WL_ERR_FORMAT;
    }
    out->base = wl_rd_le32(h + 0x104);
    out->headers_size = wl_rd_le32(h + 0x108);
    out->image_size = wl_rd_le32(h + 0x10C);
    out->cert_va = wl_rd_le32(h + 0x118);
    out->section_count = wl_rd_le32(h + 0x11C);
    uint32_t sec_va = wl_rd_le32(h + 0x120);
    out->init_flags = wl_rd_le32(h + 0x124);
    uint32_t entry = wl_rd_le32(h + 0x128);
    out->tls_dir = wl_rd_le32(h + 0x12C);
    out->stack_commit = wl_rd_le32(h + 0x130);
    out->heap_reserve = wl_rd_le32(h + 0x134);
    out->heap_commit = wl_rd_le32(h + 0x138);
    uint32_t thunk = wl_rd_le32(h + 0x158);
    if (out->headers_size > n) {
        return WL_ERR_RANGE;
    }
    if (out->base == 0 || out->image_size == 0 || out->section_count > WL_XBE_MAX_SECTIONS || out->headers_size < 0x178u) {
        return WL_ERR_FORMAT;
    }
    // the XOR-encoded entry and thunk address: the key whose result lies inside the image
    uint32_t e_retail = entry ^ WL_XBE_ENTRY_KEY_RETAIL, e_debug = entry ^ WL_XBE_ENTRY_KEY_DEBUG;
    uint32_t t_retail = thunk ^ WL_XBE_THUNK_KEY_RETAIL, t_debug = thunk ^ WL_XBE_THUNK_KEY_DEBUG;
    if (in_image(out, e_retail) && in_image(out, t_retail)) {
        out->entry = e_retail;
        out->thunk_table = t_retail;
    } else if (in_image(out, e_debug) && in_image(out, t_debug)) {
        out->entry = e_debug;
        out->thunk_table = t_debug;
        out->debug_keys = true;
    } else {
        WL_LOG_E(TAG, "neither key decodes the entry 0x%08x and thunk 0x%08x into the image", (unsigned)entry,
                 (unsigned)thunk);
        return WL_ERR_FORMAT;
    }
    // section headers, 56 bytes each, at a VA inside the headers
    uint32_t so = sec_va - out->base;
    if (sec_va < out->base || (uint64_t)so + (uint64_t)out->section_count * 56u > out->headers_size) {
        return WL_ERR_FORMAT;
    }
    for (uint32_t i = 0; i < out->section_count; i++) {
        const uint8_t *s = h + so + i * 56u;
        WlXbeSection *sec = &out->sections[i];
        sec->flags = wl_rd_le32(s + 0);
        sec->va = wl_rd_le32(s + 4);
        sec->vsize = wl_rd_le32(s + 8);
        sec->raw_offset = wl_rd_le32(s + 12);
        sec->raw_size = wl_rd_le32(s + 16);
        uint32_t name_va = wl_rd_le32(s + 20);
        if (name_va >= out->base && name_va - out->base < out->headers_size) {
            uint32_t no = name_va - out->base;
            size_t k = 0;
            while (k + 1u < sizeof sec->name && no + k < out->headers_size && h[no + k] != 0) {
                sec->name[k] = (char)h[no + k];
                k++;
            }
            sec->name[k] = '\0';
        }
        if (!in_image(out, sec->va) || (uint64_t)sec->va + sec->vsize > (uint64_t)out->base + out->image_size) {
            WL_LOG_W(TAG, "section %s at 0x%08x+0x%x is outside the image", sec->name, (unsigned)sec->va,
                     (unsigned)sec->vsize);
        }
    }
    // certificate
    if (out->cert_va >= out->base && out->cert_va - out->base + 0xD0u <= out->headers_size) {
        const uint8_t *c = h + (out->cert_va - out->base);
        out->title_id = wl_rd_le32(c + 0x08);
        for (int i = 0; i < 40; i++) {
            uint16_t ch = wl_rd_le16(c + 0x0C + 2 * i);
            out->title_name[i] = ch == 0 ? '\0' : (ch < 0x80 ? (char)ch : '?');
            if (ch == 0) {
                break;
            }
        }
        out->allowed_media = wl_rd_le32(c + 0x9C);
        out->game_region = wl_rd_le32(c + 0xA0);
        out->game_ratings = wl_rd_le32(c + 0xA4);
        out->version = wl_rd_le32(c + 0xAC);
        memcpy(out->lan_key, c + 0xB0, 16);
        memcpy(out->signature_key, c + 0xC0, 16);
    }
    return WL_OK;
}

// Copies [offset, offset + n) of the file to guest `va` through a host buffer.
static WlResult copy_file(WlXbeRead read, void *user, uint8_t *bounce, uint32_t offset, uint32_t va, uint32_t n) {
    while (n > 0) {
        uint32_t chunk = n < BOUNCE ? n : BOUNCE;
        WlResult r = read(user, offset, bounce, chunk);
        if (r != WL_OK) {
            return r;
        }
        wl_xb_copy_in(va, bounce, chunk);
        offset += chunk;
        va += chunk;
        n -= chunk;
    }
    return WL_OK;
}

// For the page census: whether a section is x86 code. Its executable flag is not enough: titles
// mark .data and .rdata executable too (Halo's are), so the sections named as data are data.
static bool census_code(const WlXbeSection *s) {
    size_t n = strlen(s->name);
    return (s->flags & WL_XBE_SEC_EXECUTABLE) != 0 && strncmp(s->name, ".data", 5) != 0 &&
           strncmp(s->name, ".rdata", 6) != 0 && strncmp(s->name, ".bss", 4) != 0 && strncmp(s->name, "$$", 2) != 0 &&
           strcmp(s->name, "DOLBY") != 0 && !(n >= 4 && strcmp(s->name + n - 4, "DATA") == 0);
}

WlResult wl_xbe_load(const WlXbe *x, WlXbeRead read, void *user) {
    return wl_xbe_load_backed(x, read, user, NULL);
}

WlResult wl_xbe_load_backed(const WlXbe *x, WlXbeRead read, void *user, struct WlPlatformFile *file) {
    if (x == NULL || read == NULL || !wl_xb_mem_ready()) {
        return WL_ERR_ARG;
    }
    uint32_t va = x->base;
    WlResult r = wl_xb_va_alloc(&va, x->image_size, 0, WL_XB_PROT_EXECUTE_READWRITE);
    if (r != WL_OK) {
        WL_LOG_E(TAG, "cannot map the image at 0x%08x+0x%x: %s", (unsigned)x->base, (unsigned)x->image_size,
                 wl_result_name(r));
        return r;
    }
    uint8_t *bounce = malloc(BOUNCE);
    if (bounce == NULL) {
        return WL_ERR_NOMEM;
    }
    r = copy_file(read, user, bounce, 0, x->base, x->headers_size);
    uint32_t code_lo = 0xFFFFFFFFu, code_hi = 0;
    for (uint32_t i = 0; r == WL_OK && i < x->section_count; i++) {
        const WlXbeSection *s = &x->sections[i];
        uint32_t n = s->raw_size < s->vsize ? s->raw_size : s->vsize;
        if (!wl_xb_range_ok(s->va, s->vsize)) {
            continue; // outside the image: logged by the parser
        }
        r = copy_file(read, user, bounce, s->raw_offset, s->va, n);
        if (r == WL_OK && file != NULL) {
            // (the file's offsets need not be page-aligned: a page is read back from any offset)
            wl_xb_file_pages(s->va, n, file, s->raw_offset);
        }
        if (s->flags & WL_XBE_SEC_EXECUTABLE) {
            if (s->va < code_lo) {
                code_lo = s->va;
            }
            if (s->va + s->vsize > code_hi) {
                code_hi = s->va + s->vsize;
            }
        }
    }
    free(bounce);
    if (r != WL_OK) {
        WL_LOG_E(TAG, "reading the image failed: %s", wl_result_name(r));
        return r;
    }
    if (wl_xb_census_on()) {
        // the page census's allocations: the code first, so that a page code shares with data is data
        for (uint32_t pass = 0; pass < 2u; pass++) {
            for (uint32_t i = 0; i < x->section_count; i++) {
                const WlXbeSection *s = &x->sections[i];
                bool code = census_code(s);
                if (code == (pass == 0) && wl_xb_range_ok(s->va, s->vsize)) {
                    wl_xb_census_alloc(code ? WL_XB_CA_XBE_CODE : WL_XB_CA_XBE_DATA, s->va, s->vsize, i);
                }
            }
        }
        wl_xb_census_alloc(WL_XB_CA_XBE_DATA, x->base, x->headers_size, 0xFFFFFFFFu);
    }
    if (code_hi > code_lo) {
        wl_xb_set_code_bounds(code_lo, code_hi);
    }
    if (x->tls_dir != 0 && wl_xb_range_ok(x->tls_dir, 24)) {
        uint32_t start = wl_xb_rd32(x->tls_dir + 0), end = wl_xb_rd32(x->tls_dir + 4);
        uint32_t zero = wl_xb_rd32(x->tls_dir + 16);
        uint32_t bytes = end > start ? end - start : 0;
        wl_xb_thread_set_tls_template(bytes > 0 ? start : 0, bytes, zero);
        WL_LOG_I(TAG, "TLS: %u bytes of data at 0x%08x, %u zero, index at 0x%08x", (unsigned)bytes, (unsigned)start,
                 (unsigned)zero, (unsigned)wl_xb_rd32(x->tls_dir + 8));
    }
    WL_LOG_I(TAG, "loaded \"%s\" (title %08x): image 0x%08x+0x%x, %u sections, entry 0x%08x, thunks 0x%08x%s",
             x->title_name, (unsigned)x->title_id, (unsigned)x->base, (unsigned)x->image_size, (unsigned)x->section_count,
             (unsigned)x->entry, (unsigned)x->thunk_table, x->debug_keys ? " (debug keys)" : "");
    return WL_OK;
}

// ---- Thunks ------------------------------------------------------------------------------------

static bool resolve_slot(uint32_t slot_va, uint32_t slot, uint32_t word, WlXbImportResolve resolve, void *user,
                         const uint16_t *remap, uint32_t remap_count) {
    uint32_t ordinal = word & 0x7FFFFFFFu;
    if (remap != NULL && ordinal < remap_count && remap[ordinal] != 0) {
        ordinal = remap[ordinal];
    }
    WlXbImport imp;
    memset(&imp, 0, sizeof imp);
    bool known = resolve(user, ordinal, &imp);
    if (known && imp.data) {
        wl_xb_wr32(slot_va, imp.data_va);
        return true;
    }
    if (!known) {
        WL_LOG_W(TAG, "kernel import ordinal %u is unknown: bound to a stub returning 0", (unsigned)ordinal);
        imp.fn = NULL; // the dispatcher logs its first call and returns 0
        imp.arg_bytes = 0;
        imp.name = "unknown";
    }
    wl_xb_thunk_bind(slot, ordinal, imp.fn, imp.arg_bytes, imp.name);
    wl_xb_wr32(slot_va, wl_xb_thunk_va(slot));
    return true;
}

WlResult wl_xbe_resolve_thunks(const WlXbe *x, WlXbImportResolve resolve, void *user, const uint16_t *remap,
                               uint32_t remap_count, bool look_behind, uint32_t *count) {
    if (x == NULL || resolve == NULL || !wl_xb_range_ok(x->thunk_table, 4)) {
        return WL_ERR_ARG;
    }
    uint32_t n = 0;
    for (; n < MAX_THUNKS; n++) {
        uint32_t va = x->thunk_table + 4u * n;
        if (!wl_xb_range_ok(va, 4)) {
            break;
        }
        uint32_t w = wl_xb_rd32(va);
        if (w == 0 || (w & 0x80000000u) == 0) {
            break;
        }
        resolve_slot(va, n, w, resolve, user, remap, remap_count);
    }
    uint32_t total = n;
    if (look_behind && wl_xb_range_ok(x->thunk_table - 4u, 4) && wl_xb_rd32(x->thunk_table - 4u) == 0) {
        for (uint32_t k = 2; k < 18 && total < MAX_THUNKS; k++) {
            uint32_t va = x->thunk_table - 4u * k;
            if (!wl_xb_range_ok(va, 4)) {
                break;
            }
            uint32_t w = wl_xb_rd32(va);
            uint32_t ord = w & 0x7FFFFFFFu;
            if ((w & 0x80000000u) == 0 || ord == 0 || ord > 400u) {
                break;
            }
            resolve_slot(va, total, w, resolve, user, remap, remap_count);
            WL_LOG_I(TAG, "extra thunk below the table at 0x%08x: ordinal %u", (unsigned)va, (unsigned)ord);
            total++;
        }
    }
    if (count != NULL) {
        *count = total;
    }
    WL_LOG_I(TAG, "kernel thunks: %u slots resolved", (unsigned)total);
    return WL_OK;
}
