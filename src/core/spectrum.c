/* Nora - spectral helpers. MIT License.
 *
 * RGB -> spectrum uplifting uses the sigmoid-polynomial model of Jakob & Hanika,
 * "A Low-Dimensional Function Space for Efficient Spectral Upsampling" (2019):
 *     s(lambda) = S(c0 x^2 + c1 x + c2),  S(t) = 1/2 + t / (2 sqrt(1 + t^2)),
 * with x the wavelength normalised to [0,1]. The coefficient table is fitted
 * at start-up by our own Gauss-Newton solver (no third-party tables), over a
 * grid parametrised like the paper: the largest RGB component selects one of
 * three tables, indexed by that component (z) and the other two divided by it.
 */
#include "spectrum.h"
#include <string.h>
#include <stdlib.h>

#define UP_RES 24
#define FIT_STEP 5   /* nm between integration samples during fitting */
#define FIT_N 95   /* (830 - 360) / FIT_STEP + 1 */

static float g_zs[UP_RES];
static float g_table[3][UP_RES][UP_RES][UP_RES][3];   /* [l][k(z)][j(y)][i(x)][coef] */
static float g_fit_w[FIT_N][3];                     /* rgb weight of each fit sample */
static v3 g_white_rgb = { 1, 1, 1 };
static float g_white_y = 1.0f;
static volatile int g_ready = 0;

static const float M_XYZ2RGB[3][3] = {
    { 3.2404542f, -1.5371385f, -0.4985314f },
    { -0.9692660f, 1.8760108f, 0.0415560f },
    { 0.0556434f, -0.2040259f, 1.0572252f },
};
static const float M_RGB2XYZ[3][3] = {
    { 0.4124564f, 0.3575761f, 0.1804375f },
    { 0.2126729f, 0.7151522f, 0.0721750f },
    { 0.0193339f, 0.1191920f, 0.9503041f },
};

v3 xyz_to_linear_srgb(v3 c) {
    return v3_make(M_XYZ2RGB[0][0] * c.x + M_XYZ2RGB[0][1] * c.y + M_XYZ2RGB[0][2] * c.z,
                   M_XYZ2RGB[1][0] * c.x + M_XYZ2RGB[1][1] * c.y + M_XYZ2RGB[1][2] * c.z,
                   M_XYZ2RGB[2][0] * c.x + M_XYZ2RGB[2][1] * c.y + M_XYZ2RGB[2][2] * c.z);
}
v3 linear_srgb_to_xyz(v3 c) {
    return v3_make(M_RGB2XYZ[0][0] * c.x + M_RGB2XYZ[0][1] * c.y + M_RGB2XYZ[0][2] * c.z,
                   M_RGB2XYZ[1][0] * c.x + M_RGB2XYZ[1][1] * c.y + M_RGB2XYZ[1][2] * c.z,
                   M_RGB2XYZ[2][0] * c.x + M_RGB2XYZ[2][1] * c.y + M_RGB2XYZ[2][2] * c.z);
}

v3 cmf_eval(float lambda) {
    float f = lambda - NORA_LAMBDA_MIN;
    int i;
    float t;
    if (f < 0.0f || f > (float)(NORA_CMF_COUNT - 1)) return v3_make(0, 0, 0);
    i = (int)f;
    if (i >= NORA_CMF_COUNT - 1) i = NORA_CMF_COUNT - 2;
    t = f - (float)i;
    return v3_make(nora_cie1931_xyz[i][0] * (1 - t) + nora_cie1931_xyz[i + 1][0] * t,
                   nora_cie1931_xyz[i][1] * (1 - t) + nora_cie1931_xyz[i + 1][1] * t,
                   nora_cie1931_xyz[i][2] * (1 - t) + nora_cie1931_xyz[i + 1][2] * t);
}

float blackbody(float lambda_nm, float T) {
    const double c = 299792458.0, h = 6.62606957e-34, kb = 1.3806488e-23;
    double l = (double)lambda_nm * 1e-9;
    double le = (2.0 * h * c * c) / (l * l * l * l * l * (exp((h * c) / (l * kb * (double)T)) - 1.0));
    return (float)le;
}

float illuminant_white(float lambda_nm) {
    static float norm = 0.0f;
    if (norm == 0.0f) norm = 1.0f / blackbody(560.0f, 6504.0f);
    return blackbody(lambda_nm, 6504.0f) * norm;
}

static float sigmoid(float x) {
    if (x > 1e15f) return 1.0f;
    if (x < -1e15f) return 0.0f;
    return 0.5f + x / (2.0f * sqrtf(1.0f + x * x));
}

