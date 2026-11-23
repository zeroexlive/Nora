/* Nora - path tracing with NEE and Kelemen-style PSSMLT. MIT License.
 *
 * PSSMLT: Kelemen, Szirmay-Kalos, Antal, Csonka, "A Simple and Robust Mutation
 * Strategy for the Metropolis Light Transport Algorithm" (2002). Primary sample
 * vectors are mutated lazily (as in pbrt-v3's MLTSampler), small steps use
 * Kelemen's exponential perturbation (s1 = 1/1024, s2 = 1/64), large steps draw
 * fresh uniform numbers. The normalisation b comes from a bootstrap pass of
 * independent paths, and the initial chain states are resampled from it.
 */
#include "render.h"
#include "bsdf.h"
#include "nthread.h"
#include "../osl/osl.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ------------------------------------------------------------- logging */

static nora_log_fn g_log_fn = NULL;
static void *g_log_user = NULL;
void nora_set_log(nora_log_fn fn, void *user) { g_log_fn = fn; g_log_user = user; }
void nora_logf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_log_fn) g_log_fn(g_log_user, buf);
    else fprintf(stderr, "%s\n", buf);
}

void nora_settings_default(nora_settings *s) {
    memset(s, 0, sizeof *s);
    s->width = 512; s->height = 512;
    s->mode = NORA_MODE_RGB;
    s->integrator = NORA_INT_MLT;
    s->spp = 64;
    s->time_limit = 0.0f;
    s->threads = 0;
    s->max_depth = 10;
    s->p_large = 0.3f;
    s->bootstrap = 100000;
    s->chains = 64;
    s->seed = 1;
}
const char *nora_mode_name(nora_mode m) { return m == NORA_MODE_SPECTRAL ? "spectral" : "rgb"; }
const char *nora_integrator_name(nora_integrator i) { return i == NORA_INT_PT ? "pt" : "mlt"; }

/* ------------------------------------------------------------- camera */

ray3 nora_camera_ray(const nora_camera *c, int w, int h, float px, float py, float lu, float lv) {
    v3 f = v3_norm(v3_sub(c->target, c->pos));
    v3 r = v3_norm(v3_cross(f, c->up));
    v3 u = v3_cross(r, f);
    float th = tanf(0.5f * c->vfov_deg * NORA_PI / 180.0f);
    float aspect = (float)w / (float)h;
    float x = (2.0f * px / (float)w - 1.0f) * th * aspect;
    float y = (1.0f - 2.0f * py / (float)h) * th;
    v3 d = v3_norm(v3_add(f, v3_add(v3_scale(r, x), v3_scale(u, y))));
    ray3 ray;
    ray.o = c->pos;
    ray.d = d;
    ray.tmin = 0.0f;
    ray.tmax = NORA_INFF;
    if (c->aperture > 0.0f) {
        float fd = c->focus_dist > 0.0f ? c->focus_dist : v3_len(v3_sub(c->target, c->pos));
        v3 pf = v3_add(c->pos, v3_scale(d, fd / v3_dot(d, f)));
        float a = 2.0f * lu - 1.0f, b = 2.0f * lv - 1.0f, rr, th2, lx = 0, ly = 0;
        if (a != 0.0f || b != 0.0f) {
            if (fabsf(a) > fabsf(b)) { rr = a; th2 = (NORA_PI / 4.0f) * (b / a); }
            else { rr = b; th2 = NORA_PI / 2.0f - (NORA_PI / 4.0f) * (a / b); }
            lx = rr * cosf(th2) * c->aperture;
            ly = rr * sinf(th2) * c->aperture;
        }
        ray.o = v3_add(c->pos, v3_add(v3_scale(r, lx), v3_scale(u, ly)));
        ray.d = v3_norm(v3_sub(pf, ray.o));
    }
    return ray;
}

/* ------------------------------------------------------------- samplers */

typedef struct pss_x { float value, backup; int64_t mod, mod_backup; } pss_x;

typedef struct mlt_state {
    pcg32 rng;
    pss_x *X;
    int n, cap;
    int64_t iter, last_large;
    int large;
    int index;
    float p_large;
} mlt_state;

