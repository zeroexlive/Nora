/* Nora - .nora scene files, OBJ export, scene dependencies. MIT License. */
#include "scenefile.h"
#include "fsutil.h"
#include "json.h"
#include "graph.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_types[NORA_MAT_TYPE_COUNT] = { "diffuse", "conductor", "dielectric", "plastic" };

/* ------------------------------------------------------------- writing */

static float clean(float x) { return x == 0.0f ? 0.0f : x; }
static void wf(js_buf *b, float x) { jb_printf(b, "%.9g", (double)clean(x)); }
static void wv3(js_buf *b, v3 v) {
    jb_puts(b, "["); wf(b, v.x); jb_puts(b, ", "); wf(b, v.y); jb_puts(b, ", "); wf(b, v.z); jb_puts(b, "]");
}
static void wpath(js_buf *b, const char *abs, const char *dir) {
    char rel[2048];
    nora_path_rel(abs, dir, rel, sizeof rel);
    jb_str(b, rel);
}

typedef struct src_key { int kind; const char *path; } src_key;

char *nora_scenefile_write_string(nora_scene *s, const nora_settings *st, const char *dir) {
    js_buf b;
    int i, k, nout = 0, first;
    int *srcmap = (int *)malloc(sizeof(int) * (size_t)(s->nsources + 1));
    int *order = (int *)malloc(sizeof(int) * (size_t)(s->nsources + 1));
    char absdir[1024];
    memset(&b, 0, sizeof b);
    if (!srcmap || !order) { free(srcmap); free(order); return NULL; }
    nora_path_abs(dir && dir[0] ? dir : ".", absdir, sizeof absdir);
    /* sources used by live meshes, deduplicated, in order of first use */
    for (i = 0; i < s->nsources; i++) srcmap[i] = -1;
    for (i = 0; i < s->nmeshes; i++) {
        int so = s->meshes[i].source;
        if (s->meshes[i].hidden || so < 0 || so >= s->nsources || srcmap[so] >= 0) continue;
        for (k = 0; k < nout; k++) {
            const nora_source *a = &s->sources[order[k]], *c = &s->sources[so];
            if (a->kind == c->kind && (a->kind != NORA_SRC_OBJ || !strcmp(a->path, c->path))) break;
        }
        if (k == nout) order[nout++] = so;
        srcmap[so] = k;
    }
    jb_puts(&b, "{\n");
    jb_printf(&b, "  \"format\": \"nora-scene\",\n  \"version\": %d,\n  \"name\": ", NORA_SCENEFILE_VERSION);
    jb_str(&b, s->name);
    jb_puts(&b, ",\n  \"sources\": [");
    for (k = 0; k < nout; k++) {
        const nora_source *so = &s->sources[order[k]];
        jb_puts(&b, k ? ",\n    " : "\n    ");
        if (so->kind == NORA_SRC_OBJ) { jb_puts(&b, "{ \"type\": \"obj\", \"path\": "); wpath(&b, so->path, absdir); jb_puts(&b, " }"); }
        else jb_printf(&b, "{ \"type\": \"builtin\", \"name\": \"%s\" }", so->kind == NORA_SRC_CORNELL ? "cornell" : "cornell-glass");
    }
    jb_puts(&b, nout ? "\n  ],\n  \"objects\": [" : "],\n  \"objects\": [");
    first = 1;
    for (i = 0; i < s->nmeshes; i++) {
        const nora_mesh *m = &s->meshes[i];
        if (m->hidden || m->source < 0 || m->source >= s->nsources) continue;
        jb_puts(&b, first ? "\n    " : ",\n    ");
        first = 0;
        jb_puts(&b, "{ \"name\": ");
        jb_str(&b, m->name);
        jb_printf(&b, ", \"source\": %d, \"mesh\": %d,\n      \"translation\": ", srcmap[m->source], m->source_mesh);
        wv3(&b, m->translate);
        jb_puts(&b, ", \"rotation\": ");
        wv3(&b, m->euler_deg);
        jb_puts(&b, ", \"scale\": ");
        wv3(&b, m->scale);
        jb_puts(&b, " }");
    }
    jb_puts(&b, first ? "],\n  \"materials\": [" : "\n  ],\n  \"materials\": [");
    for (i = 0; i < s->nmats; i++) {
        const nora_material *m = &s->mats[i];
        jb_puts(&b, i ? ",\n    " : "\n    ");
        jb_puts(&b, "{ \"name\": ");
        jb_str(&b, m->name);
        jb_printf(&b, ", \"type\": \"%s\",\n      \"color\": ", k_types[m->type]);
        wv3(&b, m->color);
        jb_puts(&b, ", \"edge_tint\": "); wv3(&b, m->edge_tint);
        jb_puts(&b, ", \"roughness\": "); wf(&b, m->roughness);
        jb_puts(&b, ", \"ior\": "); wf(&b, m->ior);
        jb_puts(&b, ", \"abbe\": "); wf(&b, m->abbe);
        jb_puts(&b, ",\n      \"emission\": "); wv3(&b, m->emission);
        jb_puts(&b, ", \"emission_strength\": "); wf(&b, m->emission_strength);
        jb_printf(&b, ", \"two_sided\": %s", m->two_sided ? "true" : "false");
        if (m->color_tex >= 0 && m->color_tex < s->ntexs) {
            char full[1024];
            nora_path_join(s->base_dir, s->texs[m->color_tex].path, full, sizeof full);
            jb_puts(&b, ",\n      \"color_texture\": ");
            wpath(&b, full, absdir);
        }
        if (m->shader >= 0 && m->shader < s->nshaders) {
            const nora_shader_slot *sl = &s->shaders[m->shader];
            jb_puts(&b, ",\n      \"shader\": { \"name\": ");
            jb_str(&b, sl->name);
            if (sl->graph_json) {
                char err[128];
                js_value *v = js_parse(sl->graph_json, err, sizeof err);
                jb_puts(&b, ", \"graph\": ");
                js_dump(&b, v);
                js_free(v);
            } else if (sl->graph_file[0]) {
                jb_puts(&b, ", \"graph_file\": "); wpath(&b, sl->graph_file, absdir);
            } else if (sl->file[0]) {
                jb_puts(&b, ", \"osl_file\": "); wpath(&b, sl->file, absdir);
            } else {
                jb_puts(&b, ", \"osl\": "); jb_str(&b, sl->source ? sl->source : "");
            }
            jb_puts(&b, " }");
        }
        jb_puts(&b, " }");
    }
    jb_puts(&b, s->nmats ? "\n  ],\n" : "],\n");
    jb_puts(&b, "  \"camera\": { \"position\": "); wv3(&b, s->cam.pos);
    jb_puts(&b, ", \"target\": "); wv3(&b, s->cam.target);
    jb_puts(&b, ", \"up\": "); wv3(&b, s->cam.up);
    jb_puts(&b, ",\n    \"fov\": "); wf(&b, s->cam.vfov_deg);
    jb_puts(&b, ", \"aperture\": "); wf(&b, s->cam.aperture);
    jb_puts(&b, ", \"focus_distance\": "); wf(&b, s->cam.focus_dist);
    jb_puts(&b, " },\n  \"environment\": "); wv3(&b, s->env_color);
    if (st) {
        jb_printf(&b, ",\n  \"render\": { \"width\": %d, \"height\": %d, \"mode\": \"%s\", \"integrator\": \"%s\", \"spp\": %d,\n",
                  st->width, st->height, nora_mode_name(st->mode), nora_integrator_name(st->integrator), st->spp);
        jb_printf(&b, "    \"max_depth\": %d, \"large_step_probability\": ", st->max_depth);
        wf(&b, st->p_large);
        jb_printf(&b, ", \"bootstrap\": %d, \"chains\": %d, \"seed\": %llu, \"time_limit\": ", st->bootstrap, st->chains,
                  (unsigned long long)st->seed);
        wf(&b, st->time_limit);
        jb_puts(&b, " }");
    }
    jb_puts(&b, "\n}\n");
    free(srcmap);
    free(order);
    return b.p;
}

