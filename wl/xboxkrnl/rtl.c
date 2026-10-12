// SPDX-License-Identifier: MIT
// Derived from xboxrecomp (https://github.com/sp00nznet/xboxrecomp), MIT, Copyright (c) 2026
// sp00nz and contributors.
// Rtl exports: critical sections over the dispatcher, ANSI/Unicode strings, memory helpers, integer
// conversions, time fields, NTSTATUS to Win32 errors, SEH unwinding of the fs:[0] chain, the printf
// family over guest varargs, DbgPrint.
//
// The status-to-error table and the critical section's layout follow xboxrecomp's kernel_bridge.c
// and kernel_rtl.c (MIT) in substance; the code is written here over the guest accessors.
#include <stdio.h>

#include "wl/xboxkrnl/kernel_internal.h"

// RTL_CRITICAL_SECTION: a synchronization event's header, LockCount, RecursionCount, OwningThread
#define CS_LOCK 0x10u
#define CS_RECURSION 0x14u
#define CS_OWNER 0x18u

static uint32_t self_thread(void) {
    WlXbThread *t = wl_xb_current();
    return t != NULL ? t->kthread : 0u;
}

void k_RtlInitializeCriticalSection(void) {
    uint32_t cs = ARG(0);
    if (wl_xb_range_ok(cs, 0x1Cu)) {
        xk_header_init(cs, OBJ_EVENT_SYNCHRONIZATION, 4, 0);
        wl_xb_wr32(cs + CS_LOCK, 0xFFFFFFFFu);
        wl_xb_wr32(cs + CS_RECURSION, 0);
        wl_xb_wr32(cs + CS_OWNER, 0);
    }
}

void k_RtlEnterCriticalSection(void) {
    uint32_t cs = ARG(0), me = self_thread();
    if (!wl_xb_range_ok(cs, 0x1Cu)) {
        return;
    }
    uint32_t lock = wl_xb_rd32(cs + CS_LOCK) + 1u;
    wl_xb_wr32(cs + CS_LOCK, lock);
    if (lock != 0) {
        if (wl_xb_rd32(cs + CS_OWNER) == me) {
            wl_xb_wr32(cs + CS_RECURSION, wl_xb_rd32(cs + CS_RECURSION) + 1u);
            return;
        }
        xk_wait(1, &cs, false, false, false, NULL); // the owner sets the event on its way out
    }
    wl_xb_wr32(cs + CS_OWNER, me);
    wl_xb_wr32(cs + CS_RECURSION, 1);
}

void k_RtlLeaveCriticalSection(void) {
    uint32_t cs = ARG(0);
    if (!wl_xb_range_ok(cs, 0x1Cu)) {
        return;
    }
    uint32_t rec = wl_xb_rd32(cs + CS_RECURSION) - 1u;
    wl_xb_wr32(cs + CS_RECURSION, rec);
    uint32_t lock = wl_xb_rd32(cs + CS_LOCK) - 1u;
    if (rec != 0) {
        wl_xb_wr32(cs + CS_LOCK, lock);
        return;
    }
    wl_xb_wr32(cs + CS_OWNER, 0);
    wl_xb_wr32(cs + CS_LOCK, lock);
    if ((int32_t)lock >= 0) {
        xk_event_set(cs); // someone waits
    }
}

void k_RtlTryEnterCriticalSection(void) {
    uint32_t cs = ARG(0), me = self_thread();
    if (!wl_xb_range_ok(cs, 0x1Cu)) {
        RET(0);
        return;
    }
    uint32_t lock = wl_xb_rd32(cs + CS_LOCK);
    if (lock == 0xFFFFFFFFu) {
        wl_xb_wr32(cs + CS_LOCK, 0);
        wl_xb_wr32(cs + CS_OWNER, me);
        wl_xb_wr32(cs + CS_RECURSION, 1);
        RET(1);
    } else if (wl_xb_rd32(cs + CS_OWNER) == me) {
        wl_xb_wr32(cs + CS_LOCK, lock + 1u);
        wl_xb_wr32(cs + CS_RECURSION, wl_xb_rd32(cs + CS_RECURSION) + 1u);
        RET(1);
    } else {
        RET(0);
    }
}

void k_RtlEnterCriticalSectionAndRegion(void) {
    k_RtlEnterCriticalSection();
}

void k_RtlLeaveCriticalSectionAndRegion(void) {
    k_RtlLeaveCriticalSection();
}

