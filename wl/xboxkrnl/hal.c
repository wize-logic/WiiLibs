// SPDX-License-Identifier: MIT
// HAL, AV, PHY, Dbg and Xe exports: the way out (HalReturnToFirmware), shutdown queries, the SMBus
// and PCI stubs, the AV encoder's answers (the cable and video standard from the configuration,
// the display mode recorded and passed to the presenter), the network PHY (no link), section
// loading (everything is loaded already).
//
// The AV option numbers and the capability word's layout (pack | standard << 8 | refresh) are the
// ones xboxrecomp's kernel_hal.c answers Direct3D with (MIT).
#include "wl/xboxkrnl/kernel_internal.h"

#define AV_OPTION_QUERY_MODE 0x01u
#define AV_OPTION_SET_MODE 0x02u
#define AV_OPTION_QUERY_AVPACK 0x06u
#define AV_OPTION_QUERY_ENCODER_TYPE 0x08u
#define AV_OPTION_QUERY_AV_CAPABILITIES 0x09u
#define AV_OPTION_QUERY_MODE_CAPS 0x0Eu
#define AV_STANDARD_NTSC_M 0x01u
#define AV_REFRESH_60HZ 0x00400000u
#define AV_FLAGS_HDTV_480I 0x00000001u
#define AV_FLAGS_HDTV_480P 0x00000002u
#define AV_FLAGS_60HZ 0x00000040u

static uint32_t g_saved_data;
static uint32_t g_display_mode;
static WlXkDisplayHook g_display_hook;

void wl_xk_set_display_hook(WlXkDisplayHook fn) {
    g_display_hook = fn;
}

// VOID HalReturnToFirmware(FIRMWARE_REENTRY Routine): HalHaltRoutine 0, HalRebootRoutine 1,
// HalQuickRebootRoutine 2, HalKdRebootRoutine 3, HalFatalErrorRebootRoutine 4
void k_HalReturnToFirmware(void) {
    uint32_t routine = ARG(0);
    static const char *const names[] = {"halt", "reboot", "quick reboot (to the dashboard or another title)",
                                        "debugger reboot", "fatal error"};
    WL_LOG_I(XK_TAG, "HalReturnToFirmware(%u: %s) from 0x%08x", (unsigned)routine,
             routine < 5u ? names[routine] : "?", (unsigned)wl_xb_kernel_caller());
    wl_xb_process_exit((int)routine);
}

void k_HalInitiateShutdown(void) {
    WL_LOG_I(XK_TAG, "HalInitiateShutdown");
    wl_xb_process_exit(0);
}

void k_HalIsResetOrShutdownPending(void) {
    RET(0);
}

// VOID HalRegisterShutdownNotification(PHAL_SHUTDOWN_REGISTRATION, BOOLEAN Register): nothing shuts
// down that way here
void k_HalRegisterShutdownNotification(void) {
}

// NTSTATUS HalReadSMBusValue(UCHAR Address, UCHAR Command, BOOLEAN ReadWord, PULONG DataValue)
void k_HalReadSMBusValue(void) {
    uint32_t addr = ARG(0) & 0xFFu, cmd = ARG(1) & 0xFFu, out = ARG(3);
    uint32_t v = 0;
    if ((addr == 0x20u || addr == 0x21u) && cmd == 0x04u) {
        // the SMC's AV pack register, in the SMC's own numbering
        static const uint8_t smc_pack[7] = {7, 6, 3, 0, 1, 2, 4}; // none std rfu scart hdtv vga svideo
        v = g_xk_cfg.av_pack < 7u ? smc_pack[g_xk_cfg.av_pack] : 7u;
    }
    if (out != 0 && wl_xb_range_ok(out, 4)) {
        wl_xb_wr32(out, v);
    }
    RET(STATUS_SUCCESS);
}

void k_HalWriteSMBusValue(void) {
    RET(STATUS_SUCCESS);
}

// VOID HalReadWritePCISpace(...): reads give zeros
void k_HalReadWritePCISpace(void) {
    uint32_t buf = ARG(3), len = ARG(4);
    if ((ARG(5) & 0xFFu) == 0 && wl_xb_range_ok(buf, len)) {
        wl_xb_fill(buf, 0, len);
    }
}

void k_HalEnableSystemInterrupt(void) {
}

void k_HalDisableSystemInterrupt(void) {
}

void k_HalReadSMCTrayState(void) {
    // tray closed, a disc in it (SMC_TRAY_STATE_MEDIA_DETECT 0x60)
    if (ARG(0) != 0 && wl_xb_range_ok(ARG(0), 4)) {
        wl_xb_wr32(ARG(0), 0x60u);
    }
    if (ARG(1) != 0 && wl_xb_range_ok(ARG(1), 4)) {
        wl_xb_wr32(ARG(1), 0);
    }
    RET(STATUS_SUCCESS);
}