float spec_coeffs_eval(const float c[3], float lambda_nm) {
    float x = (lambda_nm - NORA_LAMBDA_MIN) / (NORA_LAMBDA_MAX - NORA_LAMBDA_MIN);
    return sigmoid((c[0] * x + c[1]) * x + c[2]);
}

/* rgb produced by the coefficient spectrum (reflectance under the white illuminant) */
static void fit_eval(const float c[3], float rgb[3], float J[3][3]) {
    int k, a;
    for (a = 0; a < 3; a++) { rgb[a] = 0; J[a][0] = J[a][1] = J[a][2] = 0; }
    for (k = 0; k < FIT_N; k++) {
        float x = (float)(k * FIT_STEP) / (NORA_LAMBDA_MAX - NORA_LAMBDA_MIN);
        float t = (c[0] * x + c[1]) * x + c[2];
        float s = sigmoid(t);
        float d = 1.0f + t * t;
        float ds = 0.5f / (d * sqrtf(d));
        float b[3];
        b[0] = x * x; b[1] = x; b[2] = 1.0f;
        for (a = 0; a < 3; a++) {
            float w = g_fit_w[k][a];
            rgb[a] += s * w;
            J[a][0] += ds * b[0] * w;
            J[a][1] += ds * b[1] * w;
            J[a][2] += ds * b[2] * w;
        }
    }
}

static int solve3(float A[3][3], const float b[3], float x[3]) {
    double det = (double)A[0][0] * ((double)A[1][1] * A[2][2] - (double)A[1][2] * A[2][1]) -
                 (double)A[0][1] * ((double)A[1][0] * A[2][2] - (double)A[1][2] * A[2][0]) +
                 (double)A[0][2] * ((double)A[1][0] * A[2][1] - (double)A[1][1] * A[2][0]);
    int i;
    if (fabs(det) < 1e-24) return 0;
    for (i = 0; i < 3; i++) {
        float M[3][3];
        int r, cc;
        double di;
        for (r = 0; r < 3; r++) for (cc = 0; cc < 3; cc++) M[r][cc] = (cc == i) ? b[r] : A[r][cc];
        di = (double)M[0][0] * ((double)M[1][1] * M[2][2] - (double)M[1][2] * M[2][1]) -
             (double)M[0][1] * ((double)M[1][0] * M[2][2] - (double)M[1][2] * M[2][0]) +
             (double)M[0][2] * ((double)M[1][0] * M[2][1] - (double)M[1][1] * M[2][0]);
        x[i] = (float)(di / det);
    }
    return 1;
}

/* Gauss-Newton fit of coefficients c (warm started) to the target rgb. */
static void fit_coeffs(const float target[3], float c[3]) {
    int it, a;
    for (it = 0; it < 40; it++) {
        float rgb[3], J[3][3], r[3], dx[3], len2 = 0, rn = 0;
        fit_eval(c, rgb, J);
        for (a = 0; a < 3; a++) { r[a] = target[a] - rgb[a]; rn += r[a] * r[a]; }
        if (rn < 1e-12f) break;
        if (!solve3(J, r, dx)) break;
        for (a = 0; a < 3; a++) len2 += dx[a] * dx[a];
        if (len2 > 25.0f) {   /* limit the step: the sigmoid saturates and J goes flat */
            float s = 5.0f / sqrtf(len2);
            for (a = 0; a < 3; a++) dx[a] *= s;
        }
        for (a = 0; a < 3; a++) c[a] += dx[a];
    }
}

static float smoothstepf(float t) { return t * t * (3.0f - 2.0f * t); }

