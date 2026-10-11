// SPDX-License-Identifier: MIT
// Thread-local variables for the Wii: the emulated-TLS entry point (__emutls_get_address) that
// compilers call under -femulated-tls. Native TLS cannot work: the PowerPC ABI addresses it from
// r2, which the EABI gives the small data area and libogc no thread's storage. libgcc's own
// emulation needs pthread keys, which devkitPPC's libsysbase does not have (it aborts). Here each
// libogc thread (KThreadGetSelf) has a slot holding its copy of every variable, made from the
// variable's template the first time the thread uses it. A thread that ends calls
// wl_platform_tls_thread_exit, or its slot stays until a new thread at the same address would
// inherit it. Only this directory may include <ogc/*.h> and libogc's own headers.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <ogc/irq.h>
#include <tuxedo/thread.h>

#include "wl/platform/platform.h"

#define MAX_THREADS 64u

// libgcc's emulated-TLS control block (emutls.c), which the compiler emits for each variable
struct __emutls_object {
    uintptr_t size;
    uintptr_t align;
    union {
        uintptr_t offset; // 1 + the variable's index, once it has one
        void *ptr;
    } loc;
    void *templ;
};

typedef struct {
    KThread *thread;
    uintptr_t count; // values allocated
    void **values;   // by variable index
} Slot;

static Slot g_slots[MAX_THREADS];
static uintptr_t g_variables;

void *__emutls_get_address(struct __emutls_object *object);

static Slot *slot_of(KThread *thread) {
    Slot *free_slot = NULL;
    for (uint32_t i = 0; i < MAX_THREADS; i++) {
        if (g_slots[i].thread == thread) {
            return &g_slots[i];
        }
        if (g_slots[i].thread == NULL && free_slot == NULL) {
            free_slot = &g_slots[i];
        }
    }
    if (free_slot != NULL) {
        free_slot->thread = thread;
        free_slot->count = 0;
        free_slot->values = NULL;
    }
    return free_slot;
}

void *__emutls_get_address(struct __emutls_object *object) {
    u32 level = IRQ_Disable();
    if (object->loc.offset == 0) {
        object->loc.offset = ++g_variables;
    }
    uintptr_t index = object->loc.offset - 1;
    Slot *slot = slot_of(KThreadGetSelf());
    IRQ_Restore(level);
    WL_ASSERT(slot != NULL); // more than MAX_THREADS threads used thread-local variables

    if (index >= slot->count) {
        uintptr_t count = index + 8;
        void **values = realloc(slot->values, count * sizeof *values);
        WL_ASSERT(values != NULL);
        memset(values + slot->count, 0, (count - slot->count) * sizeof *values);
        slot->values = values;
        slot->count = count;
    }
    if (slot->values[index] == NULL) {
        uintptr_t align = object->align > sizeof(void *) ? object->align : sizeof(void *);
        void *value = aligned_alloc(align, (object->size + align - 1) & ~(align - 1));
        WL_ASSERT(value != NULL);
        if (object->templ != NULL) {
            memcpy(value, object->templ, object->size);
        } else {
            memset(value, 0, object->size);
        }
        slot->values[index] = value;
    }
    return slot->values[index];
}

void wl_platform_tls_thread_exit(void) {
    u32 level = IRQ_Disable();
    Slot *slot = NULL;
    KThread *self = KThreadGetSelf();
    for (uint32_t i = 0; i < MAX_THREADS; i++) {
        if (g_slots[i].thread == self) {
            slot = &g_slots[i];
            break;
        }
    }
    Slot released = slot != NULL ? *slot : (Slot){0};
    if (slot != NULL) {
        slot->thread = NULL;
        slot->count = 0;
        slot->values = NULL;
    }
    IRQ_Restore(level);
    for (uintptr_t i = 0; i < released.count; i++) {
        free(released.values[i]);
    }
    free(released.values);
}