// ---- Strings -------------------------------------------------------------------------------------
// ANSI_STRING / OBJECT_STRING and UNICODE_STRING: Length (USHORT, bytes), MaximumLength, Buffer

static uint32_t guest_strlen(uint32_t va, uint32_t max) {
    uint32_t n = 0;
    while (n < max && wl_xb_range_ok(va + n, 1) && wl_xb_rd8(va + n) != 0) {
        n++;
    }
    return n;
}

void k_RtlInitAnsiString(void) {
    uint32_t dst = ARG(0), src = ARG(1);
    if (!wl_xb_range_ok(dst, 8)) {
        return;
    }
    uint32_t len = src != 0 ? guest_strlen(src, 0xFFFEu) : 0;
    wl_xb_wr16(dst, (uint16_t)len);
    wl_xb_wr16(dst + 2u, (uint16_t)(src != 0 ? len + 1u : 0u));
    wl_xb_wr32(dst + 4u, src);
}

void k_RtlInitUnicodeString(void) {
    uint32_t dst = ARG(0), src = ARG(1);
    if (!wl_xb_range_ok(dst, 8)) {
        return;
    }
    uint32_t n = 0;
    while (src != 0 && n < 0x7FFEu && wl_xb_range_ok(src + 2u * n, 2) && wl_xb_rd16(src + 2u * n) != 0) {
        n++;
    }
    wl_xb_wr16(dst, (uint16_t)(2u * n));
    wl_xb_wr16(dst + 2u, (uint16_t)(src != 0 ? 2u * n + 2u : 0u));
    wl_xb_wr32(dst + 4u, src);
}

static uint8_t upper(uint8_t c) {
    return (c >= 'a' && c <= 'z') ? (uint8_t)(c - 32u) : c;
}

static int32_t compare_strings(uint32_t a, uint32_t b, bool ci) {
    uint32_t la = wl_xb_rd16(a), lb = wl_xb_rd16(b), ba = wl_xb_rd32(a + 4u), bb = wl_xb_rd32(b + 4u);
    uint32_t n = la < lb ? la : lb;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t x = wl_xb_rd8(ba + i), y = wl_xb_rd8(bb + i);
        if (ci) {
            x = upper(x);
            y = upper(y);
        }
        if (x != y) {
            return (int32_t)x - (int32_t)y;
        }
    }
    return (int32_t)la - (int32_t)lb;
}

void k_RtlEqualString(void) {
    uint32_t a = ARG(0), b = ARG(1);
    if (!wl_xb_range_ok(a, 8) || !wl_xb_range_ok(b, 8)) {
        RET(0);
        return;
    }
    RET(wl_xb_rd16(a) == wl_xb_rd16(b) && compare_strings(a, b, (ARG(2) & 0xFFu) != 0) == 0 ? 1u : 0u);
}

void k_RtlCompareString(void) {
    uint32_t a = ARG(0), b = ARG(1);
    RET(wl_xb_range_ok(a, 8) && wl_xb_range_ok(b, 8) ? (uint32_t)compare_strings(a, b, (ARG(2) & 0xFFu) != 0) : 0u);
}

void k_RtlCopyString(void) {
    uint32_t dst = ARG(0), src = ARG(1);
    if (!wl_xb_range_ok(dst, 8)) {
        return;
    }
    if (src == 0) {
        wl_xb_wr16(dst, 0);
        return;
    }
    uint32_t n = wl_xb_rd16(src), max = wl_xb_rd16(dst + 2u);
    if (n > max) {
        n = max;
    }
    for (uint32_t i = 0; i < n; i++) {
        wl_xb_wr8(wl_xb_rd32(dst + 4u) + i, wl_xb_rd8(wl_xb_rd32(src + 4u) + i));
    }
    wl_xb_wr16(dst, (uint16_t)n);
}