typedef struct smp {
    int kind;          /* 0 independent, 1 MLT */
    pcg32 rng;
    mlt_state *m;
    int fixed_film;    /* PT: the film position is given (stratified per pixel) */
    float fx, fy;
} smp;

static void mlt_reset(mlt_state *m, uint64_t seed, uint64_t index, float p_large) {
    pcg32_seed(&m->rng, nhash64(seed * 0x9E3779B97F4A7C15ULL + index), index * 2u + 1u);
    m->n = 0;
    m->iter = 0;
    m->last_large = 0;
    m->large = 1;
    m->index = 0;
    m->p_large = p_large;
}

static float kelemen_mutate(float v, float u_sign, float u_mag) {
    const float s1 = 1.0f / 1024.0f, s2 = 1.0f / 64.0f;
    float dv = s2 * expf(-logf(s2 / s1) * u_mag);
    if (u_sign < 0.5f) { v += dv; if (v >= 1.0f) v -= 1.0f; }
    else { v -= dv; if (v < 0.0f) v += 1.0f; }
    return v;
}

static void mlt_ensure(mlt_state *m, int i) {
    pss_x *x;
    if (i >= m->cap) {
        int nc = m->cap ? m->cap * 2 : 64;
        while (nc <= i) nc *= 2;
        m->X = (pss_x *)realloc(m->X, sizeof(pss_x) * (size_t)nc);
        m->cap = nc;
    }
    while (m->n <= i) {
        m->X[m->n].value = 0.0f; m->X[m->n].backup = 0.0f;
        m->X[m->n].mod = 0; m->X[m->n].mod_backup = 0;
        m->n++;
    }
    x = &m->X[i];
    if (x->mod < m->last_large) {   /* missed a large step: refresh */
        x->value = pcg32_float(&m->rng);
        x->mod = m->last_large;
    }
    x->backup = x->value;
    x->mod_backup = x->mod;
    if (m->large) {
        x->value = pcg32_float(&m->rng);
    } else {
        int64_t ns = m->iter - x->mod, k;
        if (ns > 32) ns = 32;   /* the perturbation is local: older coordinates are near-uniform anyway */
        for (k = 0; k < ns; k++) {
            float a = pcg32_float(&m->rng), b = pcg32_float(&m->rng);
            x->value = kelemen_mutate(x->value, a, b);
        }
    }
    x->mod = m->iter;
}

static float smp_next(smp *s) {
    if (s->kind == 0) return pcg32_float(&s->rng);
    {
        mlt_state *m = s->m;
        int i = m->index++;
        mlt_ensure(m, i);
        return m->X[i].value;
    }
}

static void mlt_start_iteration(mlt_state *m) {
    m->iter++;
    m->large = pcg32_float(&m->rng) < m->p_large;
    m->index = 0;
}
static void mlt_accept(mlt_state *m) { if (m->large) m->last_large = m->iter; }
static void mlt_reject(mlt_state *m) {
    int i;
    for (i = 0; i < m->n; i++) {
        if (m->X[i].mod == m->iter) { m->X[i].value = m->X[i].backup; m->X[i].mod = m->X[i].mod_backup; }
    }
    m->iter--;
}

/* ------------------------------------------------------------- tracing */

typedef struct tctx {
    const nora_scene *s;
    const nora_settings *st;
    osl_exec *osl;
    float eps;
} tctx;

static float power_heur(float a, float b) {
    float a2 = a * a, b2 = b * b;
    return a2 + b2 > 0.0f ? a2 / (a2 + b2) : 0.0f;
}

static v3 offset_origin(v3 p, v3 ng, v3 dir, float eps) {
    return v3_add(p, v3_scale(ng, v3_dot(dir, ng) > 0.0f ? eps : -eps));
}

