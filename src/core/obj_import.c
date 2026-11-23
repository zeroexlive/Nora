/* Nora - OBJ/MTL import. Geometry is parsed by the vendored third_party/obj
 * library (MIT); MTL statements are mapped to Nora materials here. MIT License.
 *
 * MTL mapping:
 *   d < 1, Tr > 0, Tf < 1 or illum 4/6/7/9  -> dielectric (Ni = IOR, Tf = tint, Pr = roughness)
 *   Pm >= 0.5 or illum 3                     -> conductor (Ks, else Kd, as reflectance; Pr or Ns roughness)
 *   Ks > 0 (and illum != 0/1)                -> plastic (Kd base, GGX coat with Ni)
 *   otherwise                                -> diffuse (Kd, map_Kd)
 *   Ke                                       -> emission (two-sided)
 * Nora extensions: "nora_type diffuse|conductor|dielectric|plastic", "nora_abbe V",
 * "nora_roughness r", "nora_edge r g b", "nora_shader file.osl" (built-in OSL compiler).
 */
#include "scene.h"
#include "obj.h"
#include "fsutil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct mtl_def {
    char name[64];
    v3 Kd, Ks, Ke, Tf, edge;
    float Ns, Ni, d, Pr, Pm, abbe, rough;
    int illum;
    int has_Ks, has_Tf, has_Pr, has_Pm, has_edge, has_rough, has_Ke;
    char map_Kd[512];
    char shader[512];
    char type[32];
} mtl_def;

static void mtl_init(mtl_def *m, const char *name) {
    memset(m, 0, sizeof *m);
    snprintf(m->name, sizeof m->name, "%s", name);
    m->Kd = v3_make(0.8f, 0.8f, 0.8f);
    m->Ns = 0.0f;
    m->Ni = 1.5f;
    m->d = 1.0f;
    m->illum = 2;
}

static char *skip_ws(char *p) { while (*p == ' ' || *p == '\t') p++; return p; }
static void rtrim(char *s) { size_t n = strlen(s); while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0; }
static v3 read3(const char *p) {
    float a = 0, b, c;
    int n = sscanf(p, "%f %f %f", &a, &b, &c);
    if (n == 1) return v3_make(a, a, a);
    if (n < 3) return v3_make(a, a, a);
    return v3_make(a, b, c);
}
/* last whitespace-separated token (map statements may carry options first) */
static const char *last_token(char *p) {
    char *t = p + strlen(p);
    while (t > p && !isspace((unsigned char)t[-1])) t--;
    return t;
}

static void dir_of(const char *path, char *dir, size_t cap) {
    const char *s1 = strrchr(path, '/'), *s2 = strrchr(path, '\\'), *s = s1 > s2 ? s1 : s2;
    if (!s) { snprintf(dir, cap, "."); return; }
    snprintf(dir, cap, "%.*s", (int)(s - path), path);
}

static int parse_mtl(const char *path, mtl_def **out, int *count) {
    FILE *f = fopen(path, "r");
    char line[1024];
    mtl_def *cur = NULL;
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        char *p = skip_ws(line);
        rtrim(p);
        if (!*p || *p == '#') continue;
        if (!strncmp(p, "newmtl", 6) && isspace((unsigned char)p[6])) {
            mtl_def *nl = (mtl_def *)realloc(*out, sizeof(mtl_def) * (size_t)(*count + 1));
            if (!nl) break;
            *out = nl;
            cur = &nl[(*count)++];
            mtl_init(cur, skip_ws(p + 6));
            continue;
        }
        if (!cur) continue;
#define KEY(k) (!strncmp(p, k, strlen(k)) && isspace((unsigned char)p[strlen(k)]))
        if (KEY("Kd")) cur->Kd = read3(p + 2);
        else if (KEY("Ks")) { cur->Ks = read3(p + 2); cur->has_Ks = 1; }
        else if (KEY("Ke")) { cur->Ke = read3(p + 2); cur->has_Ke = 1; }
        else if (KEY("Tf")) { cur->Tf = read3(p + 2); cur->has_Tf = 1; }
        else if (KEY("Ns")) cur->Ns = (float)atof(p + 2);
        else if (KEY("Ni")) cur->Ni = (float)atof(p + 2);
        else if (KEY("d")) cur->d = (float)atof(p + 1);
        else if (KEY("Tr")) cur->d = 1.0f - (float)atof(p + 2);
        else if (KEY("Pr")) { cur->Pr = (float)atof(p + 2); cur->has_Pr = 1; }
        else if (KEY("Pm")) { cur->Pm = (float)atof(p + 2); cur->has_Pm = 1; }
        else if (KEY("illum")) cur->illum = atoi(p + 5);
        else if (KEY("map_Kd")) snprintf(cur->map_Kd, sizeof cur->map_Kd, "%s", last_token(p));
        else if (KEY("nora_shader")) snprintf(cur->shader, sizeof cur->shader, "%s", skip_ws(p + 11));
        else if (KEY("nora_abbe")) cur->abbe = (float)atof(p + 9);
        else if (KEY("nora_type")) snprintf(cur->type, sizeof cur->type, "%s", skip_ws(p + 9));
        else if (KEY("nora_roughness")) { cur->rough = (float)atof(p + 14); cur->has_rough = 1; }
        else if (KEY("nora_edge")) { cur->edge = read3(p + 9); cur->has_edge = 1; }
#undef KEY
    }
    fclose(f);
    return 0;
}

