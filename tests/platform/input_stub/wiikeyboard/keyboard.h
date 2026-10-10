// SPDX-License-Identifier: MIT
// A stand-in for the parts of libogc's <wiikeyboard/keyboard.h> that input_wii.c uses (test
// only). The stub keyboard stack never starts, so input_wii.c reports no keyboard.
#pragma once

#include <stdint.h>

#define MOD_SHIFT_L (1 << 0)
#define MOD_SHIFT_R (1 << 1)
#define MOD_SHIFTLOCK (1 << 2)
#define MOD_CONTROL_L (1 << 3)
#define MOD_CONTROL_R (1 << 4)
#define MOD_META_L (1 << 6)
#define MOD_META_R (1 << 7)
#define MOD_ANYSHIFT (MOD_SHIFT_L | MOD_SHIFT_R | MOD_SHIFTLOCK)
#define MOD_ANYCONTROL (MOD_CONTROL_L | MOD_CONTROL_R)
#define MOD_ANYMETA (MOD_META_L | MOD_META_R)
#define MOD_ONESET(val, mask) (((val) & (mask)) != 0)

typedef enum { KEYBOARD_CONNECTED, KEYBOARD_DISCONNECTED, KEYBOARD_PRESSED, KEYBOARD_RELEASED } keyboard_event_type;

typedef struct {
    keyboard_event_type type;
    uint16_t modifiers;
    uint8_t keycode;
    uint16_t symbol;
} keyboard_event;

typedef void (*keyPressCallback)(char symbol);

int32_t KEYBOARD_Init(keyPressCallback keypress_cb);
int32_t KEYBOARD_Deinit(void);
int32_t KEYBOARD_GetEvent(keyboard_event *event);
