// SPDX-License-Identifier: MIT
// Derived from xboxrecomp (https://github.com/sp00nznet/xboxrecomp), MIT, Copyright (c) 2026
// sp00nz and contributors.
// Ex exports: the EEPROM's settings (ExQueryNonVolatileSetting) from the configuration, saves kept
// for the process, and the interlocked list helpers (fastcall).
//
// Setting indices and the user section's layout (XC_*, XBOX_USER_SETTINGS) are the console's as the
// public kernel headers give them; the defaults (English, NTSC-M 60 Hz, North America, no parental
// limit: XAPI quits when the title's rating is above the limit) follow xboxrecomp's kernel_xbox.c.
#include "wl/base/endian.h"
#include "wl/xboxkrnl/kernel_internal.h"

#define REG_SZ 1u
#define REG_BINARY 3u
#define REG_DWORD 4u

#define XC_TIMEZONE_BIAS 0u
#define XC_LANGUAGE 7u
#define XC_VIDEO_FLAGS 8u
#define XC_AUDIO_FLAGS 9u
#define XC_PARENTAL_CONTROL_GAMES 0xAu
#define XC_PARENTAL_CONTROL_PASSWORD 0xBu
#define XC_PARENTAL_CONTROL_MOVIES 0xCu
#define XC_MISC_FLAGS 0x11u
#define XC_DVD_REGION 0x12u
#define XC_MAX_OS 0xFFu
#define XC_FACTORY_SERIAL_NUMBER 0x100u
#define XC_FACTORY_ETHERNET_ADDR 0x101u
#define XC_FACTORY_ONLINE_KEY 0x102u
#define XC_FACTORY_AV_REGION 0x103u
#define XC_FACTORY_GAME_REGION 0x104u
#define XC_ENCRYPTED_SECTION 0xFFFEu
#define XC_MAX_ALL 0xFFFFu

#define USER_SECTION 0x60u // XBOX_USER_SETTINGS

static struct {
    uint32_t index, value;
    bool used;
} g_saved[16];

static bool saved_value(uint32_t index, uint32_t *v) {
    for (uint32_t i = 0; i < WL_ARRAY_COUNT(g_saved); i++) {
        if (g_saved[i].used && g_saved[i].index == index) {
            *v = g_saved[i].value;
            return true;
        }
    }
    return false;
}

static uint32_t dword_setting(uint32_t index, bool *known) {
    uint32_t v = 0;
    *known = true;
    if (saved_value(index, &v)) {
        return v;
    }
    switch (index) {
    case XC_TIMEZONE_BIAS: return (uint32_t)g_xk_cfg.timezone_bias;
    case XC_LANGUAGE: return g_xk_cfg.language;
    case XC_VIDEO_FLAGS: return g_xk_cfg.video_flags;
    case XC_AUDIO_FLAGS: return g_xk_cfg.audio_flags;
    case XC_PARENTAL_CONTROL_GAMES: return g_xk_cfg.parental_games;
    case XC_PARENTAL_CONTROL_PASSWORD:
    case XC_PARENTAL_CONTROL_MOVIES:
    case XC_MISC_FLAGS: return 0;
    case XC_DVD_REGION: return g_xk_cfg.dvd_region;
    case XC_FACTORY_AV_REGION: return g_xk_cfg.av_region;
    case XC_FACTORY_GAME_REGION: return g_xk_cfg.game_region;
    default:
        *known = false;
        return 0;
    }
}

// Writes the user section (XBOX_USER_SETTINGS, 0x60 bytes) into `buf`.
static void user_section(uint8_t *buf) {
    memset(buf, 0, USER_SECTION);
    bool k;
    wl_wr_le32(buf + 0x04, dword_setting(XC_TIMEZONE_BIAS, &k));
    wl_wr_le32(buf + 0x30, dword_setting(XC_LANGUAGE, &k));
    wl_wr_le32(buf + 0x34, dword_setting(XC_VIDEO_FLAGS, &k));
    wl_wr_le32(buf + 0x38, dword_setting(XC_AUDIO_FLAGS, &k));
    wl_wr_le32(buf + 0x3C, dword_setting(XC_PARENTAL_CONTROL_GAMES, &k));
    wl_wr_le32(buf + 0x58, dword_setting(XC_MISC_FLAGS, &k));
    wl_wr_le32(buf + 0x5C, dword_setting(XC_DVD_REGION, &k));
}

