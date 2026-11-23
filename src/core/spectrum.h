/* Nora - spectral helpers: CIE 1931 CMFs, hero wavelength sampling, RGB->spectrum
 * uplifting (sigmoid-polynomial, fitted at start-up) and XYZ->sRGB. MIT License. */
#ifndef NORA_SPECTRUM_H
#define NORA_SPECTRUM_H

#include "nmath.h"

#define NORA_LAMBDA_MIN 360.0f
#define NORA_LAMBDA_MAX 830.0f
#define NORA_CMF_COUNT 471
#define NORA_NWL 4

extern const float nora_cie1931_xyz[NORA_CMF_COUNT][3];

/* Four spectral samples (spectral mode) or R,G,B,unused (RGB mode). */
typedef struct sp4 { float v[NORA_NWL]; } sp4;

/* The wavelengths a path carries. In RGB mode spectral == 0 and lambda is unused. */
typedef struct swl {
    float lambda[NORA_NWL];
    int spectral;
    int secondary_dead;   /* dispersion collapsed the path onto the hero wavelength */
} swl;

/* Builds the uplifting table and normalisation constants. Idempotent; call once
 * from the main thread before rendering (the renderer and CLI do). */
void spectrum_init(void);

void swl_init_rgb(swl *w);
void swl_sample(swl *w, float u);              /* hero wavelength + 3 rotated companions */
void swl_terminate_secondary(swl *w, sp4 *beta); /* keep only the hero, reweighted */

v3 cmf_eval(float lambda);                      /* linear interpolation, 0 outside */
float blackbody(float lambda_nm, float kelvin);
float illuminant_white(float lambda_nm);        /* ~D65 (6504 K Planck), 1 at 560 nm */
v3 xyz_to_linear_srgb(v3 xyz);
v3 linear_srgb_to_xyz(v3 rgb);

/* Sigmoid-polynomial uplifting (Jakob & Hanika 2019 parametrisation, own fit). */
float spec_coeffs_eval(const float c[3], float lambda_nm);
float rgb_uplift_eval(v3 rgb, float lambda_nm); /* reflectance value at lambda, rgb clamped to [0,1] */

/* Conversions used by the shading code. */
sp4 sp_from_rgb(const swl *w, v3 rgb);          /* reflectance-like, clamped to [0,1] */
sp4 sp_from_rgb_unbounded(const swl *w, v3 rgb);/* any non-negative colour (weights) */
sp4 sp_from_rgb_illum(const swl *w, v3 rgb);    /* emission: uplift * white illuminant */
v3 sp_to_rgb(const swl *w, sp4 s);              /* estimator contribution -> linear sRGB */

/* Round trip of an RGB colour through the uplift + CMF integration (for tests). */
v3 spectrum_roundtrip_rgb(v3 rgb);

NORA_INLINE sp4 sp_const(float f) { sp4 r; int i; for (i = 0; i < NORA_NWL; i++) r.v[i] = f; return r; }
NORA_INLINE sp4 sp_add(sp4 a, sp4 b) { int i; for (i = 0; i < NORA_NWL; i++) a.v[i] += b.v[i]; return a; }
NORA_INLINE sp4 sp_sub(sp4 a, sp4 b) { int i; for (i = 0; i < NORA_NWL; i++) a.v[i] -= b.v[i]; return a; }
NORA_INLINE sp4 sp_mul(sp4 a, sp4 b) { int i; for (i = 0; i < NORA_NWL; i++) a.v[i] *= b.v[i]; return a; }
NORA_INLINE sp4 sp_scale(sp4 a, float s) { int i; for (i = 0; i < NORA_NWL; i++) a.v[i] *= s; return a; }
NORA_INLINE sp4 sp_div(sp4 a, sp4 b) { int i; for (i = 0; i < NORA_NWL; i++) a.v[i] = b.v[i] != 0.0f ? a.v[i] / b.v[i] : 0.0f; return a; }
NORA_INLINE float sp_max(sp4 a) { float m = a.v[0]; int i; for (i = 1; i < NORA_NWL; i++) m = a.v[i] > m ? a.v[i] : m; return m; }
NORA_INLINE float sp_avg(sp4 a, int n) { float s = 0; int i; for (i = 0; i < n; i++) s += a.v[i]; return s / (float)n; }
NORA_INLINE int sp_is_black(sp4 a) { int i; for (i = 0; i < NORA_NWL; i++) if (a.v[i] != 0.0f) return 0; return 1; }
NORA_INLINE int sp_has_nan(sp4 a) { int i; for (i = 0; i < NORA_NWL; i++) if (!(a.v[i] == a.v[i]) || a.v[i] > 1e30f || a.v[i] < -1e30f) return 1; return 0; }
NORA_INLINE int swl_channels(const swl *w) { return w->spectral ? NORA_NWL : 3; }

#endif
