// SPDX-License-Identifier: MIT
// The pool (guest memory for kernel objects and ExAllocatePool), objects, the handle table, and the
// Ob exports.
//
// Pool blocks carry a 16-byte header in guest memory just before them (magic, size, class), as
// the console's pool keeps one: ExQueryPoolBlockSize reads it and a bad free is caught. Small
// blocks come from 64 KB chunks of guest-physical pages in size classes (16 bytes to 2 KB, free
// lists kept on the host); larger ones get pages of their own. Pool addresses are window VAs
// (0x80000000|P), as kernel memory is on the console.
#include <stdlib.h>

#include "wl/xboxkrnl/kernel_internal.h"
#include "wl/xboxrt/page_census.h"

#define POOL_MAGIC 0x4C504C57u // "WLPL"
#define POOL_HDR 16u
#define CHUNK_BYTES 0x10000u
#define CLASSES 8u
#define MAX_HANDLES 4096u

typedef struct {
    uint32_t *items;
    uint32_t count, cap;
} VaStack;

static VaStack g_free[CLASSES];
static uint32_t g_chunk_va, g_chunk_left;

static void push(VaStack *s, uint32_t va) {
    if (s->count == s->cap) {
        uint32_t cap = s->cap ? s->cap * 2u : 64u;
        uint32_t *n = realloc(s->items, cap * sizeof *n);
        if (n == NULL) {
            return; // the block is lost to the pool: harmless
        }
        s->items = n;
        s->cap = cap;
    }
    s->items[s->count++] = va;
}

static uint32_t class_of(uint32_t bytes) {
    uint32_t c = 0, size = 16u;
    while (size < bytes && c < CLASSES) {
        size <<= 1;
        c++;
    }
    return c; // CLASSES: too big for a class
}

uint32_t xk_pool_alloc(uint32_t bytes) {
    if (bytes == 0) {
        bytes = 1;
    }
    uint32_t cls = class_of(bytes + POOL_HDR);
    uint32_t block = 0, size = 0;
    if (cls < CLASSES) {
        size = 16u << cls;
        if (g_free[cls].count > 0) {
            block = g_free[cls].items[--g_free[cls].count];
        } else {
            if (g_chunk_left < size) {
                uint32_t phys = 0;
                if (wl_xb_phys_alloc(CHUNK_BYTES, 0, 0xFFFFFFFFu, WL_XB_PAGE, &phys) != WL_OK) {
                    return 0;
                }
                g_chunk_va = WL_XB_WINDOW | phys;
                g_chunk_left = CHUNK_BYTES;
                wl_xb_census_alloc(WL_XB_CA_POOL, g_chunk_va, CHUNK_BYTES, 0);
            }
            block = g_chunk_va;
            g_chunk_va += size;
            g_chunk_left -= size;
        }
    } else {
        size = (bytes + POOL_HDR + WL_XB_PAGE - 1u) & ~(WL_XB_PAGE - 1u);
        uint32_t phys = 0;
        if (wl_xb_phys_alloc(size, 0, 0xFFFFFFFFu, WL_XB_PAGE, &phys) != WL_OK) {
            return 0;
        }
        block = WL_XB_WINDOW | phys;
        wl_xb_census_alloc(WL_XB_CA_POOL, block, size, wl_xb_kernel_caller());
    }
    wl_xb_fill(block, 0, size);
    wl_xb_wr32(block + 0u, POOL_MAGIC);
    wl_xb_wr32(block + 4u, bytes);
    wl_xb_wr32(block + 8u, cls);
    wl_xb_wr32(block + 12u, size);
    g_xk_stats.pool_bytes += size;
    return block + POOL_HDR;
}

static bool pool_block(uint32_t va) {
    return va >= WL_XB_WINDOW + POOL_HDR && (va & 15u) == 0 && wl_xb_range_ok(va - POOL_HDR, POOL_HDR) &&
           wl_xb_rd32(va - POOL_HDR) == POOL_MAGIC;
}

void xk_pool_free(uint32_t va) {
    if (!pool_block(va)) {
        WL_LOG_W(XK_TAG, "pool free of 0x%08x: not a pool block (from 0x%08x)", (unsigned)va,
                 (unsigned)wl_xb_kernel_caller());
        return;
    }
    uint32_t block = va - POOL_HDR;
    uint32_t cls = wl_xb_rd32(block + 8u), size = wl_xb_rd32(block + 12u);
    wl_xb_wr32(block, 0); // a second free is caught
    g_xk_stats.pool_bytes -= size;
    if (cls < CLASSES) {
        push(&g_free[cls], block);
    } else {
        wl_xb_phys_free(block & 0x0FFFFFFFu);
    }
}