void spectrum_init(void) {
    int k, a, l, i, j;
    v3 wxyz = { 0, 0, 0 };
    if (g_ready) return;
    /* white normalisation: the white illuminant maps to Y = 1 and rgb = (1,1,1) */
    for (k = 0; k < NORA_CMF_COUNT; k++) {
        float lam = NORA_LAMBDA_MIN + (float)k;
        float w = illuminant_white(lam);
        wxyz.x += w * nora_cie1931_xyz[k][0];
        wxyz.y += w * nora_cie1931_xyz[k][1];
        wxyz.z += w * nora_cie1931_xyz[k][2];
    }
    g_white_y = wxyz.y;
    g_white_rgb = xyz_to_linear_srgb(v3_scale(wxyz, 1.0f / g_white_y));
    /* fit integration weights */
    for (k = 0; k < FIT_N; k++) {
        float lam = NORA_LAMBDA_MIN + (float)(k * FIT_STEP);
        float w = illuminant_white(lam) * (float)FIT_STEP / g_white_y;
        float dl = (k == 0 || k == FIT_N - 1) ? 0.5f : 1.0f;   /* trapezoid */
        v3 c = cmf_eval(lam);
        v3 rgb = xyz_to_linear_srgb(v3_scale(c, w * dl));
        g_fit_w[k][0] = rgb.x / g_white_rgb.x;
        g_fit_w[k][1] = rgb.y / g_white_rgb.y;
        g_fit_w[k][2] = rgb.z / g_white_rgb.z;
    }
    for (k = 0; k < UP_RES; k++) g_zs[k] = smoothstepf(smoothstepf((float)k / (float)(UP_RES - 1)));
    for (l = 0; l < 3; l++) {
        for (j = 0; j < UP_RES; j++) {
            for (i = 0; i < UP_RES; i++) {
                float x = (float)i / (float)(UP_RES - 1), y = (float)j / (float)(UP_RES - 1);
                float c[3] = { 0, 0, 0 };
                int start = UP_RES / 5;
                for (k = start; k < UP_RES; k++) {
                    float z = g_zs[k], tgt[3];
                    tgt[l] = z; tgt[(l + 1) % 3] = x * z; tgt[(l + 2) % 3] = y * z;
                    fit_coeffs(tgt, c);
                    for (a = 0; a < 3; a++) g_table[l][k][j][i][a] = c[a];
                }
                c[0] = g_table[l][start][j][i][0]; c[1] = g_table[l][start][j][i][1]; c[2] = g_table[l][start][j][i][2];
                for (k = start - 1; k >= 1; k--) {
                    float z = g_zs[k], tgt[3];
                    tgt[l] = z; tgt[(l + 1) % 3] = x * z; tgt[(l + 2) % 3] = y * z;
                    fit_coeffs(tgt, c);
                    for (a = 0; a < 3; a++) g_table[l][k][j][i][a] = c[a];
                }
                for (a = 0; a < 3; a++) g_table[l][0][j][i][a] = g_table[l][1][j][i][a];
            }
        }
    }
    g_ready = 1;
}

/* Table lookup: bilinear in the two ratio axes for the two z slices around z.
 * The final value blends the two slice spectra (not their coefficients), which
 * stays accurate where the coefficients change quickly (dark colours). */
typedef struct uplift_q { float lo[3], hi[3], fz, scale; } uplift_q;

static void uplift_lookup(v3 rgbv, uplift_q *q) {
    float rgb[3];
    int l, a, k, i, j, lo, hi;
    float z, x, y, fx, fy;
    rgb[0] = nclampf(rgbv.x, 0, 1); rgb[1] = nclampf(rgbv.y, 0, 1); rgb[2] = nclampf(rgbv.z, 0, 1);
    q->scale = 1.0f;
    if (!g_ready) spectrum_init();
    l = (rgb[0] >= rgb[1] && rgb[0] >= rgb[2]) ? 0 : (rgb[1] >= rgb[2] ? 1 : 2);
    z = rgb[l];
    if (z <= 0.0f) { q->scale = 0.0f; q->fz = 0; for (a = 0; a < 3; a++) q->lo[a] = q->hi[a] = 0; return; }
    x = rgb[(l + 1) % 3] / z * (float)(UP_RES - 1);
    y = rgb[(l + 2) % 3] / z * (float)(UP_RES - 1);
    if (z < g_zs[1]) { q->scale = z / g_zs[1]; z = g_zs[1]; }
    lo = 1; hi = UP_RES - 1;
    while (hi - lo > 1) { int m = (lo + hi) / 2; if (g_zs[m] <= z) lo = m; else hi = m; }
    k = lo;
    q->fz = nclampf((z - g_zs[k]) / (g_zs[k + 1] - g_zs[k]), 0.0f, 1.0f);
    i = (int)x; if (i > UP_RES - 2) i = UP_RES - 2;
    j = (int)y; if (j > UP_RES - 2) j = UP_RES - 2;
    fx = x - (float)i; fy = y - (float)j;
    for (a = 0; a < 3; a++) {
        float c000 = g_table[l][k][j][i][a], c001 = g_table[l][k][j][i + 1][a];
        float c010 = g_table[l][k][j + 1][i][a], c011 = g_table[l][k][j + 1][i + 1][a];
        float c100 = g_table[l][k + 1][j][i][a], c101 = g_table[l][k + 1][j][i + 1][a];
        float c110 = g_table[l][k + 1][j + 1][i][a], c111 = g_table[l][k + 1][j + 1][i + 1][a];
        float c00 = c000 + (c001 - c000) * fx, c01 = c010 + (c011 - c010) * fx;
        float c10 = c100 + (c101 - c100) * fx, c11 = c110 + (c111 - c110) * fx;
        q->lo[a] = c00 + (c01 - c00) * fy;
        q->hi[a] = c10 + (c11 - c10) * fy;
    }
}