// NTSTATUS RtlUnicodeStringToAnsiString(PSTRING Dest, PUNICODE_STRING Source, BOOLEAN Allocate)
void k_RtlUnicodeStringToAnsiString(void) {
    uint32_t dst = ARG(0), src = ARG(1);
    bool alloc = (ARG(2) & 0xFFu) != 0;
    if (!wl_xb_range_ok(dst, 8) || !wl_xb_range_ok(src, 8)) {
        RET(STATUS_INVALID_PARAMETER);
        return;
    }
    uint32_t chars = wl_xb_rd16(src) / 2u, from = wl_xb_rd32(src + 4u);
    uint32_t buf = wl_xb_rd32(dst + 4u), max = wl_xb_rd16(dst + 2u);
    uint32_t status = STATUS_SUCCESS;
    if (alloc) {
        max = chars + 1u;
        buf = xk_pool_alloc(max);
        if (buf == 0) {
            RET(STATUS_NO_MEMORY);
            return;
        }
        wl_xb_wr32(dst + 4u, buf);
        wl_xb_wr16(dst + 2u, (uint16_t)max);
    } else if (chars >= max) {
        status = STATUS_BUFFER_OVERFLOW;
        chars = max > 0 ? max - 1u : 0u;
    }
    for (uint32_t i = 0; i < chars; i++) {
        uint16_t c = wl_xb_rd16(from + 2u * i);
        wl_xb_wr8(buf + i, (uint8_t)(c < 256u ? c : '?'));
    }
    if (chars < max) {
        wl_xb_wr8(buf + chars, 0);
    }
    wl_xb_wr16(dst, (uint16_t)chars);
    RET(status);
}

// NTSTATUS RtlAnsiStringToUnicodeString(PUNICODE_STRING Dest, PSTRING Source, BOOLEAN Allocate)
void k_RtlAnsiStringToUnicodeString(void) {
    uint32_t dst = ARG(0), src = ARG(1);
    bool alloc = (ARG(2) & 0xFFu) != 0;
    if (!wl_xb_range_ok(dst, 8) || !wl_xb_range_ok(src, 8)) {
        RET(STATUS_INVALID_PARAMETER);
        return;
    }
    uint32_t chars = wl_xb_rd16(src), from = wl_xb_rd32(src + 4u);
    uint32_t buf = wl_xb_rd32(dst + 4u), max = wl_xb_rd16(dst + 2u);
    uint32_t status = STATUS_SUCCESS;
    if (alloc) {
        max = 2u * chars + 2u;
        buf = xk_pool_alloc(max);
        if (buf == 0) {
            RET(STATUS_NO_MEMORY);
            return;
        }
        wl_xb_wr32(dst + 4u, buf);
        wl_xb_wr16(dst + 2u, (uint16_t)max);
    } else if (2u * chars >= max) {
        status = STATUS_BUFFER_OVERFLOW;
        chars = max >= 2u ? max / 2u - 1u : 0u;
    }
    for (uint32_t i = 0; i < chars; i++) {
        wl_xb_wr16(buf + 2u * i, wl_xb_rd8(from + i));
    }
    if (2u * chars + 2u <= max) {
        wl_xb_wr16(buf + 2u * chars, 0);
    }
    wl_xb_wr16(dst, (uint16_t)(2u * chars));
    RET(status);
}

void k_RtlFreeAnsiString(void) {
    uint32_t s = ARG(0);
    if (wl_xb_range_ok(s, 8) && wl_xb_rd32(s + 4u) != 0) {
        xk_pool_free(wl_xb_rd32(s + 4u));
        wl_xb_wr32(s + 4u, 0);
        wl_xb_wr16(s, 0);
        wl_xb_wr16(s + 2u, 0);
    }
}

void k_RtlFreeUnicodeString(void) {
    k_RtlFreeAnsiString();
}

void k_RtlUpperChar(void) {
    RET(upper((uint8_t)ARG(0)));
}

void k_RtlLowerChar(void) {
    uint8_t c = (uint8_t)ARG(0);
    RET((c >= 'A' && c <= 'Z') ? (uint8_t)(c + 32u) : c);
}

void k_RtlUpcaseUnicodeChar(void) {
    uint16_t c = (uint16_t)ARG(0);
    RET((c >= 'a' && c <= 'z') ? (uint16_t)(c - 32u) : c);
}

void k_RtlDowncaseUnicodeChar(void) {
    uint16_t c = (uint16_t)ARG(0);
    RET((c >= 'A' && c <= 'Z') ? (uint16_t)(c + 32u) : c);
}

// ---- Memory ----------------------------------------------------------------------------------------

void k_RtlCompareMemory(void) {
    uint32_t a = ARG(0), b = ARG(1), n = ARG(2), i = 0;
    if (wl_xb_range_ok(a, n) && wl_xb_range_ok(b, n)) {
        while (i < n && wl_xb_rd8(a + i) == wl_xb_rd8(b + i)) {
            i++;
        }
    }
    RET(i);
}