/* emitted radiance of triangle tri at barycentrics toward direction -dir (dir points at the light) */
static sp4 light_emission(tctx *tc, int tri, float b1, float b2, v3 dir, const swl *w, nora_isect *lit) {
    nora_hit h;
    ray3 r;
    nora_bsdf lb;
    h.t = 1.0f; h.b1 = b1; h.b2 = b2; h.tri = tri;
    r.o = v3_make(0, 0, 0); r.d = dir; r.tmin = 0; r.tmax = 1;
    nora_isect_fill(tc->s, &r, &h, lit);
    nora_shade(tc->s, tc->osl, lit, v3_neg(dir), w, &lb);
    return lb.Le;
}

static sp4 env_radiance(const nora_scene *s, const swl *w) {
    return sp_from_rgb_illum(w, s->env_color);
}

/* One path. Consumes the sampler in a fixed order per vertex. Writes film position. */
static sp4 trace_path(tctx *tc, smp *sm, swl *w, float *px, float *py) {
    const nora_scene *s = tc->s;
    const nora_settings *st = tc->st;
    sp4 L = sp_const(0), beta = sp_const(1);
    ray3 ray;
    int depth, specular = 1;
    float prev_pdf = 1.0f;
    v3 prev_p = v3_make(0, 0, 0);
    float u0 = smp_next(sm), u1 = smp_next(sm), lu = smp_next(sm), lv = smp_next(sm), ul = smp_next(sm);
    *px = sm->fixed_film ? sm->fx : u0 * (float)st->width;
    *py = sm->fixed_film ? sm->fy : u1 * (float)st->height;
    if (st->mode == NORA_MODE_SPECTRAL) swl_sample(w, ul);
    else swl_init_rgb(w);
    ray = nora_camera_ray(&s->cam, st->width, st->height, *px, *py, lu, lv);
    for (depth = 0;; depth++) {
        nora_hit hit;
        nora_isect it;
        nora_bsdf b;
        nora_bsdf_sample bs;
        v3 wo;
        if (!nora_intersect(s, &ray, &hit)) {
            if (v3_maxc(s->env_color) > 0.0f) {
                sp4 Le = env_radiance(s, w);
                float wgt = 1.0f;
                if (!specular && s->env_prob > 0.0f) wgt = power_heur(prev_pdf, s->env_prob / (4.0f * NORA_PI));
                L = sp_add(L, sp_scale(sp_mul(beta, Le), wgt));
            }
            break;
        }
        nora_isect_fill(s, &ray, &hit, &it);
        wo = v3_neg(ray.d);
        nora_shade(s, tc->osl, &it, wo, w, &b);
        if (b.dispersive) swl_terminate_secondary(w, &beta);
        if (!sp_is_black(b.Le)) {
            float wgt = 1.0f;
            if (!specular) {
                float pl = s->tri_light_prob ? s->tri_light_prob[hit.tri] : 0.0f;
                if (pl > 0.0f) {
                    const nora_tri *t = &s->tris[hit.tri];
                    v3 e1 = v3_sub(s->pos[t->v[1]], s->pos[t->v[0]]), e2 = v3_sub(s->pos[t->v[2]], s->pos[t->v[0]]);
                    float area = 0.5f * v3_len(v3_cross(e1, e2));
                    float d2 = hit.t * hit.t;
                    float cl = fabsf(v3_dot(it.ng, wo));
                    float pdf_l = cl > 0.0f ? pl * d2 / (area * cl) : 0.0f;
                    wgt = power_heur(prev_pdf, pdf_l);
                }
            }
            L = sp_add(L, sp_scale(sp_mul(beta, b.Le), wgt));
        }
        if (depth >= st->max_depth) break;
        if (b.n == 0) break;
        /* next event estimation */
        {
            float uls = smp_next(sm), ua = smp_next(sm), ub = smp_next(sm);
            if (nora_bsdf_has_nondelta(&b) && (s->nlights > 0 || s->env_prob > 0.0f)) {
                v3 wi = v3_make(0, 0, 1);
                float dist = NORA_INFF, pdf_l = 0.0f;
                sp4 Le = sp_const(0);
                if (s->env_prob > 0.0f && uls < s->env_prob) {
                    float z = 1.0f - 2.0f * ua, r = nsafe_sqrt(1.0f - z * z), phi = 2.0f * NORA_PI * ub;
                    wi = v3_make(r * cosf(phi), r * sinf(phi), z);
                    pdf_l = s->env_prob / (4.0f * NORA_PI);
                    Le = env_radiance(s, w);
                } else if (s->nlights > 0) {
                    float uu = s->env_prob > 0.0f ? (uls - s->env_prob) / (1.0f - s->env_prob) : uls;
                    int lo = 0, hi = s->nlights - 1, li, tri;
                    float su, b1, b2, area, cl;
                    const nora_tri *t;
                    v3 p0, p1, p2, lp, d;
                    nora_isect lit;
                    while (lo < hi) { int mid = (lo + hi) / 2; if (s->light_cdf[mid + 1] <= uu) lo = mid + 1; else hi = mid; }
                    li = lo;
                    tri = s->light_tris[li];
                    t = &s->tris[tri];
                    p0 = s->pos[t->v[0]]; p1 = s->pos[t->v[1]]; p2 = s->pos[t->v[2]];
                    su = sqrtf(ua);
                    b1 = su * (1.0f - ub);
                    b2 = su * ub;
                    lp = v3_add(v3_add(v3_scale(p0, 1.0f - b1 - b2), v3_scale(p1, b1)), v3_scale(p2, b2));
                    d = v3_sub(lp, it.p);
                    dist = v3_len(d);
                    if (dist > 0.0f) {
                        wi = v3_scale(d, 1.0f / dist);
                        area = 0.5f * v3_len(v3_cross(v3_sub(p1, p0), v3_sub(p2, p0)));
                        Le = light_emission(tc, tri, b1, b2, wi, w, &lit);
                        cl = fabsf(v3_dot(lit.ng, wi));
                        pdf_l = (cl > 0.0f && area > 0.0f) ? s->tri_light_prob[tri] * dist * dist / (area * cl) : 0.0f;
                    }
                }
                if (pdf_l > 0.0f && !sp_is_black(Le)) {
                    float bpdf;
                    sp4 f = nora_bsdf_eval(&b, wo, wi, &bpdf);
                    if (!sp_is_black(f)) {
                        ray3 sr;
                        sr.o = offset_origin(it.p, it.ng, wi, tc->eps);
                        sr.d = wi;
                        sr.tmin = 0.0f;
                        sr.tmax = dist < NORA_INFF ? dist * (1.0f - 1e-4f) - tc->eps : NORA_INFF;
                        if (!nora_occluded(s, &sr)) {
                            float wgt = power_heur(pdf_l, bpdf);
                            L = sp_add(L, sp_scale(sp_mul(sp_mul(beta, f), Le), wgt / pdf_l));
                        }
                    }
                }
            }
        }
        /* continue the path */
        {
            float a0 = smp_next(sm), a1 = smp_next(sm), a2 = smp_next(sm), a3 = smp_next(sm);
            float urr = smp_next(sm);
            if (!nora_bsdf_sample_dir(&b, wo, a0, a1, a2, a3, &bs)) break;
            beta = sp_mul(beta, bs.weight);
            specular = bs.delta;
            prev_pdf = bs.pdf;
            prev_p = it.p;
            if (depth >= 3) {
                float q = fminf(0.95f, sp_max(beta));
                if (q <= 0.0f || urr > q) break;
                beta = sp_scale(beta, 1.0f / q);
            }
            if (sp_is_black(beta) || sp_has_nan(beta)) break;
            ray.o = offset_origin(it.p, it.ng, bs.wi, tc->eps);
            ray.d = bs.wi;
            ray.tmin = 0.0f;
            ray.tmax = NORA_INFF;
        }
    }
    (void)prev_p;
    if (sp_has_nan(L)) return sp_const(0);
    return L;
}

