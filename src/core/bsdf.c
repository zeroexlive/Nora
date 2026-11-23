/* Nora - BSDF lobes and shading. MIT License.
 * GGX with visible normal sampling (Heitz 2018); rough dielectric after
 * Walter et al. 2007 in the formulation of pbrt-v4; conductor colours via
 * Gulbrandsen 2014 "artist friendly metallic Fresnel". */
#include "bsdf.h"
#include "../osl/osl.h"
#ifdef NORA_WITH_OSL
#include "osl_real.h"
#endif
#include <string.h>

float nora_fresnel_dielectric(float cos_i, float eta) {
    float sin2_i, sin2_t, cos_t, rpar, rper;
    if (eta <= 0.0f) return 1.0f;
    cos_i = nclampf(cos_i, -1.0f, 1.0f);
    if (cos_i < 0.0f) { eta = 1.0f / eta; cos_i = -cos_i; }
    sin2_i = 1.0f - cos_i * cos_i;
    sin2_t = sin2_i / (eta * eta);
    if (sin2_t >= 1.0f) return 1.0f;
    cos_t = nsafe_sqrt(1.0f - sin2_t);
    rpar = (eta * cos_i - cos_t) / (eta * cos_i + cos_t);
    rper = (cos_i - eta * cos_t) / (cos_i + eta * cos_t);
    return 0.5f * (rpar * rpar + rper * rper);
}

float nora_fresnel_conductor(float c, float n, float k) {
    float cos2, sin2, n2, k2, t0, a2b2, t1, a, t2, rs, t3, t4, rp;
    c = nclampf(fabsf(c), 0.0f, 1.0f);
    cos2 = c * c; sin2 = 1.0f - cos2;
    n2 = n * n; k2 = k * k;
    t0 = n2 - k2 - sin2;
    a2b2 = nsafe_sqrt(t0 * t0 + 4.0f * n2 * k2);
    t1 = a2b2 + cos2;
    a = nsafe_sqrt(0.5f * (a2b2 + t0));
    t2 = 2.0f * c * a;
    rs = (t1 - t2) / (t1 + t2);
    t3 = cos2 * a2b2 + sin2 * sin2;
    t4 = t2 * sin2;
    rp = rs * (t3 - t4) / (t3 + t4);
    return nclampf(0.5f * (rp + rs), 0.0f, 1.0f);
}

void nora_gulbrandsen(float r, float g, float *n, float *k) {
    float sr, nn, k2;
    r = nclampf(r, 0.0f, 0.99f);
    g = nclampf(g, 0.0f, 1.0f);
    sr = sqrtf(r);
    nn = g * (1.0f - r) / (1.0f + r) + (1.0f - g) * (1.0f + sr) / (1.0f - sr);
    k2 = (r * (nn + 1.0f) * (nn + 1.0f) - (nn - 1.0f) * (nn - 1.0f)) / (1.0f - r);
    *n = nn;
    *k = nsafe_sqrt(k2);
}

float nora_ggx_D(float alpha, v3 m) {
    float a2, d;
    if (m.z <= 0.0f) return 0.0f;
    a2 = alpha * alpha;
    d = m.z * m.z * (a2 - 1.0f) + 1.0f;
    return a2 / (NORA_PI * d * d);
}

static float ggx_lambda(float alpha, v3 w) {
    float c2 = w.z * w.z, s2 = fmaxf(0.0f, 1.0f - c2), t2;
    if (c2 <= 0.0f) return 1e8f;
    t2 = s2 / c2;
    return 0.5f * (-1.0f + sqrtf(1.0f + alpha * alpha * t2));
}
float nora_ggx_G1(float alpha, v3 w) { return 1.0f / (1.0f + ggx_lambda(alpha, w)); }
static float ggx_G(float alpha, v3 wo, v3 wi) { return 1.0f / (1.0f + ggx_lambda(alpha, wo) + ggx_lambda(alpha, wi)); }

