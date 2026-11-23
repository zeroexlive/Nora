/* Nora - scene container, SAH BVH, light distribution, textures. MIT License. */
#include "scene.h"
#include "image.h"
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
    for (i = 0; i < s->ntexs; i++) free(s->texs[i].rgb);
    free(s->texs);
    for (i = 0; i < s->nshaders; i++) { free(s->shaders[i].source); osl_program_free(s->shaders[i].prog); free_real(&s->shaders[i]); }
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

static int build_bvh(nora_scene *s) {
    bbuild b;
    int i, root;
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
        b.idx[i] = i;
    }
    root = bvh_new_node(&b);
    bvh_split(&b, root, 0, s->ntris, 0);
    s->nodes = b.nodes;
    s->nnodes = b.nnodes;
    s->prim_idx = b.idx;
    s->accel = (nora_tri_accel *)malloc(sizeof(nora_tri_accel) * (size_t)s->ntris);
    if (!s->accel) { free(b.prims); return -1; }
    for (i = 0; i < s->ntris; i++) {
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
    for (i = 0; i < s->ntris; i++) if (tri_is_emitter(s, &s->tris[i])) n++;
    s->nlights = n;
    s->light_tris = (int *)malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
    s->light_cdf = (float *)malloc(sizeof(float) * (size_t)(n + 1));
    w = (float *)malloc(sizeof(float) * (size_t)(n > 0 ? n : 1));
    if (!s->tri_light_prob || !s->light_tris || !s->light_cdf || !w) { free(w); return -1; }
    n = 0;
    for (i = 0; i < s->ntris; i++) {
        const nora_tri *t = &s->tris[i];
        if (!tri_is_emitter(s, t)) continue;
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
    for (i = 0; i < s->ntris; i++) {
        int k;
        for (k = 0; k < 3; k++) { mn = v3_min(mn, s->pos[s->tris[i].v[k]]); mx = v3_max(mx, s->pos[s->tris[i].v[k]]); }
    }
    if (s->ntris == 0) { mn = v3_splat(-1); mx = v3_splat(1); }
    *bmin = mn; *bmax = mx;
}

void nora_scene_invalidate(nora_scene *s) { s->built = 0; }

int nora_scene_build(nora_scene *s) {
    free_built(s);
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