/* ------------------------------------------------------------- renderer */

typedef struct worker {
    struct nora_render *r;
    int id;
    nthread th;
    float *film;      /* rgb */
    float *count;     /* PT: samples per pixel */
    tctx tc;
    int64_t accepted, proposed;
} worker;

struct nora_render {
    const nora_scene *scene;
    nora_settings st;
    int nthreads;
    worker *workers;
    nthread master;
    volatile int stop;
    volatile int running;
    volatile int phase;
    volatile int64_t done;       /* PT samples (pixels) or MLT mutations */
    int64_t target;
    double t_start, t_end;
    /* MLT */
    float *boot_I;
    double b;
    float *boot_cdf;
    volatile int64_t next_boot;
    volatile int64_t accepted, proposed;
    int started;
};

static float rgb_lum(v3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

static void splat(float *film, int w, int h, float px, float py, v3 c) {
    int x = (int)px, y = (int)py;
    float *p;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= w) x = w - 1;
    if (y >= h) y = h - 1;
    p = film + ((size_t)y * w + x) * 3;
    p[0] += c.x; p[1] += c.y; p[2] += c.z;
}

static int time_up(nora_render *r) {
    if (r->stop) return 1;
    if (r->st.time_limit > 0.0f && ntime_now() - r->t_start >= (double)r->st.time_limit) return 1;
    return 0;
}