int nora_scenefile_save(nora_scene *s, const nora_settings *st, const char *path, char *err, size_t errcap) {
    char dir[1024], *text;
    int rc;
    nora_path_dir(path, dir, sizeof dir);
    text = nora_scenefile_write_string(s, st, dir);
    if (!text) { if (err) snprintf(err, errcap, "out of memory"); return -1; }
    rc = nora_write_file_atomic(path, text, strlen(text));
    free(text);
    if (rc != 0) { if (err) snprintf(err, errcap, "cannot write %s", path); return -1; }
    nora_path_abs(path, s->file_path, sizeof s->file_path);
    return 0;
}

/* ------------------------------------------------------------- reading */

static v3 rv3(const js_value *v, v3 def) {
    if (!v || v->kind != JS_ARR || js_len(v) < 3) return def;
    return v3_make((float)js_num(js_at(v, 0), def.x), (float)js_num(js_at(v, 1), def.y), (float)js_num(js_at(v, 2), def.z));
}
static float rf(const js_value *o, const char *k, float def) { return (float)js_num(js_get(o, k), def); }

static int load_shader(nora_scene *s, const js_value *sh, const char *dir, char *err, size_t errcap) {
    const char *name = js_str(js_get(sh, "name"), "shader");
    const js_value *gv = js_get(sh, "graph");
    const char *gf = js_str(js_get(sh, "graph_file"), NULL), *of = js_str(js_get(sh, "osl_file"), NULL);
    const char *inl = js_str(js_get(sh, "osl"), NULL);
    char path[1024], abs[1024], e2[512];
    char *src = NULL, *gjson = NULL;
    int slot;
    abs[0] = 0;
    if (gv || gf) {
        ng_graph g;
        static char osl[65536];
        ng_init(&g);
        if (gv) {
            js_buf jb;
            memset(&jb, 0, sizeof jb);
            js_dump(&jb, gv);
            gjson = jb.p;
            if (ng_from_json(&g, gjson ? gjson : "{}", e2, sizeof e2) != 0) {
                snprintf(err, errcap, "shader '%s': graph: %s", name, e2); ng_free(&g); free(gjson); return -1;
            }
        } else {
            nora_path_join(dir, gf, path, sizeof path);
            nora_path_abs(path, abs, sizeof abs);
            if (ng_load_file(&g, abs, e2, sizeof e2) != 0) { snprintf(err, errcap, "shader '%s': %s", name, e2); ng_free(&g); return -1; }
        }
        if (ng_generate_osl(&g, osl, sizeof osl, e2, sizeof e2) != 0) {
            snprintf(err, errcap, "shader '%s': %s", name, e2); ng_free(&g); free(gjson); return -1;
        }
        ng_free(&g);
        slot = nora_scene_add_shader(s, name, osl, e2, sizeof e2);
        if (slot < 0) { snprintf(err, errcap, "shader '%s': %s", name, e2); free(gjson); return -1; }
        if (gjson) s->shaders[slot].graph_json = gjson;
        else snprintf(s->shaders[slot].graph_file, sizeof s->shaders[slot].graph_file, "%s", abs);
        return slot;
    }
    if (of) {
        nora_path_join(dir, of, path, sizeof path);
        nora_path_abs(path, abs, sizeof abs);
        src = nora_read_file(abs, NULL);
        if (!src) { snprintf(err, errcap, "shader '%s': cannot read %s", name, abs); return -1; }
    } else if (inl) {
        src = (char *)malloc(strlen(inl) + 1);
        if (src) strcpy(src, inl);
    }
    if (!src) { snprintf(err, errcap, "shader '%s': no osl, osl_file, graph or graph_file", name); return -1; }
    slot = nora_scene_add_shader(s, name, src, e2, sizeof e2);
    free(src);
    if (slot < 0) { snprintf(err, errcap, "shader '%s': %s", name, e2); return -1; }
    if (abs[0]) snprintf(s->shaders[slot].file, sizeof s->shaders[slot].file, "%s", abs);
    return slot;
}

