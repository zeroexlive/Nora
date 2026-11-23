/* Nora - core tests: spectrum, BSDFs, BVH, OBJ import, rendering. MIT License. */
#include "test.h"
#include "spectrum.h"
#include "bsdf.h"
#include "scene.h"
#include "render.h"
#include "image.h"
#include <stdlib.h>
#include <string.h>

void test_spectrum(void) {
    static const float cols[][3] = { { 0.5f, 0.5f, 0.5f }, { 1, 1, 1 }, { 0.8f, 0.2f, 0.1f }, { 0.1f, 0.7f, 0.2f },
                                     { 0.05f, 0.1f, 0.9f }, { 0.63f, 0.065f, 0.05f }, { 0.01f, 0.01f, 0.01f }, { 0.9f, 0.9f, 0.1f } };
    int i;
    swl w;
    pcg32 g;
    v3 acc = { 0, 0, 0 }, white;
    spectrum_init();
    for (i = 0; i < (int)(sizeof cols / sizeof cols[0]); i++) {
        v3 c = v3_make(cols[i][0], cols[i][1], cols[i][2]);
        v3 r = spectrum_roundtrip_rgb(c);
        CHECK_NEAR(r.x, c.x, 0.01); CHECK_NEAR(r.y, c.y, 0.01); CHECK_NEAR(r.z, c.z, 0.01);
    }
    /* uplifted reflectances stay inside [0,1] */
    for (i = 0; i < 200; i++) {
        float l = NORA_LAMBDA_MIN + (NORA_LAMBDA_MAX - NORA_LAMBDA_MIN) * (float)i / 199.0f;
        float v = rgb_uplift_eval(v3_make(0.9f, 0.05f, 0.3f), l);
        CHECK(v >= 0.0f && v <= 1.0f);
    }
    CHECK_NEAR(cmf_eval(555.0f).y, 1.0, 0.01);
    CHECK_NEAR(cmf_eval(359.0f).y, 0.0, 1e-9);
    white = xyz_to_linear_srgb(v3_make(0.95047f, 1.0f, 1.08883f));
    CHECK_NEAR(white.x, 1.0, 0.01); CHECK_NEAR(white.y, 1.0, 0.01); CHECK_NEAR(white.z, 1.0, 0.01);
    /* Monte Carlo: the white illuminant integrates to rgb (1,1,1) */
    pcg32_seed(&g, 7, 3);
    for (i = 0; i < 100000; i++) {
        swl_sample(&w, pcg32_float(&g));
        acc = v3_add(acc, sp_to_rgb(&w, sp_from_rgb_illum(&w, v3_make(1, 1, 1))));
    }
    acc = v3_scale(acc, 1.0f / 100000.0f);
    CHECK_NEAR(acc.x, 1.0, 0.02); CHECK_NEAR(acc.y, 1.0, 0.02); CHECK_NEAR(acc.z, 1.0, 0.02);
    /* hero wavelengths are spread over the range */
    swl_sample(&w, 0.9f);
    for (i = 0; i < NORA_NWL; i++) CHECK(w.lambda[i] >= NORA_LAMBDA_MIN && w.lambda[i] < NORA_LAMBDA_MAX);
    {
        sp4 beta = sp_const(1.0f);
        swl_terminate_secondary(&w, &beta);
        CHECK_NEAR(beta.v[0], 4.0, 1e-6);
        CHECK_NEAR(beta.v[1], 0.0, 1e-6);
    }
}

static nora_bsdf one_lobe(int type, float alpha, float eta, int mode) {
    nora_bsdf b;
    memset(&b, 0, sizeof b);
    b.n = 1;
    b.sel[0] = 1.0f;
    b.ng = v3_make(0, 0, 1);
    b.l[0].type = type;
    b.l[0].w = sp_const(1.0f);
    b.l[0].N = v3_make(0, 0, 1);
    b.l[0].alpha = alpha;
    b.l[0].eta = eta;
    b.l[0].mode = mode;
    b.l[0].two_sided = mode != DIEL_BOTH && mode != DIEL_TRANSMIT;
    return b;
}

