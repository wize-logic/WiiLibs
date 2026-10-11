// SPDX-License-Identifier: MIT
// 8-byte atomics for the Wii: the libatomic functions compilers call for 64-bit atomic
// operations, which the 32-bit Broadway has no instructions for (clang emits calls to
// __atomic_*_8; devkitPPC ships no libatomic). Broadway is one core and libogc switches threads
// only from interrupts, so each operation runs with interrupts off. Only this directory may
// include <ogc/*.h>.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <ogc/irq.h>

// The libatomic ABI (GCC's libatomic, which clang follows): `order` is a __ATOMIC_* value,
// ignored here (every operation is sequentially consistent).
uint64_t __atomic_load_8(const volatile void *mem, int order);
void __atomic_store_8(volatile void *mem, uint64_t value, int order);
uint64_t __atomic_exchange_8(volatile void *mem, uint64_t value, int order);
// (GCC declares this one as a built-in with a `weak` parameter that the library ABI has not: it is
// defined under another C name and given the library's by an assembler label)
bool wl_atomic_compare_exchange_8(volatile void *mem, void *expected, uint64_t desired, int success, int failure)
    __asm__("__atomic_compare_exchange_8");
uint64_t __atomic_fetch_add_8(volatile void *mem, uint64_t value, int order);
uint64_t __atomic_fetch_sub_8(volatile void *mem, uint64_t value, int order);
uint64_t __atomic_fetch_and_8(volatile void *mem, uint64_t value, int order);
uint64_t __atomic_fetch_or_8(volatile void *mem, uint64_t value, int order);
uint64_t __atomic_fetch_xor_8(volatile void *mem, uint64_t value, int order);

uint64_t __atomic_load_8(const volatile void *mem, int order) {
    (void)order;
    u32 level = IRQ_Disable();
    uint64_t value = *(const volatile uint64_t *)mem;
    IRQ_Restore(level);
    return value;
}

void __atomic_store_8(volatile void *mem, uint64_t value, int order) {
    (void)order;
    u32 level = IRQ_Disable();
    *(volatile uint64_t *)mem = value;
    IRQ_Restore(level);
}

uint64_t __atomic_exchange_8(volatile void *mem, uint64_t value, int order) {
    (void)order;
    u32 level = IRQ_Disable();
    uint64_t old = *(volatile uint64_t *)mem;
    *(volatile uint64_t *)mem = value;
    IRQ_Restore(level);
    return old;
}

bool wl_atomic_compare_exchange_8(volatile void *mem, void *expected, uint64_t desired, int success, int failure) {
    (void)success;
    (void)failure;
    uint64_t want;
    memcpy(&want, expected, sizeof want);
    u32 level = IRQ_Disable();
    uint64_t current = *(volatile uint64_t *)mem;
    bool equal = current == want;
    if (equal) {
        *(volatile uint64_t *)mem = desired;
    }
    IRQ_Restore(level);
    if (!equal) {
        memcpy(expected, &current, sizeof current);
    }
    return equal;
}

#define WL_FETCH_OP(name, op)                                                                      \
    uint64_t __atomic_fetch_##name##_8(volatile void *mem, uint64_t value, int order) {           \
        (void)order;                                                                               \
        u32 level = IRQ_Disable();                                                                 \
        uint64_t old = *(volatile uint64_t *)mem;                                                  \
        *(volatile uint64_t *)mem = old op value;                                                  \
        IRQ_Restore(level);                                                                        \
        return old;                                                                                \
    }

WL_FETCH_OP(add, +)
WL_FETCH_OP(sub, -)
WL_FETCH_OP(and, &)
WL_FETCH_OP(or, |)
WL_FETCH_OP(xor, ^)