uint32_t xk_pool_size(uint32_t va) {
    return pool_block(va) ? wl_xb_rd32(va - POOL_HDR + 4u) : 0;
}

// ---- Objects and handles -----------------------------------------------------------------------
//
// An object lives while something refers to it: its handles, the references by pointer
// (ObReferenceObjectByHandle, ObfReferenceObject; ObfDereferenceObject gives one back, found by the
// body's address) and the waits on it (a wait holds a reference until it ends, so a handle closed
// under a waiting thread does not free the body it sleeps on). Threads live on: their KTHREAD is
// the process's.

static XkObject *g_handles[MAX_HANDLES];

// The objects by body address (open addressing, linear probing, a power-of-two table).
static XkObject **g_by_body;
static uint32_t g_by_body_cap, g_by_body_used;

static uint32_t body_slot(uint32_t body, uint32_t cap) {
    return ((body >> 4) * 2654435761u) & (cap - 1u);
}

static void body_insert(XkObject **table, uint32_t cap, XkObject *o) {
    uint32_t i = body_slot(o->body, cap);
    while (table[i] != NULL) {
        i = (i + 1u) & (cap - 1u);
    }
    table[i] = o;
}

static void body_add(XkObject *o) {
    if (o->body == 0) {
        return;
    }
    if ((g_by_body_used + 1u) * 4u > g_by_body_cap * 3u) {
        uint32_t cap = g_by_body_cap ? g_by_body_cap * 2u : 256u;
        XkObject **t = calloc(cap, sizeof *t);
        if (t == NULL) {
            return; // not found by its body: ObfDereferenceObject leaves it to its handles
        }
        for (uint32_t i = 0; i < g_by_body_cap; i++) {
            if (g_by_body[i] != NULL) {
                body_insert(t, cap, g_by_body[i]);
            }
        }
        free(g_by_body);
        g_by_body = t;
        g_by_body_cap = cap;
    }
    body_insert(g_by_body, g_by_body_cap, o);
    g_by_body_used++;
}

static void body_remove(XkObject *o) {
    if (g_by_body_cap == 0 || o->body == 0) {
        return;
    }
    uint32_t mask = g_by_body_cap - 1u, i = body_slot(o->body, g_by_body_cap);
    while (g_by_body[i] != o) {
        if (g_by_body[i] == NULL) {
            return;
        }
        i = (i + 1u) & mask;
    }
    g_by_body[i] = NULL;
    g_by_body_used--;
    // close the gap: move up the entries of the run that would no longer be found
    for (uint32_t j = (i + 1u) & mask; g_by_body[j] != NULL; j = (j + 1u) & mask) {
        uint32_t home = body_slot(g_by_body[j]->body, g_by_body_cap);
        if (((j - home) & mask) >= ((j - i) & mask)) {
            g_by_body[i] = g_by_body[j];
            g_by_body[j] = NULL;
            i = j;
        }
    }
}

XkObject *xk_object_of_body(uint32_t body) {
    if (g_by_body_cap == 0 || body == 0) {
        return NULL;
    }
    for (uint32_t i = body_slot(body, g_by_body_cap); g_by_body[i] != NULL; i = (i + 1u) & (g_by_body_cap - 1u)) {
        if (g_by_body[i]->body == body) {
            return g_by_body[i];
        }
    }
    return NULL;
}

XkObject *xk_object_new(XkObjType type, uint32_t body_bytes) {
    XkObject *o = calloc(1, sizeof *o);
    if (o == NULL) {
        return NULL;
    }
    o->type = type;
    if (body_bytes > 0) {
        o->body = xk_pool_alloc(body_bytes);
        if (o->body == 0) {
            free(o);
            return NULL;
        }
        o->body_owned = true;
        body_add(o);
    }
    g_xk_stats.objects++;
    return o;
}

void xk_object_reference(XkObject *o) {
    o->refs++;
}