// SIZE_T RtlCompareMemoryUlong(PVOID Source, SIZE_T Length, ULONG Pattern): the bytes from the
// start that match the pattern repeated (whole ULONGs only)
void k_RtlCompareMemoryUlong(void) {
    uint32_t src = ARG(0), n = ARG(1) & ~3u, pattern = ARG(2), i = 0;
    if (wl_xb_range_ok(src, n)) {
        while (i < n && wl_xb_rd32(src + i) == pattern) {
            i += 4u;
        }
    }
    RET(i);
}

void k_RtlZeroMemory(void) {
    if (wl_xb_range_ok(ARG(0), ARG(1))) {
        wl_xb_fill(ARG(0), 0, ARG(1));
    }
}

void k_RtlFillMemory(void) {
    if (wl_xb_range_ok(ARG(0), ARG(1))) {
        wl_xb_fill(ARG(0), (uint8_t)ARG(2), ARG(1));
    }
}

void k_RtlFillMemoryUlong(void) {
    uint32_t dst = ARG(0), n = ARG(1) & ~3u, v = ARG(2);
    if (wl_xb_range_ok(dst, n)) {
        for (uint32_t i = 0; i < n; i += 4u) {
            wl_xb_wr32(dst + i, v);
        }
    }
}

void k_RtlMoveMemory(void) {
    uint32_t dst = ARG(0), src = ARG(1), n = ARG(2);
    if (wl_xb_range_ok(dst, n) && wl_xb_range_ok(src, n) && n > 0) {
        memmove(wl_xb_ptr(dst), wl_xb_ptr(src), n);
    }
}

// ---- Integers --------------------------------------------------------------------------------------

// NTSTATUS RtlCharToInteger(PCSZ String, ULONG Base, PULONG Value)
void k_RtlCharToInteger(void) {
    uint32_t s = ARG(0), base = ARG(1), out = ARG(2);
    char text[64];
    wl_xb_read_cstr(s, text, sizeof text);
    const char *p = text;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    bool neg = false;
    if (*p == '-' || *p == '+') {
        neg = *p++ == '-';
    }
    if (base == 0) {
        base = 10;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            base = 16;
            p += 2;
        } else if (p[0] == '0' && (p[1] == 'o' || p[1] == 'O')) {
            base = 8;
            p += 2;
        } else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) {
            base = 2;
            p += 2;
        }
    }
    if (base != 2 && base != 8 && base != 10 && base != 16) {
        RET(STATUS_INVALID_PARAMETER);
        return;
    }
    uint32_t v = 0;
    for (;; p++) {
        uint32_t d;
        if (*p >= '0' && *p <= '9') {
            d = (uint32_t)(*p - '0');
        } else if (*p >= 'a' && *p <= 'f') {
            d = (uint32_t)(*p - 'a' + 10);
        } else if (*p >= 'A' && *p <= 'F') {
            d = (uint32_t)(*p - 'A' + 10);
        } else {
            break;
        }
        if (d >= base) {
            break;
        }
        v = v * base + d;
    }
    if (out != 0 && wl_xb_range_ok(out, 4)) {
        wl_xb_wr32(out, neg ? (uint32_t)(-(int32_t)v) : v);
    }
    RET(STATUS_SUCCESS);
}

// NTSTATUS RtlIntegerToChar(ULONG Value, ULONG Base, LONG OutputLength, PSZ String)
void k_RtlIntegerToChar(void) {
    uint32_t v = ARG(0), base = ARG(1), len = ARG(2), out = ARG(3);
    if (base == 0) {
        base = 10;
    }
    if (base != 2 && base != 8 && base != 10 && base != 16) {
        RET(STATUS_INVALID_PARAMETER);
        return;
    }
    char tmp[40];
    int n = 0;
    do {
        uint32_t d = v % base;
        tmp[n++] = (char)(d < 10u ? '0' + (int)d : 'A' + (int)d - 10);
        v /= base;
    } while (v != 0);
    if ((uint32_t)n > len || !wl_xb_range_ok(out, (uint32_t)n)) {
        RET(STATUS_BUFFER_OVERFLOW);
        return;
    }
    for (int i = 0; i < n; i++) {
        wl_xb_wr8(out + (uint32_t)i, (uint8_t)tmp[n - 1 - i]);
    }
    if ((uint32_t)n < len) {
        wl_xb_wr8(out + (uint32_t)n, 0);
    }
    RET(STATUS_SUCCESS);
}

