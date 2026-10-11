// SPDX-License-Identifier: MIT
// Stack depth measurement (platform.h): a pattern written before a thread runs, and the run of it
// still untouched at the stack's low end. The thread backends run a function on such a stack
// (wl_platform_run_on_stack).
#include <string.h>

#include "wl/platform/platform.h"

void wl_platform_stack_fill(void *stack, size_t size) {
    if (stack != NULL) {
        memset(stack, (int)WL_STACK_FILL_BYTE, size);
    }
}

size_t wl_platform_stack_used(const void *stack, size_t size) {
    if (stack == NULL) {
        return 0;
    }
    const volatile uint8_t *p = stack; // another thread may be running on it
    size_t untouched = 0;
    while (untouched < size && p[untouched] == WL_STACK_FILL_BYTE) {
        untouched++;
    }
    return size - untouched;
}