static int parse_material(nora_scene *s, const js_value *mv, const char *dir, nora_material *m, char *err, size_t errcap) {
    const char *type = js_str(js_get(mv, "type"), "diffuse");
    const char *tex = js_str(js_get(mv, "color_texture"), NULL);
    const js_value *sh = js_get(mv, "shader");
    int t;
    nora_material_default(m, js_str(js_get(mv, "name"), "material"));
    for (t = 0; t < NORA_MAT_TYPE_COUNT; t++) if (!strcmp(type, k_types[t])) m->type = (nora_mat_type)t;
    m->color = rv3(js_get(mv, "color"), m->color);
    m->edge_tint = rv3(js_get(mv, "edge_tint"), m->edge_tint);
    m->roughness = rf(mv, "roughness", m->roughness);
    m->ior = rf(mv, "ior", m->ior);
    m->abbe = rf(mv, "abbe", m->abbe);
    m->emission = rv3(js_get(mv, "emission"), m->emission);
    m->emission_strength = rf(mv, "emission_strength", m->emission_strength);
    m->two_sided = js_num(js_get(mv, "two_sided"), 0) != 0.0;
    if (tex) {
        char path[1024], abs[1024];
        nora_path_join(dir, tex, path, sizeof path);
        nora_path_abs(path, abs, sizeof abs);
        m->color_tex = nora_scene_load_texture(s, abs);
        if (m->color_tex < 0) nora_logf("warning: cannot load texture %s", abs);
    }
    if (sh && sh->kind == JS_OBJ) {
        m->shader = load_shader(s, sh, dir, err, errcap);
        if (m->shader < 0) return -1;
    }
    return 0;
}

