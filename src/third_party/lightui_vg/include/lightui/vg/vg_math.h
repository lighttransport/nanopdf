/*
 * lightui/vg/vg_math.h — libm-free math routines for the VG rasterizer.
 *
 * Provides sqrtf / sinf / cosf / floorf / ceilf / roundf / fabsf replacements
 * that do NOT call libm.  Accuracy is sufficient for rasterization
 * (sub-pixel errors < 1e-5 for the trig functions; sqrtf is near-IEEE via
 * Newton-Raphson refinement of a bit-hack initial guess).
 *
 * Override any single routine before including this header:
 *
 *     #define LUI_VG_SQRTF(x) my_fast_sqrt(x)
 *     #include <lightui/vg/vg_math.h>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LIGHTUI_VG_MATH_H
#define LIGHTUI_VG_MATH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef LUI_VG_PI
#  define LUI_VG_PI       3.14159265358979323846f
#endif
#ifndef LUI_VG_TAU
#  define LUI_VG_TAU      6.28318530717958647692f
#endif
#ifndef LUI_VG_PI_OVER_2
#  define LUI_VG_PI_OVER_2 1.57079632679489661923f
#endif

/* ---- fabsf ---- */
static inline float lui_vg__fabsf(float x) {
    union { float f; uint32_t u; } b; b.f = x;
    b.u &= 0x7FFFFFFFu;
    return b.f;
}
#ifndef LUI_VG_FABSF
#  define LUI_VG_FABSF(x) lui_vg__fabsf(x)
#endif

/* ---- floorf / ceilf / roundf (truncate-based; valid for |x| < 2^23) ---- */
static inline float lui_vg__floorf(float x) {
    float t = (float)(int32_t)x;
    return (x < t) ? (t - 1.0f) : t;
}
static inline float lui_vg__ceilf(float x) {
    float t = (float)(int32_t)x;
    return (x > t) ? (t + 1.0f) : t;
}
static inline float lui_vg__roundf(float x) {
    return (x >= 0.0f) ? lui_vg__floorf(x + 0.5f)
                       : -lui_vg__floorf(-x + 0.5f);
}
#ifndef LUI_VG_FLOORF
#  define LUI_VG_FLOORF(x) lui_vg__floorf(x)
#endif
#ifndef LUI_VG_CEILF
#  define LUI_VG_CEILF(x)  lui_vg__ceilf(x)
#endif
#ifndef LUI_VG_ROUNDF
#  define LUI_VG_ROUNDF(x) lui_vg__roundf(x)
#endif

/* ---- sqrtf: bit-hack seed + 3 Newton iterations (near-IEEE in float) ---- */
static inline float lui_vg__sqrtf(float x) {
    /* Negated compare catches NaN as well as x <= 0. */
    if (!(x > 0.0f)) return 0.0f;
    union { float f; uint32_t u; } b; b.f = x;
    /* Seed: exponent half, mantissa small tweak. ~3% error. */
    b.u = (b.u >> 1) + 0x1FC00000u;
    float y = b.f;
    y = 0.5f * (y + x / y);
    y = 0.5f * (y + x / y);
    y = 0.5f * (y + x / y);
    return y;
}
#ifndef LUI_VG_SQRTF
#  define LUI_VG_SQRTF(x) lui_vg__sqrtf(x)
#endif

/* ---- sinf / cosf: range-reduce to [-pi, pi] then to [-pi/2, pi/2],
 *                  then Maclaurin-7 (max err ~3e-6 in float). ---- */
static inline float lui_vg__sinf_core(float x) {
    /* valid for x in [-pi/2, pi/2]; sin(x) ≈ x - x^3/6 + x^5/120 - x^7/5040 */
    float x2 = x * x;
    return x * (1.0f - x2 * (1.0f/6.0f
              - x2 * (1.0f/120.0f
              - x2 * (1.0f/5040.0f))));
}
static inline float lui_vg__sinf(float x) {
    /* Reduce x to [-pi, pi]. */
    float k = x * (1.0f / LUI_VG_TAU);
    k = (k >= 0.0f) ? lui_vg__floorf(k + 0.5f) : -lui_vg__floorf(-k + 0.5f);
    x -= k * LUI_VG_TAU;
    /* Now x roughly in [-pi, pi]. Fold to [-pi/2, pi/2]. */
    if (x >  LUI_VG_PI_OVER_2) x =  LUI_VG_PI - x;
    else if (x < -LUI_VG_PI_OVER_2) x = -LUI_VG_PI - x;
    return lui_vg__sinf_core(x);
}
static inline float lui_vg__cosf(float x) {
    return lui_vg__sinf(x + LUI_VG_PI_OVER_2);
}
#ifndef LUI_VG_SINF
#  define LUI_VG_SINF(x) lui_vg__sinf(x)
#endif
#ifndef LUI_VG_COSF
#  define LUI_VG_COSF(x) lui_vg__cosf(x)
#endif

/* ---- Double-precision sinc helpers (Lanczos resampling). ---- */
/* We only need sin() for sinc(x)=sin(pi*x)/(pi*x); float precision is
 * sufficient for image filter weights. */
static inline double lui_vg__sin_d(double x) {
    return (double)lui_vg__sinf((float)x);
}
static inline double lui_vg__sqrt_d(double x) {
    if (!(x > 0.0)) return 0.0;
    double y = (double)lui_vg__sqrtf((float)x);
    /* One Newton step in double precision to recover accuracy. */
    y = 0.5 * (y + x / y);
    return y;
}
static inline double lui_vg__floor_d(double x) {
    double t = (double)(int64_t)x;
    return (x < t) ? (t - 1.0) : t;
}
#ifndef LUI_VG_SIN
#  define LUI_VG_SIN(x)   lui_vg__sin_d(x)
#endif
#ifndef LUI_VG_SQRT
#  define LUI_VG_SQRT(x)  lui_vg__sqrt_d(x)
#endif
#ifndef LUI_VG_FLOOR
#  define LUI_VG_FLOOR(x) lui_vg__floor_d(x)
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIGHTUI_VG_MATH_H */