static float ns_to_roughness(float ns) {
    /* Phong exponent -> GGX alpha (Walter et al. 2007: alpha_b = sqrt(2/(Ns+2))), roughness = sqrt(alpha) */
    float a;
    if (ns <= 0.0f) return 1.0f;
    a = sqrtf(2.0f / (ns + 2.0f));
    return nclampf(sqrtf(a), 0.0f, 1.0f);
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)n + 1);
    if (buf) { size_t r = fread(buf, 1, (size_t)n, f); buf[r] = 0; }
    fclose(f);
    return buf;
}

static void add_dep(nora_scene *s, int src, const char *path) {
    nora_source *so;
    if (src < 0 || src >= s->nsources) return;
    so = &s->sources[src];
    if (so->ndeps >= NORA_MAX_DEPS) return;
    nora_path_abs(path, so->deps[so->ndeps++], sizeof so->deps[0]);
}

static void map_material(nora_scene *s, const mtl_def *d, nora_material *m) {
    int transparent = d->d < 0.99f || d->illum == 4 || d->illum == 6 || d->illum == 7 || d->illum == 9 ||
                      (d->has_Tf && v3_maxc(d->Tf) < 0.99f && v3_maxc(d->Tf) > 0.0f);
    int metal = (d->has_Pm && d->Pm >= 0.5f) || d->illum == 3;
    float rough = d->has_rough ? d->rough : (d->has_Pr ? d->Pr : ns_to_roughness(d->Ns));
    nora_material_default(m, d->name);
    if (d->type[0]) {
        if (!strcmp(d->type, "diffuse")) { transparent = 0; metal = 0; m->type = NORA_MAT_DIFFUSE; }
        else if (!strcmp(d->type, "conductor")) { transparent = 0; metal = 1; }
        else if (!strcmp(d->type, "dielectric")) { transparent = 1; metal = 0; }
        else if (!strcmp(d->type, "plastic")) { transparent = 0; metal = 0; m->type = NORA_MAT_PLASTIC; }
    }
    if (transparent) {
        m->type = NORA_MAT_DIELECTRIC;
        m->color = (d->has_Tf && v3_maxc(d->Tf) > 0.0f) ? d->Tf : v3_make(1, 1, 1);
        m->ior = d->Ni > 1.01f ? d->Ni : 1.5f;
        m->roughness = (d->has_Pr || d->has_rough) ? rough : 0.0f;
        m->abbe = d->abbe;
    } else if (metal) {
        m->type = NORA_MAT_CONDUCTOR;
        m->color = (d->has_Ks && v3_maxc(d->Ks) > 0.0f) ? d->Ks : d->Kd;
        m->edge_tint = d->has_edge ? d->edge : m->color;
        m->roughness = rough;
    } else if (m->type == NORA_MAT_PLASTIC || (d->has_Ks && v3_maxc(d->Ks) > 0.01f && d->illum >= 2)) {
        m->type = NORA_MAT_PLASTIC;
        m->color = d->Kd;
        m->roughness = rough;
        m->ior = d->Ni > 1.01f ? d->Ni : 1.5f;
    } else {
        m->type = NORA_MAT_DIFFUSE;
        m->color = d->Kd;
    }
    if (d->has_Ke && v3_maxc(d->Ke) > 0.0f) {
        m->emission = d->Ke;
        m->emission_strength = 1.0f;
        m->two_sided = 1;
    }
    if (d->map_Kd[0]) {
        m->color_tex = nora_scene_load_texture(s, d->map_Kd);
        if (m->color_tex >= 0 && m->type == NORA_MAT_DIFFUSE && v3_maxc(d->Kd) <= 0.0f) m->color = v3_make(1, 1, 1);
    }
    if (d->shader[0]) {
        char full[1024], err[512];
        char *src;
        nora_path_join(s->base_dir, d->shader, full, sizeof full);
        src = read_file(full);
        add_dep(s, 0, full);
        if (src) {
            int slot = nora_scene_add_shader(s, d->name, src, err, sizeof err);
            if (slot >= 0) { m->shader = slot; nora_path_abs(full, s->shaders[slot].file, sizeof s->shaders[slot].file); }
            else fprintf(stderr, "nora: %s: %s\n", d->shader, err);
            free(src);
        } else {
            fprintf(stderr, "nora: cannot read shader %s\n", d->shader);
        }
    }
}