/* visible normal sampling for w in the upper hemisphere (w.z may be negative: mirrored) */
static v3 ggx_sample_wm(float alpha, v3 w, float u1, float u2) {
    v3 wh = v3_norm(v3_make(alpha * w.x, alpha * w.y, w.z)), T1, T2, nh;
    float r, phi, t1, t2, s, len2;
    if (wh.z < 0.0f) wh = v3_neg(wh);
    len2 = wh.x * wh.x + wh.y * wh.y;
    T1 = len2 > 1e-12f ? v3_scale(v3_make(-wh.y, wh.x, 0.0f), 1.0f / sqrtf(len2)) : v3_make(1, 0, 0);
    T2 = v3_cross(wh, T1);
    r = sqrtf(u1);
    phi = 2.0f * NORA_PI * u2;
    t1 = r * cosf(phi);
    t2 = r * sinf(phi);
    s = 0.5f * (1.0f + wh.z);
    t2 = (1.0f - s) * sqrtf(fmaxf(0.0f, 1.0f - t1 * t1)) + s * t2;
    nh = v3_add(v3_add(v3_scale(T1, t1), v3_scale(T2, t2)), v3_scale(wh, nsafe_sqrt(1.0f - t1 * t1 - t2 * t2)));
    return v3_norm(v3_make(alpha * nh.x, alpha * nh.y, fmaxf(1e-6f, nh.z)));
}
/* pdf of the visible normal wm seen from w */
static float ggx_pdf_wm(float alpha, v3 w, v3 wm) {
    float cz = fabsf(w.z);
    if (cz <= 0.0f) return 0.0f;
    return nora_ggx_G1(alpha, w) / cz * nora_ggx_D(alpha, wm) * fabsf(v3_dot(w, wm));
}

static v3 reflect_about(v3 wo, v3 n) { return v3_sub(v3_scale(n, 2.0f * v3_dot(wo, n)), wo); }
static int refract_dir(v3 wi, v3 n, float eta, float *etap, v3 *wt) {
    float c = v3_dot(n, wi), s2, s2t, ct;
    if (c < 0.0f) { eta = 1.0f / eta; c = -c; n = v3_neg(n); }
    s2 = fmaxf(0.0f, 1.0f - c * c);
    s2t = s2 / (eta * eta);
    if (s2t >= 1.0f) return 0;
    ct = nsafe_sqrt(1.0f - s2t);
    *wt = v3_add(v3_scale(v3_neg(wi), 1.0f / eta), v3_scale(n, c / eta - ct));
    *etap = eta;
    return 1;
}

static v3 sample_cosine(float u1, float u2) {
    float r1 = 2.0f * u1 - 1.0f, r2 = 2.0f * u2 - 1.0f, r, th, x, y;
    if (r1 == 0.0f && r2 == 0.0f) return v3_make(0, 0, 1);
    if (fabsf(r1) > fabsf(r2)) { r = r1; th = (NORA_PI / 4.0f) * (r2 / r1); }
    else { r = r2; th = NORA_PI / 2.0f - (NORA_PI / 4.0f) * (r1 / r2); }
    x = r * cosf(th); y = r * sinf(th);
    return v3_make(x, y, nsafe_sqrt(1.0f - x * x - y * y));
}

static sp4 cond_fresnel(const nora_lobe *l, float c) {
    sp4 f;
    int i;
    for (i = 0; i < NORA_NWL; i++) f.v[i] = nora_fresnel_conductor(c, l->cond_n.v[i], l->cond_k.v[i]);
    return f;
}

static frame3 lobe_frame(const nora_lobe *l, v3 wo) {
    v3 n = l->N;
    if (l->two_sided && v3_dot(n, wo) < 0.0f) n = v3_neg(n);
    return frame_from(n);
}

static void diel_probs(const nora_lobe *l, float F, float *pr, float *pt) {
    *pr = F; *pt = 1.0f - F;
    if (l->mode == DIEL_REFLECT) *pt = 0.0f;
    if (l->mode == DIEL_TRANSMIT) *pr = 0.0f;
}

