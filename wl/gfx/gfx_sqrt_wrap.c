// SPDX-License-Identifier: MIT
// __wrap_sqrtf for a program that links with -Wl,--wrap=sqrtf (wl_gfx_wrap_sqrtf in cmake/WiiLibs.cmake):
// every call of sqrtf, the libraries' too, comes here. It is in its own object, so a program that does not
// wrap sqrtf does not carry it. See gfx_sqrt.h.
#include "wl/gfx/gfx_sqrt.h"

float __wrap_sqrtf(float x);
double __wrap_sqrt(double x);

float __wrap_sqrtf(float x) {
    return wl_sqrtf_exact(x);
}

// (-Wl,--wrap=sqrt: wl_gfx_wrap_sqrt)
double __wrap_sqrt(double x) {
    return wl_sqrt_exact(x);
}
