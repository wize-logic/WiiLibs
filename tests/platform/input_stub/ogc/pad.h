// SPDX-License-Identifier: MIT
// A stand-in for the parts of libogc's <ogc/pad.h> that wl/platform/wii/input_wii.c uses (test
// only): the constants with libogc's values and the calls, which read what the test set up
// (input_stub.h). input_wii.c's static asserts compare platform.h against these values; the Wii
// build compares them against the real header.
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

#define PAD_CHAN0 0
#define PAD_CHANMAX 4

#define PAD_MOTOR_STOP 0
#define PAD_MOTOR_RUMBLE 1

#define PAD_ERR_NONE 0
#define PAD_ERR_NO_CONTROLLER -1
#define PAD_ERR_NOT_READY -2
#define PAD_ERR_TRANSFER -3

#define PAD_BUTTON_LEFT 0x0001
#define PAD_BUTTON_RIGHT 0x0002
#define PAD_BUTTON_DOWN 0x0004
#define PAD_BUTTON_UP 0x0008
#define PAD_TRIGGER_Z 0x0010
#define PAD_TRIGGER_R 0x0020
#define PAD_TRIGGER_L 0x0040
#define PAD_BUTTON_A 0x0100
#define PAD_BUTTON_B 0x0200
#define PAD_BUTTON_X 0x0400
#define PAD_BUTTON_Y 0x0800
#define PAD_BUTTON_START 0x1000

typedef struct _padstatus {
    u16 button;
    s8 stickX;
    s8 stickY;
    s8 substickX;
    s8 substickY;
    u8 triggerL;
    u8 triggerR;
    u8 analogA;
    u8 analogB;
    s8 err;
} PADStatus;

u32 PAD_Init(void);
u32 PAD_Read(PADStatus *status);
void PAD_ControlMotor(s32 chan, u32 cmd);
