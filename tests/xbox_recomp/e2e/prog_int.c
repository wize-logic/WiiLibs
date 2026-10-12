/* SPDX-License-Identifier: MIT */
/* An x86 test program for the lifter, compiled freestanding with gcc -m32 and linked at 0x10000:
 * integer code. run(seed) fills out[] and returns a hash; the same image runs natively (the oracle)
 * and lifted (tests/xbox_recomp/e2e.py). No undefined behaviour, so both must agree exactly. */
typedef unsigned int u32;
typedef unsigned char u8;
typedef unsigned short u16;
typedef long long s64;
typedef unsigned long long u64;

u32 out[64];
static u32 tab[16] = {3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5, 8, 9, 7, 9, 3};
static char buf1[96], buf2[96];
struct rec { u32 a; u16 b; u8 c; signed char d; u32 e[3]; };
static struct rec recs[4];

__attribute__((noinline)) u32 crc32_bits(const u8 *p, u32 n) {
    u32 c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

__attribute__((noinline)) void isort(u32 *a, int n) {
    for (int i = 1; i < n; i++) {
        u32 v = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; }
        a[j + 1] = v;
    }
}

/* a jump table */
__attribute__((noinline)) u32 sw(u32 x) {
    switch (x & 7) {
    case 0: return x * 3;
    case 1: return x ^ 0x5555;
    case 2: return x + 17;
    case 3: return x << 3;
    case 4: return x >> 2;
    case 5: return ~x;
    case 6: return x * x;
    default: return x - 99;
    }
}

__attribute__((noinline)) u32 bits(u32 x, u32 n) {
    u32 r = (x << (n & 31)) | (x >> ((32 - n) & 31));
    r ^= __builtin_bswap32(x);
    r += (u32)__builtin_ctz(x | 0x100);
    r += (u32)__builtin_clz(x | 1);
    r += (u32)__builtin_popcount(x);
    return r;
}

/* 64-bit multiply, divide and modulo: libgcc's __udivdi3/__moddi3, lifted too */
__attribute__((noinline)) s64 wide(s64 a, s64 b, u32 d) {
    s64 m = a * b;
    u64 q = (u64)m / d;
    s64 r = m % (s64)d;
    return (s64)q + r + (m >> 7) + (s64)((u64)m >> 61);
}

__attribute__((noinline)) u32 strops(u32 seed) {
    for (int i = 0; i < 96; i++) buf1[i] = (char)(i * 7 + (int)seed);
    __builtin_memcpy(buf2, buf1, 64);
    __builtin_memset(buf2 + 64, 0xAB, 32);
    u32 h = 0;
    for (int i = 0; i < 96; i++) h = h * 31 + (u8)buf2[i];
    return h;
}

__attribute__((noinline)) u16 half(u16 a, u16 b) { return (u16)((u16)((a * b) >> 3) ^ (u16)(a - b)); }
__attribute__((noinline)) signed char sbyte(signed char a) { return (signed char)(a >> 1); }

__attribute__((noinline)) u32 fib(u32 n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

/* indirect calls: the lifted code dispatches through recomp_lookup */
typedef u32 (*op_fn)(u32, u32);
__attribute__((noinline)) u32 op_add(u32 a, u32 b) { return a + b; }
__attribute__((noinline)) u32 op_mul(u32 a, u32 b) { return a * b + 1; }
__attribute__((noinline)) u32 op_xor(u32 a, u32 b) { return (a ^ b) - 3; }
static op_fn ops[3] = {op_add, op_mul, op_xor};
__attribute__((noinline)) u32 apply(u32 seed) {
    u32 acc = seed;
    for (u32 i = 0; i < 9; i++) acc = ops[(acc + i) % 3](acc, i * 0x01010101u);
    return acc;
}

__attribute__((noinline)) u32 structs(u32 seed) {
    for (int i = 0; i < 4; i++) {
        recs[i].a = seed * (u32)(i + 1);
        recs[i].b = (u16)(seed >> i);
        recs[i].c = (u8)(seed + (u32)i);
        recs[i].d = (signed char)(0x80 + i);
        for (int j = 0; j < 3; j++) recs[i].e[j] = recs[i].a ^ (u32)(j * 0x10001);
    }
    struct rec t = recs[1];
    recs[1] = recs[3];
    recs[3] = t;
    u32 h = 0;
    for (int i = 0; i < 4; i++) h = h * 131 + recs[i].a + recs[i].b + recs[i].c + (u32)recs[i].d + recs[i].e[2];
    return h;
}

u32 run(u32 seed) {
    u32 h = seed;
    u8 data[32];
    for (int i = 0; i < 32; i++) data[i] = (u8)(seed + (u32)i * 13);
    out[0] = crc32_bits(data, 32);
    u32 a[16];
    for (int i = 0; i < 16; i++) a[i] = tab[i] * seed + (u32)i;
    isort(a, 16);
    for (int i = 0; i < 16; i++) h = h * 33 + a[i];
    out[1] = h;
    u32 s = 0;
    for (u32 i = 0; i < 40; i++) s += sw(i * 37 + seed);
    out[2] = s;
    out[3] = bits(seed * 2654435761u, seed & 63) + bits(seed, 33);
    s64 w = wide((s64)seed * 1000003, (s64)seed + 77, (seed | 1));
    out[4] = (u32)w;
    out[5] = (u32)((u64)w >> 32);
    out[6] = strops(seed);
    out[7] = half((u16)seed, (u16)(seed >> 5));
    out[8] = (u32)(int)sbyte((signed char)seed);
    out[9] = fib(12 + (seed & 3));
    out[10] = apply(seed);
    out[11] = structs(seed);
    return h ^ out[10];
}
