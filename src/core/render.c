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

typedef struct chain {
    mlt_state m;
    sp4 L;
    swl w;
    v3 rgb;
    float I, px, py;
    int64_t done;     /* mutations made by this chain */
} chain;

enum { PHASE_IDLE = 0, PHASE_BOOT = 1, PHASE_RENDER = 2, PHASE_DONE = 3, PHASE_PAUSED = 4 };

struct nora_render {
    const nora_scene *scene;
    nora_settings st;
    int nthreads;
    worker *workers;
    nthread master;
    int master_live;
    volatile int stop;
    volatile int pause;
    volatile int running;
    volatile int phase;
    volatile int64_t done;       /* PT samples (pixels) or MLT mutations */
    int64_t target;
    double t_start, t_end, elapsed_before;
    /* PT: next row unit to claim (all claimed units are completed when the workers stop) */
    volatile int64_t next_unit;
    /* MLT */
    float *boot_I;
    double b;
    float *boot_cdf;
    volatile int64_t next_boot;
    int64_t accepted, proposed;
    chain *chains;
    int nchains;
    int chains_ready;
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

static int64_t pt_units(const nora_render *r) { return (int64_t)r->st.spp * r->st.height; }
static int64_t mlt_total(const nora_render *r) { return (int64_t)r->st.spp * r->st.width * r->st.height; }
static int64_t chain_quota(const nora_render *r, int c) {
    int64_t total = mlt_total(r);
    return total / r->nchains + (c < (int)(total % r->nchains) ? 1 : 0);
}

static int work_left(const nora_render *r) {
    int c;
    if (r->st.integrator == NORA_INT_PT) return r->next_unit < pt_units(r);
    if (!r->chains_ready) return 1;
    if (r->b <= 0.0) return 0;
    for (c = 0; c < r->nchains; c++) if (r->chains[c].done < chain_quota(r, c)) return 1;
    return 0;
}

/* PT: each claimed unit is one image row at one sample index */
static void pt_worker(void *arg) {
    worker *wk = (worker *)arg;
    nora_render *r = wk->r;
    const int W = r->st.width, H = r->st.height;
    const int64_t units = pt_units(r);
    smp sm;
    swl w;
    memset(&sm, 0, sizeof sm);
    for (;;) {
        int64_t u;
        int y, x, pass;
        if (time_up(r)) break;   /* checked before claiming, so every claimed unit is finished */
        u = natomic_add64(&r->next_unit, 1) - 1;
        if (u >= units) { natomic_add64(&r->next_unit, -1); break; }
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

/* initial chain states resampled from the bootstrap distribution */
static int init_chains(nora_render *r) {
    int c;
    smp sm;
    tctx *tc = &r->workers[0].tc;
    r->chains = (chain *)calloc((size_t)r->nchains, sizeof(chain));
    if (!r->chains) return -1;
    memset(&sm, 0, sizeof sm);
    sm.kind = 1;
    for (c = 0; c < r->nchains; c++) {
        chain *ch = &r->chains[c];
        float u;
        int lo = 0, hi = r->st.bootstrap - 1;
        pcg32 cr;
        pcg32_seed(&cr, nhash64(r->st.seed * 31u + (uint64_t)c), (uint64_t)c + 17u);
        u = pcg32_float(&cr);
        while (lo < hi) { int mid = (lo + hi) / 2; if (r->boot_cdf[mid + 1] <= u) lo = mid + 1; else hi = mid; }
        mlt_reset(&ch->m, r->st.seed, (uint64_t)lo, r->st.p_large);
        sm.m = &ch->m;
        ch->L = trace_path(tc, &sm, &ch->w, &ch->px, &ch->py);
        ch->rgb = sp_to_rgb(&ch->w, ch->L);
        ch->I = fmaxf(0.0f, rgb_lum(ch->rgb));
        if (!(ch->I == ch->I) || ch->I > 1e20f) ch->I = 0.0f;
        ch->done = 0;
        /* reseed the chain's own stream so chains starting at the same index diverge */
        pcg32_seed(&ch->m.rng, nhash64(r->st.seed * 1315423911u + (uint64_t)c * 7919u), (uint64_t)c * 2u + 3u);
    }
    r->chains_ready = 1;
    return 0;
}

static void mlt_chain_worker(void *arg) {
    worker *wk = (worker *)arg;
    nora_render *r = wk->r;
    const int W = r->st.width, H = r->st.height;
    int64_t batch = (int64_t)W * H / 32 + 64;
    smp sm;
    memset(&sm, 0, sizeof sm);
    sm.kind = 1;
    for (;;) {
        int c, any = 0;
        for (c = wk->id; c < r->nchains; c += r->nthreads) {
            chain *cc = &r->chains[c];
            int64_t left = chain_quota(r, c) - cc->done, n, j;
            if (left <= 0) continue;
            any = 1;
            n = left < batch ? left : batch;
            sm.m = &cc->m;
            for (j = 0; j < n; j++) {
                float px, py, I, a;
                swl w;
                sp4 L;
                v3 rgb;
                if ((j & 255) == 255 && time_up(r)) break;
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
            cc->done += j;
            natomic_add64(&r->done, j);
            if (time_up(r)) break;
        }
        if (!any || time_up(r)) break;
    }
}

static void run_workers(nora_render *r, nthread_fn fn) {
    int i;
    for (i = 0; i < r->nthreads; i++) {
        if (nthread_start(&r->workers[i].th, fn, &r->workers[i]) != 0) fn(&r->workers[i]);
    }
    for (i = 0; i < r->nthreads; i++) nthread_join(&r->workers[i].th);
}

static void finish_bootstrap(nora_render *r) {
    int i;
    double sum = 0.0, acc = 0.0;
    r->boot_cdf[0] = 0.0f;
    for (i = 0; i < r->st.bootstrap; i++) sum += r->boot_I[i];
    r->b = sum / (double)r->st.bootstrap;
    for (i = 0; i < r->st.bootstrap; i++) {
        acc += r->boot_I[i];
        r->boot_cdf[i + 1] = sum > 0.0 ? (float)(acc / sum) : (float)(i + 1) / (float)r->st.bootstrap;
    }
    r->boot_cdf[r->st.bootstrap] = 1.0f;
    nora_logf("MLT bootstrap: %d paths, b = %.6g", r->st.bootstrap, r->b);
}

static void master_fn(void *arg) {
    nora_render *r = (nora_render *)arg;
    r->t_start = ntime_now();
    if (r->st.integrator == NORA_INT_PT) {
        r->phase = PHASE_RENDER;
        run_workers(r, pt_worker);
    } else {
        int i;
        if (!r->chains_ready) {
            r->phase = PHASE_BOOT;
            r->next_boot = 0;
            run_workers(r, mlt_boot_worker);
            if (!r->stop) {
                finish_bootstrap(r);
                if (r->b > 0.0 && init_chains(r) != 0) nora_logf("MLT: out of memory");
                if (r->b <= 0.0) { r->chains_ready = 1; nora_logf("MLT: the scene is black from this camera (b = 0)"); }
            }
        }
        if (r->chains_ready && r->b > 0.0 && !r->stop) {
            r->phase = PHASE_RENDER;
            run_workers(r, mlt_chain_worker);
        }
        {
            int64_t a = 0, p = 0;
            for (i = 0; i < r->nthreads; i++) { a += r->workers[i].accepted; p += r->workers[i].proposed; }
            r->accepted = a; r->proposed = p;
        }
    }
    r->t_end = ntime_now();
    r->elapsed_before += r->t_end - r->t_start;
    r->phase = (r->pause && work_left(r)) ? PHASE_PAUSED : PHASE_DONE;
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
    r->nchains = r->st.chains;
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
    }
    r->target = (int64_t)r->st.spp * (int64_t)npix;
    return r;
}

static int launch(nora_render *r) {
    r->stop = 0;
    r->pause = 0;
    natomic_store_int(&r->running, 1);
    r->t_start = ntime_now();
    if (nthread_start(&r->master, master_fn, r) != 0) master_fn(r);
    else r->master_live = 1;
    return 0;
}

int nora_render_start(nora_render *r) {
    if (!r || r->started) return -1;
    r->started = 1;
    return launch(r);
}

void nora_render_request_stop(nora_render *r) { if (r) natomic_store_int(&r->stop, 1); }

void nora_render_wait(nora_render *r) {
    if (r && r->master_live) { nthread_join(&r->master); r->master_live = 0; }
}

int nora_render_pause(nora_render *r) {
    if (!r || !r->started) return -1;
    if (natomic_load_int(&r->running)) {
        natomic_store_int(&r->pause, 1);
        natomic_store_int(&r->stop, 1);
    }
    nora_render_wait(r);
    return 0;
}

int nora_render_resume(nora_render *r) {
    if (!r) return -1;
    if (natomic_load_int(&r->running)) return 0;
    nora_render_wait(r);
    if (!work_left(r)) return 1;
    r->started = 1;
    return launch(r);
}

int nora_render_is_paused(nora_render *r) { return r && !natomic_load_int(&r->running) && r->phase == PHASE_PAUSED; }
int nora_render_can_resume(nora_render *r) { return r && !natomic_load_int(&r->running) && work_left(r); }

int nora_render_set_spp(nora_render *r, int spp) {
    if (!r || natomic_load_int(&r->running) || spp <= 0) return -1;
    r->st.spp = spp;
    r->target = (int64_t)spp * (int64_t)r->st.width * r->st.height;
    return 0;
}
void nora_render_set_time_limit(nora_render *r, float seconds) { if (r && !natomic_load_int(&r->running)) r->st.time_limit = seconds; }

int nora_render_is_running(nora_render *r) { return r ? natomic_load_int(&r->running) : 0; }

float nora_render_progress(nora_render *r) {
    double p = 0.0;
    if (!r) return 0.0f;
    if (r->phase == PHASE_DONE && (!r->stop || !work_left(r))) return 1.0f;
    if (r->phase == PHASE_BOOT) return 0.0f;
    if (r->target > 0) p = (double)natomic_load64(&r->done) / (double)r->target;
    if (r->st.time_limit > 0.0f && r->phase == PHASE_RENDER) {
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
    o->elapsed = r->elapsed_before + (natomic_load_int(&r->running) ? ntime_now() - r->t_start : 0.0);
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
    if (natomic_load_int(&r->running)) nora_render_request_stop(r);
    nora_render_wait(r);
    for (i = 0; i < r->nthreads; i++) {
        free(r->workers[i].film);
        free(r->workers[i].count);
        osl_exec_free(r->workers[i].tc.osl);
    }
    free(r->workers);
    free(r->boot_I);
    free(r->boot_cdf);
    if (r->chains) for (i = 0; i < r->nchains; i++) free(r->chains[i].m.X);
    free(r->chains);
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

/* ------------------------------------------------------------ checkpoints
 * Little-endian binary, see README "Checkpoint format (.nrck)". */

#define NRCK_MAGIC "NRCK"
#define NRCK_END 0x454e4421u   /* "!DNE" */

typedef struct wbuf { unsigned char *p; size_t n, cap; int fail; } wbuf;
static void wb_bytes(wbuf *b, const void *d, size_t n) {
    if (b->fail) return;
    if (b->n + n > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        unsigned char *np;
        while (nc < b->n + n) nc *= 2;
        np = (unsigned char *)realloc(b->p, nc);
        if (!np) { b->fail = 1; return; }
        b->p = np; b->cap = nc;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void wb_u32(wbuf *b, uint32_t v) { unsigned char c[4]; int i; for (i = 0; i < 4; i++) c[i] = (unsigned char)(v >> (8 * i)); wb_bytes(b, c, 4); }
static void wb_u64(wbuf *b, uint64_t v) { unsigned char c[8]; int i; for (i = 0; i < 8; i++) c[i] = (unsigned char)(v >> (8 * i)); wb_bytes(b, c, 8); }
static void wb_i32(wbuf *b, int32_t v) { wb_u32(b, (uint32_t)v); }
static void wb_i64(wbuf *b, int64_t v) { wb_u64(b, (uint64_t)v); }
static void wb_f32(wbuf *b, float f) { uint32_t u; memcpy(&u, &f, 4); wb_u32(b, u); }
static void wb_f64(wbuf *b, double f) { uint64_t u; memcpy(&u, &f, 8); wb_u64(b, u); }

typedef struct rbuf { const unsigned char *p; size_t n, at; int fail; } rbuf;
static const unsigned char *rb_take(rbuf *b, size_t n) {
    const unsigned char *q;
    if (b->fail || b->at + n > b->n) { b->fail = 1; return NULL; }
    q = b->p + b->at;
    b->at += n;
    return q;
}
static uint32_t rb_u32(rbuf *b) { const unsigned char *c = rb_take(b, 4); uint32_t v = 0; int i; if (c) for (i = 0; i < 4; i++) v |= (uint32_t)c[i] << (8 * i); return v; }
static uint64_t rb_u64(rbuf *b) { const unsigned char *c = rb_take(b, 8); uint64_t v = 0; int i; if (c) for (i = 0; i < 8; i++) v |= (uint64_t)c[i] << (8 * i); return v; }
static int32_t rb_i32(rbuf *b) { return (int32_t)rb_u32(b); }
static int64_t rb_i64(rbuf *b) { return (int64_t)rb_u64(b); }
static float rb_f32(rbuf *b) { uint32_t u = rb_u32(b); float f; memcpy(&f, &u, 4); return f; }
static double rb_f64(rbuf *b) { uint64_t u = rb_u64(b); double f; memcpy(&f, &u, 8); return f; }

int nora_render_save_checkpoint(nora_render *r, uint64_t scene_hash, const char *path, char *err, size_t errcap) {
    wbuf b;
    size_t npix, i;
    int t, c, rc = 0;
    float *film;
    FILE *f;
    char tmp[1100];
    if (!r) return -1;
    if (natomic_load_int(&r->running)) { if (err) snprintf(err, errcap, "pause the render before saving a checkpoint"); return -1; }
    npix = (size_t)r->st.width * r->st.height;
    memset(&b, 0, sizeof b);
    wb_bytes(&b, NRCK_MAGIC, 4);
    wb_u32(&b, NORA_CHECKPOINT_VERSION);
    wb_u64(&b, scene_hash);
    /* settings that define the image */
    wb_i32(&b, r->st.width); wb_i32(&b, r->st.height);
    wb_i32(&b, (int32_t)r->st.mode); wb_i32(&b, (int32_t)r->st.integrator);
    wb_i32(&b, r->st.spp); wb_i32(&b, r->st.max_depth);
    wb_f32(&b, r->st.p_large); wb_i32(&b, r->st.bootstrap); wb_i32(&b, r->nchains);
    wb_u64(&b, r->st.seed);
    /* normalisation and counters */
    wb_f64(&b, r->b);
    wb_i64(&b, natomic_load64(&r->done));
    wb_i64(&b, natomic_load64(&r->next_unit));
    {
        int64_t a = 0, p = 0;
        for (t = 0; t < r->nthreads; t++) { a += r->workers[t].accepted; p += r->workers[t].proposed; }
        wb_i64(&b, a); wb_i64(&b, p);
    }
    wb_f64(&b, r->elapsed_before);
    /* film: per-pixel sums over all threads, then PT sample counts */
    film = (float *)calloc(npix * 3, sizeof(float));
    if (!film) { free(b.p); if (err) snprintf(err, errcap, "out of memory"); return -1; }
    for (t = 0; t < r->nthreads; t++) for (i = 0; i < npix * 3; i++) film[i] += r->workers[t].film[i];
    wb_u32(&b, 3);
    for (i = 0; i < npix * 3; i++) wb_f32(&b, film[i]);
    if (r->st.integrator == NORA_INT_PT) {
        for (i = 0; i < npix; i++) {
            float n = 0.0f;
            for (t = 0; t < r->nthreads; t++) n += r->workers[t].count[i];
            wb_f32(&b, n);
        }
    }
    free(film);
    /* MLT chains */
    wb_i32(&b, r->st.integrator == NORA_INT_MLT && r->chains_ready && r->chains ? r->nchains : 0);
    if (r->st.integrator == NORA_INT_MLT && r->chains_ready && r->chains) {
        for (c = 0; c < r->nchains; c++) {
            const chain *ch = &r->chains[c];
            const mlt_state *m = &ch->m;
            wb_u64(&b, m->rng.state); wb_u64(&b, m->rng.inc);
            wb_i64(&b, m->iter); wb_i64(&b, m->last_large);
            wb_i32(&b, m->large); wb_i32(&b, m->index);
            wb_f32(&b, ch->rgb.x); wb_f32(&b, ch->rgb.y); wb_f32(&b, ch->rgb.z);
            wb_f32(&b, ch->I); wb_f32(&b, ch->px); wb_f32(&b, ch->py);
            wb_i64(&b, ch->done);
            wb_i32(&b, m->n);
            for (i = 0; i < (size_t)m->n; i++) {
                wb_f32(&b, m->X[i].value); wb_f32(&b, m->X[i].backup);
                wb_i64(&b, m->X[i].mod); wb_i64(&b, m->X[i].mod_backup);
            }
        }
    } else {
        /* chains_ready = 0 with b known means "b = 0, nothing to do" */
    }
    wb_i32(&b, r->chains_ready);
    wb_u32(&b, NRCK_END);
    if (b.fail) { free(b.p); if (err) snprintf(err, errcap, "out of memory"); return -1; }
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f || fwrite(b.p, 1, b.n, f) != b.n) rc = -1;
    if (f && fclose(f) != 0) rc = -1;
    if (rc == 0) {
        remove(path);
        if (rename(tmp, path) != 0) rc = -1;
    }
    if (rc != 0) { remove(tmp); if (err) snprintf(err, errcap, "cannot write %s", path); }
    free(b.p);
    return rc;
}

nora_render *nora_render_load_checkpoint(const nora_scene *scene, uint64_t scene_hash, const char *path, int threads,
                                         char *err, size_t errcap) {
    FILE *f = fopen(path, "rb");
    unsigned char *data;
    long len;
    rbuf b;
    nora_settings st;
    nora_render *r = NULL;
    uint32_t ver;
    uint64_t hash;
    size_t npix, i;
    int nch, c;
    char dummy[8];
    if (!err) { err = dummy; errcap = sizeof dummy; }
    if (!f) { snprintf(err, errcap, "cannot open %s", path); return NULL; }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (unsigned char *)malloc(len > 0 ? (size_t)len : 1);
    if (!data || len <= 0 || fread(data, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(data); snprintf(err, errcap, "cannot read %s", path); return NULL; }
    fclose(f);
    b.p = data; b.n = (size_t)len; b.at = 0; b.fail = 0;
    if (len < 16 || memcmp(data, NRCK_MAGIC, 4) != 0) { free(data); snprintf(err, errcap, "%s is not a Nora checkpoint", path); return NULL; }
    b.at = 4;
    ver = rb_u32(&b);
    if (ver != NORA_CHECKPOINT_VERSION) { free(data); snprintf(err, errcap, "checkpoint version %u is not supported (expected %d)", ver, NORA_CHECKPOINT_VERSION); return NULL; }
    hash = rb_u64(&b);
    if (hash != scene_hash) {
        free(data);
        snprintf(err, errcap, "the checkpoint was saved for a different scene (scene hash %016llx, current scene %016llx)",
                 (unsigned long long)hash, (unsigned long long)scene_hash);
        return NULL;
    }
    nora_settings_default(&st);
    st.width = rb_i32(&b); st.height = rb_i32(&b);
    st.mode = (nora_mode)rb_i32(&b); st.integrator = (nora_integrator)rb_i32(&b);
    st.spp = rb_i32(&b); st.max_depth = rb_i32(&b);
    st.p_large = rb_f32(&b); st.bootstrap = rb_i32(&b); st.chains = rb_i32(&b);
    st.seed = rb_u64(&b);
    st.threads = threads;
    if (b.fail || st.width <= 0 || st.height <= 0 || st.width > 65536 || st.height > 65536 || st.chains <= 0) {
        free(data); snprintf(err, errcap, "corrupt checkpoint header"); return NULL;
    }
    r = nora_render_create(scene, &st);
    if (!r) { free(data); snprintf(err, errcap, "cannot create the renderer"); return NULL; }
    npix = (size_t)st.width * st.height;
    r->b = rb_f64(&b);
    r->done = rb_i64(&b);
    r->next_unit = rb_i64(&b);
    r->workers[0].accepted = rb_i64(&b);
    r->workers[0].proposed = rb_i64(&b);
    r->accepted = r->workers[0].accepted;
    r->proposed = r->workers[0].proposed;
    r->elapsed_before = rb_f64(&b);
    if (rb_u32(&b) != 3) b.fail = 1;
    for (i = 0; i < npix * 3 && !b.fail; i++) r->workers[0].film[i] = rb_f32(&b);
    if (st.integrator == NORA_INT_PT) for (i = 0; i < npix && !b.fail; i++) r->workers[0].count[i] = rb_f32(&b);
    nch = rb_i32(&b);
    if (nch != 0 && nch != r->nchains) b.fail = 1;
    if (nch > 0 && !b.fail) {
        r->chains = (chain *)calloc((size_t)nch, sizeof(chain));
        if (!r->chains) b.fail = 1;
        for (c = 0; c < nch && !b.fail; c++) {
            chain *ch = &r->chains[c];
            mlt_state *m = &ch->m;
            int n, k;
            m->rng.state = rb_u64(&b); m->rng.inc = rb_u64(&b);
            m->iter = rb_i64(&b); m->last_large = rb_i64(&b);
            m->large = rb_i32(&b); m->index = rb_i32(&b);
            m->p_large = st.p_large;
            ch->rgb.x = rb_f32(&b); ch->rgb.y = rb_f32(&b); ch->rgb.z = rb_f32(&b);
            ch->I = rb_f32(&b); ch->px = rb_f32(&b); ch->py = rb_f32(&b);
            ch->done = rb_i64(&b);
            n = rb_i32(&b);
            if (n < 0 || (size_t)n * 24 > b.n - b.at) { b.fail = 1; break; }
            m->cap = n > 0 ? n : 1;
            m->X = (pss_x *)malloc(sizeof(pss_x) * (size_t)m->cap);
            if (!m->X) { b.fail = 1; break; }
            m->n = n;
            for (k = 0; k < n; k++) {
                m->X[k].value = rb_f32(&b); m->X[k].backup = rb_f32(&b);
                m->X[k].mod = rb_i64(&b); m->X[k].mod_backup = rb_i64(&b);
            }
        }
    }
    r->chains_ready = rb_i32(&b);
    if (r->chains_ready && st.integrator == NORA_INT_MLT && nch == 0 && r->b > 0.0) b.fail = 1;
    if (rb_u32(&b) != NRCK_END) b.fail = 1;
    free(data);
    if (b.fail) { nora_render_destroy(r); snprintf(err, errcap, "corrupt or truncated checkpoint %s", path); return NULL; }
    r->started = 1;
    r->phase = work_left(r) ? PHASE_PAUSED : PHASE_DONE;
    return r;
}