static float uplift_q_eval(const uplift_q *q, float lambda) {
    float a, b;
    if (q->scale == 0.0f) return 0.0f;
    a = spec_coeffs_eval(q->lo, lambda);
    b = spec_coeffs_eval(q->hi, lambda);
    return q->scale * (a + (b - a) * q->fz);
}

float rgb_uplift_eval(v3 rgb, float lambda_nm) {
    uplift_q q;
    uplift_lookup(rgb, &q);
    return uplift_q_eval(&q, lambda_nm);
}

void swl_init_rgb(swl *w) {
    int i;
    memset(w, 0, sizeof *w);
    for (i = 0; i < NORA_NWL; i++) w->lambda[i] = 0.0f;
    w->spectral = 0;
}

void swl_sample(swl *w, float u) {
    const float range = NORA_LAMBDA_MAX - NORA_LAMBDA_MIN;
    int i;
    w->spectral = 1;
    w->secondary_dead = 0;
    for (i = 0; i < NORA_NWL; i++) {
        float l = u * range + (float)i * range / (float)NORA_NWL;
        if (l >= range) l -= range;
        w->lambda[i] = NORA_LAMBDA_MIN + l;
    }
}

void swl_terminate_secondary(swl *w, sp4 *beta) {
    int i;
    if (!w->spectral || w->secondary_dead) return;
    w->secondary_dead = 1;
    for (i = 1; i < NORA_NWL; i++) beta->v[i] = 0.0f;
    beta->v[0] *= (float)NORA_NWL;
}

sp4 sp_from_rgb(const swl *w, v3 rgb) {
    sp4 r;
    int i;
    if (!w->spectral) { r.v[0] = rgb.x; r.v[1] = rgb.y; r.v[2] = rgb.z; r.v[3] = 0; return r; }
    {
        uplift_q q;
        uplift_lookup(rgb, &q);
        for (i = 0; i < NORA_NWL; i++) r.v[i] = uplift_q_eval(&q, w->lambda[i]);
    }
    return r;
}

sp4 sp_from_rgb_unbounded(const swl *w, v3 rgb) {
    float m = v3_maxc(rgb);
    sp4 r;
    if (!w->spectral) { r.v[0] = rgb.x; r.v[1] = rgb.y; r.v[2] = rgb.z; r.v[3] = 0; return r; }
    if (m <= 0.0f) return sp_const(0.0f);
    if (m <= 1.0f) return sp_from_rgb(w, rgb);
    r = sp_from_rgb(w, v3_scale(rgb, 1.0f / m));
    return sp_scale(r, m);
}

sp4 sp_from_rgb_illum(const swl *w, v3 rgb) {
    sp4 r = sp_from_rgb_unbounded(w, rgb);
    int i;
    if (!w->spectral) return r;
    for (i = 0; i < NORA_NWL; i++) r.v[i] *= illuminant_white(w->lambda[i]);
    return r;
}

v3 sp_to_rgb(const swl *w, sp4 s) {
    v3 xyz = { 0, 0, 0 }, rgb;
    int i, n;
    float inv_pdf, k;
    if (!w->spectral) return v3_make(s.v[0], s.v[1], s.v[2]);
    inv_pdf = NORA_LAMBDA_MAX - NORA_LAMBDA_MIN;   /* uniform wavelength pdf */
    n = NORA_NWL;
    k = inv_pdf / (float)n / g_white_y;
    for (i = 0; i < n; i++) {
        if (s.v[i] == 0.0f) continue;
        xyz = v3_add(xyz, v3_scale(cmf_eval(w->lambda[i]), s.v[i] * k));
    }
    rgb = xyz_to_linear_srgb(xyz);
    return v3_make(rgb.x / g_white_rgb.x, rgb.y / g_white_rgb.y, rgb.z / g_white_rgb.z);
}

v3 spectrum_roundtrip_rgb(v3 rgb) {
    v3 xyz = { 0, 0, 0 }, out;
    int k;
    uplift_q q;
    if (!g_ready) spectrum_init();
    uplift_lookup(rgb, &q);
    for (k = 0; k < NORA_CMF_COUNT; k++) {
        float lam = NORA_LAMBDA_MIN + (float)k;
        float v = uplift_q_eval(&q, lam);
        xyz = v3_add(xyz, v3_scale(v3_make(nora_cie1931_xyz[k][0], nora_cie1931_xyz[k][1], nora_cie1931_xyz[k][2]),
                                   v * illuminant_white(lam) / g_white_y));
    }
    out = xyz_to_linear_srgb(xyz);
    return v3_make(out.x / g_white_rgb.x, out.y / g_white_rgb.y, out.z / g_white_rgb.z);
}