void test_bsdf(void) {
    pcg32 g;
    int i, k;
    CHECK_NEAR(nora_fresnel_dielectric(1.0f, 1.5f), 0.04, 1e-4);
    CHECK_NEAR(nora_fresnel_dielectric(-0.1f, 1.5f), 1.0, 1e-6);   /* total internal reflection inside */
    CHECK_NEAR(nora_fresnel_dielectric(0.3f, 0.0f), 1.0, 1e-6);    /* eta 0: perfect mirror */
    {
        float n, kk;
        nora_gulbrandsen(0.9f, 0.95f, &n, &kk);
        CHECK_NEAR(nora_fresnel_conductor(1.0f, n, kk), 0.9, 0.01);
        nora_gulbrandsen(0.3f, 0.5f, &n, &kk);
        CHECK_NEAR(nora_fresnel_conductor(1.0f, n, kk), 0.3, 0.01);
    }
    /* GGX D is normalised: integral of D(m) cos(theta_m) over the hemisphere = 1 */
    pcg32_seed(&g, 1, 1);
    for (k = 0; k < 3; k++) {
        float alpha = k == 0 ? 0.1f : (k == 1 ? 0.4f : 0.9f);
        double sum = 0.0;
        int N = 400000;
        for (i = 0; i < N; i++) {
            float u1 = pcg32_float(&g), u2 = pcg32_float(&g);
            float z = u1, r = sqrtf(1.0f - z * z), phi = 2.0f * NORA_PI * u2;
            v3 m = v3_make(r * cosf(phi), r * sinf(phi), z);
            sum += nora_ggx_D(alpha, m) * m.z * 2.0 * NORA_PI;   /* uniform hemisphere pdf 1/(2pi) */
        }
        CHECK_NEAR(sum / N, 1.0, 0.05);
    }
    /* white furnace: a white diffuse lobe returns weight 1 */
    {
        nora_bsdf b = one_lobe(LOBE_DIFFUSE, 0, 0, DIEL_REFLECT);
        double sum = 0;
        int ok = 0;
        v3 wo = v3_norm(v3_make(0.3f, 0.2f, 0.9f));
        b.l[0].two_sided = 1;
        for (i = 0; i < 20000; i++) {
            nora_bsdf_sample s;
            if (nora_bsdf_sample_dir(&b, wo, pcg32_float(&g), pcg32_float(&g), pcg32_float(&g), pcg32_float(&g), &s)) { sum += s.weight.v[0]; ok++; }
        }
        CHECK(ok == 20000);
        CHECK_NEAR(sum / 20000.0, 1.0, 1e-3);
    }
    /* rough dielectric: sampled weights agree with eval/pdf, energy <= 1 (in radiance units, with eta^2) */
    {
        nora_bsdf b = one_lobe(LOBE_DIELECTRIC, 0.3f, 1.5f, DIEL_BOTH);
        v3 wo = v3_norm(v3_make(0.4f, -0.1f, 0.8f));
        int bad = 0, n = 0;
        double energy = 0;
        for (i = 0; i < 20000; i++) {
            nora_bsdf_sample s;
            float pdf;
            sp4 f;
            if (!nora_bsdf_sample_dir(&b, wo, pcg32_float(&g), pcg32_float(&g), pcg32_float(&g), pcg32_float(&g), &s)) continue;
            n++;
            f = nora_bsdf_eval(&b, wo, s.wi, &pdf);
            if (pdf <= 0 || fabsf(f.v[0] / pdf - s.weight.v[0]) > 1e-3f * (1.0f + s.weight.v[0])) bad++;
            /* undo the 1/eta^2 radiance scaling for transmission so the throughput is <= 1 */
            energy += s.weight.v[0] * (s.wi.z < 0 ? 1.5f * 1.5f : 1.0f);
        }
        CHECK(bad == 0);
        CHECK(n > 15000);
        CHECK(energy / 20000.0 <= 1.02);
        CHECK(energy / 20000.0 >= 0.85);
    }
    /* rough conductor: weights finite, energy <= 1 */
    {
        nora_bsdf b = one_lobe(LOBE_CONDUCTOR, 0.5f, 0, DIEL_REFLECT);
        double energy = 0;
        v3 wo = v3_norm(v3_make(-0.5f, 0.1f, 0.6f));
        for (k = 0; k < NORA_NWL; k++) { b.l[0].cond_n.v[k] = 0.2f; b.l[0].cond_k.v[k] = 3.0f; }
        b.l[0].two_sided = 1;
        for (i = 0; i < 20000; i++) {
            nora_bsdf_sample s;
            if (nora_bsdf_sample_dir(&b, wo, pcg32_float(&g), pcg32_float(&g), pcg32_float(&g), pcg32_float(&g), &s)) energy += s.weight.v[0];
        }
        CHECK(energy / 20000.0 <= 1.0);
        CHECK(energy / 20000.0 > 0.6);
    }
    /* smooth glass: reflect + refract probabilities follow Fresnel and weights are 1 and 1/eta^2 */
    {
        nora_bsdf b = one_lobe(LOBE_DIELECTRIC, 0.0f, 1.5f, DIEL_BOTH);
        nora_bsdf_sample s;
        CHECK(nora_bsdf_sample_dir(&b, v3_make(0, 0, 1), 0.5f, 0.5f, 0.5f, 0.01f, &s));
        CHECK(s.delta && s.wi.z > 0.99f);
        CHECK_NEAR(s.weight.v[0], 1.0, 1e-4);
        CHECK(nora_bsdf_sample_dir(&b, v3_make(0, 0, 1), 0.5f, 0.5f, 0.5f, 0.9f, &s));
        CHECK(s.delta && s.wi.z < -0.99f);
        CHECK_NEAR(s.weight.v[0], 1.0 / 2.25, 1e-4);
    }
}

