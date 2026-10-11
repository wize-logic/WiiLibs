// SPDX-License-Identifier: MIT
// System buttons and exit (platform.h). The reset and power callbacks run in interrupt context:
// they only latch a bit that the main loop collects with wl_platform_system_events. The log is
// flushed before leaving so the SD file is whole.
#include <gccore.h>
#include <ogc/stm.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <wiiuse/wpad.h>

#include "wl/platform/platform.h"

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

WL_NORETURN void wl_platform_exit(WlExitTarget target) {
    wl_platform_log_flush();
    wl_platform_log_shutdown(); // closes the SD file before the console goes down
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