/* returns f * |cos(wi)| in the lobe's frame */
static sp4 lobe_eval(const nora_lobe *l, v3 wo, v3 wi, float *pdf) {
    *pdf = 0.0f;
    switch (l->type) {
    case LOBE_DIFFUSE:
        if (wo.z <= 0.0f || wi.z <= 0.0f) return sp_const(0);
        *pdf = wi.z * NORA_INV_PI;
        return sp_scale(l->w, wi.z * NORA_INV_PI);
    case LOBE_CONDUCTOR: {
        v3 wm;
        float D, G;
        if (l->alpha < 1e-4f || wo.z <= 0.0f || wi.z <= 0.0f) return sp_const(0);
        wm = v3_norm(v3_add(wo, wi));
        D = nora_ggx_D(l->alpha, wm);
        G = ggx_G(l->alpha, wo, wi);
        *pdf = ggx_pdf_wm(l->alpha, wo, wm) / (4.0f * fabsf(v3_dot(wo, wm)));
        return sp_mul(l->w, sp_scale(cond_fresnel(l, v3_dot(wo, wm)), D * G / (4.0f * wo.z)));
    }
    case LOBE_DIELECTRIC: {
        float co = wo.z, ci = wi.z, etap = 1.0f, F, pr, pt, eta = l->eta;
        int reflect = co * ci > 0.0f;
        v3 wm;
        if (l->alpha < 1e-4f || eta == 1.0f) return sp_const(0);
        if (eta <= 0.0f && !reflect) return sp_const(0);
        if (!reflect) etap = co > 0.0f ? eta : 1.0f / eta;
        wm = v3_add(v3_scale(wi, etap), wo);
        if (ci == 0.0f || co == 0.0f || v3_dot(wm, wm) == 0.0f) return sp_const(0);
        wm = v3_norm(wm);
        if (wm.z < 0.0f) wm = v3_neg(wm);
        if (v3_dot(wm, wi) * ci < 0.0f || v3_dot(wm, wo) * co < 0.0f) return sp_const(0);
        F = nora_fresnel_dielectric(v3_dot(wo, wm), eta);
        diel_probs(l, F, &pr, &pt);
        if (pr + pt <= 0.0f) return sp_const(0);
        if (reflect) {
            float D = nora_ggx_D(l->alpha, wm), G = ggx_G(l->alpha, wo, wi);
            if (pr <= 0.0f) return sp_const(0);
            *pdf = ggx_pdf_wm(l->alpha, wo, wm) / (4.0f * fabsf(v3_dot(wo, wm))) * pr / (pr + pt);
            return sp_scale(l->w, D * G * F / fabsf(4.0f * co));
        } else {
            float D = nora_ggx_D(l->alpha, wm), G = ggx_G(l->alpha, wo, wi);
            float dd = v3_dot(wi, wm) + v3_dot(wo, wm) / etap, denom = dd * dd;
            float ft;
            if (pt <= 0.0f || denom <= 0.0f) return sp_const(0);
            *pdf = ggx_pdf_wm(l->alpha, wo, wm) * fabsf(v3_dot(wi, wm)) / denom * pt / (pr + pt);
            ft = D * (1.0f - F) * G * fabsf(v3_dot(wi, wm) * v3_dot(wo, wm) / (co * denom));
            ft /= etap * etap;
            return sp_scale(l->w, ft);
        }
    }
    default:
        return sp_const(0);
    }
}

static int lobe_sample(const nora_lobe *l, v3 wo, float u1, float u2, float u3, v3 *wi, sp4 *fcos, float *pdf, int *delta) {
    *delta = 0;
    switch (l->type) {
    case LOBE_DIFFUSE:
        if (wo.z <= 0.0f) return 0;
        *wi = sample_cosine(u1, u2);
        if (wi->z <= 0.0f) return 0;
        *pdf = wi->z * NORA_INV_PI;
        *fcos = sp_scale(l->w, wi->z * NORA_INV_PI);
        return 1;
    case LOBE_CONDUCTOR:
        if (wo.z <= 0.0f) return 0;
        if (l->alpha < 1e-4f) {
            *wi = v3_make(-wo.x, -wo.y, wo.z);
            *delta = 1;
            *pdf = 1.0f;
            *fcos = sp_mul(l->w, cond_fresnel(l, wo.z));
            return 1;
        } else {
            v3 wm = ggx_sample_wm(l->alpha, wo, u1, u2);
            *wi = reflect_about(wo, wm);
            if (wi->z <= 0.0f) return 0;
            *fcos = lobe_eval(l, wo, *wi, pdf);
            return *pdf > 0.0f;
        }
    case LOBE_DIELECTRIC: {
        float eta = l->eta;
        if (eta == 1.0f && l->mode != DIEL_REFLECT) {   /* index matched: pass straight through */
            *wi = v3_neg(wo); *delta = 1; *pdf = 1.0f; *fcos = l->w; return 1;
        }
        if (l->alpha < 1e-4f) {
            float F = nora_fresnel_dielectric(wo.z, eta), pr, pt;
            diel_probs(l, F, &pr, &pt);
            if (pr + pt <= 0.0f) return 0;
            *delta = 1;
            if (u3 < pr / (pr + pt)) {
                *wi = v3_make(-wo.x, -wo.y, wo.z);
                *pdf = pr / (pr + pt);
                *fcos = sp_scale(l->w, F / *pdf);
                return 1;
            } else {
                float etap;
                v3 t;
                if (!refract_dir(wo, v3_make(0, 0, 1), eta, &etap, &t)) return 0;
                *wi = t;
                *pdf = pt / (pr + pt);
                *fcos = sp_scale(l->w, (1.0f - F) / *pdf / (etap * etap));
                return 1;
            }
        } else {
            v3 wm = ggx_sample_wm(l->alpha, wo, u1, u2);
            float F, pr, pt;
            if (wo.z == 0.0f) return 0;
            F = nora_fresnel_dielectric(v3_dot(wo, wm), eta);
            diel_probs(l, F, &pr, &pt);
            if (pr + pt <= 0.0f) return 0;
            if (u3 < pr / (pr + pt)) {
                *wi = reflect_about(wo, wm);
                if (wi->z * wo.z <= 0.0f) return 0;
            } else {
                float etap;
                v3 t;
                if (!refract_dir(wo, wm, eta, &etap, &t)) return 0;
                if (t.z * wo.z >= 0.0f || t.z == 0.0f) return 0;
                *wi = t;
            }
            *fcos = lobe_eval(l, wo, *wi, pdf);
            return *pdf > 0.0f;
        }
    }
    case LOBE_TRANSPARENT:
        *wi = v3_neg(wo);
        *delta = 1;
        *pdf = 1.0f;
        *fcos = l->w;
        return 1;
    }
    return 0;
}

