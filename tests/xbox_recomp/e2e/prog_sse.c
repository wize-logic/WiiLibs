/* SPDX-License-Identifier: MIT */
/* An x86 test program for the lifter: the corners -- shift counts of 32 and more, float-to-int
 * conversions out of range and on ties, SSE packed and scalar, MMX, lock-prefixed atomics, rep
 * string instructions, bsf/bsr, 64/32 division. Compiled with gcc -m32 -march=pentium3 -msse
 * -mfpmath=sse (the Xbox's CPU), run natively and lifted; both must agree exactly. */
#include <mmintrin.h>
#include <xmmintrin.h>
typedef unsigned int u32;
typedef unsigned char u8;

u32 out[96];
static volatile u32 counter = 5;
static u32 sa[8], sb[8];
static u8 pix[32] = {1, 2, 3, 4, 5, 6, 7, 8, 250, 251, 252, 253, 254, 255, 0, 10,
                     100, 110, 120, 130, 140, 150, 160, 170, 9, 8, 7, 6, 5, 4, 3, 2};
static float fv[8] = {1.5f, -2.25f, 3.0e9f, -3.0e9f, 0.5f, 2.5f, -2.5f, -0.5f};

__attribute__((noinline)) u32 shl_cl(u32 x, u32 c) { __asm__("shll %%cl, %0" : "+r"(x) : "c"(c) : "cc"); return x; }
__attribute__((noinline)) u32 shr_cl(u32 x, u32 c) { __asm__("shrl %%cl, %0" : "+r"(x) : "c"(c) : "cc"); return x; }
__attribute__((noinline)) u32 sar_cl(u32 x, u32 c) { __asm__("sarl %%cl, %0" : "+r"(x) : "c"(c) : "cc"); return x; }
__attribute__((noinline)) u32 shl16_cl(u32 x, u32 c) { unsigned short v = (unsigned short)x; __asm__("shlw %%cl, %0" : "+r"(v) : "c"(c) : "cc"); return v; }
__attribute__((noinline)) u32 rol_cl(u32 x, u32 c) { __asm__("roll %%cl, %0" : "+r"(x) : "c"(c) : "cc"); return x; }
__attribute__((noinline)) u32 shld_cl(u32 x, u32 y, u32 c) { __asm__("shldl %%cl, %1, %0" : "+r"(x) : "r"(y), "c"(c) : "cc"); return x; }
__attribute__((noinline)) u32 cvtt(float f) { return (u32)_mm_cvttss_si32(_mm_set_ss(f)); }
__attribute__((noinline)) u32 cvtr(float f) { return (u32)_mm_cvtss_si32(_mm_set_ss(f)); }
__attribute__((noinline)) u32 div64(u32 hi, u32 lo, u32 d) { u32 q, r; __asm__("divl %4" : "=a"(q), "=d"(r) : "a"(lo), "d"(hi), "r"(d)); return q ^ (r << 7); }
__attribute__((noinline)) u32 idiv64(int hi, u32 lo, int d) { int q, r; __asm__("idivl %4" : "=a"(q), "=d"(r) : "a"(lo), "d"(hi), "r"(d)); return (u32)q ^ ((u32)r << 7); }
__attribute__((noinline)) u32 mul64(u32 a, u32 b) { u32 lo, hi; __asm__("mull %3" : "=a"(lo), "=d"(hi) : "a"(a), "r"(b)); return lo ^ hi; }
__attribute__((noinline)) u32 bsfr(u32 x) { u32 r = 77, q = 55; __asm__("bsfl %1, %0" : "+r"(r) : "r"(x) : "cc"); __asm__("bsrl %1, %0" : "+r"(q) : "r"(x) : "cc"); return r * 100 + q; }
__attribute__((noinline)) u32 atom(u32 add) {
    u32 a = __sync_fetch_and_add(&counter, add);
    u32 b = __sync_val_compare_and_swap(&counter, a + add, 1234u + add);
    return a + b * 3 + counter;
}
__attribute__((noinline)) u32 rep_ops(void) {
    for (int i = 0; i < 8; i++) sa[i] = 0x01020304u * (u32)(i + 1);
    u32 *s = sa, *d = sb, n = 8;
    __asm__ volatile("cld; rep movsl" : "+S"(s), "+D"(d), "+c"(n) :: "memory");
    u32 *d2 = sb + 2, val = 0xAABBCCDDu;
    n = 3;
    __asm__ volatile("rep stosl" : "+D"(d2), "+c"(n) : "a"(val) : "memory");
    u8 *p = (u8 *)sa;
    u32 cnt = 32;
    u8 key = 0x40;
    __asm__ volatile("repne scasb" : "+D"(p), "+c"(cnt) : "a"(key) : "cc", "memory");
    return (u32)(p - (u8 *)sa) * 1000 + cnt + sb[3] + sb[7];
}
__attribute__((noinline)) u32 mmx(void) {
    __m64 a = *(__m64 *)&pix[0], b = *(__m64 *)&pix[8], c = *(__m64 *)&pix[16];
    __m64 r = _mm_unpacklo_pi8(a, b);
    r = _mm_add_pi16(r, _mm_unpackhi_pi8(c, a));
    r = _mm_adds_pu8(r, b);
    r = _mm_madd_pi16(r, _mm_set1_pi16(3));
    __m64 p = _mm_packs_pu16(_mm_unpacklo_pi8(a, _mm_setzero_si64()), _mm_unpackhi_pi8(b, _mm_setzero_si64()));
    r = _mm_xor_si64(r, _mm_slli_pi32(p, 3));
    r = _mm_add_pi32(r, _mm_srli_pi16(c, 2));
    u32 lo = (u32)_mm_cvtsi64_si32(r);
    u32 hi = (u32)_mm_cvtsi64_si32(_mm_srli_si64(r, 32));
    ((__m64 *)&out[80])[0] = r;
    _mm_empty();
    return lo * 31 + hi + (u32)_mm_movemask_pi8(c);
}
__attribute__((noinline)) u32 sse_packed(void) {
    __m128 a = _mm_loadu_ps(&fv[0]), b = _mm_loadu_ps(&fv[4]);
    __m128 r = _mm_add_ps(_mm_mul_ps(a, b), _mm_shuffle_ps(a, b, 0x1B));
    r = _mm_min_ps(r, _mm_set1_ps(100.f));
    _mm_storeu_ps((float *)&out[64], r);
    __m128 m = _mm_cmplt_ps(a, b);
    return (u32)_mm_movemask_ps(m) * 17 + (u32)_mm_cvtss_si32(_mm_sqrt_ss(_mm_set_ss(fv[7] < 0 ? -fv[7] * 9.f : fv[7])));
}
__attribute__((noinline)) float sse_scalar(float x, float y) {
    float r = x * y + 0.5f;
    r = r / (y + 3.0f) - x;
    return r < 0 ? -r : r;
}

