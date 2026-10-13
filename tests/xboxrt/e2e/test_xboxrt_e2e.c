// SPDX-License-Identifier: MIT
// The end-to-end test: the synthetic title (title.c, an XBE made by mkxbe_title.py) lifted to C by
// xboxrecomp's pipeline at build time and linked here, booted by wl_xb_boot_run on the host or
// ppc-be. The title calls kernel imports, touches the NV2A's registers, makes a thread, writes a
// file and leaves through HalReturnToFirmware(2); the test then reads what it recorded in its
// `results` array from guest memory.
// Usage: test_xboxrt_e2e <folder with title.xbe>
#define _DEFAULT_SOURCE
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "greatest.h"
#include "title_syms.h"
#include "wl/platform/host/fs_host.h"
#include "wl/xboxrt/boot.h"
#include "wl/xboxrt/mem.h"

static const char *g_dir;

static uint32_t result(uint32_t i) {
    return wl_xb_rd32(E2E_RESULTS_VA + 4u * i);
}

TEST lifted_title_runs(void) {
    char title[PATH_MAX + 8], data[PATH_MAX + 8], abs_dir[PATH_MAX], tmp[256];
    ASSERT(realpath(g_dir, abs_dir) != NULL);
    snprintf(tmp, sizeof tmp, "/tmp/wl_xbrt_e2e_%d", (int)getpid());
    mkdir(tmp, 0777);
    wl_host_fs_set_root(WL_DEVICE_SD, "/");
    snprintf(title, sizeof title, "sd:/%s", abs_dir + 1);
    snprintf(data, sizeof data, "sd:/%s", tmp + 1);

    WlXbTitleConfig cfg;
    wl_xb_title_config_default(&cfg);
    cfg.title_dir = title;
    cfg.xbe_name = "title.xbe";
    cfg.data_dir = data;
    cfg.gpu_stub = true;
    int code = wl_xb_boot_run(&cfg);
    ASSERT_EQ_FMT(2, code, "%d"); // HalReturnToFirmware(2)

    ASSERT_EQ_FMT(0x58424F58u, result(0), "%08x");  // R_MAGIC: the entry point ran
    ASSERT_EQ_FMT(1u, result(1), "%u");             // NtAllocateVirtualMemory succeeded
    ASSERT_EQ_FMT(0x12345678u, result(2), "%08x");  // and its memory holds what was written
    ASSERT_EQ_FMT(0x80100000u, result(3), "%08x");  // the pinned contiguous block
    ASSERT_EQ_FMT(0x00100000u, result(4), "%08x");  // MmGetPhysicalAddress of it
    ASSERT_EQ_FMT(0xCAFEBABEu, result(5), "%08x");  // read back through the window
    ASSERT(result(6) < 1000u);                       // the PFB flush bit cleared (a device read)
    ASSERT_EQ_FMT(0x00012340u, result(7), "%08x");  // DMA_GET followed DMA_PUT (device write, read)
    ASSERT_EQ_FMT(0x02A000A1u, result(8), "%08x");  // PMC_BOOT_0
    ASSERT_EQ_FMT(1u, result(9), "%u");             // the performance counter moved forward
    ASSERT_EQ_FMT(1u, result(10), "%u");            // the thread ran
    ASSERT_EQ_FMT(0x5A5Au, result(11), "%08x");     // with its context
    ASSERT_EQ_FMT(0u, result(12), "%08x");          // the wait for its event returned STATUS_WAIT_0
    ASSERT_EQ_FMT(0u, result(13), "%08x");          // NtCreateFile succeeded
    ASSERT_EQ_FMT(19u, result(14), "%u");           // NtWriteFile wrote it all
    ASSERT_EQ_FMT(2u, result(15), "%u");            // InterlockedIncrement (fastcall), twice in all
    ASSERT_EQ_FMT(6765u, result(17), "%u");         // fib(20): direct recursive calls
    ASSERT_EQ_FMT(1u, result(18), "%u");            // R_DONE
    char path[512];
    snprintf(path, sizeof path, "%s/E/result.txt", tmp);
    FILE *f = fopen(path, "rb");
    ASSERT(f != NULL);
    char text[64] = {0};
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    ASSERT_EQ(19u, n);
    ASSERT_STR_EQ("lifted and running\n", text);
    PASS();
}

SUITE(e2e_suite) {
    RUN_TEST(lifted_title_runs);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    g_dir = getenv("WL_E2E_DIR");
    if (g_dir == NULL) {
        g_dir = ".";
    }
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(e2e_suite);
    GREATEST_MAIN_END();
}