static int lobe_is_delta(const nora_lobe *l) {
    if (l->type == LOBE_TRANSPARENT) return 1;
    if (l->type == LOBE_CONDUCTOR || l->type == LOBE_DIELECTRIC) return l->alpha < 1e-4f || (l->type == LOBE_DIELECTRIC && l->eta == 1.0f);
    return 0;
}

static int lobe_transmits(const nora_lobe *l) {
    return l->type == LOBE_DIELECTRIC && l->mode != DIEL_REFLECT;
}

int nora_bsdf_has_nondelta(const nora_bsdf *b) {
    int i;
    for (i = 0; i < b->n; i++) if (b->sel[i] > 0.0f && !lobe_is_delta(&b->l[i])) return 1;
    return 0;
}

sp4 nora_bsdf_eval(const nora_bsdf *b, v3 wo, v3 wi, float *pdf) {
    sp4 f = sp_const(0);
    float p = 0.0f;
    int i, geo_reflect = v3_dot(wo, b->ng) * v3_dot(wi, b->ng) > 0.0f;
    for (i = 0; i < b->n; i++) {
        const nora_lobe *l = &b->l[i];
        frame3 fr;
        v3 wol, wil;
        float lp;
        sp4 lf;
        if (b->sel[i] <= 0.0f || lobe_is_delta(l)) continue;
        if (!geo_reflect && !lobe_transmits(l)) continue;
        fr = lobe_frame(l, wo);
        wol = frame_to_local(&fr, wo);
        wil = frame_to_local(&fr, wi);
        if (lobe_transmits(l) && ((wol.z * wil.z > 0.0f) != geo_reflect)) continue;
        lf = lobe_eval(l, wol, wil, &lp);
        f = sp_add(f, lf);
        p += b->sel[i] * lp;
    }
    *pdf = p;
    return f;
}

