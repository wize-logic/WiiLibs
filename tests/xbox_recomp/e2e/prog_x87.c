/* SPDX-License-Identifier: MIT */
/* An x86 test program for the lifter: x87 floating point (gcc -m32 -mfpmath=387), the way a 2001
 * title's maths compiles. Runs natively at precision control 53 bits (the model's binary64) and
 * lifted; both must agree exactly. No transcendentals: x87 and libm differ in their last place. */
typedef unsigned int u32;
typedef long long s64;

u32 out[64];
static float fm[16] = {1.5f, 2.f, -3.f, .25f, 0.f, 1.f, 2.f, 3.f, -1.f, .5f, .125f, 7.f, 4.f, -2.f, 1.f, .75f};
static float fr[16];
static double dv[8] = {1.0, 2.5, -3.75, 1e10, 3.14159265358979, 0.001, -0.5, 1234.5678};
static volatile long double ext[4];

__attribute__((noinline)) void matmul(const float *a, const float *b, float *c) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j];
            c[i * 4 + j] = s;
        }
}
__attribute__((noinline)) float vlen(float x, float y, float z) { return __builtin_sqrtf(x * x + y * y + z * z); }
__attribute__((noinline)) void vnorm(float *v) {
    float l = vlen(v[0], v[1], v[2]);
    float inv = 1.0f / l;
    v[0] *= inv; v[1] *= inv; v[2] *= inv;
}
__attribute__((noinline)) int f2i(float f) { return (int)f; }
__attribute__((noinline)) int d2i(double d) { return (int)d; }
__attribute__((noinline)) s64 d2l(double d) { return (s64)d; }
__attribute__((noinline)) double l2d(s64 v) { return (double)v * 0.5; }
__attribute__((noinline)) int fcmp(double a, double b) { return (a < b) * 4 + (a == b) * 2 + (a > b); }
__attribute__((noinline)) double horner(double x) {
    double r = 0.25;
    for (int i = 0; i < 6; i++) r = r * x + dv[i & 7];
    return r;
}
/* long double spills: fstp m80 and fld m80 */
__attribute__((noinline)) double ext_roundtrip(double a, double b) {
    ext[0] = a;
    ext[1] = b;
    ext[2] = ext[0] * ext[1];
    ext[3] = -ext[2];
    return (double)ext[3] + (double)ext[0];
}

static void put_f(int i, float f) { union { float f; u32 u; } c; c.f = f; out[i] = c.u; }
static void put_d(int i, double d) { union { double d; u32 u[2]; } c; c.d = d; out[i] = c.u[0]; out[i + 1] = c.u[1]; }

u32 run(u32 seed) {
    matmul(fm, fm, fr);
    for (int i = 0; i < 16; i++) put_f(i, fr[i]);
    float v[3] = {1.f + (float)(seed & 3), 2.f, 3.f};
    vnorm(v);
    put_f(16, v[0]); put_f(17, v[1]); put_f(18, v[2]);
    out[19] = (u32)f2i(v[0] * 1000.f + 0.5f);
    out[20] = (u32)d2i(dv[7] * (double)(seed & 15));
    out[21] = (u32)d2i(-dv[7]);
    s64 l = d2l(-123456789.75 * (double)(seed & 31));
    out[22] = (u32)l; out[23] = (u32)((unsigned long long)l >> 32);
    put_d(24, l2d((s64)seed * 100000 - 5000000000LL));
    out[26] = (u32)fcmp(dv[seed & 7], dv[(seed >> 3) & 7]);
    put_d(27, horner((double)(seed & 255) / 64.0));
    put_d(29, ext_roundtrip(dv[seed & 7], (double)seed));
    return out[19] ^ out[26];
}