void test_bvh(void) {
    nora_scene *s = nora_scene_create();
    pcg32 g;
    int i, mism = 0, hits = 0;
    nora_material m;
    nora_material_default(&m, "m");
    nora_scene_add_material(s, &m);
    pcg32_seed(&g, 42, 1);
    for (i = 0; i < 2000; i++) {
        v3 c = v3_make(pcg32_float(&g) * 10 - 5, pcg32_float(&g) * 10 - 5, pcg32_float(&g) * 10 - 5);
        nora_tri t;
        int k;
        memset(&t, 0, sizeof t);
        for (k = 0; k < 3; k++) {
            t.v[k] = nora_scene_add_pos(s, v3_add(c, v3_make(pcg32_float(&g) - 0.5f, pcg32_float(&g) - 0.5f, pcg32_float(&g) - 0.5f)));
            t.n[k] = t.t[k] = -1;
        }
        nora_scene_add_tri(s, &t);
    }
    CHECK(nora_scene_build(s) == 0);
    CHECK(s->nnodes > 1);
    for (i = 0; i < 3000; i++) {
        ray3 r;
        nora_hit h;
        float best = NORA_INFF;
        int j, got;
        r.o = v3_make(pcg32_float(&g) * 16 - 8, pcg32_float(&g) * 16 - 8, pcg32_float(&g) * 16 - 8);
        r.d = v3_norm(v3_make(pcg32_float(&g) - 0.5f, pcg32_float(&g) - 0.5f, pcg32_float(&g) - 0.5f));
        r.tmin = 0; r.tmax = NORA_INFF;
        for (j = 0; j < s->ntris; j++) {   /* brute force Moller-Trumbore */
            v3 p0 = s->pos[s->tris[j].v[0]], e1 = v3_sub(s->pos[s->tris[j].v[1]], p0), e2 = v3_sub(s->pos[s->tris[j].v[2]], p0);
            v3 pv = v3_cross(r.d, e2), tv, qv;
            float det = v3_dot(e1, pv), inv, u, v, t;
            if (fabsf(det) < 1e-12f) continue;
            inv = 1.0f / det;
            tv = v3_sub(r.o, p0);
            u = v3_dot(tv, pv) * inv;
            if (u < 0 || u > 1) continue;
            qv = v3_cross(tv, e1);
            v = v3_dot(r.d, qv) * inv;
            if (v < 0 || u + v > 1) continue;
            t = v3_dot(e2, qv) * inv;
            if (t > 0 && t < best) best = t;
        }
        got = nora_intersect(s, &r, &h);
        if (best < NORA_INFF) hits++;
        if ((best < NORA_INFF) != got || (got && fabsf(h.t - best) > 1e-3f)) mism++;
        CHECK(nora_occluded(s, &r) == (best < NORA_INFF));
    }
    CHECK(mism == 0);
    CHECK(hits > 100);
    nora_scene_destroy(s);
}