int nora_bsdf_sample_dir(const nora_bsdf *b, v3 wo, float u0, float u1, float u2, float u3, nora_bsdf_sample *out) {
    int i, pick = -1, delta;
    float acc = 0.0f, lp;
    const nora_lobe *l;
    frame3 fr;
    v3 wol, wil;
    sp4 fcos;
    memset(out, 0, sizeof *out);
    for (i = 0; i < b->n; i++) {
        acc += b->sel[i];
        if (u0 < acc) { pick = i; break; }
    }
    if (pick < 0) {
        for (i = b->n - 1; i >= 0; i--) if (b->sel[i] > 0.0f) { pick = i; break; }
        if (pick < 0) return 0;
    }
    l = &b->l[pick];
    fr = lobe_frame(l, wo);
    wol = frame_to_local(&fr, wo);
    if (!lobe_sample(l, wol, u1, u2, u3, &wil, &fcos, &lp, &delta)) return 0;
    out->wi = frame_to_world(&fr, wil);
    if (delta) {
        out->delta = 1;
        out->transparent = l->type == LOBE_TRANSPARENT || (l->type == LOBE_DIELECTRIC && l->eta == 1.0f);
        out->pdf = b->sel[pick];
        out->weight = sp_scale(fcos, 1.0f / b->sel[pick]);   /* fcos already divided by the lobe's own pdf */
        return 1;
    }
    {
        float pdf;
        sp4 f = nora_bsdf_eval(b, wo, out->wi, &pdf);
        if (pdf <= 0.0f) return 0;
        out->pdf = pdf;
        out->weight = sp_scale(f, 1.0f / pdf);
        if (sp_has_nan(out->weight)) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------- shading */

void nora_isect_fill(const nora_scene *s, const ray3 *r, const nora_hit *h, nora_isect *it) {
    const nora_tri *t = &s->tris[h->tri];
    v3 p0 = s->pos[t->v[0]], p1 = s->pos[t->v[1]], p2 = s->pos[t->v[2]];
    float b0 = 1.0f - h->b1 - h->b2;
    v3 e1 = v3_sub(p1, p0), e2 = v3_sub(p2, p0);
    it->tri = h->tri;
    it->mat = t->mat;
    it->p = v3_add(v3_add(v3_scale(p0, b0), v3_scale(p1, h->b1)), v3_scale(p2, h->b2));
    it->ng = v3_norm(v3_cross(e1, e2));
    if (t->n[0] >= 0 && t->n[1] >= 0 && t->n[2] >= 0) {
        v3 n = v3_add(v3_add(v3_scale(s->nrm[t->n[0]], b0), v3_scale(s->nrm[t->n[1]], h->b1)), v3_scale(s->nrm[t->n[2]], h->b2));
        it->ns = v3_norm(n);
        if (v3_dot(it->ns, it->ng) < 0.0f) it->ng = v3_neg(it->ng);
    } else {
        it->ns = it->ng;
    }
    if (t->t[0] >= 0 && t->t[1] >= 0 && t->t[2] >= 0) {
        const float *a = &s->uv[t->t[0] * 2], *b = &s->uv[t->t[1] * 2], *c = &s->uv[t->t[2] * 2];
        float du1 = b[0] - a[0], du2 = c[0] - a[0], dv1 = b[1] - a[1], dv2 = c[1] - a[1];
        float det = du1 * dv2 - dv1 * du2;
        it->u = a[0] * b0 + b[0] * h->b1 + c[0] * h->b2;
        it->v = a[1] * b0 + b[1] * h->b1 + c[1] * h->b2;
        if (fabsf(det) > 1e-12f) {
            float inv = 1.0f / det;
            it->dpdu = v3_scale(v3_sub(v3_scale(e1, dv2), v3_scale(e2, dv1)), inv);
            it->dpdv = v3_scale(v3_sub(v3_scale(e2, du1), v3_scale(e1, du2)), inv);
        } else {
            onb_from(it->ns, &it->dpdu, &it->dpdv);
        }
    } else {
        it->u = h->b1;
        it->v = h->b2;
        it->dpdu = e1;
        it->dpdv = e2;
    }
    it->front = v3_dot(it->ng, r->d) < 0.0f;
}

static int host_texture(void *user, const char *name, float s, float t, float rgb[3]) {
    const nora_scene *sc = (const nora_scene *)user;
    int i;
    for (i = 0; i < sc->ntexs; i++) {
        size_t lp = strlen(sc->texs[i].path), ln = strlen(name);
        if (strcmp(sc->texs[i].path, name) == 0 || (lp >= ln && strcmp(sc->texs[i].path + lp - ln, name) == 0)) {
            v3 c = nora_texture_sample(&sc->texs[i], s, t);
            rgb[0] = c.x; rgb[1] = c.y; rgb[2] = c.z;
            return 1;
        }
    }
    rgb[0] = 1; rgb[1] = 0; rgb[2] = 1;
    return 0;
}

static void push_lobe(nora_bsdf *b, const nora_lobe *l) {
    if (b->n < NORA_MAX_LOBES) b->l[b->n++] = *l;
}

static float cauchy_ior(float nd, float abbe, float lambda_nm) {
    const float lf = 0.4861f, lc = 0.6563f, ld = 0.5876f;
    float B, A, lm = lambda_nm * 1e-3f;
    if (abbe <= 0.0f) return nd;
    B = (nd - 1.0f) / (abbe * (1.0f / (lf * lf) - 1.0f / (lc * lc)));
    A = nd - B / (ld * ld);
    return A + B / (lm * lm);
}

static void finalize_sel(nora_bsdf *b) {
    float tot = 0.0f;
    int i;
    for (i = 0; i < b->n; i++) {
        float a = sp_max(b->l[i].w);
        b->sel[i] = a > 0.0f ? fmaxf(a, 1e-3f) : 0.0f;
        tot += b->sel[i];
    }
    for (i = 0; i < b->n; i++) b->sel[i] = tot > 0.0f ? b->sel[i] / tot : 0.0f;
}

static void shade_osl(const nora_scene *s, struct osl_exec *ex, const nora_shader_slot *sl, const nora_isect *it, v3 wo,
                      const swl *w, nora_bsdf *out) {
    osl_globals g;
    osl_host host;
    osl_closure_list cl;
    v3 N = it->ns, Ng = it->ng;
    int i;
    if (v3_dot(N, wo) < 0.0f) N = v3_neg(N);
    if (v3_dot(Ng, wo) < 0.0f) Ng = v3_neg(Ng);
    memset(&g, 0, sizeof g);
    g.P[0] = it->p.x; g.P[1] = it->p.y; g.P[2] = it->p.z;
    g.N[0] = N.x; g.N[1] = N.y; g.N[2] = N.z;
    g.Ng[0] = Ng.x; g.Ng[1] = Ng.y; g.Ng[2] = Ng.z;
    g.I[0] = -wo.x; g.I[1] = -wo.y; g.I[2] = -wo.z;
    g.dPdu[0] = it->dpdu.x; g.dPdu[1] = it->dpdu.y; g.dPdu[2] = it->dpdu.z;
    g.dPdv[0] = it->dpdv.x; g.dPdv[1] = it->dpdv.y; g.dPdv[2] = it->dpdv.z;
    g.u = it->u; g.v = it->v;
    g.backfacing = !it->front;
    host.texture = host_texture;
    host.user = (void *)s;
#ifdef NORA_WITH_OSL
    if (sl->real && nora_real_osl_execute(sl->real, &g, &cl) == 0) {
        /* executed by liboslexec */
    } else
#endif
    if (!ex || osl_execute(sl->prog, ex, &g, &host, NULL, 0, &cl) != 0) {
        nora_lobe l;
        memset(&l, 0, sizeof l);
        l.type = LOBE_DIFFUSE; l.w = sp_from_rgb(w, v3_make(1, 0, 1)); l.N = N; l.two_sided = 1;
        push_lobe(out, &l);
        return;
    }
    for (i = 0; i < cl.n; i++) {
        const osl_closure *c = &cl.c[i];
        v3 wrgb = v3_make(c->w[0], c->w[1], c->w[2]);
        nora_lobe l;
        memset(&l, 0, sizeof l);
        l.N = v3_norm(v3_make(c->N[0], c->N[1], c->N[2]));
        if (v3_dot(l.N, l.N) < 0.5f) l.N = N;
        l.w = sp_from_rgb_unbounded(w, wrgb);
        switch (c->type) {
        case OSL_CL_DIFFUSE:
            l.type = LOBE_DIFFUSE; l.two_sided = 1; push_lobe(out, &l); break;
        case OSL_CL_MICROFACET:
            l.type = LOBE_DIELECTRIC;
            l.alpha = fmaxf(c->alpha, 0.0f);
            if (l.alpha > 0.0f && l.alpha < 1e-3f) l.alpha = 1e-3f;
            l.eta = c->eta;
            l.mode = c->refract == 0 ? DIEL_REFLECT : (c->refract == 1 ? DIEL_TRANSMIT : DIEL_BOTH);
            l.two_sided = l.mode == DIEL_REFLECT;
            if (l.mode != DIEL_REFLECT && l.eta <= 0.0f) l.eta = 1.5f;
            push_lobe(out, &l);
            break;
        case OSL_CL_REFLECTION:
            l.type = LOBE_DIELECTRIC; l.alpha = 0.0f; l.eta = c->eta; l.mode = DIEL_REFLECT; l.two_sided = 1;
            push_lobe(out, &l);
            break;
        case OSL_CL_REFRACTION:
            l.type = LOBE_DIELECTRIC; l.alpha = 0.0f; l.eta = c->eta > 0.0f ? c->eta : 1.5f; l.mode = DIEL_TRANSMIT;
            push_lobe(out, &l);
            break;
        case OSL_CL_EMISSION:
            out->Le = sp_add(out->Le, sp_from_rgb_illum(w, wrgb));
            break;
        case OSL_CL_TRANSPARENT:
            l.type = LOBE_TRANSPARENT; push_lobe(out, &l); break;
        default: break;
        }
    }
}

void nora_shade(const nora_scene *s, struct osl_exec *ex, const nora_isect *it, v3 wo, const swl *w, nora_bsdf *out) {
    nora_material def;
    const nora_material *m;
    nora_lobe l;
    v3 base;
    memset(out, 0, sizeof *out);
    out->ng = it->ng;
    if (it->mat >= 0 && it->mat < s->nmats) m = &s->mats[it->mat];
    else { nora_material_default(&def, "default"); m = &def; }
    if (m->shader >= 0 && m->shader < s->nshaders && s->shaders[m->shader].prog) {
        shade_osl(s, ex, &s->shaders[m->shader], it, wo, w, out);
        finalize_sel(out);
        return;
    }
    base = m->color;
    if (m->color_tex >= 0 && m->color_tex < s->ntexs)
        base = v3_mul(base, nora_texture_sample(&s->texs[m->color_tex], it->u, 1.0f - it->v));
    memset(&l, 0, sizeof l);
    l.N = it->ns;
    switch (m->type) {
    case NORA_MAT_DIFFUSE:
    default:
        l.type = LOBE_DIFFUSE; l.two_sided = 1; l.w = sp_from_rgb(w, base);
        push_lobe(out, &l);
        break;
    case NORA_MAT_CONDUCTOR: {
        int i;
        l.type = LOBE_CONDUCTOR; l.two_sided = 1;
        l.alpha = m->roughness * m->roughness;
        l.w = sp_const(1.0f);
        if (!w->spectral) {
            for (i = 0; i < 3; i++) nora_gulbrandsen(v3_get(base, i), v3_get(m->edge_tint, i), &l.cond_n.v[i], &l.cond_k.v[i]);
            l.cond_n.v[3] = 1.0f; l.cond_k.v[3] = 0.0f;
        } else {
            sp4 r = sp_from_rgb(w, base), g = sp_from_rgb(w, m->edge_tint);
            for (i = 0; i < NORA_NWL; i++) nora_gulbrandsen(r.v[i], g.v[i], &l.cond_n.v[i], &l.cond_k.v[i]);
        }
        push_lobe(out, &l);
        break;
    }
    case NORA_MAT_DIELECTRIC:
        l.type = LOBE_DIELECTRIC;
        l.alpha = m->roughness * m->roughness;
        l.mode = DIEL_BOTH;
        l.w = sp_from_rgb(w, base);
        l.eta = m->ior > 0.0f ? m->ior : 1.5f;
        if (w->spectral && m->abbe > 0.0f) {
            l.eta = cauchy_ior(l.eta, m->abbe, w->lambda[0]);
            out->dispersive = 1;
        }
        push_lobe(out, &l);
        break;
    case NORA_MAT_PLASTIC: {
        nora_lobe c;
        float co = fabsf(v3_dot(it->ns, wo));
        float F = nora_fresnel_dielectric(co, m->ior > 0.0f ? m->ior : 1.5f);
        l.type = LOBE_DIFFUSE; l.two_sided = 1; l.w = sp_scale(sp_from_rgb(w, base), 1.0f - F);
        push_lobe(out, &l);
        memset(&c, 0, sizeof c);
        c.type = LOBE_DIELECTRIC; c.N = it->ns; c.two_sided = 1; c.mode = DIEL_REFLECT;
        c.alpha = fmaxf(m->roughness * m->roughness, 0.0f);
        c.eta = m->ior > 0.0f ? m->ior : 1.5f;
        c.w = sp_const(1.0f);
        push_lobe(out, &c);
        break;
    }
    }
    if (m->emission_strength > 0.0f && v3_maxc(m->emission) > 0.0f && (it->front || m->two_sided))
        out->Le = sp_from_rgb_illum(w, v3_scale(m->emission, m->emission_strength));
    finalize_sel(out);
}