void xk_object_release(XkObject *o) {
    if (o->refs == 0) {
        WL_LOG_W(XK_TAG, "object %p (type %d) released once too often", (void *)o, (int)o->type);
        return;
    }
    if (--o->refs > 0 || o->type == XK_OBJ_THREAD) {
        return;
    }
    body_remove(o);
    if (o->type == XK_OBJ_FILE && o->file != NULL) {
        xk_file_close(o->file);
    }
    if (o->type == XK_OBJ_TIMER && o->body != 0) {
        xk_timer_cancel(o->body);
    }
    if (o->body_owned) {
        xk_pool_free(o->body);
    }
    free(o->link_target);
    free(o);
    g_xk_stats.objects--;
}

uint32_t xk_handle_open(XkObject *obj) {
    for (uint32_t i = 0; i < MAX_HANDLES; i++) {
        if (g_handles[i] == NULL) {
            g_handles[i] = obj;
            obj->refs++;
            g_xk_stats.handles++;
            return (i + 1u) << 2;
        }
    }
    WL_LOG_E(XK_TAG, "handle table full");
    return 0;
}

XkObject *xk_handle_get(uint32_t handle) {
    if (handle == XK_CURRENT_THREAD) {
        WlXbThread *t = wl_xb_current();
        return t != NULL ? xk_thread_object(t) : NULL;
    }
    if ((handle & 3u) != 0 || handle == 0 || (handle >> 2) > MAX_HANDLES) {
        return NULL;
    }
    return g_handles[(handle >> 2) - 1u];
}

XkObject *xk_handle_get_type(uint32_t handle, XkObjType type) {
    XkObject *o = xk_handle_get(handle);
    return o != NULL && o->type == type ? o : NULL;
}

uint32_t xk_handle_close(uint32_t handle) {
    if (handle == XK_CURRENT_THREAD || handle == XK_CURRENT_PROCESS) {
        return STATUS_SUCCESS;
    }
    XkObject *o = xk_handle_get(handle);
    if (o == NULL) {
        return STATUS_INVALID_HANDLE;
    }
    g_handles[(handle >> 2) - 1u] = NULL;
    g_xk_stats.handles--;
    xk_object_release(o);
    return STATUS_SUCCESS;
}

uint32_t xk_handle_waitable(uint32_t handle) {
    XkObject *o = xk_handle_get(handle);
    if (o == NULL) {
        return 0;
    }
    return o->body; // files: their completion event body (set by the file layer)
}

XkThread *xk_thread_state(WlXbThread *t) {
    if (t->kernel == NULL) {
        t->kernel = calloc(1, sizeof(XkThread));
    }
    return t->kernel;
}

XkObject *xk_thread_object(WlXbThread *t) {
    XkThread *k = xk_thread_state(t);
    if (k == NULL) {
        return NULL;
    }
    if (k->object == NULL) {
        XkObject *o = xk_object_new(XK_OBJ_THREAD, 0);
        if (o == NULL) {
            return NULL;
        }
        o->body = t->kthread;
        o->thread = t;
        o->refs = 1; // the thread's own reference
        body_add(o);
        k->object = o;
    }
    return k->object;
}

// ---- Ob exports ----------------------------------------------------------------------------------

// NTSTATUS ObReferenceObjectByHandle(HANDLE Handle, POBJECT_TYPE ObjectType, PVOID *ReturnedObject)
void k_ObReferenceObjectByHandle(void) {
    uint32_t handle = ARG(0), type = ARG(1), out = ARG(2);
    XkObject *o = xk_handle_get(handle);
    if (o == NULL) {
        if (out != 0) {
            wl_xb_wr32(out, 0);
        }
        RET(STATUS_INVALID_HANDLE);
        return;
    }
    if (type != 0 && type != xk_object_type_va(o->type)) {
        RET(STATUS_OBJECT_TYPE_MISMATCH);
        return;
    }
    o->refs++;
    if (out != 0) {
        wl_xb_wr32(out, o->body);
    }
    RET(STATUS_SUCCESS);
}

// VOID FASTCALL ObfDereferenceObject(PVOID Object): gives back a reference by pointer; the last
// one frees the object (a file is closed). A body the object table does not know (an object the
// title initialised itself, KeInitializeEvent's) is left alone.
void k_ObfDereferenceObject(void) {
    XkObject *o = xk_object_of_body(g_ecx);
    if (o != NULL) {
        xk_object_release(o);
    }
    RET(0);
}

// VOID FASTCALL ObfReferenceObject(PVOID Object)
void k_ObfReferenceObject(void) {
    XkObject *o = xk_object_of_body(g_ecx);
    if (o != NULL) {
        o->refs++;
    }
    RET(0);
}
