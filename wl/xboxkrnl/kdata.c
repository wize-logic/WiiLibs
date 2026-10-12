// SPDX-License-Identifier: MIT
// Derived from xboxrecomp (https://github.com/sp00nznet/xboxrecomp), MIT, Copyright (c) 2026
// sp00nz and contributors.
// The kernel's data exports: variables a title reads through its thunk table (KeTickCount,
// XboxHardwareInfo, the object types, the disk's strings, keys). They live on two pages of kernel
// memory (window VAs); a data export's thunk slot gets the address of its variable.
//
// Values follow xboxrecomp's kernel_data_init (MIT): a retail console (hardware flags 0, NV2A A1,
// MCPX B1), three cache partitions, zero keys (a title signs its saves with them consistently), an
// empty IDE channel object (code walks its device queue's list head at +0x28).
#include "wl/xboxkrnl/kernel_internal.h"

#define KD_BYTES 0x2000u
#define KD_HW_INFO 0x000u
#define KD_KRNL_VERSION 0x010u
#define KD_TICK_COUNT 0x020u
#define KD_TIME_INCREMENT 0x024u
#define KD_LAUNCH_DATA 0x030u
#define KD_TYPES 0x040u // one 0x20-byte OBJECT_TYPE per XkObjType (and a few more)
#define KD_TYPE_BYTES 0x20u
#define KD_CACHE_PARTS 0x200u
#define KD_DISK_MODEL 0x210u
#define KD_DISK_SERIAL 0x260u
#define KD_IMAGE_NAME 0x2A0u
#define KD_HD_KEY 0x300u
#define KD_SIG_KEY 0x310u
#define KD_LAN_KEY 0x320u
#define KD_ALT_SIG_KEYS 0x330u
#define KD_PUBLIC_KEY 0x430u
#define KD_EEPROM_KEY 0x560u
#define KD_SMC_VIDEO 0x580u
#define KD_IDEX 0x600u // 0x200 bytes
#define KD_SYSTEM_TIME 0x800u
#define KD_INTERRUPT_TIME 0x810u
#define KD_MM_GLOBAL 0x820u
#define KD_DEBUGGER 0x880u
#define KD_BUGCHECK 0x890u
#define KD_HANDLE_TABLE 0x8B0u

static uint32_t g_base;

static void ansi(uint32_t at, const char *text) {
    uint32_t buf = at + 0x10u;
    uint32_t len = (uint32_t)strlen(text);
    wl_xb_copy_in(buf, text, len + 1u);
    wl_xb_wr16(at, (uint16_t)len);
    wl_xb_wr16(at + 2u, (uint16_t)(len + 1u));
    wl_xb_wr32(at + 4u, buf);
}

void xk_kdata_init(const WlXkConfig *cfg) {
    (void)cfg;
    uint32_t phys = 0;
    if (wl_xb_phys_alloc(KD_BYTES, 0, 0xFFFFFFFFu, WL_XB_PAGE, &phys) != WL_OK) {
        WL_LOG_E(XK_TAG, "no memory for the kernel's data exports");
        return;
    }
    g_base = WL_XB_WINDOW | phys;
    uint32_t b = g_base;
    wl_xb_wr32(b + KD_HW_INFO, 0);    // Flags: retail
    wl_xb_wr8(b + KD_HW_INFO + 4u, 0xA1); // GpuRevision
    wl_xb_wr8(b + KD_HW_INFO + 5u, 0xB1); // McpRevision
    wl_xb_wr16(b + KD_KRNL_VERSION + 0u, 1);
    wl_xb_wr16(b + KD_KRNL_VERSION + 2u, 0);
    wl_xb_wr16(b + KD_KRNL_VERSION + 4u, 5838);
    wl_xb_wr16(b + KD_KRNL_VERSION + 6u, 1);
    wl_xb_wr32(b + KD_TIME_INCREMENT, 10000); // 1 ms in 100 ns units: KeTickCount's step
    wl_xb_wr32(b + KD_LAUNCH_DATA, 0);        // no launch data: a cold boot
    wl_xb_wr32(b + KD_CACHE_PARTS, 3);
    ansi(b + KD_DISK_MODEL, "WIILIBS VIRTUAL DISK");
    ansi(b + KD_DISK_SERIAL, "WL0000000000");
    ansi(b + KD_IMAGE_NAME, "\\Device\\CdRom0\\default.xbe");
    wl_xb_wr32(b + KD_SMC_VIDEO, 0);
    wl_xb_wr32(b + KD_IDEX + 0x28u, b + KD_IDEX + 0x28u); // DeviceQueue.DeviceListHead: empty
    wl_xb_wr32(b + KD_IDEX + 0x2Cu, b + KD_IDEX + 0x28u);
    wl_xb_wr8(b + KD_DEBUGGER, 0);      // KdDebuggerEnabled
    wl_xb_wr8(b + KD_DEBUGGER + 1u, 1); // KdDebuggerNotPresent
    for (uint32_t t = 0; t < 12u; t++) {
        uint32_t ty = b + KD_TYPES + t * KD_TYPE_BYTES;
        wl_xb_wr32(ty, ty); // distinct and non-zero: only the address matters to a title
    }
}