// LARGE_INTEGER RtlExtendedIntegerMultiply(LARGE_INTEGER Multiplicand, LONG Multiplier)
void k_RtlExtendedIntegerMultiply(void) {
    int64_t a = (int64_t)((uint64_t)ARG(0) | ((uint64_t)ARG(1) << 32));
    int64_t r = a * (int32_t)ARG(2);
    g_eax = (uint32_t)r;
    g_edx = (uint32_t)((uint64_t)r >> 32);
}

// LARGE_INTEGER RtlExtendedLargeIntegerDivide(LARGE_INTEGER Dividend, ULONG Divisor, PULONG Remainder)
void k_RtlExtendedLargeIntegerDivide(void) {
    uint64_t a = (uint64_t)ARG(0) | ((uint64_t)ARG(1) << 32);
    uint32_t d = ARG(2), rem = ARG(3);
    uint64_t q = d != 0 ? a / d : 0;
    if (rem != 0 && wl_xb_range_ok(rem, 4)) {
        wl_xb_wr32(rem, d != 0 ? (uint32_t)(a % d) : 0u);
    }
    g_eax = (uint32_t)q;
    g_edx = (uint32_t)(q >> 32);
}

// ---- Time ------------------------------------------------------------------------------------------
// TIME_FIELDS: Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday (CSHORTs); the time is
// 100 ns since 1601-01-01.