nora_scene *nora_scene_load_obj(const char *path, char *err, size_t errcap) {
    obj_t *o = obj_load(path);
    nora_scene *s;
    mtl_def *defs = NULL;
    int ndefs = 0, i, *matmap = NULL, any_emit = 0;
    size_t t, g;
    int *order = NULL, *gcount = NULL, *gstart = NULL, src;
    if (!o) { if (err) snprintf(err, errcap, "cannot load OBJ file '%s'", path); return NULL; }
    s = nora_scene_create();
    if (!s) { obj_free(o); return NULL; }
    {
        const char *b = strrchr(path, '/');
        snprintf(s->name, sizeof s->name, "%s", b ? b + 1 : path);
    }
    dir_of(path, s->base_dir, sizeof s->base_dir);
    src = nora_scene_add_source(s, NORA_SRC_OBJ, path);
    for (t = 0; t < o->mtllib_count; t++) {
        char full[1024];
        const char *used = full;
        snprintf(full, sizeof full, "%s/%s", s->base_dir, o->mtllibs[t]);
        if (parse_mtl(full, &defs, &ndefs) != 0) {
            used = o->mtllibs[t];
            if (parse_mtl(o->mtllibs[t], &defs, &ndefs) != 0) { fprintf(stderr, "nora: cannot read material library %s\n", full); used = full; }
        }
        add_dep(s, src, used);
    }
    /* materials referenced by the OBJ, in order of first use */
    matmap = (int *)malloc(sizeof(int) * (o->material_count + 1));
    {
        nora_material m;
        nora_material_default(&m, "default");
        nora_scene_add_material(s, &m);   /* material 0: faces without usemtl */
    }
    for (t = 0; t < o->material_count; t++) {
        nora_material m;
        int found = -1;
        for (i = 0; i < ndefs; i++) if (!strcmp(defs[i].name, o->material_names[t])) { found = i; break; }
        if (found >= 0) map_material(s, &defs[found], &m);
        else nora_material_default(&m, o->material_names[t]);
        if (m.emission_strength > 0.0f) any_emit = 1;
        matmap[t] = nora_scene_add_material(s, &m);
    }
    for (t = 0; t < o->vertex_count; t++) nora_scene_add_pos(s, v3_make(o->vertices[t].x, o->vertices[t].y, o->vertices[t].z));
    for (t = 0; t < o->normal_count; t++) nora_scene_add_nrm(s, v3_norm(v3_make(o->normals[t].x, o->normals[t].y, o->normals[t].z)));
    for (t = 0; t < o->texcoord_count; t++) nora_scene_add_uv(s, o->texcoords[t].u, o->texcoords[t].v);
    /* triangles grouped by o/g so each mesh is a contiguous range */
    {
        size_t ntri = o->face_count / 3;
        gcount = (int *)calloc(o->group_count + 1, sizeof(int));
        gstart = (int *)calloc(o->group_count + 1, sizeof(int));
        order = (int *)malloc(sizeof(int) * (ntri + 1));
        for (t = 0; t < ntri; t++) gcount[o->face_groups[t]]++;
        for (g = 1; g <= o->group_count; g++) gstart[g] = gstart[g - 1] + gcount[g - 1];
        {
            int *fill = (int *)calloc(o->group_count + 1, sizeof(int));
            for (t = 0; t < ntri; t++) {
                int gg = o->face_groups[t];
                order[gstart[gg] + fill[gg]++] = (int)t;
            }
            free(fill);
        }
        for (g = 0; g < o->group_count; g++) {
            int k, mesh;
            if (gcount[g] == 0) continue;
            mesh = nora_scene_add_mesh(s, o->group_names[g], s->ntris, gcount[g]);
            for (k = 0; k < gcount[g]; k++) {
                int ti = order[gstart[g] + k], c;
                nora_tri tr;
                memset(&tr, 0, sizeof tr);
                for (c = 0; c < 3; c++) {
                    const face_t *fc = &o->faces[ti * 3 + c];
                    tr.v[c] = fc->x - 1;
                    tr.t[c] = fc->y > 0 && (size_t)fc->y <= o->texcoord_count ? fc->y - 1 : -1;
                    tr.n[c] = fc->z > 0 && (size_t)fc->z <= o->normal_count ? fc->z - 1 : -1;
                }
                if (tr.t[0] < 0 || tr.t[1] < 0 || tr.t[2] < 0) tr.t[0] = tr.t[1] = tr.t[2] = -1;
                if (tr.n[0] < 0 || tr.n[1] < 0 || tr.n[2] < 0) tr.n[0] = tr.n[1] = tr.n[2] = -1;
                tr.mat = o->face_materials[ti] >= 0 ? matmap[o->face_materials[ti]] : 0;
                tr.mesh = mesh;
                nora_scene_add_tri(s, &tr);
            }
        }
    }
    free(order); free(gcount); free(gstart); free(matmap); free(defs);
    obj_free(o);
    for (i = 0; i < s->nmeshes; i++) { s->meshes[i].source = src; s->meshes[i].source_mesh = i; }
    nora_scene_own_vertices(s);
    nora_scene_frame_camera(s);
    if (!any_emit) s->env_color = v3_make(1.0f, 1.0f, 1.0f);
    return s;
}