void k_HalWriteSMCScratchRegister(void) {
}

void k_HalEnableSecureTrayEject(void) {
}

// ---- AV --------------------------------------------------------------------------------------------

void k_AvGetSavedDataAddress(void) {
    RET(g_saved_data);
}

void k_AvSetSavedDataAddress(void) {
    g_saved_data = ARG(0);
}

// VOID AvSendTVEncoderOption(PVOID RegisterBase, ULONG Option, ULONG Param, PULONG Result)
void k_AvSendTVEncoderOption(void) {
    uint32_t option = ARG(1), param = ARG(2), result = ARG(3);
    uint32_t v = 0;
    switch (option) {
    case AV_OPTION_QUERY_AVPACK:
        v = (g_xk_cfg.av_pack & 0xFFu) | (AV_STANDARD_NTSC_M << 8) | AV_REFRESH_60HZ;
        break;
    case AV_OPTION_QUERY_MODE:
        v = g_display_mode;
        break;
    case AV_OPTION_SET_MODE:
        g_display_mode = param;
        break;
    case AV_OPTION_QUERY_AV_CAPABILITIES:
    case AV_OPTION_QUERY_MODE_CAPS:
        v = AV_FLAGS_HDTV_480I | AV_FLAGS_60HZ | (g_xk_cfg.av_pack == 4u ? AV_FLAGS_HDTV_480P : 0u);
        break;
    case AV_OPTION_QUERY_ENCODER_TYPE:
        v = 4; // a Conexant encoder, as most retail consoles have
        break;
    default:
        WL_LOG_D(XK_TAG, "AvSendTVEncoderOption(0x%x, 0x%x): 0", (unsigned)option, (unsigned)param);
        break;
    }
    if (result != 0 && wl_xb_range_ok(result, 4)) {
        wl_xb_wr32(result, v);
    }
}

// ULONG AvSetDisplayMode(PVOID RegisterBase, ULONG Step, ULONG Mode, ULONG Format, ULONG Pitch,
//                        ULONG FrameBuffer)
void k_AvSetDisplayMode(void) {
    uint32_t mode = ARG(2), format = ARG(3), pitch = ARG(4), fb = ARG(5);
    g_display_mode = mode;
    WL_LOG_I(XK_TAG, "AvSetDisplayMode step %u mode 0x%08x format 0x%x pitch %u framebuffer 0x%08x", (unsigned)ARG(1),
             (unsigned)mode, (unsigned)format, (unsigned)pitch, (unsigned)fb);
    if (g_display_hook != NULL) {
        g_display_hook(mode, format, pitch, fb & 0x0FFFFFFFu);
    }
    RET(0);
}

// ---- PHY (the network) --------------------------------------------------------------------------

// NTSTATUS PhyInitialize(BOOLEAN ForceReset, PVOID Param): the PHY answers, no cable
void k_PhyInitialize(void) {
    RET(STATUS_SUCCESS);
}

// DWORD PhyGetLinkState(BOOLEAN Update): 0, no link
void k_PhyGetLinkState(void) {
    RET(0);
}

// ---- Debugger ------------------------------------------------------------------------------------

void k_DbgBreakPoint(void) {
    WL_LOG_W(XK_TAG, "DbgBreakPoint from 0x%08x", (unsigned)wl_xb_kernel_caller());
}

void k_DbgBreakPointWithStatus(void) {
    WL_LOG_W(XK_TAG, "DbgBreakPointWithStatus(0x%x) from 0x%08x", (unsigned)ARG(0), (unsigned)wl_xb_kernel_caller());
}

// ---- Xe: sections ---------------------------------------------------------------------------------
// The XBE section header the title passes: its reference count at +24. Every section is resident.

void k_XeLoadSection(void) {
    uint32_t sec = ARG(0);
    if (!wl_xb_range_ok(sec, 28)) {
        RET(STATUS_INVALID_PARAMETER);
        return;
    }
    wl_xb_wr32(sec + 24u, wl_xb_rd32(sec + 24u) + 1u);
    RET(STATUS_SUCCESS);
}

void k_XeUnloadSection(void) {
    uint32_t sec = ARG(0);
    if (!wl_xb_range_ok(sec, 28)) {
        RET(STATUS_INVALID_PARAMETER);
        return;
    }
    uint32_t refs = wl_xb_rd32(sec + 24u);
    if (refs > 0) {
        wl_xb_wr32(sec + 24u, refs - 1u);
    }
    RET(STATUS_SUCCESS);
}

void xk_link_hal(void) {
}