static bool leap(int64_t y) {
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static const int k_mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

void k_RtlTimeToTimeFields(void) {
    uint32_t tp = ARG(0), out = ARG(1);
    if (!wl_xb_range_ok(tp, 8) || !wl_xb_range_ok(out, 16)) {
        return;
    }
    uint64_t t = wl_xb_rd64(tp);
    uint64_t ms_total = t / 10000u;
    uint32_t ms = (uint32_t)(ms_total % 1000u);
    uint64_t secs = ms_total / 1000u;
    int64_t days = (int64_t)(secs / 86400u);
    uint32_t sod = (uint32_t)(secs % 86400u);
    int weekday = (int)((days + 1) % 7); // 1601-01-01 was a Monday
    int64_t year = 1601;
    for (;;) {
        int64_t len = leap(year) ? 366 : 365;
        if (days < len) {
            break;
        }
        days -= len;
        year++;
    }
    int month = 0;
    for (; month < 12; month++) {
        int len = k_mdays[month] + (month == 1 && leap(year) ? 1 : 0);
        if (days < len) {
            break;
        }
        days -= len;
    }
    wl_xb_wr16(out + 0u, (uint16_t)year);
    wl_xb_wr16(out + 2u, (uint16_t)(month + 1));
    wl_xb_wr16(out + 4u, (uint16_t)(days + 1));
    wl_xb_wr16(out + 6u, (uint16_t)(sod / 3600u));
    wl_xb_wr16(out + 8u, (uint16_t)(sod / 60u % 60u));
    wl_xb_wr16(out + 10u, (uint16_t)(sod % 60u));
    wl_xb_wr16(out + 12u, (uint16_t)ms);
    wl_xb_wr16(out + 14u, (uint16_t)weekday);
}

// BOOLEAN RtlTimeFieldsToTime(PTIME_FIELDS TimeFields, PLARGE_INTEGER Time)
void k_RtlTimeFieldsToTime(void) {
    uint32_t in = ARG(0), tp = ARG(1);
    if (!wl_xb_range_ok(in, 16) || !wl_xb_range_ok(tp, 8)) {
        RET(0);
        return;
    }
    int year = (int16_t)wl_xb_rd16(in), month = (int16_t)wl_xb_rd16(in + 2u), day = (int16_t)wl_xb_rd16(in + 4u);
    int hour = (int16_t)wl_xb_rd16(in + 6u), minute = (int16_t)wl_xb_rd16(in + 8u), second = (int16_t)wl_xb_rd16(in + 10u);
    int ms = (int16_t)wl_xb_rd16(in + 12u);
    if (year < 1601 || month < 1 || month > 12 || day < 1 || hour > 23 || minute > 59 || second > 59 || ms > 999 ||
        day > k_mdays[month - 1] + (month == 2 && leap(year) ? 1 : 0)) {
        RET(0);
        return;
    }
    int64_t days = 0;
    for (int y = 1601; y < year; y++) {
        days += leap(y) ? 366 : 365;
    }
    for (int m = 0; m < month - 1; m++) {
        days += k_mdays[m] + (m == 1 && leap(year) ? 1 : 0);
    }
    days += day - 1;
    uint64_t t = ((uint64_t)days * 86400u + (uint64_t)hour * 3600u + (uint64_t)minute * 60u + (uint64_t)second) * 10000000u +
                 (uint64_t)ms * 10000u;
    wl_xb_wr64(tp, t);
    RET(1);
}

// ---- Status to Win32 error -------------------------------------------------------------------------

uint32_t xk_dos_error(uint32_t s) {
    static const uint32_t map[][2] = {
        {0x00000000u, 0},    {0x00000103u, 997},  {0x00000102u, 1460}, {0x00000104u, 0},    {0x80000005u, 234},
        {0x80000006u, 18},   {0xC0000001u, 31},   {0xC0000002u, 1},    {0xC0000004u, 24},   {0xC0000005u, 998},
        {0xC0000008u, 6},    {0xC000000Du, 87},   {0xC000000Fu, 2},    {0xC0000010u, 1},    {0xC0000011u, 38},
        {0xC0000017u, 8},    {0xC0000018u, 487},  {0xC0000022u, 5},    {0xC0000023u, 122},  {0xC0000024u, 6},
        {0xC0000033u, 123},  {0xC0000034u, 2},    {0xC0000035u, 183},  {0xC000003Au, 3},    {0xC0000043u, 32},
        {0xC0000046u, 288},  {0xC0000047u, 298},  {0xC000007Fu, 112},  {0xC000009Au, 1450}, {0xC00000BAu, 5},
        {0xC00000BBu, 50},   {0xC0000101u, 145},  {0xC0000103u, 267},  {0xC0000121u, 5},    {0xC000004Bu, 5},
    };
    for (size_t i = 0; i < WL_ARRAY_COUNT(map); i++) {
        if (map[i][0] == s) {
            return map[i][1];
        }
    }
    return 317; // ERROR_MR_MID_NOT_FOUND
}

void k_RtlNtStatusToDosError(void) {
    RET(xk_dos_error(ARG(0)));
}

// ---- Exceptions ---------------------------------------------------------------------------------
// Translated code cannot jump into another function's __except block, so exceptions are not
// dispatched: a raise is logged and returns (the title goes on as after a handled one); an unwind
// unlinks the fs:[0] frames above the target without running their handlers.

// VOID RtlRaiseException(PEXCEPTION_RECORD ExceptionRecord)
void k_RtlRaiseException(void) {
    uint32_t rec = ARG(0);
    if (wl_xb_range_ok(rec, 20)) {
        WL_LOG_W(XK_TAG, "RtlRaiseException code 0x%08x flags 0x%x address 0x%08x (from 0x%08x): not dispatched",
                 (unsigned)wl_xb_rd32(rec), (unsigned)wl_xb_rd32(rec + 4u), (unsigned)wl_xb_rd32(rec + 12u),
                 (unsigned)wl_xb_kernel_caller());
    }
}

// VOID RtlUnwind(PVOID TargetFrame, PVOID TargetIp, PEXCEPTION_RECORD, PVOID ReturnValue)
void k_RtlUnwind(void) {
    uint32_t target = ARG(0);
    uint32_t frame = wl_xb_rd32(g_fs_base);
    uint32_t steps = 0;
    while (frame != 0xFFFFFFFFu && frame != target && steps++ < 256u && wl_xb_range_ok(frame, 8)) {
        frame = wl_xb_rd32(frame); // Next
    }
    if (frame == target || target == 0) {
        wl_xb_wr32(g_fs_base, target == 0 ? 0xFFFFFFFFu : target);
    } else {
        WL_LOG_W(XK_TAG, "RtlUnwind: frame 0x%08x is not on the SEH chain", (unsigned)target);
    }
    RET(ARG(3));
}

void k_RtlRip(void) {
    char api[64], expr[128], msg[128];
    wl_xb_read_cstr(ARG(0), api, sizeof api);
    wl_xb_read_cstr(ARG(1), expr, sizeof expr);
    wl_xb_read_cstr(ARG(2), msg, sizeof msg);
    WL_LOG_E(XK_TAG, "RtlRip: %s: %s %s", api, expr, msg);
}

void k_RtlAssert(void) {
    char expr[128], file[128], msg[128];
    wl_xb_read_cstr(ARG(0), expr, sizeof expr);
    wl_xb_read_cstr(ARG(1), file, sizeof file);
    wl_xb_read_cstr(ARG(3), msg, sizeof msg);
    WL_LOG_E(XK_TAG, "RtlAssert: %s at %s:%u %s", expr, file, (unsigned)ARG(2), msg);
}

void k_RtlGetCallersAddress(void) {
    if (wl_xb_range_ok(ARG(0), 4)) {
        wl_xb_wr32(ARG(0), wl_xb_kernel_caller());
    }
    if (wl_xb_range_ok(ARG(1), 4)) {
        wl_xb_wr32(ARG(1), 0);
    }
}

// ---- printf over guest varargs ------------------------------------------------------------------

typedef struct {
    uint32_t va; // the next argument
} GuestArgs;

static uint32_t next32(GuestArgs *a) {
    uint32_t v = wl_xb_range_ok(a->va, 4) ? wl_xb_rd32(a->va) : 0u;
    a->va += 4u;
    return v;
}

static uint64_t next64(GuestArgs *a) {
    uint64_t lo = next32(a);
    return lo | ((uint64_t)next32(a) << 32);
}

// Formats the guest format string at `fmt` with the guest arguments at `args` into `out`
// (always terminated); the length the full text has.
static uint32_t guest_format(char *out, uint32_t cap, uint32_t fmt, uint32_t args) {
    char f[512];
    wl_xb_read_cstr(fmt, f, sizeof f);
    GuestArgs a = {args};
    uint32_t n = 0;
    for (const char *p = f; *p != '\0'; p++) {
        if (*p != '%') {
            if (n + 1u < cap) {
                out[n] = *p;
            }
            n++;
            continue;
        }
        char spec[48];
        size_t s = 0;
        spec[s++] = '%';
        p++;
        while (*p != '\0' && strchr("-+ #0", *p) != NULL && s < 8) {
            spec[s++] = *p++;
        }
        if (*p == '*') {
            s += (size_t)snprintf(spec + s, sizeof spec - s, "%d", (int)next32(&a));
            p++;
        } else {
            while (*p >= '0' && *p <= '9' && s < 20) {
                spec[s++] = *p++;
            }
        }
        if (*p == '.') {
            spec[s++] = *p++;
            if (*p == '*') {
                s += (size_t)snprintf(spec + s, sizeof spec - s, "%d", (int)next32(&a));
                p++;
            } else {
                while (*p >= '0' && *p <= '9' && s < 36) {
                    spec[s++] = *p++;
                }
            }
        }
        int longs = 0;
        bool wide = false;
        for (;;) {
            if (*p == 'l') {
                longs++;
                p++;
            } else if (*p == 'h') {
                p++;
            } else if (*p == 'w') {
                wide = true;
                p++;
            } else if (p[0] == 'I' && p[1] == '6' && p[2] == '4') {
                longs = 2;
                p += 3;
            } else {
                break;
            }
        }
        char piece[256];
        char c = *p;
        piece[0] = '\0';
        if (c == '\0') {
            break;
        }
        switch (c) {
        case 'd':
        case 'i':
        case 'u':
        case 'x':
        case 'X':
        case 'o':
            if (longs >= 2) {
                spec[s++] = 'l';
                spec[s++] = 'l';
                spec[s++] = c;
                spec[s] = '\0';
                uint64_t v = next64(&a);
                if (c == 'd' || c == 'i') {
                    snprintf(piece, sizeof piece, spec, (long long)v);
                } else {
                    snprintf(piece, sizeof piece, spec, (unsigned long long)v);
                }
            } else {
                spec[s++] = c;
                spec[s] = '\0';
                uint32_t v = next32(&a);
                if (c == 'd' || c == 'i') {
                    snprintf(piece, sizeof piece, spec, (int)(int32_t)v);
                } else {
                    snprintf(piece, sizeof piece, spec, (unsigned)v);
                }
            }
            break;
        case 'p':
            snprintf(piece, sizeof piece, "%08X", (unsigned)next32(&a));
            break;
        case 'c':
        case 'C':
            piece[0] = (char)next32(&a);
            piece[1] = '\0';
            break;
        case 'e':
        case 'E':
        case 'f':
        case 'g':
        case 'G': {
            uint64_t bits = next64(&a);
            double d;
            memcpy(&d, &bits, 8);
            spec[s++] = c;
            spec[s] = '\0';
            snprintf(piece, sizeof piece, spec, d);
            break;
        }
        case 's':
        case 'S': {
            uint32_t sva = next32(&a);
            char str[200];
            if (sva == 0) {
                strcpy(str, "(null)");
            } else if (c == 'S' || wide || longs > 0) {
                uint32_t k = 0;
                while (k + 1u < sizeof str && wl_xb_range_ok(sva + 2u * k, 2) && wl_xb_rd16(sva + 2u * k) != 0) {
                    uint16_t ch = wl_xb_rd16(sva + 2u * k);
                    str[k++] = (char)(ch < 128u ? ch : '?');
                }
                str[k] = '\0';
            } else {
                wl_xb_read_cstr(sva, str, sizeof str);
            }
            spec[s++] = 's';
            spec[s] = '\0';
            snprintf(piece, sizeof piece, spec, str);
            break;
        }
        case 'Z': { // an ANSI_STRING *
            uint32_t sva = next32(&a);
            char str[200];
            if (!xk_read_ansi(sva, str, sizeof str)) {
                strcpy(str, "(null)");
            }
            snprintf(piece, sizeof piece, "%s", str);
            break;
        }
        case 'n':
            next32(&a);
            break;
        case '%':
            strcpy(piece, "%");
            break;
        default:
            snprintf(piece, sizeof piece, "%%%c", c);
            break;
        }
        for (const char *q = piece; *q != '\0'; q++) {
            if (n + 1u < cap) {
                out[n] = *q;
            }
            n++;
        }
    }
    if (cap > 0) {
        out[n < cap ? n : cap - 1u] = '\0';
    }
    return n;
}

static void log_guest_text(const char *text) {
    size_t len = strlen(text);
    char line[512];
    snprintf(line, sizeof line, "%s", text);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }
    if (len > 0) {
        WL_LOG_I("guest", "%s", line);
    }
}