void test_obj(void) {
    FILE *f = fopen("nora_test_obj.obj", "w");
    char err[256];
    nora_scene *s;
    int gl, cu, lt, pl, df;
    CHECK(f != NULL);
    if (!f) return;
    fputs("# test\nmtllib nora_test_obj.mtl\no Box\n"
          "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nv 0 0 1\nv 1 0 1\nv 1 1 1\nv 0 1 1\n"
          "vn 0 0 -1\nvn 0 0 1\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
          "usemtl Glass\nf 1/1/1 2/2/1 3/3/1 4/4/1\n"          /* quad, all indices */
          "usemtl Copper\nf -4//2 -3//2 -2//2 -1//2\n"         /* negative indices, v//vn */
          "g Lamp\nusemtl Lamp\nf 1 2 6\n"                     /* positions only */
          "usemtl Plastic\nf 4/1 3/2 7/3 8/4\n"                /* v/vt */
          "usemtl Unknown\nf 1 5 8 4 3\n", f);                 /* pentagon, undefined material */
    fclose(f);
    f = fopen("nora_test_obj.mtl", "w");
    fputs("newmtl Glass\nKd 0 0 0\nNi 1.52\nd 0.1\nillum 7\nnora_abbe 40\n\n"
          "newmtl Copper\nKs 0.95 0.64 0.54\nPm 1\nPr 0.3\n\n"
          "newmtl Lamp\nKd 0.8 0.8 0.8\nKe 4 4 4\n\n"
          "newmtl Plastic\nKd 0.2 0.3 0.8\nKs 0.5 0.5 0.5\nNs 200\nillum 2\n", f);
    fclose(f);
    s = nora_scene_load_obj("nora_test_obj.obj", err, sizeof err);
    CHECK(s != NULL);
    if (!s) { printf("  %s\n", err); return; }
    CHECK(s->npos == 8 + 8);   /* objects own their vertices: "Lamp" gets its own copies of the 8 it shares with "Box" */
    CHECK(s->nnrm == 2);
    CHECK(s->nuv == 4);
    CHECK(s->ntris == 2 + 2 + 1 + 2 + 3);
    gl = nora_scene_find_material(s, "Glass");
    cu = nora_scene_find_material(s, "Copper");
    lt = nora_scene_find_material(s, "Lamp");
    pl = nora_scene_find_material(s, "Plastic");
    df = nora_scene_find_material(s, "Unknown");
    CHECK(gl > 0 && cu > 0 && lt > 0 && pl > 0 && df > 0);
    if (gl > 0) { CHECK(s->mats[gl].type == NORA_MAT_DIELECTRIC); CHECK_NEAR(s->mats[gl].ior, 1.52, 1e-5); CHECK_NEAR(s->mats[gl].abbe, 40, 1e-5); }
    if (cu > 0) { CHECK(s->mats[cu].type == NORA_MAT_CONDUCTOR); CHECK_NEAR(s->mats[cu].roughness, 0.3, 1e-5); CHECK_NEAR(s->mats[cu].color.x, 0.95, 1e-5); }
    if (lt > 0) { CHECK(s->mats[lt].emission_strength > 0); CHECK_NEAR(s->mats[lt].emission.x, 4, 1e-5); }
    if (pl > 0) CHECK(s->mats[pl].type == NORA_MAT_PLASTIC);
    if (df > 0) CHECK(s->mats[df].type == NORA_MAT_DIFFUSE);
    CHECK(s->nmeshes == 2);   /* "Box" and "Lamp" */
    CHECK(nora_scene_build(s) == 0);
    CHECK(s->nlights == 1);
    {
        /* every triangle of the quad uses valid normal and uv indices */
        int i, ok = 1;
        for (i = 0; i < s->ntris; i++) {
            int k;
            for (k = 0; k < 3; k++) {
                if (s->tris[i].v[k] < 0 || s->tris[i].v[k] >= s->npos) ok = 0;
                if (s->tris[i].n[k] >= s->nnrm || s->tris[i].t[k] >= s->nuv) ok = 0;
            }
        }
        CHECK(ok);
    }
    nora_scene_destroy(s);
    CHECK(nora_scene_load_obj("does_not_exist.obj", err, sizeof err) == NULL);
    remove("nora_test_obj.obj");
    remove("nora_test_obj.mtl");
}

static double mean_lum(const float *img, int n) {
    double s = 0;
    int i;
    for (i = 0; i < n; i++) s += 0.2126 * img[i * 3] + 0.7152 * img[i * 3 + 1] + 0.0722 * img[i * 3 + 2];
    return s / n;
}
static int has_nan(const float *img, int n) {
    int i;
    for (i = 0; i < n * 3; i++) if (!(img[i] == img[i])) return 1;
    return 0;
}