nora_scene *nora_scenefile_read_string(const char *text, const char *dir, nora_settings *st, int *has_st, char *err, size_t errcap) {
    char e2[512], absdir[1024];
    js_value *root = js_parse(text, e2, sizeof e2);
    const js_value *srcs, *objs, *mats, *cam, *rnd;
    nora_scene *s = NULL, **loaded = NULL;
    int nsrc, i;
    char dummy[8];
    if (!err) { err = dummy; errcap = sizeof dummy; }
    err[0] = 0;
    if (has_st) *has_st = 0;
    if (!root || root->kind != JS_OBJ) { snprintf(err, errcap, "not a scene file: %s", root ? "not an object" : e2); js_free(root); return NULL; }
    if (strcmp(js_str(js_get(root, "format"), ""), "nora-scene") != 0) { snprintf(err, errcap, "not a Nora scene (format != nora-scene)"); js_free(root); return NULL; }
    if ((int)js_num(js_get(root, "version"), 0) > NORA_SCENEFILE_VERSION) {
        snprintf(err, errcap, "scene file version %d is newer than this Nora (%d)", (int)js_num(js_get(root, "version"), 0), NORA_SCENEFILE_VERSION);
        js_free(root);
        return NULL;
    }
    nora_path_abs(dir && dir[0] ? dir : ".", absdir, sizeof absdir);
    srcs = js_get(root, "sources");
    objs = js_get(root, "objects");
    mats = js_get(root, "materials");
    nsrc = js_len(srcs);
    s = nora_scene_create();
    loaded = (nora_scene **)calloc((size_t)nsrc + 1, sizeof *loaded);
    if (!s || !loaded) goto fail;
    snprintf(s->name, sizeof s->name, "%s", js_str(js_get(root, "name"), "scene"));
    nora_strlcpy(s->base_dir, absdir, sizeof s->base_dir);
    /* sources */
    for (i = 0; i < nsrc; i++) {
        const js_value *sv = js_at(srcs, i);
        const char *type = js_str(js_get(sv, "type"), "obj");
        if (!strcmp(type, "builtin")) {
            const char *name = js_str(js_get(sv, "name"), "cornell");
            int glass = !strcmp(name, "cornell-glass");
            if (!glass && strcmp(name, "cornell") != 0) { snprintf(err, errcap, "unknown built-in scene '%s'", name); goto fail; }
            loaded[i] = nora_scene_cornell(glass);
        } else {
            char path[1024], abs[1024];
            nora_path_join(absdir, js_str(js_get(sv, "path"), ""), path, sizeof path);
            nora_path_abs(path, abs, sizeof abs);
            loaded[i] = nora_scene_load_obj(abs, e2, sizeof e2);
            if (!loaded[i]) { snprintf(err, errcap, "%s", e2); goto fail; }
        }
        if (!loaded[i] || loaded[i]->nsources < 1) { snprintf(err, errcap, "cannot load source %d", i); goto fail; }
        if (nora_scene_add_source(s, loaded[i]->sources[0].kind, NULL) < 0) goto fail;
        s->sources[i] = loaded[i]->sources[0];
    }
    if (!s->owned) nora_scene_own_vertices(s);
    /* materials first: they take precedence over the sources' materials of the same name */
    for (i = 0; i < js_len(mats); i++) {
        nora_material m;
        if (parse_material(s, js_at(mats, i), absdir, &m, err, errcap) != 0) goto fail;
        if (nora_scene_find_material(s, m.name) >= 0) continue;
        nora_scene_add_material(s, &m);
    }
    /* objects */
    for (i = 0; i < js_len(objs); i++) {
        const js_value *ov = js_at(objs, i);
        int so = (int)js_num(js_get(ov, "source"), 0), mi = (int)js_num(js_get(ov, "mesh"), -1), nm;
        const char *name = js_str(js_get(ov, "name"), NULL);
        nora_mesh *me;
        if (so < 0 || so >= nsrc) { snprintf(err, errcap, "object %d: bad source %d", i, so); goto fail; }
        if (mi < 0 || mi >= loaded[so]->nmeshes) {
            int k;
            mi = -1;
            for (k = 0; name && k < loaded[so]->nmeshes; k++) if (!strcmp(loaded[so]->meshes[k].name, name)) { mi = k; break; }
            if (mi < 0) { nora_logf("warning: object '%s' not found in its source, skipped", name ? name : "?"); continue; }
        }
        nm = nora_scene_append(s, loaded[so], &mi, 1);
        if (nm < 0) { snprintf(err, errcap, "object %d: cannot add", i); goto fail; }
        me = &s->meshes[nm];
        if (name) snprintf(me->name, sizeof me->name, "%s", name);
        me->source = so;
        me->source_mesh = mi;
        me->translate = rv3(js_get(ov, "translation"), v3_make(0, 0, 0));
        me->scale = rv3(js_get(ov, "scale"), v3_make(1, 1, 1));
        nora_mesh_set_euler(me, rv3(js_get(ov, "rotation"), v3_make(0, 0, 0)));
    }
    /* camera, environment */
    cam = js_get(root, "camera");
    if (cam) {
        s->cam.pos = rv3(js_get(cam, "position"), s->cam.pos);
        s->cam.target = rv3(js_get(cam, "target"), s->cam.target);
        s->cam.up = rv3(js_get(cam, "up"), s->cam.up);
        s->cam.vfov_deg = rf(cam, "fov", s->cam.vfov_deg);
        s->cam.aperture = rf(cam, "aperture", 0);
        s->cam.focus_dist = rf(cam, "focus_distance", 0);
    } else {
        nora_scene_apply_transforms(s);
        nora_scene_frame_camera(s);
    }
    s->env_color = rv3(js_get(root, "environment"), v3_make(0, 0, 0));
    rnd = js_get(root, "render");
    if (st) {
        nora_settings_default(st);
        if (rnd && rnd->kind == JS_OBJ) {
            const char *mode = js_str(js_get(rnd, "mode"), "rgb"), *in = js_str(js_get(rnd, "integrator"), "mlt");
            if (has_st) *has_st = 1;
            st->width = (int)js_num(js_get(rnd, "width"), st->width);
            st->height = (int)js_num(js_get(rnd, "height"), st->height);
            st->mode = !strcmp(mode, "spectral") ? NORA_MODE_SPECTRAL : NORA_MODE_RGB;
            st->integrator = !strcmp(in, "pt") ? NORA_INT_PT : NORA_INT_MLT;
            st->spp = (int)js_num(js_get(rnd, "spp"), st->spp);
            st->max_depth = (int)js_num(js_get(rnd, "max_depth"), st->max_depth);
            st->p_large = rf(rnd, "large_step_probability", st->p_large);
            st->bootstrap = (int)js_num(js_get(rnd, "bootstrap"), st->bootstrap);
            st->chains = (int)js_num(js_get(rnd, "chains"), st->chains);
            st->seed = (uint64_t)js_num(js_get(rnd, "seed"), (double)st->seed);
            st->time_limit = rf(rnd, "time_limit", st->time_limit);
        }
    }
    nora_scene_apply_transforms(s);
    for (i = 0; i < nsrc; i++) nora_scene_destroy(loaded[i]);
    free(loaded);
    js_free(root);
    return s;
fail:
    if (loaded) for (i = 0; i < nsrc; i++) nora_scene_destroy(loaded[i]);
    free(loaded);
    nora_scene_destroy(s);
    js_free(root);
    if (!err[0]) snprintf(err, errcap, "cannot load the scene");
    return NULL;
}