/* PT: each claimed unit is one image row at one sample index */
static void pt_worker(void *arg) {
    worker *wk = (worker *)arg;
    nora_render *r = wk->r;
    const int W = r->st.width, H = r->st.height;
    const int64_t units = (int64_t)r->st.spp * H;
    smp sm;
    swl w;
    memset(&sm, 0, sizeof sm);
    for (;;) {
        int64_t u = natomic_add64(&r->next_boot, 1) - 1;
        int y, x, pass;
        if (u >= units || time_up(r)) break;
        pass = (int)(u / H);
        y = (int)(u % H);
        sm.kind = 0;
        pcg32_seed(&sm.rng, nhash64(r->st.seed ^ ((uint64_t)u * 0x2545F4914F6CDD1DULL)), (uint64_t)pass * 2654435761u + 1u);
        for (x = 0; x < W; x++) {
            float px, py;
            sp4 L;
            v3 c;
            sm.fixed_film = 1;
            sm.fx = (float)x + pcg32_float(&sm.rng);
            sm.fy = (float)y + pcg32_float(&sm.rng);
            L = trace_path(&wk->tc, &sm, &w, &px, &py);
            c = sp_to_rgb(&w, L);
            if (!(c.x == c.x) || !(c.y == c.y) || !(c.z == c.z)) c = v3_make(0, 0, 0);
            splat(wk->film, W, H, px, py, c);
            wk->count[(size_t)y * W + x] += 1.0f;
        }
        natomic_add64(&r->done, W);
    }
}

static void mlt_boot_worker(void *arg) {
    worker *wk = (worker *)arg;
    nora_render *r = wk->r;
    mlt_state m;
    smp sm;
    swl w;
    memset(&m, 0, sizeof m);
    memset(&sm, 0, sizeof sm);
    sm.kind = 1;
    sm.m = &m;
    for (;;) {
        int64_t i = natomic_add64(&r->next_boot, 1) - 1;
        float px, py;
        sp4 L;
        if (i >= r->st.bootstrap || r->stop) break;
        mlt_reset(&m, r->st.seed, (uint64_t)i, r->st.p_large);
        L = trace_path(&wk->tc, &sm, &w, &px, &py);
        r->boot_I[i] = fmaxf(0.0f, rgb_lum(sp_to_rgb(&w, L)));
        if (!(r->boot_I[i] == r->boot_I[i]) || r->boot_I[i] > 1e20f) r->boot_I[i] = 0.0f;
    }
    free(m.X);
}

typedef struct chain {
    mlt_state m;
    sp4 L;
    swl w;
    v3 rgb;
    float I, px, py;
    int64_t remaining;
} chain;

