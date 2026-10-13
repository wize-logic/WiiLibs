// SPDX-License-Identifier: MIT
// System buttons and exit (platform.h). The reset and power callbacks run in interrupt context:
// they only latch a bit that the main loop (or the system watcher, shutdown.c) collects with
// wl_platform_system_events. Leaving winds the program down first (shutdown.c), then flushes
// and closes the log, and unmounts the SD card and USB drive, so that the files are whole.
#include <gccore.h>
#include <ogc/stm.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <wiiuse/wpad.h>

#include "wl/platform/platform.h"
#include "wl/platform/shutdown_internal.h"
#include "wl/platform/shutdown_internal.h"

static atomic_uint g_events;
static bool g_sys_init;

static void on_reset(u32 irq, void *ctx) {
    (void)irq;
    (void)ctx;
    atomic_fetch_or(&g_events, (unsigned)WL_SYS_RESET);
}

static void on_power(void) {
    atomic_fetch_or(&g_events, (unsigned)WL_SYS_POWER);
}

static void on_remote_power(s32 chan) {
    (void)chan;
    atomic_fetch_or(&g_events, (unsigned)WL_SYS_POWER);
}

void wl_platform_system_init(void) {
    if (g_sys_init) {
        return;
    }
    g_sys_init = true;
    SYS_SetResetCallback(on_reset);
    SYS_SetPowerCallback(on_power);
    WPAD_SetPowerButtonCallback(on_remote_power);
}

uint32_t wl_platform_system_events(void) {
    return atomic_exchange(&g_events, 0u);
}

// The Homebrew Channel (and the loaders that copy it) leave a reload stub at 0x80001800 with
// "STUBHAXX" after its first word; libogc's exit jumps there. Without one (Dolphin booting the DOL
// itself) there is nothing to go back to and the jump lands at 0, so leaving powers off instead.
static bool loader_stub(void) {
    return memcmp((const void *)0x80001804, "STUBHAXX", 8) == 0;
}

WL_NORETURN void wl_platform_exit(WlExitTarget target) {
    if (target == WL_EXIT_LOADER && !loader_stub()) {
        target = WL_EXIT_POWER_OFF;
    }
    wl_platform_shutdown_prepare(target);
    wl_platform_log_flush();
    wl_platform_log_shutdown(); // closes the SD file before the console goes down
    // the caches of the SD card and the USB drive written out (nothing is mounted twice)
    wl_platform_fs_unmount(WL_DEVICE_SD);
    wl_platform_fs_unmount(WL_DEVICE_USB);
    switch (target) {
    case WL_EXIT_WII_MENU: SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0); break;
    case WL_EXIT_POWER_OFF: STM_ShutdownToStandby(); break; // also ends emulation under Dolphin --batch
    case WL_EXIT_LOADER:
    default: exit(0); break; // back to the Homebrew Channel or whatever loaded us
    }
    for (;;) {
        VIDEO_WaitVSync();
    }
}
