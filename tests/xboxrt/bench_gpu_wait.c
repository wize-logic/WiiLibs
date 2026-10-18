// SPDX-License-Identifier: MIT
// A benchmark of a GPU wait's hand-off (thread.h's wl_xb_gpu_wait_begin and wl_xb_gpu_wait_end, as
// nv2a_adapter.c makes them around GX's waits): the guest lock given up and taken back with no other
// thread ready, the processor's cost of a wait apart from the GPU's time. Prints the waits and the
// ones that gave the lock up (all).
//
//     bench_gpu_wait [waits]
//
// Count instructions with tools/qemu_icount.py on a ppc-be build (--base with fewer waits for the
// cost of one).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "wl/xboxrt/mem.h"
#include "wl/xboxrt/thread.h"

int main(int argc, char **argv) {
    uint32_t waits = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 100000u;
    WlXbMemConfig cfg;
    wl_xb_mem_config_default(&cfg);
    WlXbThread *boot = NULL;
    if (wl_xb_mem_init(&cfg) != WL_OK || wl_xb_threads_init(NULL, &boot) != WL_OK) {
        fprintf(stderr, "bench_gpu_wait: no guest memory or threads\n");
        return 2;
    }
    uint32_t given = 0;
    for (uint32_t i = 0; i < waits; i++) {
        WlXbThread *self = wl_xb_gpu_wait_begin();
        given += self != NULL;
        wl_xb_gpu_wait_end(self);
    }
    WlXbThreadStats st;
    wl_xb_thread_stats(&st);
    printf("bench_gpu_wait: %u waits, %u gave the lock up, %u counted\n", (unsigned)waits, (unsigned)given,
           (unsigned)st.gpu_blocks);
    return given == waits && st.gpu_blocks == waits && wl_xb_lock_held() ? 0 : 1;
}