static void mlt_chain_worker(void *arg) {
    worker *wk = (worker *)arg;
    nora_render *r = wk->r;
    const int W = r->st.width, H = r->st.height;
    int nchains = r->st.chains, c, mine = 0;
    int64_t total = (int64_t)r->st.spp * W * H;
    chain *ch;
    smp sm;
    pcg32 pick;
    int64_t batch = (int64_t)W * H / 32 + 64;
    for (c = wk->id; c < nchains; c += r->nthreads) mine++;
    if (mine == 0) return;
    ch = (chain *)calloc((size_t)mine, sizeof(chain));
    if (!ch) return;
    pcg32_seed(&pick, nhash64(r->st.seed + 7u), 99u);
    memset(&sm, 0, sizeof sm);
    sm.kind = 1;
    /* initial states resampled from the bootstrap distribution */
    {
        int k = 0;
        for (c = wk->id; c < nchains; c += r->nthreads, k++) {
            float u;
            int lo = 0, hi = r->st.bootstrap - 1;
            pcg32 cr;
            pcg32_seed(&cr, nhash64(r->st.seed * 31u + (uint64_t)c), (uint64_t)c + 17u);
            u = pcg32_float(&cr);
            while (lo < hi) { int mid = (lo + hi) / 2; if (r->boot_cdf[mid + 1] <= u) lo = mid + 1; else hi = mid; }
            mlt_reset(&ch[k].m, r->st.seed, (uint64_t)lo, r->st.p_large);
            sm.m = &ch[k].m;
            ch[k].L = trace_path(&wk->tc, &sm, &ch[k].w, &ch[k].px, &ch[k].py);
            ch[k].rgb = sp_to_rgb(&ch[k].w, ch[k].L);
            ch[k].I = fmaxf(0.0f, rgb_lum(ch[k].rgb));
            ch[k].remaining = total / nchains + (c < (int)(total % nchains) ? 1 : 0);
            /* reseed the chain's own stream so chains starting at the same index diverge */
            pcg32_seed(&ch[k].m.rng, nhash64(r->st.seed * 1315423911u + (uint64_t)c * 7919u), (uint64_t)c * 2u + 3u);
        }
    }
    (void)pick;
    for (;;) {
        int k, any = 0;
        for (k = 0; k < mine; k++) {
            chain *cc = &ch[k];
            int64_t n = cc->remaining < batch ? cc->remaining : batch, j;
            if (n <= 0) continue;
            any = 1;
            sm.m = &cc->m;
            for (j = 0; j < n; j++) {
                float px, py, I, a;
                swl w;
                sp4 L;
                v3 rgb;
                mlt_start_iteration(&cc->m);
                L = trace_path(&wk->tc, &sm, &w, &px, &py);
                rgb = sp_to_rgb(&w, L);
                I = fmaxf(0.0f, rgb_lum(rgb));
                if (!(I == I) || I > 1e20f) I = 0.0f;
                a = cc->I > 0.0f ? fminf(1.0f, I / cc->I) : 1.0f;
                if (I > 0.0f) splat(wk->film, W, H, px, py, v3_scale(rgb, a / I));
                if (cc->I > 0.0f) splat(wk->film, W, H, cc->px, cc->py, v3_scale(cc->rgb, (1.0f - a) / cc->I));
                wk->proposed++;
                if (pcg32_float(&cc->m.rng) < a) {
                    cc->px = px; cc->py = py; cc->rgb = rgb; cc->I = I; cc->L = L; cc->w = w;
                    mlt_accept(&cc->m);
                    wk->accepted++;
                } else {
                    mlt_reject(&cc->m);
                }
            }
            cc->remaining -= n;
            natomic_add64(&r->done, n);
            if (time_up(r)) break;
        }
        if (!any || time_up(r)) break;
    }
    for (c = 0; c < mine; c++) free(ch[c].m.X);
    free(ch);
}

static void run_workers(nora_render *r, nthread_fn fn) {
    int i;
    for (i = 0; i < r->nthreads; i++) {
        if (nthread_start(&r->workers[i].th, fn, &r->workers[i]) != 0) fn(&r->workers[i]);
    }
    for (i = 0; i < r->nthreads; i++) nthread_join(&r->workers[i].th);
}