uint32_t xk_kdata_tick_va(void) {
    return g_base != 0 ? g_base + KD_TICK_COUNT : 0u;
}

uint32_t xk_object_type_va(XkObjType type) {
    return g_base != 0 ? g_base + KD_TYPES + (uint32_t)type * KD_TYPE_BYTES : 0u;
}

uint32_t xk_kdata_va(uint32_t ordinal) {
    if (g_base == 0) {
        return 0;
    }
    uint32_t off;
    switch (ordinal) {
    case 16: return xk_object_type_va(XK_OBJ_EVENT);     // ExEventObjectType
    case 22: return xk_object_type_va(XK_OBJ_MUTANT);    // ExMutantObjectType
    case 30: return xk_object_type_va(XK_OBJ_SEMAPHORE); // ExSemaphoreObjectType
    case 31: return xk_object_type_va(XK_OBJ_TIMER);     // ExTimerObjectType
    case 64: off = KD_TYPES + 9u * KD_TYPE_BYTES; break;  // IoCompletionObjectType
    case 70: return xk_object_type_va(XK_OBJ_DEVICE);    // IoDeviceObjectType
    case 71: return xk_object_type_va(XK_OBJ_FILE);      // IoFileObjectType
    case 240: off = KD_TYPES + 10u * KD_TYPE_BYTES; break; // ObDirectoryObjectType
    case 249: return xk_object_type_va(XK_OBJ_SYMLINK);  // ObSymbolicLinkObjectType
    case 259: return xk_object_type_va(XK_OBJ_THREAD);   // PsThreadObjectType
    case 40: off = KD_CACHE_PARTS; break;
    case 41: off = KD_DISK_MODEL; break;
    case 42: off = KD_DISK_SERIAL; break;
    case 88: off = KD_DEBUGGER; break;
    case 89: off = KD_DEBUGGER + 1u; break;
    case 102: off = KD_MM_GLOBAL; break;
    case 120: off = KD_INTERRUPT_TIME; break;
    case 154: off = KD_SYSTEM_TIME; break;
    case 156: off = KD_TICK_COUNT; break;
    case 157: off = KD_TIME_INCREMENT; break;
    case 162: off = KD_BUGCHECK; break;
    case 164: off = KD_LAUNCH_DATA; break;
    case 245: off = KD_HANDLE_TABLE; break;
    case 321: off = KD_EEPROM_KEY; break;
    case 322: off = KD_HW_INFO; break;
    case 323: off = KD_HD_KEY; break;
    case 324: off = KD_KRNL_VERSION; break;
    case 325: off = KD_SIG_KEY; break;
    case 326: off = KD_IMAGE_NAME; break;
    case 353: off = KD_LAN_KEY; break;
    case 354: off = KD_ALT_SIG_KEYS; break;
    case 355: off = KD_PUBLIC_KEY; break;
    case 356: off = KD_SMC_VIDEO; break;
    case 357: off = KD_IDEX; break;
    default: return 0;
    }
    return g_base + off;
}