nora_scene *nora_scenefile_load(const char *path, nora_settings *st, int *has_st, char *err, size_t errcap) {
    char dir[1024], *text = nora_read_file(path, NULL);
    nora_scene *s;
    if (!text) { if (err) snprintf(err, errcap, "cannot read %s", path); return NULL; }
    nora_path_dir(path, dir, sizeof dir);
    s = nora_scenefile_read_string(text, dir, st, has_st, err, errcap);
    free(text);
    if (s) nora_path_abs(path, s->file_path, sizeof s->file_path);
    return s;
}

int nora_is_scene_file(const char *path) {
    const char *e = nora_path_ext(path);
    return e[0] && (!strcmp(e, ".nora") || !strcmp(e, ".NORA"));
}

nora_scene *nora_scene_open(const char *arg, nora_settings *st, int *has_st, char *err, size_t errcap) {
    if (has_st) *has_st = 0;
    if (st) nora_settings_default(st);
    if (!arg || !strcmp(arg, "cornell")) return nora_scene_cornell(0);
    if (!strcmp(arg, "cornell-glass")) return nora_scene_cornell(1);
    if (nora_is_scene_file(arg)) return nora_scenefile_load(arg, st, has_st, err, errcap);
    return nora_scene_load_obj(arg, err, errcap);
}