static void master_fn(void *arg) {
    nora_render *r = (nora_render *)arg;
    r->t_start = ntime_now();
    if (r->st.integrator == NORA_INT_PT) {
        r->phase = 2;
        r->next_boot = 0;
        run_workers(r, pt_worker);
    } else {
        int i;
        double sum = 0.0;
        r->phase = 1;
        r->next_boot = 0;
        run_workers(r, mlt_boot_worker);
        if (!r->stop) {
            r->boot_cdf[0] = 0.0f;
            for (i = 0; i < r->st.bootstrap; i++) sum += r->boot_I[i];
            r->b = sum / (double)r->st.bootstrap;
            {
                double acc = 0.0;
                for (i = 0; i < r->st.bootstrap; i++) {
                    acc += r->boot_I[i];
                    r->boot_cdf[i + 1] = sum > 0.0 ? (float)(acc / sum) : (float)(i + 1) / (float)r->st.bootstrap;
                }
                r->boot_cdf[r->st.bootstrap] = 1.0f;
            }
            nora_logf("MLT bootstrap: %d paths, b = %.6g", r->st.bootstrap, r->b);
            if (r->b > 0.0) {
                r->phase = 2;
                run_workers(r, mlt_chain_worker);
            } else {
                nora_logf("MLT: the scene is black from this camera (b = 0)");
            }
        }
        {
            int64_t a = 0, p = 0;
            for (i = 0; i < r->nthreads; i++) { a += r->workers[i].accepted; p += r->workers[i].proposed; }
            r->accepted = a; r->proposed = p;
        }
    }
    r->t_end = ntime_now();
    r->phase = 3;
    natomic_store_int(&r->running, 0);
}

nora_render *nora_render_create(const nora_scene *scene, const nora_settings *st) {
    nora_render *r;
    int i;
    size_t npix;
    if (!scene || !scene->built || st->width <= 0 || st->height <= 0) return NULL;
    spectrum_init();
    r = (nora_render *)calloc(1, sizeof *r);
    if (!r) return NULL;
    r->scene = scene;
    r->st = *st;
    if (r->st.spp <= 0) r->st.spp = 1;
    if (r->st.max_depth <= 0) r->st.max_depth = 10;
    if (r->st.p_large <= 0.0f || r->st.p_large > 1.0f) r->st.p_large = 0.3f;
    if (r->st.bootstrap <= 0) r->st.bootstrap = 100000;
    if (r->st.chains <= 0) r->st.chains = 64;
    r->nthreads = st->threads > 0 ? st->threads : nthread_cpu_count();
    if (r->nthreads > 256) r->nthreads = 256;
    npix = (size_t)st->width * st->height;
    r->workers = (worker *)calloc((size_t)r->nthreads, sizeof(worker));
    if (!r->workers) { free(r); return NULL; }
    {
        v3 ext = v3_sub(scene->bmax, scene->bmin);
        float e = v3_maxc(ext);
        for (i = 0; i < r->nthreads; i++) {
            worker *wk = &r->workers[i];
            wk->r = r;
            wk->id = i;
            wk->film = (float *)calloc(npix * 3, sizeof(float));
            wk->count = r->st.integrator == NORA_INT_PT ? (float *)calloc(npix, sizeof(float)) : NULL;
            wk->tc.s = scene;
            wk->tc.st = &r->st;
            wk->tc.osl = osl_exec_create();
            wk->tc.eps = 1e-4f * (e > 1.0f ? e : 1.0f);
            if (!wk->film || (r->st.integrator == NORA_INT_PT && !wk->count)) { nora_render_destroy(r); return NULL; }
        }
    }
    if (r->st.integrator == NORA_INT_MLT) {
        r->boot_I = (float *)calloc((size_t)r->st.bootstrap, sizeof(float));
        r->boot_cdf = (float *)calloc((size_t)r->st.bootstrap + 1, sizeof(float));
        if (!r->boot_I || !r->boot_cdf) { nora_render_destroy(r); return NULL; }
        r->target = (int64_t)r->st.spp * (int64_t)npix;
    } else {
        r->target = (int64_t)r->st.spp * (int64_t)npix;
    }
    return r;
}

int nora_render_start(nora_render *r) {
    if (!r || r->started) return -1;
    r->started = 1;
    r->stop = 0;
    natomic_store_int(&r->running, 1);
    r->t_start = ntime_now();
    if (nthread_start(&r->master, master_fn, r) != 0) {
        master_fn(r);
    }
    return 0;
}