void test_render(void) {
    nora_scene *s = nora_scene_cornell(0);
    nora_settings st;
    float *a, *b, *c, *d;
    double la, lb, lc, ld;
    int W = 32, H = 32;
    nora_render_stats rs;
    CHECK(s && nora_scene_build(s) == 0);
    CHECK(s->nlights == 2);
    a = (float *)malloc(sizeof(float) * 3 * W * H);
    b = (float *)malloc(sizeof(float) * 3 * W * H);
    c = (float *)malloc(sizeof(float) * 3 * W * H);
    d = (float *)malloc(sizeof(float) * 3 * W * H);
    nora_settings_default(&st);
    st.width = W; st.height = H; st.threads = 2;
    st.integrator = NORA_INT_PT; st.spp = 64; st.mode = NORA_MODE_RGB;
    CHECK(nora_render_blocking(s, &st, a, &rs) == 0);
    CHECK(rs.samples == (int64_t)W * H * 64);
    st.mode = NORA_MODE_SPECTRAL;
    CHECK(nora_render_blocking(s, &st, b, NULL) == 0);
    st.integrator = NORA_INT_MLT; st.spp = 64; st.bootstrap = 20000; st.mode = NORA_MODE_RGB;
    CHECK(nora_render_blocking(s, &st, c, &rs) == 0);
    CHECK(rs.acceptance > 0.05 && rs.acceptance < 0.99);
    CHECK(rs.b > 0);
    st.mode = NORA_MODE_SPECTRAL;
    CHECK(nora_render_blocking(s, &st, d, NULL) == 0);
    la = mean_lum(a, W * H); lb = mean_lum(b, W * H); lc = mean_lum(c, W * H); ld = mean_lum(d, W * H);
    printf("  mean luminance: pt rgb %.4f, pt spectral %.4f, mlt rgb %.4f, mlt spectral %.4f\n", la, lb, lc, ld);
    CHECK(la > 0.05 && la < 1.0);
    CHECK(fabs(lb - la) / la < 0.05);
    CHECK(fabs(lc - la) / la < 0.08);
    CHECK(fabs(ld - la) / la < 0.08);
    CHECK(!has_nan(a, W * H) && !has_nan(b, W * H) && !has_nan(c, W * H) && !has_nan(d, W * H));
    /* the path tracer is deterministic for a fixed seed: samples depend only on (seed, pixel, pass);
     * threads may add passes to a pixel in a different order, so compare up to float rounding */
    st.integrator = NORA_INT_PT; st.spp = 4; st.mode = NORA_MODE_RGB;
    nora_render_blocking(s, &st, a, NULL);
    nora_render_blocking(s, &st, b, NULL);
    {
        int k, diff = 0;
        for (k = 0; k < 3 * W * H; k++) if (fabsf(a[k] - b[k]) > 1e-4f * (1.0f + fabsf(a[k]))) diff++;
        CHECK(diff == 0);
    }
    /* async API: start, stop, resolve */
    {
        nora_render *r;
        st.integrator = NORA_INT_MLT; st.spp = 100000; st.bootstrap = 5000;
        r = nora_render_create(s, &st);
        CHECK(r != NULL);
        CHECK(nora_render_start(r) == 0);
        while (nora_render_progress(r) <= 0.0f && nora_render_is_running(r)) { }
        nora_render_request_stop(r);
        nora_render_wait(r);
        CHECK(!nora_render_is_running(r));
        CHECK(nora_render_resolve(r, a) == 0);
        nora_render_destroy(r);
    }
    nora_scene_destroy(s);
    /* dispersive glass in spectral mode */
    s = nora_scene_cornell(1);
    CHECK(s && nora_scene_build(s) == 0);
    st.integrator = NORA_INT_PT; st.spp = 16; st.mode = NORA_MODE_SPECTRAL;
    CHECK(nora_render_blocking(s, &st, a, NULL) == 0);
    CHECK(!has_nan(a, W * H));
    CHECK(mean_lum(a, W * H) > 0.05);
    /* image writers */
    CHECK(nora_write_image("nora_test.png", a, W, H, 0, NORA_TONEMAP_CLAMP) == 0);
    CHECK(nora_write_image("nora_test.exr", a, W, H, 0, NORA_TONEMAP_CLAMP) == 0);
    CHECK(nora_write_image("nora_test.pfm", a, W, H, 0, NORA_TONEMAP_CLAMP) == 0);
    {
        int w = 0, h = 0;
        float *back = nora_load_image_linear("nora_test.png", &w, &h);
        CHECK(back && w == W && h == H);
        free(back);
    }
    {
        FILE *f = fopen("nora_test.exr", "rb");
        unsigned char m[4] = { 0, 0, 0, 0 };
        CHECK(f != NULL);
        if (f) { CHECK(fread(m, 1, 4, f) == 4); fclose(f); }
        CHECK(m[0] == 0x76 && m[1] == 0x2f && m[2] == 0x31 && m[3] == 0x01);
    }
    remove("nora_test.png"); remove("nora_test.exr"); remove("nora_test.pfm");
    nora_scene_destroy(s);
    free(a); free(b); free(c); free(d);
}
