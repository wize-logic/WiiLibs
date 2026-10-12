// SPDX-License-Identifier: MIT
// A stand-in for the parts of libogc's <wiiuse/wpad.h> and <wiiuse/wiiuse.h> that input_wii.c
// uses (test only): the constants with libogc's values, the data structures it reads and the
// calls, which answer from what the test set up (input_stub.h).
#pragma once

#include <stdint.h>

#ifndef OGC_STUB_TYPES
#define OGC_STUB_TYPES
typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef int32_t s32;
#endif

#define WPAD_CHAN_ALL -1
#define WPAD_MAX_WIIMOTES 5

#define WPAD_BUTTON_2 0x0001
#define WPAD_BUTTON_1 0x0002
#define WPAD_BUTTON_B 0x0004
#define WPAD_BUTTON_A 0x0008
#define WPAD_BUTTON_MINUS 0x0010
#define WPAD_BUTTON_HOME 0x0080
#define WPAD_BUTTON_LEFT 0x0100
#define WPAD_BUTTON_RIGHT 0x0200
#define WPAD_BUTTON_DOWN 0x0400
#define WPAD_BUTTON_UP 0x0800
#define WPAD_BUTTON_PLUS 0x1000

#define WPAD_NUNCHUK_BUTTON_Z (0x0001 << 16)
#define WPAD_NUNCHUK_BUTTON_C (0x0002 << 16)

#define WPAD_CLASSIC_BUTTON_UP (0x0001u << 16)
#define WPAD_CLASSIC_BUTTON_LEFT (0x0002u << 16)
#define WPAD_CLASSIC_BUTTON_ZR (0x0004u << 16)
#define WPAD_CLASSIC_BUTTON_X (0x0008u << 16)
#define WPAD_CLASSIC_BUTTON_A (0x0010u << 16)
#define WPAD_CLASSIC_BUTTON_Y (0x0020u << 16)
#define WPAD_CLASSIC_BUTTON_B (0x0040u << 16)
#define WPAD_CLASSIC_BUTTON_ZL (0x0080u << 16)
#define WPAD_CLASSIC_BUTTON_FULL_R (0x0200u << 16)
#define WPAD_CLASSIC_BUTTON_PLUS (0x0400u << 16)
#define WPAD_CLASSIC_BUTTON_HOME (0x0800u << 16)
#define WPAD_CLASSIC_BUTTON_MINUS (0x1000u << 16)
#define WPAD_CLASSIC_BUTTON_FULL_L (0x2000u << 16)
#define WPAD_CLASSIC_BUTTON_DOWN (0x4000u << 16)
#define WPAD_CLASSIC_BUTTON_RIGHT (0x8000u << 16)

enum { WPAD_EXP_NONE = 0, WPAD_EXP_NUNCHUK, WPAD_EXP_CLASSIC, WPAD_EXP_GUITARHERO3, WPAD_EXP_WIIBOARD, WPAD_EXP_UNKNOWN = 255 };
enum { WPAD_FMT_BTNS = 0, WPAD_FMT_BTNS_ACC, WPAD_FMT_BTNS_ACC_IR };

#define WPAD_ERR_NONE 0
#define WPAD_ERR_NO_CONTROLLER -1
#define WPAD_ERR_NOT_READY -2
#define WPAD_ERR_TRANSFER -3

struct joystick_t {
    float ang; // degrees from +y, clockwise
    float mag; // 0..1
};
struct nunchuk_t {
    struct joystick_t js;
};
struct classic_ctrl_t {
    uint8_t rs_raw;
    uint8_t ls_raw;
    struct joystick_t ljs;
    struct joystick_t rjs;
};
struct gforce_t {
    float x, y, z;
};
struct ir_t {
    int raw_valid;
    float ax, ay;
    int valid;
    float x, y;
};
struct expansion_t {
    int type;
    union {
        struct nunchuk_t nunchuk;
        struct classic_ctrl_t classic;
    };
};

typedef struct _wpad_data {
    s16 err;
    u32 data_present;
    u8 battery_level;
    u32 btns_h;
    struct ir_t ir;
    struct gforce_t gforce;
    struct expansion_t exp;
} WPADData;

s32 WPAD_Init(void);
s32 WPAD_SetDataFormat(s32 chan, s32 fmt);
s32 WPAD_SetVRes(s32 chan, u32 xres, u32 yres);
s32 WPAD_Probe(s32 chan, u32 *type);
s32 WPAD_ScanPads(void);
s32 WPAD_Rumble(s32 chan, int status);
WPADData *WPAD_Data(int chan);
void WPAD_GForce(int chan, struct gforce_t *gforce);