u32 run(u32 seed) {
    u32 k = 0;
    static const u32 cs[] = {0, 1, 5, 31, 32, 33, 40, 63, 64, 65, 255};
    for (int i = 0; i < 10; i++) {
        u32 c = cs[i];
        out[k++] = shl_cl(seed | 0x80000001u, c);
        out[k++] = shr_cl(seed | 0x80000001u, c);
        out[k++] = sar_cl(seed | 0x80000001u, c);
        out[k++] = shl16_cl(seed, c);
        out[k++] = rol_cl(seed, c);
        out[k++] = shld_cl(seed, ~seed, c);
    }
    for (int i = 0; i < 4; i++) out[60 + i] = cvtt(fv[i + (seed & 1) * 4]);
    for (int i = 0; i < 8; i++) out[84 + i] = cvtr(fv[i]);
    out[68] = sse_packed();
    out[69] = mmx();
    out[70] = div64(5, seed, 7 + (seed & 3));
    out[71] = idiv64(-3, seed, -9 - (int)(seed & 7));
    out[72] = mul64(seed, seed ^ 0xFFFF);
    out[73] = bsfr(seed | 0x10);
    out[74] = atom(seed & 15);
    out[75] = rep_ops();
    union { float f; u32 u; } c;
    c.f = sse_scalar((float)(seed & 1023) * 0.25f, 1.5f);
    out[76] = c.u;
    return k;
}