/* ---------------------------------------------------------- dependencies */

static int add_unique(char (*out)[1024], int n, int cap, const char *p) {
    int i;
    if (!p || !p[0] || n >= cap) return n;
    for (i = 0; i < n; i++) if (!strcmp(out[i], p)) return n;
    snprintf(out[n], 1024, "%s", p);
    return n + 1;
}

int nora_scene_dependencies(const nora_scene *s, char (*out)[1024], int cap) {
    int n = 0, i, k;
    char *used = (char *)calloc((size_t)s->nsources + 1, 1);
    n = add_unique(out, n, cap, s->file_path);
    for (i = 0; i < s->nmeshes; i++)
        if (!s->meshes[i].hidden && s->meshes[i].source >= 0 && s->meshes[i].source < s->nsources && used) used[s->meshes[i].source] = 1;
    for (i = 0; i < s->nsources; i++) {
        if (!used || !used[i] || s->sources[i].kind != NORA_SRC_OBJ) continue;
        n = add_unique(out, n, cap, s->sources[i].path);
        for (k = 0; k < s->sources[i].ndeps; k++) n = add_unique(out, n, cap, s->sources[i].deps[k]);
    }
    for (i = 0; i < s->nshaders; i++) {
        n = add_unique(out, n, cap, s->shaders[i].file);
        n = add_unique(out, n, cap, s->shaders[i].graph_file);
    }
    free(used);
    return n;
}