// ULONG DbgPrint(PCSTR Format, ...) - cdecl: the format at esp, its arguments after
void k_DbgPrint(void) {
    char text[512];
    guest_format(text, sizeof text, ARG(0), g_esp + 4u);
    log_guest_text(text);
    RET(STATUS_SUCCESS);
}

static void put_result(uint32_t buf, uint32_t size, const char *text, uint32_t n) {
    uint32_t keep = size == 0 ? 0 : (n < size ? n : size - 1u);
    if (wl_xb_range_ok(buf, keep + (size > 0 ? 1u : 0u))) {
        wl_xb_copy_in(buf, text, keep);
        if (size > 0) {
            wl_xb_wr8(buf + keep, 0);
        }
    }
}

// int RtlSprintf(PCHAR Buffer, PCHAR Format, ...)
void k_RtlSprintf(void) {
    char text[2048];
    uint32_t n = guest_format(text, sizeof text, ARG(1), g_esp + 8u);
    put_result(ARG(0), n + 1u, text, n);
    RET(n);
}

// int RtlSnprintf(PCHAR Buffer, SIZE_T BufferSize, PCHAR Format, ...)
void k_RtlSnprintf(void) {
    char text[2048];
    uint32_t n = guest_format(text, sizeof text, ARG(2), g_esp + 12u);
    put_result(ARG(0), ARG(1), text, n);
    RET(n < ARG(1) ? n : 0xFFFFFFFFu);
}

// int RtlVsprintf(PCHAR Buffer, PCHAR Format, va_list ArgList)
void k_RtlVsprintf(void) {
    char text[2048];
    uint32_t n = guest_format(text, sizeof text, ARG(1), ARG(2));
    put_result(ARG(0), n + 1u, text, n);
    RET(n);
}

// int RtlVsnprintf(PCHAR Buffer, SIZE_T BufferSize, PCHAR Format, va_list ArgList)
void k_RtlVsnprintf(void) {
    char text[2048];
    uint32_t n = guest_format(text, sizeof text, ARG(2), ARG(3));
    put_result(ARG(0), ARG(1), text, n);
    RET(n < ARG(1) ? n : 0xFFFFFFFFu);
}

void xk_link_rtl(void) {
}
