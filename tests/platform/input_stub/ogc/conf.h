// SPDX-License-Identifier: MIT
// A stand-in for the parts of libogc's <ogc/conf.h> that wl/platform/wii/input_wii.c uses (test
// only): the two calls, answering from g_input_stub (input_stub.h).
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

s32 CONF_Init(void);
s32 CONF_GetPadMotorMode(void);