/* ---------------------------------------------------------------- export */

static void mtl_name(char *out, size_t cap, const char *name) {
    size_t i;
    snprintf(out, cap, "%s", name[0] ? name : "material");
    for (i = 0; out[i]; i++) if (out[i] == ' ' || out[i] == '\t') out[i] = '_';
}

int nora_scene_export_obj(nora_scene *s, const char *path, char *err, size_t errcap) {
    char mtlpath[1100], dir[1024], base[512];
    FILE *f, *fm;
    int i, k, t, voff = 1, noff = 1;
    const char *ext;
    if (!s->owned && nora_scene_own_vertices(s) != 0) { if (err) snprintf(err, errcap, "out of memory"); return -1; }
    nora_scene_apply_transforms(s);
    snprintf(mtlpath, sizeof mtlpath, "%s", path);
    ext = nora_path_ext(mtlpath);
    if (ext[0]) mtlpath[ext - mtlpath] = 0;
    snprintf(base, sizeof base, "%s", nora_path_base(mtlpath));
    strncat(mtlpath, ".mtl", sizeof mtlpath - strlen(mtlpath) - 1);
    nora_path_dir(path, dir, sizeof dir);
    f = fopen(path, "w");
    if (!f) { if (err) snprintf(err, errcap, "cannot write %s", path); return -1; }
    fm = fopen(mtlpath, "w");
    if (!fm) { fclose(f); if (err) snprintf(err, errcap, "cannot write %s", mtlpath); return -1; }
    fprintf(f, "# Nora %s export, transforms baked in\nmtllib %s.mtl\n", s->name, base);
    for (i = 0; i < s->nuv; i++) fprintf(f, "vt %.9g %.9g\n", (double)s->uv[i * 2], (double)s->uv[i * 2 + 1]);
    for (i = 0; i < s->nmeshes; i++) {
        const nora_mesh *m = &s->meshes[i];
        int cur = -2;
        int dupe = 0, j;
        if (m->hidden || m->tri_count == 0) continue;
        /* OBJ readers merge groups of the same name: keep object names unique */
        for (j = 0; j < i; j++) if (!s->meshes[j].hidden && s->meshes[j].tri_count > 0 && !strcmp(s->meshes[j].name, m->name)) dupe++;
        if (dupe) fprintf(f, "o %s.%d\n", m->name, dupe);
        else fprintf(f, "o %s\n", m->name);
        for (k = 0; k < m->npos; k++) { v3 p = s->pos[m->first_pos + k]; fprintf(f, "v %.9g %.9g %.9g\n", (double)p.x, (double)p.y, (double)p.z); }
        for (k = 0; k < m->nnrm; k++) { v3 p = s->nrm[m->first_nrm + k]; fprintf(f, "vn %.9g %.9g %.9g\n", (double)p.x, (double)p.y, (double)p.z); }
        for (t = m->first_tri; t < m->first_tri + m->tri_count; t++) {
            const nora_tri *tr = &s->tris[t];
            if (tr->mat != cur) {
                char mn[80];
                cur = tr->mat;
                mtl_name(mn, sizeof mn, cur >= 0 && cur < s->nmats ? s->mats[cur].name : "default");
                fprintf(f, "usemtl %s\n", mn);
            }
            fprintf(f, "f");
            for (k = 0; k < 3; k++) {
                int v = tr->v[k] - m->first_pos + voff;
                if (tr->t[k] >= 0 && tr->n[k] >= 0) fprintf(f, " %d/%d/%d", v, tr->t[k] + 1, tr->n[k] - m->first_nrm + noff);
                else if (tr->t[k] >= 0) fprintf(f, " %d/%d", v, tr->t[k] + 1);
                else if (tr->n[k] >= 0) fprintf(f, " %d//%d", v, tr->n[k] - m->first_nrm + noff);
                else fprintf(f, " %d", v);
            }
            fprintf(f, "\n");
        }
        voff += m->npos;
        noff += m->nnrm;
    }
    fprintf(fm, "# Nora %s export\n", s->name);
    for (i = 0; i < s->nmats; i++) {
        const nora_material *m = &s->mats[i];
        char mn[80];
        mtl_name(mn, sizeof mn, m->name);
        fprintf(fm, "\nnewmtl %s\nnora_type %s\n", mn, k_types[m->type]);
        fprintf(fm, "Kd %.6g %.6g %.6g\n", (double)m->color.x, (double)m->color.y, (double)m->color.z);
        if (m->type == NORA_MAT_CONDUCTOR) {
            fprintf(fm, "Ks %.6g %.6g %.6g\nPm 1\nillum 3\n", (double)m->color.x, (double)m->color.y, (double)m->color.z);
            fprintf(fm, "nora_edge %.6g %.6g %.6g\n", (double)m->edge_tint.x, (double)m->edge_tint.y, (double)m->edge_tint.z);
        } else if (m->type == NORA_MAT_DIELECTRIC) {
            fprintf(fm, "Tf %.6g %.6g %.6g\nNi %.6g\nd 0\nillum 7\n", (double)m->color.x, (double)m->color.y, (double)m->color.z, (double)m->ior);
            if (m->abbe > 0) fprintf(fm, "nora_abbe %.6g\n", (double)m->abbe);
        } else if (m->type == NORA_MAT_PLASTIC) {
            fprintf(fm, "Ks 0.04 0.04 0.04\nNi %.6g\nillum 2\n", (double)m->ior);
        }
        if (m->type != NORA_MAT_DIFFUSE) fprintf(fm, "nora_roughness %.6g\n", (double)m->roughness);
        if (m->emission_strength > 0 && v3_maxc(m->emission) > 0)
            fprintf(fm, "Ke %.6g %.6g %.6g\n", (double)(m->emission.x * m->emission_strength), (double)(m->emission.y * m->emission_strength),
                    (double)(m->emission.z * m->emission_strength));
        if (m->color_tex >= 0 && m->color_tex < s->ntexs) {
            char full[1024], rel[2048];
            nora_path_join(s->base_dir, s->texs[m->color_tex].path, full, sizeof full);
            nora_path_rel(full, dir, rel, sizeof rel);
            fprintf(fm, "map_Kd %s\n", rel);
        }
        if (m->shader >= 0 && m->shader < s->nshaders) {
            const nora_shader_slot *sl = &s->shaders[m->shader];
            char rel[2048];
            if (sl->file[0]) {
                nora_path_rel(sl->file, dir, rel, sizeof rel);
            } else {
                /* write the (generated or edited) source next to the OBJ */
                char op[1200];
                snprintf(rel, sizeof rel, "%s_%s.osl", base, mn);
                nora_path_join(dir, rel, op, sizeof op);
                if (nora_write_file_atomic(op, sl->source ? sl->source : "", sl->source ? strlen(sl->source) : 0) != 0) rel[0] = 0;
            }
            if (rel[0]) fprintf(fm, "nora_shader %s\n", rel);
        }
    }
    fclose(fm);
    if (fclose(f) != 0) { if (err) snprintf(err, errcap, "cannot write %s", path); return -1; }
    return 0;
}