// NTSTATUS ExQueryNonVolatileSetting(DWORD ValueIndex, DWORD *Type, PVOID Value, SIZE_T ValueLength,
//                                    PSIZE_T ResultLength)
void k_ExQueryNonVolatileSetting(void) {
    uint32_t index = ARG(0), type_p = ARG(1), value = ARG(2), len = ARG(3), result_p = ARG(4);
    uint8_t bytes[USER_SECTION];
    uint32_t type = REG_DWORD, size = 4;
    bool known = true;
    if (index == XC_MAX_OS || index == XC_MAX_ALL) {
        user_section(bytes);
        type = REG_BINARY;
        size = USER_SECTION;
    } else if (index == XC_FACTORY_SERIAL_NUMBER) {
        memcpy(bytes, "000000000000", 12);
        type = REG_BINARY;
        size = 12;
    } else if (index == XC_FACTORY_ETHERNET_ADDR) {
        static const uint8_t mac[6] = {0x00, 0x50, 0xF2, 0x00, 0x00, 0x01};
        memcpy(bytes, mac, 6);
        type = REG_BINARY;
        size = 6;
    } else if (index == XC_FACTORY_ONLINE_KEY || index == XC_ENCRYPTED_SECTION) {
        memset(bytes, 0, 16);
        type = REG_BINARY;
        size = 16;
    } else {
        wl_wr_le32(bytes, dword_setting(index, &known));
    }
    if (!known) {
        WL_LOG_W(XK_TAG, "ExQueryNonVolatileSetting(0x%x): unknown setting, 0", (unsigned)index);
    }
    if (type_p != 0 && wl_xb_range_ok(type_p, 4)) {
        wl_xb_wr32(type_p, type);
    }
    if (result_p != 0 && wl_xb_range_ok(result_p, 4)) {
        wl_xb_wr32(result_p, size);
    }
    if (len < size || !wl_xb_range_ok(value, size)) {
        RET(STATUS_BUFFER_TOO_SMALL);
        return;
    }
    wl_xb_copy_in(value, bytes, size);
    RET(STATUS_SUCCESS);
}

// NTSTATUS ExSaveNonVolatileSetting(DWORD ValueIndex, DWORD Type, PVOID Value, SIZE_T ValueLength):
// kept for the process's life (a DWORD setting), not written anywhere
void k_ExSaveNonVolatileSetting(void) {
    uint32_t index = ARG(0), value = ARG(2), len = ARG(3);
    if (len >= 4 && wl_xb_range_ok(value, 4)) {
        uint32_t v = wl_xb_rd32(value);
        for (uint32_t i = 0; i < WL_ARRAY_COUNT(g_saved); i++) {
            if (!g_saved[i].used || g_saved[i].index == index) {
                g_saved[i].used = true;
                g_saved[i].index = index;
                g_saved[i].value = v;
                break;
            }
        }
    }
    RET(STATUS_SUCCESS);
}

// ---- Interlocked lists (fastcall: ecx = ListHead, edx = ListEntry) -------------------------------
// LIST_ENTRY: Flink, Blink

void k_ExfInterlockedInsertHeadList(void) {
    uint32_t head = g_ecx, entry = g_edx;
    uint32_t first = wl_xb_rd32(head);
    wl_xb_wr32(entry, first);
    wl_xb_wr32(entry + 4u, head);
    wl_xb_wr32(first + 4u, entry);
    wl_xb_wr32(head, entry);
    RET(first == head ? 0u : first);
}

void k_ExfInterlockedInsertTailList(void) {
    uint32_t head = g_ecx, entry = g_edx;
    uint32_t last = wl_xb_rd32(head + 4u);
    wl_xb_wr32(entry, head);
    wl_xb_wr32(entry + 4u, last);
    wl_xb_wr32(last, entry);
    wl_xb_wr32(head + 4u, entry);
    RET(last == head ? 0u : last);
}

void k_ExfInterlockedRemoveHeadList(void) {
    uint32_t head = g_ecx;
    uint32_t first = wl_xb_rd32(head);
    if (first == head) {
        RET(0);
        return;
    }
    uint32_t next = wl_xb_rd32(first);
    wl_xb_wr32(head, next);
    wl_xb_wr32(next + 4u, head);
    RET(first);
}

void xk_link_ex(void) {
}
