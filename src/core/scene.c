/* Nora - scene container, SAH BVH, light distribution, textures. MIT License. */
#include "scene.h"
#include "image.h"
#include "fsutil.h"
#include "../osl/osl.h"
#ifdef NORA_WITH_OSL
#include "osl_real.h"
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void free_real(nora_shader_slot *sl);

const char *const nora_mat_type_names[NORA_MAT_TYPE_COUNT] = { "Diffuse", "Conductor", "Dielectric", "Plastic" };

#define GROW(arr, n, cap, T)                                                     \
    do {                                                                         \
        if ((n) >= (cap)) {                                                      \
            int nc_ = (cap) ? (cap) * 2 : 16;                                    \
            void *np_ = realloc((arr), sizeof(T) * (size_t)nc_);                 \
            if (!np_) return -1;                                                 \
            (arr) = (T *)np_;                                                    \
            (cap) = nc_;                                                         \
        }                                                                        \
    } while (0)

nora_scene *nora_scene_create(void) {
    nora_scene *s = (nora_scene *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->cam.pos = v3_make(0, 0, 5);
    s->cam.target = v3_make(0, 0, 0);
    s->cam.up = v3_make(0, 1, 0);
    s->cam.vfov_deg = 40.0f;
    strcpy(s->name, "scene");
    return s;
}

static void free_built(nora_scene *s) {
    free(s->nodes); s->nodes = NULL; s->nnodes = 0;
    free(s->prim_idx); s->prim_idx = NULL;
    free(s->accel); s->accel = NULL;
    free(s->light_tris); s->light_tris = NULL; s->nlights = 0;
    free(s->light_cdf); s->light_cdf = NULL;
    free(s->tri_light_prob); s->tri_light_prob = NULL;
    s->built = 0;
}

void nora_scene_destroy(nora_scene *s) {
    int i;
    if (!s) return;
    free_built(s);
    free(s->pos); free(s->nrm); free(s->uv); free(s->tris); free(s->mats); free(s->meshes);
    free(s->sources); free(s->rest_pos); free(s->rest_nrm);
    for (i = 0; i < s->ntexs; i++) free(s->texs[i].rgb);
    free(s->texs);
    for (i = 0; i < s->nshaders; i++) { free(s->shaders[i].source); osl_program_free(s->shaders[i].prog); free_real(&s->shaders[i]); free(s->shaders[i].graph_json); }
    free(s->shaders);
    free(s);
}

int nora_scene_add_pos(nora_scene *s, v3 p) { GROW(s->pos, s->npos, s->cap_pos, v3); s->pos[s->npos] = p; return s->npos++; }
int nora_scene_add_nrm(nora_scene *s, v3 n) { GROW(s->nrm, s->nnrm, s->cap_nrm, v3); s->nrm[s->nnrm] = n; return s->nnrm++; }
int nora_scene_add_uv(nora_scene *s, float u, float v) {
    if (s->nuv >= s->cap_uv) {
        int nc = s->cap_uv ? s->cap_uv * 2 : 16;
        float *np = (float *)realloc(s->uv, sizeof(float) * 2 * (size_t)nc);
        if (!np) return -1;
        s->uv = np; s->cap_uv = nc;
    }
    s->uv[s->nuv * 2] = u; s->uv[s->nuv * 2 + 1] = v;
    return s->nuv++;
}
int nora_scene_add_tri(nora_scene *s, const nora_tri *t) {
    GROW(s->tris, s->ntris, s->cap_tris, nora_tri);
    s->tris[s->ntris] = *t;
    s->built = 0;
    return s->ntris++;
}
int nora_scene_add_material(nora_scene *s, const nora_material *m) {
    GROW(s->mats, s->nmats, s->cap_mats, nora_material);
    s->mats[s->nmats] = *m;
    return s->nmats++;
}
int nora_scene_add_mesh(nora_scene *s, const char *name, int first_tri, int tri_count) {
    GROW(s->meshes, s->nmeshes, s->cap_meshes, nora_mesh);
    memset(&s->meshes[s->nmeshes], 0, sizeof(nora_mesh));
    strncpy(s->meshes[s->nmeshes].name, name ? name : "mesh", 63);
    s->meshes[s->nmeshes].first_tri = first_tri;
    s->meshes[s->nmeshes].tri_count = tri_count;
    s->meshes[s->nmeshes].scale = v3_make(1, 1, 1);
    s->meshes[s->nmeshes].rot = quat_identity();
    s->meshes[s->nmeshes].source = -1;
    s->meshes[s->nmeshes].source_mesh = -1;
    return s->nmeshes++;
}
int nora_scene_find_material(const nora_scene *s, const char *name) {
    int i;
    for (i = 0; i < s->nmats; i++) if (strcmp(s->mats[i].name, name) == 0) return i;
    return -1;
}

void nora_material_default(nora_material *m, const char *name) {
    memset(m, 0, sizeof *m);
    strncpy(m->name, name ? name : "material", sizeof m->name - 1);
    m->type = NORA_MAT_DIFFUSE;
    m->color = v3_make(0.8f, 0.8f, 0.8f);
    m->color_tex = -1;
    m->edge_tint = v3_make(1, 1, 1);
    m->roughness = 0.0f;
    m->ior = 1.5f;
    m->abbe = 0.0f;
    m->shader = -1;
}

int nora_scene_load_texture(nora_scene *s, const char *path) {
    int i, w = 0, h = 0;
    float *d;
    char full[1024];
    for (i = 0; i < s->ntexs; i++) if (strcmp(s->texs[i].path, path) == 0) return i;
    d = nora_load_image_linear(path, &w, &h);
    if (!d && s->base_dir[0] && path[0] != '/') {
        snprintf(full, sizeof full, "%s/%s", s->base_dir, path);
        d = nora_load_image_linear(full, &w, &h);
    }
    if (!d) return -1;
    GROW(s->texs, s->ntexs, s->cap_texs, nora_texture);
    memset(&s->texs[s->ntexs], 0, sizeof(nora_texture));
    strncpy(s->texs[s->ntexs].path, path, sizeof s->texs[0].path - 1);
    s->texs[s->ntexs].w = w;
    s->texs[s->ntexs].h = h;
    s->texs[s->ntexs].rgb = d;
    return s->ntexs++;
}

v3 nora_texture_sample(const nora_texture *t, float s, float tt) {
    float x, y, fx, fy;
    int x0, y0, x1, y1;
    const float *p;
    v3 a, b, c, d;
    if (!t || !t->rgb || t->w <= 0) return v3_make(1, 0, 1);
    s = s - floorf(s); tt = tt - floorf(tt);
    x = s * (float)t->w - 0.5f; y = tt * (float)t->h - 0.5f;
    x0 = (int)floorf(x); y0 = (int)floorf(y);
    fx = x - (float)x0; fy = y - (float)y0;
    x1 = x0 + 1; y1 = y0 + 1;
    x0 = ((x0 % t->w) + t->w) % t->w; x1 = ((x1 % t->w) + t->w) % t->w;
    y0 = ((y0 % t->h) + t->h) % t->h; y1 = ((y1 % t->h) + t->h) % t->h;
    p = t->rgb;
#define PX(xx, yy) v3_make(p[((size_t)(yy) * t->w + (xx)) * 3], p[((size_t)(yy) * t->w + (xx)) * 3 + 1], p[((size_t)(yy) * t->w + (xx)) * 3 + 2])
    a = PX(x0, y0); b = PX(x1, y0); c = PX(x0, y1); d = PX(x1, y1);
#undef PX
    return v3_lerp(v3_lerp(a, b, fx), v3_lerp(c, d, fx), fy);
}

static int g_real_osl = 0;
int nora_use_real_osl(int on) {
#ifdef NORA_WITH_OSL
    g_real_osl = on && nora_real_osl_available();
    return g_real_osl;
#else
    (void)on;
    g_real_osl = 0;
    return 0;
#endif
}

static void compile_real(nora_shader_slot *sl, const char *source) {
#ifdef NORA_WITH_OSL
    char err[512];
    if (sl->real) { nora_real_osl_free(sl->real); sl->real = NULL; }
    if (!g_real_osl) return;
    sl->real = nora_real_osl_compile(source, err, sizeof err);
    if (!sl->real) fprintf(stderr, "nora: real OSL: %s (using the built-in compiler)\n", err);
#else
    (void)sl; (void)source;
#endif
}

static void free_real(nora_shader_slot *sl) {
#ifdef NORA_WITH_OSL
    if (sl->real) nora_real_osl_free(sl->real);
#endif
    sl->real = NULL;
}

static int scan_may_emit(const char *src) { return src && strstr(src, "emission") != NULL; }

static int load_shader_textures(nora_scene *s, const osl_program *p) {
    int i, n = osl_program_texture_count(p), bad = 0;
    for (i = 0; i < n; i++) if (nora_scene_load_texture(s, osl_program_texture_name(p, i)) < 0) bad++;
    return bad;
}

int nora_scene_add_shader(nora_scene *s, const char *name, const char *source, char *err, size_t errcap) {
    osl_program *p = osl_compile(source, err, errcap);
    nora_shader_slot *sl;
    if (!p) return -1;
    GROW(s->shaders, s->nshaders, s->cap_shaders, nora_shader_slot);
    sl = &s->shaders[s->nshaders];
    memset(sl, 0, sizeof *sl);
    strncpy(sl->name, name ? name : osl_program_name(p), sizeof sl->name - 1);
    sl->source = (char *)malloc(strlen(source) + 1);
    if (sl->source) strcpy(sl->source, source);
    sl->prog = p;
    sl->may_emit = osl_program_may_emit(p) || scan_may_emit(source);
    load_shader_textures(s, p);
    compile_real(sl, source);
    s->built = 0;
    return s->nshaders++;
}

int nora_scene_replace_shader(nora_scene *s, int slot, const char *source, char *err, size_t errcap) {
    osl_program *p;
    nora_shader_slot *sl;
    if (slot < 0 || slot >= s->nshaders) return -1;
    p = osl_compile(source, err, errcap);
    if (!p) return -1;
    sl = &s->shaders[slot];
    osl_program_free(sl->prog);
    free(sl->source);
    sl->source = (char *)malloc(strlen(source) + 1);
    if (sl->source) strcpy(sl->source, source);
    sl->prog = p;
    sl->may_emit = osl_program_may_emit(p) || scan_may_emit(source);
    load_shader_textures(s, p);
    compile_real(sl, source);
    s->built = 0;
    return slot;
}

/* ------------------------------------------------------------------ BVH */

typedef struct bprim { float bmin[3], bmax[3], c[3]; } bprim;
typedef struct bbuild {
    nora_scene *s;
    bprim *prims;
    int *idx;
    nora_bvh_node *nodes;
    int nnodes, cap;
} bbuild;

#define BVH_BINS 16
#define BVH_LEAF 4

static float area3(const float mn[3], const float mx[3]) {
    float dx = mx[0] - mn[0], dy = mx[1] - mn[1], dz = mx[2] - mn[2];
    if (dx < 0 || dy < 0 || dz < 0) return 0.0f;
    return 2.0f * (dx * dy + dy * dz + dz * dx);
}

static int bvh_new_node(bbuild *b) {
    if (b->nnodes >= b->cap) {
        int nc = b->cap ? b->cap * 2 : 256;
        nora_bvh_node *nn = (nora_bvh_node *)realloc(b->nodes, sizeof(nora_bvh_node) * (size_t)nc);
        if (!nn) return -1;
        b->nodes = nn; b->cap = nc;
    }
    return b->nnodes++;
}

static void bvh_split(bbuild *b, int ni, int first, int count, int depth) {
    float bmin[3] = { NORA_INFF, NORA_INFF, NORA_INFF }, bmax[3] = { -NORA_INFF, -NORA_INFF, -NORA_INFF };
    float cmin[3] = { NORA_INFF, NORA_INFF, NORA_INFF }, cmax[3] = { -NORA_INFF, -NORA_INFF, -NORA_INFF };
    int i, a, best_axis = -1, best_bin = -1, mid, left, right;
    float best_cost = NORA_INFF, leaf_cost;
    for (i = first; i < first + count; i++) {
        const bprim *p = &b->prims[b->idx[i]];
        for (a = 0; a < 3; a++) {
            bmin[a] = fminf(bmin[a], p->bmin[a]); bmax[a] = fmaxf(bmax[a], p->bmax[a]);
            cmin[a] = fminf(cmin[a], p->c[a]); cmax[a] = fmaxf(cmax[a], p->c[a]);
        }
    }
    for (a = 0; a < 3; a++) { b->nodes[ni].bmin[a] = bmin[a]; b->nodes[ni].bmax[a] = bmax[a]; }
    b->nodes[ni].left_or_first = first;
    b->nodes[ni].count = count;
    if (count <= BVH_LEAF || depth > 60) return;
    leaf_cost = (float)count;
    for (a = 0; a < 3; a++) {
        float ext = cmax[a] - cmin[a];
        int cnt[BVH_BINS];
        float lmn[BVH_BINS][3], lmx[BVH_BINS][3];
        float rarea[BVH_BINS];
        int rcnt[BVH_BINS];
        float accmn[3], accmx[3];
        int k, acc;
        if (ext <= 1e-12f) continue;
        for (k = 0; k < BVH_BINS; k++) {
            cnt[k] = 0;
            lmn[k][0] = lmn[k][1] = lmn[k][2] = NORA_INFF;
            lmx[k][0] = lmx[k][1] = lmx[k][2] = -NORA_INFF;
        }
        for (i = first; i < first + count; i++) {
            const bprim *p = &b->prims[b->idx[i]];
            int bin = (int)((p->c[a] - cmin[a]) / ext * BVH_BINS);
            int q;
            if (bin >= BVH_BINS) bin = BVH_BINS - 1;
            if (bin < 0) bin = 0;
            cnt[bin]++;
            for (q = 0; q < 3; q++) { lmn[bin][q] = fminf(lmn[bin][q], p->bmin[q]); lmx[bin][q] = fmaxf(lmx[bin][q], p->bmax[q]); }
        }
        /* sweep from the right */
        accmn[0] = accmn[1] = accmn[2] = NORA_INFF; accmx[0] = accmx[1] = accmx[2] = -NORA_INFF; acc = 0;
        for (k = BVH_BINS - 1; k > 0; k--) {
            int q;
            acc += cnt[k];
            for (q = 0; q < 3; q++) { accmn[q] = fminf(accmn[q], lmn[k][q]); accmx[q] = fmaxf(accmx[q], lmx[k][q]); }
            rarea[k] = area3(accmn, accmx);
            rcnt[k] = acc;
        }
        accmn[0] = accmn[1] = accmn[2] = NORA_INFF; accmx[0] = accmx[1] = accmx[2] = -NORA_INFF; acc = 0;
        for (k = 0; k < BVH_BINS - 1; k++) {
            int q;
            float cost;
            acc += cnt[k];
            for (q = 0; q < 3; q++) { accmn[q] = fminf(accmn[q], lmn[k][q]); accmx[q] = fmaxf(accmx[q], lmx[k][q]); }
            if (acc == 0 || rcnt[k + 1] == 0) continue;
            cost = 0.125f + (area3(accmn, accmx) * (float)acc + rarea[k + 1] * (float)rcnt[k + 1]) / fmaxf(area3(bmin, bmax), 1e-20f);
            if (cost < best_cost) { best_cost = cost; best_axis = a; best_bin = k; }
        }
    }
    if (best_axis < 0) {
        /* all centroids coincide: split in the middle of the list if too big */
        if (count <= 16) return;
        mid = first + count / 2;
    } else {
        float ext = cmax[best_axis] - cmin[best_axis];
        int lo = first, hi = first + count - 1;
        if (best_cost >= leaf_cost && count <= 16) return;
        while (lo <= hi) {
            const bprim *p = &b->prims[b->idx[lo]];
            int bin = (int)((p->c[best_axis] - cmin[best_axis]) / ext * BVH_BINS);
            if (bin >= BVH_BINS) bin = BVH_BINS - 1;
            if (bin <= best_bin) lo++;
            else { int t = b->idx[lo]; b->idx[lo] = b->idx[hi]; b->idx[hi] = t; hi--; }
        }
        mid = lo;
        if (mid == first || mid == first + count) mid = first + count / 2;
    }
    left = bvh_new_node(b);
    right = bvh_new_node(b);
    if (left < 0 || right < 0) return;
    b->nodes[ni].left_or_first = left;
    b->nodes[ni].count = 0;
    bvh_split(b, left, first, mid - first, depth + 1);
    bvh_split(b, right, mid, first + count - mid, depth + 1);
}

static int tri_live(const nora_scene *s, int i) {
    int m = s->tris[i].mesh;
    return !(m >= 0 && m < s->nmeshes && s->meshes[m].hidden);
}

static int build_bvh(nora_scene *s) {
    bbuild b;
    int i, root, nlive = 0;
    memset(&b, 0, sizeof b);
    b.s = s;
    if (s->ntris == 0) return 0;
    b.prims = (bprim *)malloc(sizeof(bprim) * (size_t)s->ntris);
    b.idx = (int *)malloc(sizeof(int) * (size_t)s->ntris);
    if (!b.prims || !b.idx) { free(b.prims); free(b.idx); return -1; }
    for (i = 0; i < s->ntris; i++) {
        v3 p0 = s->pos[s->tris[i].v[0]], p1 = s->pos[s->tris[i].v[1]], p2 = s->pos[s->tris[i].v[2]];
        v3 mn = v3_min(p0, v3_min(p1, p2)), mx = v3_max(p0, v3_max(p1, p2));
        b.prims[i].bmin[0] = mn.x; b.prims[i].bmin[1] = mn.y; b.prims[i].bmin[2] = mn.z;
        b.prims[i].bmax[0] = mx.x; b.prims[i].bmax[1] = mx.y; b.prims[i].bmax[2] = mx.z;
        b.prims[i].c[0] = 0.5f * (mn.x + mx.x); b.prims[i].c[1] = 0.5f * (mn.y + mx.y); b.prims[i].c[2] = 0.5f * (mn.z + mx.z);
        if (tri_live(s, i)) b.idx[nlive++] = i;
    }
    if (nlive == 0) { free(b.prims); free(b.idx); return 0; }
    root = bvh_new_node(&b);
    bvh_split(&b, root, 0, nlive, 0);
    s->nodes = b.nodes;
    s->nnodes = b.nnodes;
    s->prim_idx = b.idx;
    s->accel = (nora_tri_accel *)malloc(sizeof(nora_tri_accel) * (size_t)nlive);
    if (!s->accel) { free(b.prims); return -1; }
    for (i = 0; i < nlive; i++) {
        const nora_tri *t = &s->tris[b.idx[i]];
        v3 p0 = s->pos[t->v[0]];
        s->accel[i].p0 = p0;
        s->accel[i].e1 = v3_sub(s->pos[t->v[1]], p0);
        s->accel[i].e2 = v3_sub(s->pos[t->v[2]], p0);
    }
    free(b.prims);
    return 0;
}

static int tri_is_emitter(const nora_scene *s, const nora_tri *t) {
    const nora_material *m;
    if (t->mat < 0 || t->mat >= s->nmats) return 0;
    m = &s->mats[t->mat];
    if (m->shader >= 0 && m->shader < s->nshaders) return s->shaders[m->shader].may_emit;
    return m->emission_strength > 0.0f && v3_maxc(m->emission) > 0.0f;
}

static float tri_area(const nora_scene *s, const nora_tri *t) {
    v3 e1 = v3_sub(s->pos[t->v[1]], s->pos[t->v[0]]), e2 = v3_sub(s->pos[t->v[2]], s->pos[t->v[0]]);
    return 0.5f * v3_len(v3_cross(e1, e2));
}

static int build_lights(nora_scene *s) {
    int i, n = 0;
    double total = 0.0;
    float *w;
    s->tri_light_prob = (float *)calloc((size_t)(s->ntris > 0 ? s->ntris : 1), sizeof(float));
    for (i = 0; i < s->ntris; i++) if (tri_live(s, i) && tri_is_emitter(s, &s->tris[i])) n++;
    s->nlights = n;
    s->light_tris = (int *)malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
    s->light_cdf = (float *)malloc(sizeof(float) * (size_t)(n + 1));
    w = (float *)malloc(sizeof(float) * (size_t)(n > 0 ? n : 1));
    if (!s->tri_light_prob || !s->light_tris || !s->light_cdf || !w) { free(w); return -1; }
    n = 0;
    for (i = 0; i < s->ntris; i++) {
        const nora_tri *t = &s->tris[i];
        if (!tri_live(s, i) || !tri_is_emitter(s, t)) continue;
        {
            const nora_material *m = &s->mats[t->mat];
            float power = (m->shader >= 0) ? 1.0f : nlum(m->emission) * m->emission_strength;
            if (power <= 0.0f) power = 1e-6f;
            w[n] = tri_area(s, t) * power;
            s->light_tris[n++] = i;
            total += w[n - 1];
        }
    }
    s->env_prob = 0.0f;
    if (v3_maxc(s->env_color) > 0.0f) s->env_prob = n > 0 ? 0.25f : 1.0f;
    s->light_cdf[0] = 0.0f;
    for (i = 0; i < n; i++) s->light_cdf[i + 1] = s->light_cdf[i] + (float)(w[i] / (total > 0 ? total : 1.0));
    if (n > 0) s->light_cdf[n] = 1.0f;
    for (i = 0; i < n; i++)
        s->tri_light_prob[s->light_tris[i]] = (1.0f - s->env_prob) * (s->light_cdf[i + 1] - s->light_cdf[i]);
    free(w);
    return 0;
}

void nora_scene_bounds(const nora_scene *s, v3 *bmin, v3 *bmax) {
    int i;
    v3 mn = v3_splat(NORA_INFF), mx = v3_splat(-NORA_INFF);
    int any = 0;
    for (i = 0; i < s->ntris; i++) {
        int k;
        if (!tri_live(s, i)) continue;
        any = 1;
        for (k = 0; k < 3; k++) { mn = v3_min(mn, s->pos[s->tris[i].v[k]]); mx = v3_max(mx, s->pos[s->tris[i].v[k]]); }
    }
    if (!any) { mn = v3_splat(-1); mx = v3_splat(1); }
    *bmin = mn; *bmax = mx;
}

void nora_scene_invalidate(nora_scene *s) { s->built = 0; }

int nora_scene_build(nora_scene *s) {
    free_built(s);
    if (s->owned) nora_scene_apply_transforms(s);
    nora_scene_bounds(s, &s->bmin, &s->bmax);
    if (build_bvh(s) != 0) return -1;
    if (build_lights(s) != 0) return -1;
    s->built = 1;
    return 0;
}

void nora_scene_frame_camera(nora_scene *s) {
    v3 mn, mx, c, ext;
    float r, d;
    nora_scene_bounds(s, &mn, &mx);
    c = v3_scale(v3_add(mn, mx), 0.5f);
    ext = v3_sub(mx, mn);
    r = 0.5f * v3_len(ext);
    if (r <= 0) r = 1;
    s->cam.vfov_deg = 40.0f;
    d = r / sinf(0.5f * s->cam.vfov_deg * NORA_PI / 180.0f);
    s->cam.target = c;
    s->cam.pos = v3_add(c, v3_scale(v3_norm(v3_make(0.35f, 0.3f, 1.0f)), d * 1.05f));
    s->cam.up = v3_make(0, 1, 0);
    s->cam.aperture = 0.0f;
    s->cam.focus_dist = 0.0f;
}

/* ------------------------------------------------------------ traversal */

static int ray_box(const float bmin[3], const float bmax[3], const float o[3], const float inv[3], float tmax, float *tnear) {
    float t0 = 0.0f, t1 = tmax;
    int a;
    for (a = 0; a < 3; a++) {
        float tn = (bmin[a] - o[a]) * inv[a];
        float tf = (bmax[a] - o[a]) * inv[a];
        if (tn > tf) { float t = tn; tn = tf; tf = t; }
        t0 = tn > t0 ? tn : t0;
        t1 = tf < t1 ? tf : t1;
        if (t0 > t1) return 0;
    }
    *tnear = t0;
    return 1;
}

static int tri_hit(const nora_tri_accel *t, const ray3 *r, float tmax, float *tt, float *b1, float *b2) {
    v3 pv = v3_cross(r->d, t->e2);
    float det = v3_dot(t->e1, pv), inv, u, v, d;
    v3 tv, qv;
    if (det > -1e-12f && det < 1e-12f) return 0;
    inv = 1.0f / det;
    tv = v3_sub(r->o, t->p0);
    u = v3_dot(tv, pv) * inv;
    if (u < 0.0f || u > 1.0f) return 0;
    qv = v3_cross(tv, t->e1);
    v = v3_dot(r->d, qv) * inv;
    if (v < 0.0f || u + v > 1.0f) return 0;
    d = v3_dot(t->e2, qv) * inv;
    if (d <= r->tmin || d >= tmax) return 0;
    *tt = d; *b1 = u; *b2 = v;
    return 1;
}

static int traverse(const nora_scene *s, const ray3 *r, nora_hit *hit, int any) {
    int stack[96], sp = 0, found = 0, ni = 0;
    float o[3], inv[3], tmax = r->tmax, tn;
    if (!s->built || s->nnodes == 0) return 0;
    o[0] = r->o.x; o[1] = r->o.y; o[2] = r->o.z;
    inv[0] = 1.0f / (r->d.x != 0.0f ? r->d.x : 1e-30f);
    inv[1] = 1.0f / (r->d.y != 0.0f ? r->d.y : 1e-30f);
    inv[2] = 1.0f / (r->d.z != 0.0f ? r->d.z : 1e-30f);
    if (!ray_box(s->nodes[0].bmin, s->nodes[0].bmax, o, inv, tmax, &tn)) return 0;
    for (;;) {
        const nora_bvh_node *n = &s->nodes[ni];
        if (n->count > 0) {
            int i;
            for (i = n->left_or_first; i < n->left_or_first + n->count; i++) {
                float t, b1, b2;
                if (tri_hit(&s->accel[i], r, tmax, &t, &b1, &b2)) {
                    found = 1;
                    if (any) return 1;
                    tmax = t;
                    hit->t = t; hit->b1 = b1; hit->b2 = b2; hit->tri = s->prim_idx[i];
                }
            }
        } else {
            int l = n->left_or_first, rr = l + 1;
            float tl = 0, tr = 0;
            int hl = ray_box(s->nodes[l].bmin, s->nodes[l].bmax, o, inv, tmax, &tl);
            int hr = ray_box(s->nodes[rr].bmin, s->nodes[rr].bmax, o, inv, tmax, &tr);
            if (hl && hr) {
                if (tr < tl) { int t = l; l = rr; rr = t; }
                if (sp < 96) stack[sp++] = rr;
                ni = l;
                continue;
            } else if (hl) { ni = l; continue; }
            else if (hr) { ni = rr; continue; }
        }
        if (sp == 0) break;
        ni = stack[--sp];
    }
    return found;
}

int nora_intersect(const nora_scene *s, const ray3 *r, nora_hit *hit) { return traverse(s, r, hit, 0); }
int nora_occluded(const nora_scene *s, const ray3 *r) { nora_hit h; return traverse(s, r, &h, 1); }

/* ------------------------------------------------------------ instances */

static int ensure_rest_cap(nora_scene *s) {
    if (s->cap_rest_pos < s->cap_pos) {
        v3 *np = (v3 *)realloc(s->rest_pos, sizeof(v3) * (size_t)s->cap_pos);
        if (!np) return -1;
        s->rest_pos = np; s->cap_rest_pos = s->cap_pos;
    }
    if (s->cap_rest_nrm < s->cap_nrm) {
        v3 *nn = (v3 *)realloc(s->rest_nrm, sizeof(v3) * (size_t)s->cap_nrm);
        if (!nn) return -1;
        s->rest_nrm = nn; s->cap_rest_nrm = s->cap_nrm;
    }
    return 0;
}
static int push_pos(nora_scene *s, v3 rest, v3 world) {
    int i = nora_scene_add_pos(s, world);
    if (i < 0 || ensure_rest_cap(s) != 0) return -1;
    s->rest_pos[i] = rest;
    return i;
}
static int push_nrm(nora_scene *s, v3 rest, v3 world) {
    int i = nora_scene_add_nrm(s, world);
    if (i < 0 || ensure_rest_cap(s) != 0) return -1;
    s->rest_nrm[i] = rest;
    return i;
}

void nora_mesh_set_euler(nora_mesh *m, v3 e) { m->euler_deg = e; m->rot = quat_from_euler_deg(e); }
void nora_mesh_set_rot(nora_mesh *m, quat q) { m->rot = quat_normalize(q); m->euler_deg = quat_to_euler_deg(m->rot); }
v3 nora_mesh_xform_point(const nora_mesh *m, v3 p) {
    return v3_add(v3_add(m->pivot, m->translate), quat_rotate(m->rot, v3_mul(m->scale, v3_sub(p, m->pivot))));
}
v3 nora_mesh_center(const nora_mesh *m) { return v3_add(m->pivot, m->translate); }

int nora_scene_own_vertices(nora_scene *s) {
    const v3 *srcp = s->owned ? s->rest_pos : s->pos, *srcn = s->owned ? s->rest_nrm : s->nrm;
    int cap_p = s->ntris * 3 + 1, cap_n = s->ntris * 3 + 1, np = 0, nn = 0, m, t, k;
    v3 *P = (v3 *)malloc(sizeof(v3) * (size_t)cap_p), *N = (v3 *)malloc(sizeof(v3) * (size_t)cap_n);
    int *pstamp = (int *)malloc(sizeof(int) * (size_t)(s->npos + 1)), *pmap = (int *)malloc(sizeof(int) * (size_t)(s->npos + 1));
    int *nstamp = (int *)malloc(sizeof(int) * (size_t)(s->nnrm + 1)), *nmap = (int *)malloc(sizeof(int) * (size_t)(s->nnrm + 1));
    char *done = (char *)calloc((size_t)s->ntris + 1, 1);
    if (!P || !N || !pstamp || !pmap || !nstamp || !nmap || !done) {
        free(P); free(N); free(pstamp); free(pmap); free(nstamp); free(nmap); free(done);
        return -1;
    }
    for (k = 0; k < s->npos; k++) pstamp[k] = -1;
    for (k = 0; k < s->nnrm; k++) nstamp[k] = -1;
    for (m = 0; m <= s->nmeshes; m++) {
        int lo, hi;
        nora_mesh *me = m < s->nmeshes ? &s->meshes[m] : NULL;
        if (me) { lo = me->first_tri; hi = me->first_tri + me->tri_count; me->first_pos = np; me->first_nrm = nn; }
        else { lo = 0; hi = s->ntris; }              /* triangles outside every mesh */
        for (t = lo; t < hi && t < s->ntris; t++) {
            nora_tri *tr = &s->tris[t];
            if (done[t]) continue;
            done[t] = 1;
            if (me) tr->mesh = m;
            for (k = 0; k < 3; k++) {
                int v = tr->v[k], n = tr->n[k];
                if (pstamp[v] != m) { pstamp[v] = m; pmap[v] = np; P[np++] = srcp[v]; }
                tr->v[k] = pmap[v];
                if (n >= 0) {
                    if (nstamp[n] != m) { nstamp[n] = m; nmap[n] = nn; N[nn++] = srcn[n]; }
                    tr->n[k] = nmap[n];
                }
            }
        }
        if (me) {
            v3 mn = v3_splat(NORA_INFF), mx = v3_splat(-NORA_INFF);
            me->npos = np - me->first_pos;
            me->nnrm = nn - me->first_nrm;
            for (k = me->first_pos; k < np; k++) { mn = v3_min(mn, P[k]); mx = v3_max(mx, P[k]); }
            me->pivot = me->npos > 0 ? v3_scale(v3_add(mn, mx), 0.5f) : v3_make(0, 0, 0);
        }
    }
    free(pstamp); free(pmap); free(nstamp); free(nmap); free(done);
    free(s->pos); free(s->nrm); free(s->rest_pos); free(s->rest_nrm);
    s->pos = P; s->npos = np; s->cap_pos = cap_p;
    s->nrm = N; s->nnrm = nn; s->cap_nrm = cap_n;
    s->rest_pos = (v3 *)malloc(sizeof(v3) * (size_t)cap_p);
    s->rest_nrm = (v3 *)malloc(sizeof(v3) * (size_t)cap_n);
    if (!s->rest_pos || !s->rest_nrm) return -1;
    s->cap_rest_pos = cap_p; s->cap_rest_nrm = cap_n;
    memcpy(s->rest_pos, P, sizeof(v3) * (size_t)np);
    memcpy(s->rest_nrm, N, sizeof(v3) * (size_t)nn);
    s->owned = 1;
    nora_scene_apply_transforms(s);
    s->built = 0;
    return 0;
}

void nora_scene_apply_transforms(nora_scene *s) {
    int m, i;
    if (!s->owned) return;
    for (m = 0; m < s->nmeshes; m++) {
        const nora_mesh *me = &s->meshes[m];
        int ident = me->translate.x == 0.0f && me->translate.y == 0.0f && me->translate.z == 0.0f && me->scale.x == 1.0f &&
                    me->scale.y == 1.0f && me->scale.z == 1.0f && me->rot.x == 0.0f && me->rot.y == 0.0f && me->rot.z == 0.0f;
        v3 inv;
        if (ident) {
            memcpy(s->pos + me->first_pos, s->rest_pos + me->first_pos, sizeof(v3) * (size_t)me->npos);
            memcpy(s->nrm + me->first_nrm, s->rest_nrm + me->first_nrm, sizeof(v3) * (size_t)me->nnrm);
            continue;
        }
        inv = v3_make(me->scale.x != 0.0f ? 1.0f / me->scale.x : 1e8f, me->scale.y != 0.0f ? 1.0f / me->scale.y : 1e8f,
                         me->scale.z != 0.0f ? 1.0f / me->scale.z : 1e8f);
        for (i = me->first_pos; i < me->first_pos + me->npos; i++) s->pos[i] = nora_mesh_xform_point(me, s->rest_pos[i]);
        for (i = me->first_nrm; i < me->first_nrm + me->nnrm; i++) s->nrm[i] = v3_norm(quat_rotate(me->rot, v3_mul(s->rest_nrm[i], inv)));
    }
    s->built = 0;
}

void nora_mesh_world_bounds(const nora_scene *s, int mesh, v3 *bmin, v3 *bmax) {
    v3 mn = v3_splat(NORA_INFF), mx = v3_splat(-NORA_INFF);
    int t, k;
    const nora_mesh *me = &s->meshes[mesh];
    for (t = me->first_tri; t < me->first_tri + me->tri_count; t++)
        for (k = 0; k < 3; k++) { mn = v3_min(mn, s->pos[s->tris[t].v[k]]); mx = v3_max(mx, s->pos[s->tris[t].v[k]]); }
    if (me->tri_count == 0) { mn = mx = nora_mesh_center(me); }
    *bmin = mn; *bmax = mx;
}

int nora_scene_live_mesh_count(const nora_scene *s) {
    int i, n = 0;
    for (i = 0; i < s->nmeshes; i++) if (!s->meshes[i].hidden) n++;
    return n;
}

int nora_scene_add_source(nora_scene *s, nora_source_kind kind, const char *path) {
    GROW(s->sources, s->nsources, s->cap_sources, nora_source);
    memset(&s->sources[s->nsources], 0, sizeof(nora_source));
    s->sources[s->nsources].kind = kind;
    if (path) nora_path_abs(path, s->sources[s->nsources].path, sizeof s->sources[0].path);
    return s->nsources++;
}

static int mesh_name_used(const nora_scene *s, const char *name) {
    int i;
    for (i = 0; i < s->nmeshes; i++) if (!s->meshes[i].hidden && !strcmp(s->meshes[i].name, name)) return 1;
    return 0;
}

int nora_scene_duplicate_mesh(nora_scene *s, int m) {
    nora_mesh src;
    int nm, i, k, n = 1, fp, fn;
    char name[64];
    if (m < 0 || m >= s->nmeshes) return -1;
    if (!s->owned && nora_scene_own_vertices(s) != 0) return -1;
    src = s->meshes[m];
    {
        /* "Name" -> "Name.001", "Name.001" -> "Name.002" */
        char base[64];
        const char *dot = strrchr(src.name, '.');
        snprintf(base, sizeof base, "%s", src.name);
        if (dot && strlen(dot) == 4 && dot[1] >= '0' && dot[1] <= '9') base[dot - src.name] = 0;
        do { snprintf(name, sizeof name, "%.56s.%03d", base, n++ % 1000); } while (mesh_name_used(s, name) && n < 1000);
    }
    nm = nora_scene_add_mesh(s, name, s->ntris, src.tri_count);
    if (nm < 0) return -1;
    fp = s->npos;
    fn = s->nnrm;
    for (i = 0; i < src.npos; i++) if (push_pos(s, s->rest_pos[src.first_pos + i], s->pos[src.first_pos + i]) < 0) return -1;
    for (i = 0; i < src.nnrm; i++) if (push_nrm(s, s->rest_nrm[src.first_nrm + i], s->nrm[src.first_nrm + i]) < 0) return -1;
    for (i = 0; i < src.tri_count; i++) {
        nora_tri t = s->tris[src.first_tri + i];
        for (k = 0; k < 3; k++) {
            t.v[k] = t.v[k] - src.first_pos + fp;
            if (t.n[k] >= 0) t.n[k] = t.n[k] - src.first_nrm + fn;
        }
        t.mesh = nm;
        if (nora_scene_add_tri(s, &t) < 0) return -1;
    }
    {
        nora_mesh *d = &s->meshes[nm];
        d->translate = src.translate; d->scale = src.scale; d->euler_deg = src.euler_deg; d->rot = src.rot;
        d->pivot = src.pivot; d->source = src.source; d->source_mesh = src.source_mesh;
        d->first_pos = fp; d->npos = src.npos; d->first_nrm = fn; d->nnrm = src.nnrm;
    }
    s->built = 0;
    return nm;
}

static int find_texture(const nora_scene *s, const char *path) {
    int i;
    for (i = 0; i < s->ntexs; i++) if (!strcmp(s->texs[i].path, path)) return i;
    return -1;
}

static int copy_texture(nora_scene *dst, const nora_scene *src, int ti) {
    const nora_texture *t;
    char full[1024];
    int i;
    size_t n;
    if (ti < 0 || ti >= src->ntexs) return -1;
    t = &src->texs[ti];
    if (nora_path_is_abs(t->path) || !src->base_dir[0]) snprintf(full, sizeof full, "%s", t->path);
    else { char j[1024]; nora_path_join(src->base_dir, t->path, j, sizeof j); nora_path_abs(j, full, sizeof full); }
    if ((i = find_texture(dst, full)) >= 0) return i;
    GROW(dst->texs, dst->ntexs, dst->cap_texs, nora_texture);
    memset(&dst->texs[dst->ntexs], 0, sizeof(nora_texture));
    nora_strlcpy(dst->texs[dst->ntexs].path, full, sizeof dst->texs[0].path);
    dst->texs[dst->ntexs].w = t->w;
    dst->texs[dst->ntexs].h = t->h;
    n = (size_t)t->w * (size_t)t->h * 3;
    dst->texs[dst->ntexs].rgb = (float *)malloc(sizeof(float) * (n ? n : 1));
    if (dst->texs[dst->ntexs].rgb && n) memcpy(dst->texs[dst->ntexs].rgb, t->rgb, sizeof(float) * n);
    return dst->ntexs++;
}

static int copy_shader(nora_scene *dst, const nora_shader_slot *sl) {
    char err[512];
    int slot = nora_scene_add_shader(dst, sl->name, sl->source ? sl->source : "", err, sizeof err);
    nora_shader_slot *d;
    if (slot < 0) { fprintf(stderr, "nora: shader %s: %s\n", sl->name, err); return -1; }
    d = &dst->shaders[slot];
    snprintf(d->file, sizeof d->file, "%s", sl->file);
    snprintf(d->graph_file, sizeof d->graph_file, "%s", sl->graph_file);
    if (sl->graph_json) {
        d->graph_json = (char *)malloc(strlen(sl->graph_json) + 1);
        if (d->graph_json) strcpy(d->graph_json, sl->graph_json);
    }
    return slot;
}

static int map_material(nora_scene *dst, const nora_scene *src, int mi, int *matmap) {
    nora_material m;
    int found;
    if (mi < 0 || mi >= src->nmats) return 0;
    if (matmap[mi] >= 0) return matmap[mi];
    found = nora_scene_find_material(dst, src->mats[mi].name);
    if (found >= 0) return matmap[mi] = found;
    m = src->mats[mi];
    m.color_tex = m.color_tex >= 0 ? copy_texture(dst, src, m.color_tex) : -1;
    m.shader = (m.shader >= 0 && m.shader < src->nshaders) ? copy_shader(dst, &src->shaders[m.shader]) : -1;
    return matmap[mi] = nora_scene_add_material(dst, &m);
}

int nora_scene_append(nora_scene *dst, const nora_scene *src, const int *list, int n) {
    int *matmap, *uvmap, i, k, first = dst->nmeshes, soff = dst->nsources, li;
    if (!src->owned) return -1;
    if (!dst->owned && nora_scene_own_vertices(dst) != 0) return -1;
    matmap = (int *)malloc(sizeof(int) * (size_t)(src->nmats + 1));
    uvmap = (int *)malloc(sizeof(int) * (size_t)(src->nuv + 1));
    if (!matmap || !uvmap) { free(matmap); free(uvmap); return -1; }
    for (i = 0; i < src->nmats; i++) matmap[i] = -1;
    for (i = 0; i < src->nuv; i++) uvmap[i] = -1;
    for (i = 0; !list && i < src->nsources; i++) {
        if (nora_scene_add_source(dst, src->sources[i].kind, NULL) < 0) break;
        dst->sources[dst->nsources - 1] = src->sources[i];
    }
    for (li = 0; li < (list ? n : src->nmeshes); li++) {
        int mi = list ? list[li] : li, nm, fp, fn;
        nora_mesh sm;
        if (mi < 0 || mi >= src->nmeshes) continue;
        sm = src->meshes[mi];
        if (!list && sm.hidden) continue;
        nm = nora_scene_add_mesh(dst, sm.name, dst->ntris, sm.tri_count);
        if (nm < 0) break;
        fp = dst->npos; fn = dst->nnrm;
        for (i = 0; i < sm.npos; i++) push_pos(dst, src->rest_pos[sm.first_pos + i], src->pos[sm.first_pos + i]);
        for (i = 0; i < sm.nnrm; i++) push_nrm(dst, src->rest_nrm[sm.first_nrm + i], src->nrm[sm.first_nrm + i]);
        for (i = 0; i < sm.tri_count; i++) {
            nora_tri t = src->tris[sm.first_tri + i];
            for (k = 0; k < 3; k++) {
                t.v[k] = t.v[k] - sm.first_pos + fp;
                if (t.n[k] >= 0) t.n[k] = t.n[k] - sm.first_nrm + fn;
                if (t.t[k] >= 0) {
                    int u = t.t[k];
                    if (uvmap[u] < 0) uvmap[u] = nora_scene_add_uv(dst, src->uv[u * 2], src->uv[u * 2 + 1]);
                    t.t[k] = uvmap[u];
                }
            }
            t.mat = map_material(dst, src, t.mat, matmap);
            t.mesh = nm;
            nora_scene_add_tri(dst, &t);
        }
        {
            nora_mesh *d = &dst->meshes[nm];
            d->translate = sm.translate; d->scale = sm.scale; d->euler_deg = sm.euler_deg; d->rot = sm.rot;
            d->pivot = sm.pivot;
            d->source = (!list && sm.source >= 0) ? sm.source + soff : -1;
            d->source_mesh = sm.source_mesh;
            d->first_pos = fp; d->npos = sm.npos; d->first_nrm = fn; d->nnrm = sm.nnrm;
        }
    }
    free(matmap); free(uvmap);
    dst->built = 0;
    return dst->nmeshes > first ? first : -1;
}

int nora_scene_import_obj(nora_scene *s, const char *path, int *count, char *err, size_t errcap) {
    nora_scene *src = nora_scene_load_obj(path, err, errcap);
    int first;
    if (count) *count = 0;
    if (!src) return -1;
    first = nora_scene_append(s, src, NULL, 0);
    nora_scene_destroy(src);
    if (first < 0) { if (err) snprintf(err, errcap, "'%s' has no geometry", path); return -1; }
    if (count) *count = s->nmeshes - first;
    nora_scene_apply_transforms(s);
    return first;
}

/* ----------------------------------------------------------------- hash */

typedef struct fnv { uint64_t h; } fnv;
static void fnv_bytes(fnv *f, const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    size_t i;
    for (i = 0; i < n; i++) { f->h ^= b[i]; f->h *= 0x100000001b3ULL; }
}
static void fnv_f(fnv *f, float x) { uint32_t u; if (x == 0.0f) x = 0.0f; memcpy(&u, &x, 4); fnv_bytes(f, &u, 4); }
static void fnv_v3(fnv *f, v3 v) { fnv_f(f, v.x); fnv_f(f, v.y); fnv_f(f, v.z); }
static void fnv_i(fnv *f, int x) { int32_t v = (int32_t)x; fnv_bytes(f, &v, 4); }
static void fnv_s(fnv *f, const char *s) { if (s) fnv_bytes(f, s, strlen(s)); fnv_i(f, -7); }

uint64_t nora_scene_hash(nora_scene *s) {
    fnv f;
    int i, k;
    f.h = 0xcbf29ce484222325ULL;
    for (i = 0; i < s->ntris; i++) {
        const nora_tri *t = &s->tris[i];
        if (!tri_live(s, i)) continue;
        for (k = 0; k < 3; k++) {
            fnv_v3(&f, s->pos[t->v[k]]);
            if (t->n[k] >= 0) fnv_v3(&f, s->nrm[t->n[k]]); else fnv_i(&f, -1);
            if (t->t[k] >= 0) { fnv_f(&f, s->uv[t->t[k] * 2]); fnv_f(&f, s->uv[t->t[k] * 2 + 1]); } else fnv_i(&f, -1);
        }
        fnv_i(&f, t->mat);
    }
    for (i = 0; i < s->nmats; i++) {
        const nora_material *m = &s->mats[i];
        fnv_i(&f, (int)m->type); fnv_v3(&f, m->color); fnv_i(&f, m->color_tex); fnv_v3(&f, m->edge_tint);
        fnv_f(&f, m->roughness); fnv_f(&f, m->ior); fnv_f(&f, m->abbe); fnv_v3(&f, m->emission);
        fnv_f(&f, m->emission_strength); fnv_i(&f, m->two_sided);
        fnv_s(&f, (m->shader >= 0 && m->shader < s->nshaders) ? s->shaders[m->shader].source : "");
    }
    for (i = 0; i < s->ntexs; i++) {
        fnv_i(&f, s->texs[i].w); fnv_i(&f, s->texs[i].h);
        if (s->texs[i].rgb) fnv_bytes(&f, s->texs[i].rgb, sizeof(float) * 3 * (size_t)s->texs[i].w * (size_t)s->texs[i].h);
    }
    fnv_v3(&f, s->cam.pos); fnv_v3(&f, s->cam.target); fnv_v3(&f, s->cam.up);
    fnv_f(&f, s->cam.vfov_deg); fnv_f(&f, s->cam.aperture); fnv_f(&f, s->cam.focus_dist);
    fnv_v3(&f, s->env_color);
    return f.h;
}