void nora_render_request_stop(nora_render *r) { if (r) natomic_store_int(&r->stop, 1); }
void nora_render_wait(nora_render *r) { if (r && r->started) nthread_join(&r->master); }
int nora_render_is_running(nora_render *r) { return r ? natomic_load_int(&r->running) : 0; }

float nora_render_progress(nora_render *r) {
    double p = 0.0;
    if (!r) return 0.0f;
    if (r->phase == 3) return 1.0f;
    if (r->phase == 1) return 0.0f;
    if (r->target > 0) p = (double)natomic_load64(&r->done) / (double)r->target;
    if (r->st.time_limit > 0.0f && r->phase == 2) {
        double tp = (ntime_now() - r->t_start) / r->st.time_limit;
        if (tp > p) p = tp;
    }
    return (float)(p > 1.0 ? 1.0 : p);
}

void nora_render_get_stats(nora_render *r, nora_render_stats *o) {
    int i;
    int64_t a = 0, p = 0;
    memset(o, 0, sizeof *o);
    if (!r) return;
    o->elapsed = (r->phase == 3 ? r->t_end : ntime_now()) - r->t_start;
    if (!r->started) o->elapsed = 0;
    o->samples = natomic_load64(&r->done);
    o->target = r->target;
    o->b = r->b;
    o->phase = r->phase;
    for (i = 0; i < r->nthreads; i++) { a += r->workers[i].accepted; p += r->workers[i].proposed; }
    o->acceptance = p > 0 ? (double)a / (double)p : 0.0;
}

int nora_render_resolve(nora_render *r, float *rgb) {
    size_t npix, i;
    int t;
    if (!r) return -1;
    npix = (size_t)r->st.width * r->st.height;
    memset(rgb, 0, npix * 3 * sizeof(float));
    if (r->st.integrator == NORA_INT_PT) {
        for (i = 0; i < npix; i++) {
            float n = 0.0f, acc[3] = { 0, 0, 0 };
            for (t = 0; t < r->nthreads; t++) {
                n += r->workers[t].count[i];
                acc[0] += r->workers[t].film[i * 3];
                acc[1] += r->workers[t].film[i * 3 + 1];
                acc[2] += r->workers[t].film[i * 3 + 2];
            }
            if (n > 0.0f) { rgb[i * 3] = acc[0] / n; rgb[i * 3 + 1] = acc[1] / n; rgb[i * 3 + 2] = acc[2] / n; }
        }
    } else {
        int64_t done = natomic_load64(&r->done);
        double scale;
        if (done <= 0 || r->b <= 0.0) return 0;
        scale = r->b * (double)npix / (double)done;
        for (t = 0; t < r->nthreads; t++) {
            const float *f = r->workers[t].film;
            for (i = 0; i < npix * 3; i++) rgb[i] += f[i];
        }
        for (i = 0; i < npix * 3; i++) rgb[i] = (float)(rgb[i] * scale);
    }
    return 0;
}

const nora_settings *nora_render_settings(const nora_render *r) { return &r->st; }

void nora_render_destroy(nora_render *r) {
    int i;
    if (!r) return;
    if (r->started && natomic_load_int(&r->running)) {
        nora_render_request_stop(r);
    }
    if (r->started) { nthread_join(&r->master); r->started = 0; }
    for (i = 0; i < r->nthreads; i++) {
        free(r->workers[i].film);
        free(r->workers[i].count);
        osl_exec_free(r->workers[i].tc.osl);
    }
    free(r->workers);
    free(r->boot_I);
    free(r->boot_cdf);
    free(r);
}

int nora_render_blocking(const nora_scene *scene, const nora_settings *st, float *rgb, nora_render_stats *stats) {
    nora_render *r = nora_render_create(scene, st);
    if (!r) return -1;
    nora_render_start(r);
    nora_render_wait(r);
    nora_render_resolve(r, rgb);
    if (stats) nora_render_get_stats(r, stats);
    nora_render_destroy(r);
    return 0;
}
