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

static XkObject *g_handles[MAX_HANDLES];

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
    }
    g_xk_stats.objects++;
    return o;
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
    if (--o->refs > 0 || o->type == XK_OBJ_THREAD) {
        return STATUS_SUCCESS; // threads live on: their KTHREAD is the process's
    }
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

// VOID FASTCALL ObfDereferenceObject(PVOID Object) / ObfReferenceObject: objects here live as
// long as a handle names them; the references by pointer are counted only for threads' sake
void k_ObfDereferenceObject(void) {
    RET(0);
}

void k_ObfReferenceObject(void) {
    RET(0);
}

