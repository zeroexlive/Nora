/* Nora - interactive GUI. MIT License.
 *
 * Docked layout (CUI dockspace): Viewport, Node Editor, Outliner, Properties,
 * Render Settings, OSL Source and Log, under a File / Edit / View / Render menu
 * bar.
 *
 * The viewport has two modes:
 *   Render  the progressive render as a texture drawn through a CUI canvas.
 *           While the camera moves a fast low-resolution path traced preview
 *           runs; once the camera is still Nora switches to progressive MLT.
 *   Edit    a rasterized solid (optionally wireframe) preview made by Nora's own
 *           software rasterizer (src/core/raster.c) into the same texture path,
 *           so it works with CUI's software renderer too. The path tracer never
 *           runs in this mode. Click picks an object (ID buffer), W / E / R pick
 *           the move / rotate / scale gizmo, dragging an axis handle transforms.
 *           The BVH is rebuilt when switching to Render or starting a render.
 *
 * Camera: left drag orbits (in Edit mode: on empty space), Shift+left or middle
 * drag pans, right drag (or Ctrl+left) dollies; + and - zoom, F frames the scene.
 * CUI 1.4 does not expose the mouse wheel to custom canvases, so zoom uses drags,
 * keys and a slider.
 */
#include "gui.h"
#include "render.h"
#include "image.h"
#include "scene.h"
#include "scenefile.h"
#include "edit.h"
#include "watch.h"
#include "raster.h"
#include "fsutil.h"
#include "graph.h"
#include "osl.h"
#include "nthread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

#define LOG_LINES 256
#define SRC_CAP (64 * 1024)
#define MAX_DEPS 64
#define RING_PTS 48

enum { VIEW_EDIT = 0, VIEW_RENDER = 1 };
enum { GIZMO_MOVE = 0, GIZMO_ROTATE = 1, GIZMO_SCALE = 2 };

typedef struct gui_log {
    nmutex mu;
    char lines[LOG_LINES][400];
    int head, count;
} gui_log;

typedef struct mat_graph { ng_graph g; int used; } mat_graph;

/* gizmo geometry in canvas pixels, recomputed every frame */
typedef struct gizmo_geo {
    int valid;
    cui_vec2 center;
    cui_vec2 tip[3];               /* move / scale handles */
    cui_vec2 ring[3][RING_PTS];    /* rotate rings */
    v3 axis[3];                    /* world axes the handles act along / around */
    float world_len;               /* world length of a handle */
} gizmo_geo;

struct nora_gui {
    nora_gui_host host;
    nora_scene *scene;
    char scene_arg[1024];
    char scene_file[1024];      /* .nora file of the scene, "" if it has none yet */

    /* render settings (UI side) */
    int mode_idx, integrator_idx;
    float p_large, exposure;
    double spp, time_limit, threads, max_depth, width, height;
    bool auto_mlt;
    int tonemap_idx;

    /* active render */
    nora_render *render;
    int render_kind;            /* 0 none, 1 preview, 2 progressive / final */
    int render_w, render_h;
    float *rgb;
    unsigned char *rgba;
    cui_texture tex;
    int tex_w, tex_h;
    double last_tex_update;
    int tex_dirty;
    int was_running;

    /* viewport mode and edit-mode raster */
    int view_mode;
    bool wireframe;
    int gizmo_mode;
    unsigned char *ed_rgba;
    float *ed_depth;
    int *ed_ids;
    int ed_w, ed_h, ed_dirty;
    cui_texture ed_tex;
    cui_rect vp_rect;           /* the viewport canvas, last frame */
    gizmo_geo giz;
    int giz_hover;              /* axis under the mouse, -1 none */
    int giz_drag;               /* axis being dragged, -1 none */
    nora_xform giz_start;
    cui_vec2 giz_press;
    int prev_left;
    int click_pending;          /* left press on empty space: pick on release unless it became a drag */
    cui_vec2 press_pos;

    /* camera */
    float yaw, pitch, dist;
    v3 target;
    double last_cam_change;
    int cam_pending;            /* camera moved: start MLT once it is still */
    int dragging;               /* 0 none, 1 orbit, 2 pan, 3 dolly */
    cui_vec2 drag_last;

    /* selection */
    int sel_mat, sel_mesh;

    /* editing */
    nora_history hist;
    double last_field_edit;
    int field_mesh;
    double f_t[3], f_r[3], f_s[3];

    /* hot reload */
    nora_watch watch;
    bool hot_reload;
    char deps[MAX_DEPS][1024];
    int ndeps;

    /* shader graph per material */
    mat_graph *graphs;
    int ngraphs;
    cui_node_view view;
    int add_type;
    char graph_path[512];
    char *src;                  /* OSL source in the code editor */
    int src_from_graph;         /* the editor shows the generated code */
    double src_edit_time;
    int src_needs_check;
    char compile_msg[512];
    int compile_ok;

    char save_path[512];        /* rendered image */
    char scene_path[512];       /* path fields used when there is no file dialog */
    char import_path[512];
    char export_path[512];
    char ckpt_path[512];
    char err[512];
    gui_log log;

    /* dock state */
    bool open[7];
    int frame;
};

static nora_gui *g_log_target = NULL;

static void gui_log_fn(void *user, const char *msg) {
    nora_gui *g = (nora_gui *)user;
    int i;
    if (!g) return;
    nmutex_lock(&g->log.mu);
    i = (g->log.head + g->log.count) % LOG_LINES;
    if (g->log.count == LOG_LINES) g->log.head = (g->log.head + 1) % LOG_LINES;
    else g->log.count++;
    snprintf(g->log.lines[i], sizeof g->log.lines[i], "%s", msg);
    nmutex_unlock(&g->log.mu);
}

static void logf_(nora_gui *g, const char *fmt, ...) {
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    gui_log_fn(g, buf);
}

/* --------------------------------------------------------------- camera */

static void cam_from_scene(nora_gui *g) {
    v3 d = v3_sub(g->scene->cam.pos, g->scene->cam.target);
    g->dist = v3_len(d);
    if (g->dist <= 0) g->dist = 1;
    g->target = g->scene->cam.target;
    g->yaw = atan2f(d.x, d.z);
    g->pitch = asinf(nclampf(d.y / g->dist, -1, 1));
}

static void cam_to_scene(nora_gui *g) {
    v3 d = v3_make(cosf(g->pitch) * sinf(g->yaw), sinf(g->pitch), cosf(g->pitch) * cosf(g->yaw));
    g->scene->cam.target = g->target;
    g->scene->cam.pos = v3_add(g->target, v3_scale(d, g->dist));
    g->scene->cam.up = v3_make(0, 1, 0);
}

/* ------------------------------------------------------------ rendering */

static void stop_render(nora_gui *g) {
    if (!g->render) return;
    nora_render_request_stop(g->render);
    nora_render_wait(g->render);
    nora_render_destroy(g->render);
    g->render = NULL;
    g->render_kind = 0;
}

static void base_settings(nora_gui *g, nora_settings *st) {
    nora_settings_default(st);
    st->mode = g->mode_idx ? NORA_MODE_SPECTRAL : NORA_MODE_RGB;
    st->integrator = g->integrator_idx ? NORA_INT_PT : NORA_INT_MLT;
    st->p_large = g->p_large;
    st->spp = (int)g->spp;
    st->time_limit = (float)g->time_limit;
    st->threads = (int)g->threads;
    st->max_depth = (int)g->max_depth;
    st->width = (int)g->width;
    st->height = (int)g->height;
}

static void settings_to_ui(nora_gui *g, const nora_settings *st) {
    g->mode_idx = st->mode == NORA_MODE_SPECTRAL;
    g->integrator_idx = st->integrator == NORA_INT_PT;
    g->p_large = st->p_large;
    g->spp = st->spp;
    g->time_limit = st->time_limit;
    g->max_depth = st->max_depth;
    g->width = st->width;
    g->height = st->height;
}

static int ensure_built(nora_gui *g) {
    if (g->scene->built) return 0;
    if (nora_scene_build(g->scene) != 0) { logf_(g, "error: scene build failed"); return -1; }
    return 0;
}

static void alloc_frame(nora_gui *g, int w, int h) {
    g->render_w = w;
    g->render_h = h;
    free(g->rgb); free(g->rgba);
    g->rgb = (float *)calloc((size_t)w * h * 3, sizeof(float));
    g->rgba = (unsigned char *)calloc((size_t)w * h * 4, 1);
    g->tex_dirty = 1;
}

static void start_render(nora_gui *g, int kind) {
    nora_settings st;
    stop_render(g);
    if (g->view_mode == VIEW_EDIT) return;   /* the path tracer never runs in Edit mode */
    if (ensure_built(g) != 0) return;        /* rebuilds the BVH after edits */
    base_settings(g, &st);
    if (kind == 1) {   /* fast preview: path tracing at a third of the resolution, progressive */
        st.integrator = NORA_INT_PT;
        st.width = st.width / 3 > 16 ? st.width / 3 : 16;
        st.height = st.height / 3 > 16 ? st.height / 3 : 16;
        st.spp = 4096;
        st.time_limit = 0;
        st.max_depth = st.max_depth < 6 ? st.max_depth : 6;
    } else {
        st.bootstrap = st.width * st.height / 4 > 20000 ? st.width * st.height / 4 : 20000;
        if (st.bootstrap > 200000) st.bootstrap = 200000;
    }
    g->render = nora_render_create(g->scene, &st);
    if (!g->render) { logf_(g, "error: cannot create the renderer"); return; }
    g->render_kind = kind;
    alloc_frame(g, st.width, st.height);
    nora_render_start(g->render);
    if (kind == 2)
        logf_(g, "render: %s, %s, %dx%d, %d %s%s", nora_integrator_name(st.integrator), nora_mode_name(st.mode), st.width, st.height, st.spp,
              st.integrator == NORA_INT_MLT ? "mutations/px" : "spp", st.time_limit > 0 ? " (time limited)" : "");
}

static void restart_preview(nora_gui *g) {
    if (g->view_mode == VIEW_EDIT) { g->ed_dirty = 1; return; }
    start_render(g, 1);
    g->cam_pending = 1;
    g->last_cam_change = ntime_now();
}

/* geometry, materials or transforms changed */
static void scene_changed(nora_gui *g) {
    stop_render(g);
    nora_scene_apply_transforms(g->scene);
    nora_scene_invalidate(g->scene);
    if (g->view_mode == VIEW_RENDER) nora_scene_build(g->scene);
    restart_preview(g);
}

static void camera_changed(nora_gui *g) {
    if (g->view_mode == VIEW_EDIT) { cam_to_scene(g); g->ed_dirty = 1; return; }
    stop_render(g);
    cam_to_scene(g);
    restart_preview(g);
}

static void update_texture(nora_gui *g, cui_ctx *ui, int force) {
    double now = ntime_now();
    if (!g->render) return;
    if (!force && !g->tex_dirty && (now - g->last_tex_update < 0.25 || !nora_render_is_running(g->render))) return;
    nora_render_resolve(g->render, g->rgb);
    nora_image_to_srgb8(g->rgb, g->render_w, g->render_h, g->exposure, (nora_tonemap)g->tonemap_idx, g->rgba);
    if (g->tex) cui_image_destroy(ui, g->tex);
    g->tex = cui_image_create(ui, g->render_w, g->render_h, g->rgba);
    g->tex_w = g->render_w;
    g->tex_h = g->render_h;
    g->last_tex_update = now;
    g->tex_dirty = 0;
}

static int save_image(nora_gui *g, const char *path) {
    if (!g->render || !g->rgb) { logf_(g, "save: nothing rendered yet"); return -1; }
    nora_render_resolve(g->render, g->rgb);
    if (nora_write_image(path, g->rgb, g->render_w, g->render_h, g->exposure, (nora_tonemap)g->tonemap_idx) != 0) {
        logf_(g, "save: cannot write %s", path);
        return -1;
    }
    logf_(g, "saved %s (%dx%d)", path, g->render_w, g->render_h);
    return 0;
}

static int pause_render(nora_gui *g) {
    if (!g->render || !nora_render_is_running(g->render)) return -1;
    nora_render_pause(g->render);
    g->cam_pending = 0;
    g->tex_dirty = 1;
    logf_(g, "render paused");
    return 0;
}

static int resume_render(nora_gui *g) {
    int rc;
    if (!g->render || nora_render_is_running(g->render)) return -1;
    if (g->view_mode == VIEW_EDIT) { logf_(g, "resume: switch to Render mode first"); return -1; }
    rc = nora_render_resume(g->render);
    if (rc == 1) { logf_(g, "resume: the render is complete (raise the samples and resume)"); return -1; }
    if (rc == 0) logf_(g, "render resumed");
    return rc;
}

static int save_checkpoint(nora_gui *g, const char *path) {
    char err[512];
    if (!g->render || g->render_kind != 2) { logf_(g, "checkpoint: start a render first"); return -1; }
    if (nora_render_is_running(g->render)) pause_render(g);
    if (nora_render_save_checkpoint(g->render, nora_scene_hash(g->scene), path, err, sizeof err) != 0) {
        logf_(g, "checkpoint: %s", err);
        return -1;
    }
    logf_(g, "saved checkpoint %s", path);
    return 0;
}

static int load_checkpoint(nora_gui *g, const char *path) {
    char err[512];
    nora_render *r;
    const nora_settings *st;
    if (ensure_built(g) != 0) return -1;
    r = nora_render_load_checkpoint(g->scene, nora_scene_hash(g->scene), path, (int)g->threads, err, sizeof err);
    if (!r) { logf_(g, "checkpoint: %s", err); snprintf(g->err, sizeof g->err, "%s", err); return -1; }   /* the current render goes on */
    stop_render(g);
    if (g->view_mode == VIEW_EDIT) { g->view_mode = VIEW_RENDER; logf_(g, "Render mode"); }
    g->render = r;
    g->render_kind = 2;
    g->cam_pending = 0;
    st = nora_render_settings(r);
    settings_to_ui(g, st);
    alloc_frame(g, st->width, st->height);
    logf_(g, "loaded checkpoint %s (%dx%d, %s)", path, st->width, st->height, nora_integrator_name(st->integrator));
    if (nora_render_set_spp(r, (int)g->spp) == 0 && nora_render_can_resume(r)) nora_render_resume(r);
    return 0;
}

/* --------------------------------------------------------------- scene */

static void graphs_reset(nora_gui *g) {
    int i;
    for (i = 0; i < g->ngraphs; i++) ng_free(&g->graphs[i].g);
    free(g->graphs);
    g->graphs = NULL;
    g->ngraphs = 0;
}

static void ensure_graphs(nora_gui *g) {
    int i;
    if (g->ngraphs >= g->scene->nmats) return;
    g->graphs = (mat_graph *)realloc(g->graphs, sizeof(mat_graph) * (size_t)g->scene->nmats);
    for (i = g->ngraphs; i < g->scene->nmats; i++) {
        const nora_material *m = &g->scene->mats[i];
        char err[256];
        ng_init(&g->graphs[i].g);
        g->graphs[i].used = 0;
        /* a material whose shader came from a graph opens that graph */
        if (m->shader >= 0 && m->shader < g->scene->nshaders) {
            const nora_shader_slot *sl = &g->scene->shaders[m->shader];
            if (sl->graph_json && ng_from_json(&g->graphs[i].g, sl->graph_json, err, sizeof err) == 0) { g->graphs[i].used = 1; continue; }
            if (sl->graph_file[0] && ng_load_file(&g->graphs[i].g, sl->graph_file, err, sizeof err) == 0) { g->graphs[i].used = 1; continue; }
        }
        ng_default_graph(&g->graphs[i].g);
    }
    g->ngraphs = g->scene->nmats;
}

static void watch_update(nora_gui *g) {
    g->ndeps = nora_scene_dependencies(g->scene, g->deps, MAX_DEPS);
    nora_watch_set(&g->watch, g->deps, g->ndeps, ntime_now());
}

static void select_material(nora_gui *g, int m);

static int find_mesh(const nora_scene *s, const char *name) {
    int i;
    for (i = 0; i < s->nmeshes; i++) if (!s->meshes[i].hidden && !strcmp(s->meshes[i].name, name)) return i;
    return -1;
}

static void select_mesh(nora_gui *g, int m) {
    if (m < 0 || m >= g->scene->nmeshes || g->scene->meshes[m].hidden) m = -1;
    g->sel_mesh = m;
    g->field_mesh = -2;   /* refresh the numeric fields */
    g->ed_dirty = 1;
    if (m >= 0 && g->scene->meshes[m].tri_count > 0) {
        int mat = g->scene->tris[g->scene->meshes[m].first_tri].mat;
        if (mat >= 0 && mat != g->sel_mat) select_material(g, mat);
    }
}

/* takes ownership of s */
static void set_scene(nora_gui *g, nora_scene *s, const char *arg, int keep_camera) {
    nora_camera cam;
    int had = g->scene != NULL;
    memset(&cam, 0, sizeof cam);
    stop_render(g);
    if (had) cam = g->scene->cam;
    if (g->scene) nora_scene_destroy(g->scene);
    g->scene = s;
    if (had && keep_camera) s->cam = cam;
    if (arg != g->scene_arg) snprintf(g->scene_arg, sizeof g->scene_arg, "%s", arg ? arg : "cornell-glass");
    snprintf(g->scene_file, sizeof g->scene_file, "%s", s->file_path);
    cam_from_scene(g);
    graphs_reset(g);
    ensure_graphs(g);
    g->sel_mat = s->nmats > 0 ? 0 : -1;
    g->sel_mesh = -1;
    g->field_mesh = -2;
    nora_history_clear(&g->hist);
    nora_scene_apply_transforms(s);
    if (g->view_mode == VIEW_RENDER) nora_scene_build(s);
    watch_update(g);
    g->ed_dirty = 1;
}

static int load_scene(nora_gui *g, const char *arg) {
    nora_scene *s;
    nora_settings st;
    int has_st = 0;
    char err[512];
    s = nora_scene_open(arg ? arg : "cornell-glass", &st, &has_st, err, sizeof err);
    if (!s) { logf_(g, "error: %s", err); snprintf(g->err, sizeof g->err, "%s", err); return -1; }
    if (has_st) settings_to_ui(g, &st);
    set_scene(g, s, arg, 0);
    if (g->view_mode == VIEW_RENDER && !s->built) { logf_(g, "error: scene build failed"); return -1; }
    logf_(g, "scene: %s, %d objects, %d triangles, %d materials%s%s", s->name, nora_scene_live_mesh_count(s), s->ntris, s->nmats,
          s->file_path[0] ? " - " : "", s->file_path);
    return 0;
}

static int open_scene(nora_gui *g, const char *path) {
    if (load_scene(g, path) != 0) return -1;
    if (g->sel_mat >= 0) select_material(g, g->sel_mat);
    restart_preview(g);
    return 0;
}

static int import_object(nora_gui *g, const char *path) {
    char err[512];
    int first, n0 = g->scene->nmeshes;
    stop_render(g);
    first = nora_edit_import(&g->hist, g->scene, path, err, sizeof err);
    if (first < 0) { logf_(g, "import: %s", err); snprintf(g->err, sizeof g->err, "%s", err); restart_preview(g); return -1; }
    ensure_graphs(g);
    logf_(g, "imported %s: %d objects", path, g->scene->nmeshes - n0);
    select_mesh(g, first);
    watch_update(g);
    scene_changed(g);
    return 0;
}

static int save_scene(nora_gui *g, const char *path) {
    char err[512];
    nora_settings st;
    base_settings(g, &st);
    if (!path || !path[0]) path = g->scene_file;
    if (!path[0]) { logf_(g, "save: no file name"); return -1; }
    if (nora_scenefile_save(g->scene, &st, path, err, sizeof err) != 0) { logf_(g, "save: %s", err); return -1; }
    snprintf(g->scene_file, sizeof g->scene_file, "%s", g->scene->file_path);
    nora_strlcpy(g->scene_path, path, sizeof g->scene_path);
    watch_update(g);   /* our own write is not a change */
    logf_(g, "saved scene %s", path);
    return 0;
}

static int export_obj(nora_gui *g, const char *path) {
    char err[512];
    int rc = nora_scene_export_obj(g->scene, path, err, sizeof err);
    if (rc != 0) { logf_(g, "export: %s", err); return -1; }
    g->scene->built = 0;
    if (g->view_mode == VIEW_RENDER && !g->render) nora_scene_build(g->scene);
    logf_(g, "exported %s (transforms baked in)", path);
    return 0;
}

/* copies material settings of a freshly loaded source over same-name materials */
static void refresh_materials(nora_scene *dst, const nora_scene *src) {
    int i;
    for (i = 0; i < src->nmats; i++) {
        int k = nora_scene_find_material(dst, src->mats[i].name);
        nora_material *d;
        const nora_material *m = &src->mats[i];
        if (k < 0) continue;
        d = &dst->mats[k];
        d->type = m->type; d->color = m->color; d->edge_tint = m->edge_tint; d->roughness = m->roughness;
        d->ior = m->ior; d->abbe = m->abbe; d->emission = m->emission; d->emission_strength = m->emission_strength;
        d->two_sided = m->two_sided;
        if (m->color_tex >= 0 && m->color_tex < src->ntexs) {
            char full[1024];
            nora_path_join(src->base_dir, src->texs[m->color_tex].path, full, sizeof full);
            d->color_tex = nora_scene_load_texture(dst, full);
        } else {
            d->color_tex = -1;
        }
    }
}

/* hot reload: rebuild the scene from its files, keeping the selection and camera */
static int reload_scene(nora_gui *g, const char *changed) {
    char err[512], sel[64], dir[1024];
    nora_scene *s = NULL;
    nora_settings st;
    int has_st = 0, i, k, was_rendering = g->render_kind == 2 && g->render && nora_render_is_running(g->render);
    sel[0] = 0;
    if (g->sel_mesh >= 0) snprintf(sel, sizeof sel, "%s", g->scene->meshes[g->sel_mesh].name);
    if (g->scene_file[0] && !strcmp(changed, g->scene_file)) {
        s = nora_scenefile_load(g->scene_file, &st, &has_st, err, sizeof err);
    } else {
        /* a referenced file changed: write the current scene out and read it back, which re-reads
         * every OBJ / OSL / graph file and keeps the transforms and edits */
        char *text;
        nora_settings cur;
        base_settings(g, &cur);
        if (g->scene_file[0]) nora_path_dir(g->scene_file, dir, sizeof dir);
        else nora_path_abs(".", dir, sizeof dir);
        text = nora_scenefile_write_string(g->scene, &cur, dir);
        s = text ? nora_scenefile_read_string(text, dir, NULL, NULL, err, sizeof err) : NULL;
        free(text);
        if (s) {
            snprintf(s->file_path, sizeof s->file_path, "%s", g->scene_file);
            /* edits to an MTL library win over the materials kept in memory */
            for (i = 0; i < s->nsources; i++) {
                int hit = !strcmp(s->sources[i].path, changed);
                for (k = 0; k < s->sources[i].ndeps; k++) if (!strcmp(s->sources[i].deps[k], changed)) hit = 1;
                if (hit && s->sources[i].kind == NORA_SRC_OBJ) {
                    nora_scene *fresh = nora_scene_load_obj(s->sources[i].path, err, sizeof err);
                    if (fresh) { refresh_materials(s, fresh); nora_scene_destroy(fresh); }
                }
            }
        }
    }
    if (!s) {
        logf_(g, "hot reload: %s (keeping the current scene)", err);
        return -1;
    }
    if (has_st) settings_to_ui(g, &st);
    set_scene(g, s, g->scene_arg, 1);
    cam_to_scene(g);
    if (sel[0]) select_mesh(g, find_mesh(g->scene, sel));
    if (g->sel_mat >= 0) select_material(g, g->sel_mat);
    logf_(g, "hot reload: %s changed, scene reloaded", nora_path_base(changed));
    if (was_rendering) { start_render(g, 2); }
    else restart_preview(g);
    return 0;
}

static void poll_files(nora_gui *g, int force) {
    int changed[8], n;
    if (!g->hot_reload || g->ndeps == 0) return;
    if (force) g->watch.last = -1e30;
    n = nora_watch_poll(&g->watch, ntime_now(), changed, 8);
    if (n > 0) {
        char path[1024];
        snprintf(path, sizeof path, "%s", g->watch.paths[changed[0]]);
        reload_scene(g, path);
    }
}

/* ---------------------------------------------------------- shader graph */

static void regenerate_source(nora_gui *g) {
    char err[256];
    if (g->sel_mat < 0 || g->sel_mat >= g->ngraphs) return;
    if (ng_generate_osl(&g->graphs[g->sel_mat].g, g->src, SRC_CAP, err, sizeof err) != 0) {
        snprintf(g->compile_msg, sizeof g->compile_msg, "graph: %s", err);
        g->compile_ok = 0;
        return;
    }
    g->src_from_graph = 1;
    g->src_needs_check = 1;
    g->src_edit_time = 0;
}

static void check_source(nora_gui *g) {
    char err[512];
    osl_program *p = osl_compile(g->src, err, sizeof err);
    if (p) {
        snprintf(g->compile_msg, sizeof g->compile_msg, "OK: shader '%s', %d instructions, %d parameters", osl_program_name(p),
                 osl_program_instruction_count(p), osl_param_count(p));
        g->compile_ok = 1;
        osl_program_free(p);
    } else {
        snprintf(g->compile_msg, sizeof g->compile_msg, "%s", err);
        g->compile_ok = 0;
    }
    g->src_needs_check = 0;
}

static int assign_source(nora_gui *g) {
    char err[512];
    nora_material *m;
    nora_shader_slot *sl;
    int slot;
    if (g->sel_mat < 0 || g->sel_mat >= g->scene->nmats) return -1;
    stop_render(g);
    m = &g->scene->mats[g->sel_mat];
    if (m->shader >= 0) slot = nora_scene_replace_shader(g->scene, m->shader, g->src, err, sizeof err);
    else slot = nora_scene_add_shader(g->scene, m->name, g->src, err, sizeof err);
    if (slot < 0) {
        snprintf(g->compile_msg, sizeof g->compile_msg, "%s", err);
        g->compile_ok = 0;
        logf_(g, "OSL error: %s", err);
        restart_preview(g);
        return -1;
    }
    m->shader = slot;
    /* remember where the shader came from, for scene files */
    sl = &g->scene->shaders[slot];
    free(sl->graph_json);
    sl->graph_json = NULL;
    sl->file[0] = 0;
    sl->graph_file[0] = 0;
    if (g->src_from_graph) sl->graph_json = ng_to_json(&g->graphs[g->sel_mat].g);
    g->graphs[g->sel_mat].used = g->src_from_graph;
    logf_(g, "assigned OSL shader to material '%s'", m->name);
    watch_update(g);
    scene_changed(g);
    return 0;
}

static void select_material(nora_gui *g, int m) {
    if (m < 0 || m >= g->scene->nmats) return;
    g->sel_mat = m;
    ensure_graphs(g);
    if (g->scene->mats[m].shader >= 0 && !g->graphs[m].used) {
        /* a shader that did not come from the graph: show its source */
        const nora_shader_slot *sl = &g->scene->shaders[g->scene->mats[m].shader];
        snprintf(g->src, SRC_CAP, "%s", sl->source ? sl->source : "");
        g->src_from_graph = 0;
        g->src_needs_check = 1;
    } else {
        regenerate_source(g);
    }
}

/* ---------------------------------------------------------------- edits */

static void set_view_mode(nora_gui *g, int mode) {
    if (mode == g->view_mode) return;
    g->view_mode = mode;
    if (mode == VIEW_EDIT) {
        stop_render(g);          /* no path tracing while editing */
        g->cam_pending = 0;
        g->ed_dirty = 1;
        logf_(g, "Edit mode");
    } else {
        nora_scene_apply_transforms(g->scene);
        nora_scene_invalidate(g->scene);
        if (nora_scene_build(g->scene) != 0) logf_(g, "error: scene build failed");   /* BVH over the edited instances */
        logf_(g, "Render mode: BVH rebuilt (%d nodes)", g->scene->nnodes);
        restart_preview(g);
    }
}

static void after_edit(nora_gui *g) {
    nora_scene_apply_transforms(g->scene);
    g->field_mesh = -2;
    if (g->sel_mesh >= 0 && g->scene->meshes[g->sel_mesh].hidden) g->sel_mesh = -1;
    scene_changed(g);
}

static int do_undo(nora_gui *g) {
    const char *label = NULL;
    stop_render(g);
    if (!nora_history_undo(&g->hist, g->scene, &label)) { logf_(g, "nothing to undo"); restart_preview(g); return -1; }
    logf_(g, "undo: %s", label);
    after_edit(g);
    return 0;
}

static int do_redo(nora_gui *g) {
    const char *label = NULL;
    stop_render(g);
    if (!nora_history_redo(&g->hist, g->scene, &label)) { logf_(g, "nothing to redo"); restart_preview(g); return -1; }
    logf_(g, "redo: %s", label);
    after_edit(g);
    return 0;
}

static int do_delete(nora_gui *g) {
    char name[64];
    if (g->sel_mesh < 0) return -1;
    stop_render(g);
    snprintf(name, sizeof name, "%s", g->scene->meshes[g->sel_mesh].name);
    if (nora_edit_delete(&g->hist, g->scene, g->sel_mesh) != 0) { restart_preview(g); return -1; }
    logf_(g, "deleted %s", name);
    g->sel_mesh = -1;
    after_edit(g);
    return 0;
}

static int do_duplicate(nora_gui *g) {
    int nm;
    if (g->sel_mesh < 0) return -1;
    stop_render(g);
    nm = nora_edit_duplicate(&g->hist, g->scene, g->sel_mesh);
    if (nm < 0) { restart_preview(g); return -1; }
    logf_(g, "duplicated %s as %s", g->scene->meshes[g->sel_mesh].name, g->scene->meshes[nm].name);
    g->sel_mesh = nm;
    after_edit(g);
    return 0;
}

/* a transform typed into Properties; consecutive edits of one object within a second are one undo step */
static void field_transform(nora_gui *g, const nora_xform *x) {
    nora_history *h = &g->hist;
    double now = ntime_now();
    int m = g->sel_mesh;
    if (m < 0) return;
    if (h->pos == h->n && h->n > 0 && h->ops[h->n - 1].kind == NORA_EDIT_TRANSFORM && h->ops[h->n - 1].mesh == m &&
        !strcmp(h->ops[h->n - 1].label, "Edit transform") && now - g->last_field_edit < 1.0) {
        nora_xform_set(&g->scene->meshes[m], x);
        h->ops[h->n - 1].after = *x;
    } else {
        nora_edit_transform(h, g->scene, m, x, "Edit transform");
    }
    g->last_field_edit = now;
    stop_render(g);
    nora_scene_apply_transforms(g->scene);
    scene_changed(g);
}

static int set_transform_cmd(nora_gui *g, int what, v3 v) {
    nora_xform x;
    if (g->sel_mesh < 0) return -1;
    nora_xform_get(&g->scene->meshes[g->sel_mesh], &x);
    if (what == 0) x.translate = v;
    else if (what == 1) { x.euler_deg = v; x.rot = quat_from_euler_deg(v); }
    else x.scale = v;
    stop_render(g);
    nora_edit_transform(&g->hist, g->scene, g->sel_mesh, &x, what == 0 ? "Move" : (what == 1 ? "Rotate" : "Scale"));
    g->field_mesh = -2;
    scene_changed(g);
    return 0;
}

/* ---------------------------------------------------------------- gizmo */

static cui_vec2 to_canvas(nora_gui *g, float px, float py) {
    cui_vec2 v;
    v.x = g->vp_rect.x + px * (g->vp_rect.w / (float)(g->ed_w > 0 ? g->ed_w : 1));
    v.y = g->vp_rect.y + py * (g->vp_rect.h / (float)(g->ed_h > 0 ? g->ed_h : 1));
    return v;
}

static int project(nora_gui *g, v3 p, cui_vec2 *out) {
    float px, py;
    if (g->ed_w <= 0 || g->ed_h <= 0) return 0;
    if (!nora_project(&g->scene->cam, g->ed_w, g->ed_h, p, &px, &py, NULL)) return 0;
    *out = to_canvas(g, px, py);
    return 1;
}

static void gizmo_update_geo(nora_gui *g) {
    gizmo_geo *z = &g->giz;
    const nora_mesh *m;
    v3 c;
    int a, k;
    z->valid = 0;
    if (g->view_mode != VIEW_EDIT || g->sel_mesh < 0 || g->ed_w <= 0) return;
    m = &g->scene->meshes[g->sel_mesh];
    c = nora_mesh_center(m);
    z->world_len = 0.16f * v3_len(v3_sub(c, g->scene->cam.pos)) * tanf(0.5f * g->scene->cam.vfov_deg * NORA_PI / 180.0f) * 2.0f;
    if (!project(g, c, &z->center)) return;
    for (a = 0; a < 3; a++) {
        v3 ax = v3_make(a == 0, a == 1, a == 2);
        if (g->gizmo_mode == GIZMO_SCALE) ax = quat_rotate(m->rot, ax);   /* scale acts along the object's own axes */
        z->axis[a] = ax;
        if (!project(g, v3_add(c, v3_scale(ax, z->world_len)), &z->tip[a])) return;
        {
            v3 t, b;
            onb_from(ax, &t, &b);
            for (k = 0; k < RING_PTS; k++) {
                float ang = 2.0f * NORA_PI * (float)k / (float)RING_PTS;
                v3 p = v3_add(c, v3_scale(v3_add(v3_scale(t, cosf(ang)), v3_scale(b, sinf(ang))), z->world_len * 0.9f));
                if (!project(g, p, &z->ring[a][k])) z->ring[a][k] = z->center;
            }
        }
    }
    z->valid = 1;
}

static float seg_dist(cui_vec2 p, cui_vec2 a, cui_vec2 b) {
    float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy, t;
    t = l2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / l2 : 0;
    t = nclampf(t, 0, 1);
    dx = a.x + t * dx - p.x;
    dy = a.y + t * dy - p.y;
    return sqrtf(dx * dx + dy * dy);
}

static int gizmo_hit(nora_gui *g, cui_vec2 p) {
    const gizmo_geo *z = &g->giz;
    int a, k, best = -1;
    float bd = 8.0f;
    if (!z->valid) return -1;
    for (a = 0; a < 3; a++) {
        float d = NORA_INFF;
        if (g->gizmo_mode == GIZMO_ROTATE) {
            for (k = 0; k < RING_PTS; k++) d = fminf(d, seg_dist(p, z->ring[a][k], z->ring[a][(k + 1) % RING_PTS]));
        } else {
            d = seg_dist(p, z->center, z->tip[a]);
        }
        if (d < bd) { bd = d; best = a; }
    }
    return best;
}

static void gizmo_begin(nora_gui *g, int axis, cui_vec2 p) {
    g->giz_drag = axis;
    g->giz_press = p;
    nora_xform_get(&g->scene->meshes[g->sel_mesh], &g->giz_start);
}

static void gizmo_drag(nora_gui *g, cui_vec2 p) {
    const gizmo_geo *z = &g->giz;
    nora_mesh *m = &g->scene->meshes[g->sel_mesh];
    int a = g->giz_drag;
    nora_xform x = g->giz_start;
    float ax = z->tip[a].x - z->center.x, ay = z->tip[a].y - z->center.y, l2 = ax * ax + ay * ay;
    float dx = p.x - g->giz_press.x, dy = p.y - g->giz_press.y;
    if (g->gizmo_mode == GIZMO_MOVE) {
        float t = l2 > 1e-3f ? (dx * ax + dy * ay) / l2 : 0.0f;
        x.translate = v3_add(g->giz_start.translate, v3_scale(z->axis[a], t * z->world_len));
    } else if (g->gizmo_mode == GIZMO_SCALE) {
        float t = l2 > 1e-3f ? (dx * ax + dy * ay) / l2 : 0.0f, f = fmaxf(0.01f, 1.0f + t);
        if (a == 0) x.scale.x *= f; else if (a == 1) x.scale.y *= f; else x.scale.z *= f;
    } else {
        float a0 = atan2f(g->giz_press.y - z->center.y, g->giz_press.x - z->center.x);
        float a1 = atan2f(p.y - z->center.y, p.x - z->center.x);
        float ang = a1 - a0;
        v3 f, r, u;
        nora_camera_basis(&g->scene->cam, &f, &r, &u);
        /* screen y points down: the object follows the mouse around the ring */
        if (v3_dot(z->axis[a], f) < 0.0f) ang = -ang;
        x.rot = quat_normalize(quat_mul(quat_axis_angle(z->axis[a], ang), g->giz_start.rot));
        x.euler_deg = quat_to_euler_deg(x.rot);
    }
    nora_xform_set(m, &x);
    nora_scene_apply_transforms(g->scene);
    g->field_mesh = -2;
    g->ed_dirty = 1;
}

static void gizmo_end(nora_gui *g) {
    nora_xform now;
    static const char *const labels[3] = { "Move", "Rotate", "Scale" };
    if (g->giz_drag < 0) return;
    nora_xform_get(&g->scene->meshes[g->sel_mesh], &now);
    nora_history_push_transform(&g->hist, g->sel_mesh, &g->giz_start, &now, labels[g->gizmo_mode]);
    g->giz_drag = -1;
    g->scene->built = 0;
}

/* picks the object under canvas point p: ID buffer in Edit mode, a BVH ray in Render mode */
static int pick_at(nora_gui *g, cui_vec2 p) {
    float u = (p.x - g->vp_rect.x) / (g->vp_rect.w > 0 ? g->vp_rect.w : 1);
    float v = (p.y - g->vp_rect.y) / (g->vp_rect.h > 0 ? g->vp_rect.h : 1);
    if (u < 0 || v < 0 || u >= 1 || v >= 1) return -1;
    if (g->view_mode == VIEW_EDIT) {
        int x = (int)(u * g->ed_w), y = (int)(v * g->ed_h);
        if (!g->ed_ids || x < 0 || y < 0 || x >= g->ed_w || y >= g->ed_h) return -1;
        return g->ed_ids[(size_t)y * g->ed_w + x];
    } else {
        /* the render is letterboxed into the canvas at its own aspect */
        float s, ix, iy;
        nora_hit hit;
        ray3 ray;
        int w = (int)g->width, h = (int)g->height;
        if (ensure_built(g) != 0) return -1;
        s = fminf(g->vp_rect.w / (float)w, g->vp_rect.h / (float)h);
        ix = (p.x - (g->vp_rect.x + (g->vp_rect.w - w * s) * 0.5f)) / s;
        iy = (p.y - (g->vp_rect.y + (g->vp_rect.h - h * s) * 0.5f)) / s;
        if (ix < 0 || iy < 0 || ix >= w || iy >= h) return -1;
        ray = nora_camera_ray(&g->scene->cam, w, h, ix, iy, 0.5f, 0.5f);
        if (!nora_intersect(g->scene, &ray, &hit)) return -1;
        return g->scene->tris[hit.tri].mesh;
    }
}

static void update_raster(nora_gui *g, cui_ctx *ui) {
    int w = (int)g->vp_rect.w, h = (int)g->vp_rect.h;
    nora_raster_opts o;
    if (g->view_mode != VIEW_EDIT) return;
    if (w < 16) w = 16;
    if (h < 16) h = 16;
    if (w > 2048) w = 2048;
    if (h > 2048) h = 2048;
    if (w != g->ed_w || h != g->ed_h) {
        free(g->ed_rgba); free(g->ed_depth); free(g->ed_ids);
        g->ed_rgba = (unsigned char *)malloc((size_t)w * h * 4);
        g->ed_depth = (float *)malloc(sizeof(float) * (size_t)w * h);
        g->ed_ids = (int *)malloc(sizeof(int) * (size_t)w * h);
        g->ed_w = w; g->ed_h = h;
        g->ed_dirty = 1;
    }
    if (!g->ed_dirty || !g->ed_rgba) return;
    memset(&o, 0, sizeof o);
    o.wireframe = g->wireframe;
    o.selected = g->sel_mesh;
    nora_raster(g->scene, &g->scene->cam, w, h, &o, g->ed_rgba, g->ed_depth, g->ed_ids);
    if (g->ed_tex) cui_image_destroy(ui, g->ed_tex);
    g->ed_tex = cui_image_create(ui, w, h, g->ed_rgba);
    g->ed_dirty = 0;
}

/* ------------------------------------------------------------- creation */

nora_gui *nora_gui_create(const char *scene, const nora_gui_host *host) {
    nora_gui *g = (nora_gui *)calloc(1, sizeof *g);
    int i;
    if (!g) return NULL;
    if (host) g->host = *host;
    nmutex_init(&g->log.mu);
    g_log_target = g;
    nora_set_log(gui_log_fn, g);
    spectrum_init();
    g->src = (char *)calloc(SRC_CAP, 1);
    g->mode_idx = 0;
    g->integrator_idx = 0;
    g->p_large = 0.3f;
    g->spp = 256;
    g->time_limit = 0;
    g->threads = 0;
    g->max_depth = 10;
    g->width = 512;
    g->height = 512;
    g->auto_mlt = true;
    g->exposure = 0.0f;
    g->view_mode = VIEW_RENDER;
    g->gizmo_mode = GIZMO_MOVE;
    g->giz_drag = -1;
    g->giz_hover = -1;
    g->hot_reload = true;
    g->add_type = ng_type_by_id("diffuse");
    nora_history_init(&g->hist);
    nora_watch_init(&g->watch, 0.5);
    snprintf(g->save_path, sizeof g->save_path, "nora_render.png");
    snprintf(g->graph_path, sizeof g->graph_path, "graph.json");
    snprintf(g->scene_path, sizeof g->scene_path, "scene.nora");
    snprintf(g->import_path, sizeof g->import_path, "model.obj");
    snprintf(g->export_path, sizeof g->export_path, "export.obj");
    snprintf(g->ckpt_path, sizeof g->ckpt_path, "render.nrck");
    for (i = 0; i < 7; i++) g->open[i] = true;
    logf_(g, "Nora 0.2.0 - Metropolis light transport, RGB and spectral");
    if (load_scene(g, scene) != 0 && load_scene(g, "cornell-glass") != 0) { nora_gui_destroy(g); return NULL; }
    select_material(g, 0);
    start_render(g, 1);
    g->cam_pending = 1;
    g->last_cam_change = ntime_now();
    return g;
}

void nora_gui_destroy(nora_gui *g) {
    if (!g) return;
    stop_render(g);
    nora_set_log(NULL, NULL);
    if (g_log_target == g) g_log_target = NULL;
    graphs_reset(g);
    if (g->scene) nora_scene_destroy(g->scene);
    nora_history_free(&g->hist);
    nora_watch_free(&g->watch);
    free(g->rgb); free(g->rgba); free(g->src);
    free(g->ed_rgba); free(g->ed_depth); free(g->ed_ids);
    nmutex_destroy(&g->log.mu);
    free(g);
}

int nora_gui_is_rendering(nora_gui *g) { return g && g->render && nora_render_is_running(g->render); }
float nora_gui_progress(nora_gui *g) { return g && g->render ? nora_render_progress(g->render) : 0.0f; }
const char *nora_gui_last_error(nora_gui *g) { return g ? g->err : ""; }
nora_scene *nora_gui_scene(nora_gui *g) { return g ? g->scene : NULL; }
int nora_gui_selected_object(nora_gui *g) { return g ? g->sel_mesh : -1; }
int nora_gui_view_mode(nora_gui *g) { return g ? g->view_mode : 0; }
int nora_gui_is_paused(nora_gui *g) { return g && g->render && nora_render_is_paused(g->render); }
int nora_gui_render_samples(nora_gui *g, long long *samples) {
    nora_render_stats rs;
    if (!g || !g->render) return -1;
    nora_render_get_stats(g->render, &rs);
    *samples = (long long)rs.samples;
    return 0;
}
int nora_gui_viewport_rect(nora_gui *g, float *x, float *y, float *w, float *h) {
    if (!g || g->vp_rect.w <= 0) return -1;
    *x = g->vp_rect.x; *y = g->vp_rect.y; *w = g->vp_rect.w; *h = g->vp_rect.h;
    return 0;
}
int nora_gui_project(nora_gui *g, int mesh, float *x, float *y) {
    cui_vec2 p;
    if (!g || mesh < 0 || mesh >= g->scene->nmeshes || !project(g, nora_mesh_center(&g->scene->meshes[mesh]), &p)) return -1;
    *x = p.x; *y = p.y;
    return 0;
}
int nora_gui_gizmo_handle(nora_gui *g, int axis, float *x, float *y) {
    if (!g || !g->giz.valid || axis < 0 || axis > 2) return -1;
    if (g->gizmo_mode == GIZMO_ROTATE) { *x = g->giz.ring[axis][RING_PTS / 8].x; *y = g->giz.ring[axis][RING_PTS / 8].y; }
    else { *x = 0.25f * g->giz.center.x + 0.75f * g->giz.tip[axis].x; *y = 0.25f * g->giz.center.y + 0.75f * g->giz.tip[axis].y; }
    return 0;
}

static int parse_v3(const char *s, v3 *v) { return sscanf(s, "%f %f %f", &v->x, &v->y, &v->z) == 3 ? 0 : -1; }

int nora_gui_command(nora_gui *g, const char *cmd) {
    char a[512];
    int x, y;
    float fx, fy;
    v3 v;
    if (!g || !cmd) return -1;
    if (!strcmp(cmd, "render")) { g->cam_pending = 0; if (g->view_mode == VIEW_EDIT) set_view_mode(g, VIEW_RENDER); start_render(g, 2); return g->render ? 0 : -1; }
    if (!strcmp(cmd, "stop")) { if (g->render) nora_render_request_stop(g->render); return 0; }
    if (!strcmp(cmd, "pause")) return pause_render(g);
    if (!strcmp(cmd, "resume")) return resume_render(g);
    if (!strcmp(cmd, "preview")) { start_render(g, 1); return 0; }
    if (!strcmp(cmd, "assign")) return assign_source(g);
    if (!strcmp(cmd, "undo")) return do_undo(g);
    if (!strcmp(cmd, "redo")) return do_redo(g);
    if (!strcmp(cmd, "delete")) return do_delete(g);
    if (!strcmp(cmd, "duplicate")) return do_duplicate(g);
    if (!strcmp(cmd, "poll")) { poll_files(g, 1); return 0; }
    if (!strcmp(cmd, "view edit")) { set_view_mode(g, VIEW_EDIT); return 0; }
    if (!strcmp(cmd, "view render")) { set_view_mode(g, VIEW_RENDER); return 0; }
    if (!strcmp(cmd, "wireframe on") || !strcmp(cmd, "wireframe off")) { g->wireframe = !strcmp(cmd, "wireframe on"); g->ed_dirty = 1; return 0; }
    if (!strcmp(cmd, "hotreload on") || !strcmp(cmd, "hotreload off")) { g->hot_reload = !strcmp(cmd, "hotreload on"); if (g->hot_reload) watch_update(g); return 0; }
    if (!strcmp(cmd, "gizmo move")) { g->gizmo_mode = GIZMO_MOVE; return 0; }
    if (!strcmp(cmd, "gizmo rotate")) { g->gizmo_mode = GIZMO_ROTATE; return 0; }
    if (!strcmp(cmd, "gizmo scale")) { g->gizmo_mode = GIZMO_SCALE; return 0; }
    if (!strncmp(cmd, "move ", 5) && parse_v3(cmd + 5, &v) == 0) return set_transform_cmd(g, 0, v);
    if (!strncmp(cmd, "rotate ", 7) && parse_v3(cmd + 7, &v) == 0) return set_transform_cmd(g, 1, v);
    if (!strncmp(cmd, "scale ", 6) && parse_v3(cmd + 6, &v) == 0) return set_transform_cmd(g, 2, v);
    if (sscanf(cmd, "mode %511s", a) == 1) { g->mode_idx = !strcmp(a, "spectral"); return 0; }
    if (sscanf(cmd, "integrator %511s", a) == 1) { g->integrator_idx = !strcmp(a, "pt"); return 0; }
    if (sscanf(cmd, "spp %d", &x) == 1) { g->spp = x; if (g->render && !nora_render_is_running(g->render)) nora_render_set_spp(g->render, x); return 0; }
    if (sscanf(cmd, "size %d %d", &x, &y) == 2) { g->width = x; g->height = y; return 0; }
    if (sscanf(cmd, "selectobj %511[^\n]", a) == 1) { int m = find_mesh(g->scene, a); if (m < 0) return -1; select_mesh(g, m); return 0; }
    if (sscanf(cmd, "select %511[^\n]", a) == 1) {
        int m = nora_scene_find_material(g->scene, a);
        if (m < 0) return -1;
        select_material(g, m);
        return 0;
    }
    if (sscanf(cmd, "graph %511[^\n]", a) == 1) {
        if (g->sel_mat < 0) return -1;
        if (!strcmp(a, "default")) ng_default_graph(&g->graphs[g->sel_mat].g);
        else if (ng_load_file(&g->graphs[g->sel_mat].g, a, g->err, sizeof g->err) != 0) return -1;
        regenerate_source(g);
        return 0;
    }
    if (sscanf(cmd, "open %511[^\n]", a) == 1) return open_scene(g, a);
    if (sscanf(cmd, "import %511[^\n]", a) == 1) return import_object(g, a);
    if (sscanf(cmd, "savescene %511[^\n]", a) == 1) return save_scene(g, a);
    if (!strcmp(cmd, "savescene")) return save_scene(g, NULL);
    if (sscanf(cmd, "exportobj %511[^\n]", a) == 1) return export_obj(g, a);
    if (sscanf(cmd, "checkpoint %511[^\n]", a) == 1) return save_checkpoint(g, a);
    if (sscanf(cmd, "loadcheckpoint %511[^\n]", a) == 1) return load_checkpoint(g, a);
    if (sscanf(cmd, "save %511[^\n]", a) == 1) return save_image(g, a);
    if (sscanf(cmd, "orbit %f %f", &fx, &fy) == 2) { g->yaw += fx; g->pitch = nclampf(g->pitch + fy, -1.5f, 1.5f); camera_changed(g); return 0; }
    return -1;
}

/* --------------------------------------------------------- file actions */

static int ask_path(nora_gui *g, int save, const char *name, const char *filter, char *field, size_t cap) {
    char path[1024];
    if (g->host.file_dialog) {
        if (!g->host.file_dialog(g->host.user, save, name, filter, path, sizeof path)) return 0;
        nora_strlcpy(field, path, cap);
    }
    return field[0] != 0;   /* without a dialog the path field in Render Settings is used */
}

static void menu_open(nora_gui *g) {
    if (ask_path(g, 0, "Scenes", "nora;obj", g->scene_path, sizeof g->scene_path)) open_scene(g, g->scene_path);
}
static void menu_import(nora_gui *g) {
    if (ask_path(g, 0, "OBJ models", "obj", g->import_path, sizeof g->import_path)) import_object(g, g->import_path);
}
static void menu_save(nora_gui *g, int as) {
    if (!as && g->scene_file[0]) { save_scene(g, g->scene_file); return; }
    if (ask_path(g, 1, "Nora scenes", "nora", g->scene_path, sizeof g->scene_path)) save_scene(g, g->scene_path);
}
static void menu_export(nora_gui *g) {
    if (ask_path(g, 1, "OBJ models", "obj", g->export_path, sizeof g->export_path)) export_obj(g, g->export_path);
}
static void menu_save_image(nora_gui *g) {
    if (ask_path(g, 1, "Images", "png;exr;pfm", g->save_path, sizeof g->save_path)) save_image(g, g->save_path);
}
static void menu_checkpoint(nora_gui *g, int load) {
    if (ask_path(g, !load, "Nora checkpoints", "nrck", g->ckpt_path, sizeof g->ckpt_path)) {
        if (load) load_checkpoint(g, g->ckpt_path);
        else save_checkpoint(g, g->ckpt_path);
    }
}

static void menu_bar(nora_gui *g, cui_ctx *ui) {
    int rendering = nora_gui_is_rendering(g);
    cui_menu_bar(ui, "menubar") {
        cui_menu_bar_menu(ui, "File") {
            if (cui_menu_item(ui, "Open Scene...", .icon = CUI_ICON_FOLDER_OPEN, .shortcut = "Ctrl+O")) menu_open(g);
            if (cui_menu_item(ui, "Import Object...", .icon = CUI_ICON_DOWNLOAD, .shortcut = "Ctrl+I")) menu_import(g);
            cui_menu_separator(ui);
            if (cui_menu_item(ui, "Save Scene", .icon = CUI_ICON_SAVE, .shortcut = "Ctrl+S")) menu_save(g, 0);
            if (cui_menu_item(ui, "Save Scene As...", .shortcut = "Ctrl+Shift+S")) menu_save(g, 1);
            if (cui_menu_item(ui, "Export OBJ...", .icon = CUI_ICON_UPLOAD)) menu_export(g);
            if (cui_menu_item(ui, "Save Image...", .icon = CUI_ICON_IMAGE)) menu_save_image(g);
            cui_menu_separator(ui);
            if (cui_menu_item(ui, "Save Checkpoint...", .disabled = !g->render || g->render_kind != 2)) menu_checkpoint(g, 0);
            if (cui_menu_item(ui, "Resume from Checkpoint...")) menu_checkpoint(g, 1);
        }
        cui_menu_bar_menu(ui, "Edit") {
            if (cui_menu_item(ui, "Undo", .icon = CUI_ICON_UNDO_2, .shortcut = "Ctrl+Z", .disabled = !nora_history_can_undo(&g->hist))) do_undo(g);
            if (cui_menu_item(ui, "Redo", .icon = CUI_ICON_REDO_2, .shortcut = "Ctrl+Y", .disabled = !nora_history_can_redo(&g->hist))) do_redo(g);
            cui_menu_separator(ui);
            if (cui_menu_item(ui, "Duplicate", .icon = CUI_ICON_COPY, .shortcut = "Shift+D", .disabled = g->sel_mesh < 0)) do_duplicate(g);
            if (cui_menu_item(ui, "Delete", .icon = CUI_ICON_TRASH_2, .shortcut = "Del", .disabled = g->sel_mesh < 0, .danger = true)) do_delete(g);
        }
        cui_menu_bar_menu(ui, "View") {
            if (cui_menu_item(ui, "Edit mode", .shortcut = "Tab", .checked = g->view_mode == VIEW_EDIT)) set_view_mode(g, VIEW_EDIT);
            if (cui_menu_item(ui, "Render mode", .shortcut = "Tab", .checked = g->view_mode == VIEW_RENDER)) set_view_mode(g, VIEW_RENDER);
            cui_menu_separator(ui);
            if (cui_menu_item(ui, "Wireframe", .checked = g->wireframe)) { g->wireframe = !g->wireframe; g->ed_dirty = 1; }
            if (cui_menu_item(ui, "Move gizmo", .shortcut = "W", .checked = g->gizmo_mode == GIZMO_MOVE)) g->gizmo_mode = GIZMO_MOVE;
            if (cui_menu_item(ui, "Rotate gizmo", .shortcut = "E", .checked = g->gizmo_mode == GIZMO_ROTATE)) g->gizmo_mode = GIZMO_ROTATE;
            if (cui_menu_item(ui, "Scale gizmo", .shortcut = "R", .checked = g->gizmo_mode == GIZMO_SCALE)) g->gizmo_mode = GIZMO_SCALE;
            if (cui_menu_item(ui, "Frame scene", .shortcut = "F")) { nora_scene_frame_camera(g->scene); cam_from_scene(g); camera_changed(g); }
        }
        cui_menu_bar_menu(ui, "Render") {
            if (cui_menu_item(ui, "Render", .icon = CUI_ICON_PLAY, .shortcut = "F12")) nora_gui_command(g, "render");
            if (cui_menu_item(ui, "Pause", .icon = CUI_ICON_PAUSE, .disabled = !rendering || g->render_kind != 2)) pause_render(g);
            if (cui_menu_item(ui, "Resume", .icon = CUI_ICON_PLAY, .disabled = !g->render || rendering || !nora_render_can_resume(g->render))) resume_render(g);
            if (cui_menu_item(ui, "Stop", .icon = CUI_ICON_SQUARE, .disabled = !rendering)) { nora_render_request_stop(g->render); g->cam_pending = 0; }
        }
    }
}

/* global shortcuts */
static void shortcuts(nora_gui *g, cui_ctx *ui) {
    if (cui_wants_keyboard(ui)) return;
    if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_Z)) do_undo(g);
    if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_Y) || cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_Z)) do_redo(g);
    if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_O)) menu_open(g);
    if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_I)) menu_import(g);
    if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_S)) menu_save(g, 0);
    if (cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_S)) menu_save(g, 1);
    if (cui_key_pressed(ui, CUI_KEY_F12)) nora_gui_command(g, "render");
}

/* ------------------------------------------------------------- panels */

typedef struct vp_paint {
    cui_texture tex;
    int w, h, fill;
    char overlay[160];
    gizmo_geo giz;
    int gizmo_mode, hot;
} vp_paint;

static void paint_viewport(cui_ctx *ui, cui_rect r, void *user) {
    vp_paint *p = (vp_paint *)user;
    static const cui_color axis_col[3] = { { 235, 70, 70, 255 }, { 90, 210, 90, 255 }, { 80, 140, 255, 255 } };
    cui_draw_rect(ui, r, (cui_corners){ 0, 0, 0, 0 }, CUI_RGB(0x16181c));
    if (p->tex && p->w > 0 && p->h > 0) {
        if (p->fill) {
            cui_draw_image(ui, p->tex, r, (cui_rect){ 0, 0, 1, 1 }, (cui_corners){ 0, 0, 0, 0 }, CUI_RGB(0xffffff));
        } else {
            float s = fminf(r.w / (float)p->w, r.h / (float)p->h);
            cui_rect img = { r.x + (r.w - p->w * s) * 0.5f, r.y + (r.h - p->h * s) * 0.5f, p->w * s, p->h * s };
            cui_draw_image(ui, p->tex, img, (cui_rect){ 0, 0, 1, 1 }, (cui_corners){ 0, 0, 0, 0 }, CUI_RGB(0xffffff));
        }
    }
    if (p->giz.valid) {
        int a;
        for (a = 0; a < 3; a++) {
            cui_color c = a == p->hot ? CUI_RGB(0xffe040) : axis_col[a];
            if (p->gizmo_mode == 1) {
                cui_draw_polyline(ui, p->giz.ring[a], RING_PTS, a == p->hot ? 3.0f : 2.0f, c, true);
            } else {
                cui_draw_line(ui, p->giz.center, p->giz.tip[a], a == p->hot ? 3.5f : 2.5f, c);
                if (p->gizmo_mode == 2) cui_draw_rect(ui, (cui_rect){ p->giz.tip[a].x - 5, p->giz.tip[a].y - 5, 10, 10 }, CUI_CORNERS(1), c);
                else cui_draw_circle(ui, p->giz.tip[a], 5.5f, c);
            }
        }
        cui_draw_circle(ui, p->giz.center, 3.5f, CUI_RGB(0xffffff));
    }
    if (p->overlay[0])
        cui_draw_text(ui, CUI_FONT_MONO, 12, (cui_vec2){ r.x + 10, r.y + 8 }, CUI_RGBA(0xffffffd0), p->overlay, -1);
}

static void viewport_toolbar(nora_gui *g, cui_ctx *ui) {
    static const char *const modes[] = { "Edit", "Render" };
    static const char *const gizmos[] = { "Move (W)", "Rotate (E)", "Scale (R)" };
    int mode = g->view_mode;
    cui_row(ui, .gap = 8, .pad = 4, .align = CUI_ALIGN_CENTER, .width = CUI_FILL) {
        if (cui_segmented(ui, "viewmode", modes, 2, &mode)) set_view_mode(g, mode);
        if (g->view_mode == VIEW_EDIT) {
            if (cui_switch(ui, "Wireframe", &g->wireframe)) g->ed_dirty = 1;
            cui_segmented(ui, "gizmo", gizmos, 3, &g->gizmo_mode);
        } else if (g->render && g->render_kind == 2) {
            if (nora_render_is_running(g->render)) {
                if (cui_button(ui, "Pause", .icon = CUI_ICON_PAUSE, .size = CUI_SIZE_SM)) pause_render(g);
            } else if (nora_render_can_resume(g->render)) {
                if (cui_button(ui, "Resume", .icon = CUI_ICON_PLAY, .size = CUI_SIZE_SM)) resume_render(g);
            }
        }
    }
}

static void viewport_panel(nora_gui *g, cui_ctx *ui) {
    vp_paint vp;
    cui_canvas_result cr;
    cui_vec2 m = cui_mouse_pos(ui);
    int mods = cui_key_mods(ui), left = cui_mouse_down(ui, CUI_MOUSE_LEFT), pressed = left && !g->prev_left;
    int edit = g->view_mode == VIEW_EDIT;
    nora_render_stats rs;
    viewport_toolbar(g, ui);
    memset(&vp, 0, sizeof vp);
    if (edit) {
        update_raster(g, ui);
        gizmo_update_geo(g);
        vp.tex = g->ed_tex; vp.w = g->ed_w; vp.h = g->ed_h; vp.fill = 1;
        vp.giz = g->giz;
        vp.gizmo_mode = g->gizmo_mode;
        vp.hot = g->giz_drag >= 0 ? g->giz_drag : g->giz_hover;
        snprintf(vp.overlay, sizeof vp.overlay, "edit  %d objects%s%s", nora_scene_live_mesh_count(g->scene),
                 g->sel_mesh >= 0 ? "  selected: " : "", g->sel_mesh >= 0 ? g->scene->meshes[g->sel_mesh].name : "");
    } else {
        vp.tex = g->tex; vp.w = g->tex_w; vp.h = g->tex_h;
        vp.hot = -1;
        if (g->render) {
            nora_render_get_stats(g->render, &rs);
            snprintf(vp.overlay, sizeof vp.overlay, "%s  %s  %dx%d  %.0f%%  %.1fs%s%s",
                     g->render_kind == 1 ? "preview (PT)" : nora_integrator_name(nora_render_settings(g->render)->integrator),
                     nora_mode_name(nora_render_settings(g->render)->mode), g->render_w, g->render_h, 100.0 * nora_render_progress(g->render),
                     rs.elapsed, rs.phase == 1 ? "  bootstrapping" : "", rs.phase == 4 ? "  paused" : "");
        }
    }
    cr = cui_canvas(ui, "viewport", CUI_FILL, CUI_FILL, paint_viewport, &vp, sizeof vp);
    if (cr.rect.w != g->vp_rect.w || cr.rect.h != g->vp_rect.h) g->ed_dirty = 1;
    g->vp_rect = cr.rect;
    if (edit) g->giz_hover = g->giz_drag < 0 && cr.hovered ? gizmo_hit(g, m) : -1;

    /* left button: gizmo drag, pick (click) or orbit (drag) */
    if (pressed && cr.hovered && !g->dragging && g->giz_drag < 0) {
        if (edit && g->giz_hover >= 0) gizmo_begin(g, g->giz_hover, m);
        else if (!(mods & (CUI_MOD_SHIFT | CUI_MOD_CTRL))) { g->click_pending = 1; g->press_pos = m; g->dragging = 1; g->drag_last = m; }
    }
    if (g->giz_drag >= 0) {
        if (left) { if (m.x != g->giz_press.x || m.y != g->giz_press.y) gizmo_drag(g, m); }
        else { gizmo_end(g); scene_changed(g); }
    }
    if (g->click_pending && left && (fabsf(m.x - g->press_pos.x) > 3 || fabsf(m.y - g->press_pos.y) > 3)) g->click_pending = 0;
    if (g->click_pending && !left) {
        int hit = pick_at(g, g->press_pos);
        g->click_pending = 0;
        g->dragging = 0;
        select_mesh(g, hit);
        if (hit >= 0) logf_(g, "selected %s", g->scene->meshes[hit].name);
    }
    /* camera controls */
    if (!g->dragging && g->giz_drag < 0 && cr.hovered) {
        if (pressed) g->dragging = (mods & CUI_MOD_SHIFT) ? 2 : ((mods & CUI_MOD_CTRL) ? 3 : 1);
        else if (cui_mouse_down(ui, CUI_MOUSE_MIDDLE)) g->dragging = 2;
        else if (cui_mouse_down(ui, CUI_MOUSE_RIGHT)) g->dragging = 3;
        if (g->dragging) g->drag_last = m;
    }
    if (g->dragging) {
        int held = left || cui_mouse_down(ui, CUI_MOUSE_MIDDLE) || cui_mouse_down(ui, CUI_MOUSE_RIGHT);
        float dx = m.x - g->drag_last.x, dy = m.y - g->drag_last.y;
        if (!held) g->dragging = 0;
        else if (!g->click_pending && (dx != 0 || dy != 0)) {
            if (g->dragging == 1) {
                g->yaw -= dx * 0.008f;
                g->pitch = nclampf(g->pitch + dy * 0.008f, -1.5f, 1.5f);
            } else if (g->dragging == 2) {
                v3 f = v3_norm(v3_sub(g->scene->cam.target, g->scene->cam.pos));
                v3 rt = v3_norm(v3_cross(f, v3_make(0, 1, 0)));
                v3 up = v3_cross(rt, f);
                float k = g->dist * 0.0015f;
                g->target = v3_add(g->target, v3_add(v3_scale(rt, -dx * k), v3_scale(up, dy * k)));
            } else {
                g->dist *= expf(dy * 0.01f);
            }
            g->drag_last = m;
            camera_changed(g);
        }
    }
    g->prev_left = left;
    if (cr.hovered && !cui_wants_keyboard(ui)) {
        int no_mods = !(mods & (CUI_MOD_CTRL | CUI_MOD_ALT | CUI_MOD_SUPER));
        if (cui_key_pressed(ui, CUI_KEY_EQUAL)) { g->dist *= 0.9f; camera_changed(g); }
        if (cui_key_pressed(ui, CUI_KEY_MINUS)) { g->dist /= 0.9f; camera_changed(g); }
        if (cui_key_pressed(ui, CUI_KEY_LEFT)) { g->yaw += 0.08f; camera_changed(g); }
        if (cui_key_pressed(ui, CUI_KEY_RIGHT)) { g->yaw -= 0.08f; camera_changed(g); }
        if (cui_key_pressed(ui, CUI_KEY_UP)) { g->pitch = nclampf(g->pitch + 0.08f, -1.5f, 1.5f); camera_changed(g); }
        if (cui_key_pressed(ui, CUI_KEY_DOWN)) { g->pitch = nclampf(g->pitch - 0.08f, -1.5f, 1.5f); camera_changed(g); }
        if (no_mods && cui_key_pressed(ui, CUI_KEY_F)) { nora_scene_frame_camera(g->scene); cam_from_scene(g); camera_changed(g); }
        if (no_mods && cui_key_pressed(ui, CUI_KEY_TAB)) set_view_mode(g, edit ? VIEW_RENDER : VIEW_EDIT);
        if (no_mods && !(mods & CUI_MOD_SHIFT)) {
            if (cui_key_pressed(ui, CUI_KEY_W)) g->gizmo_mode = GIZMO_MOVE;
            if (cui_key_pressed(ui, CUI_KEY_E)) g->gizmo_mode = GIZMO_ROTATE;
            if (cui_key_pressed(ui, CUI_KEY_R)) g->gizmo_mode = GIZMO_SCALE;
            if (cui_key_pressed(ui, CUI_KEY_DELETE) || cui_key_pressed(ui, CUI_KEY_X)) do_delete(g);
            if (cui_key_pressed(ui, CUI_KEY_ESCAPE)) select_mesh(g, -1);
        }
        if (cui_shortcut(ui, CUI_MOD_SHIFT, CUI_KEY_D) || cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_D)) do_duplicate(g);
    }
}

static void outliner_panel(nora_gui *g, cui_ctx *ui) {
    int i;
    cui_scroll_begin(ui, &(cui_scroll_opts){ ._ = 0, .id = "outliner_scroll", .width = CUI_FILL, .height = CUI_FILL, .pad = 8, .gap = 2 });
    cui_text(ui, g->scene->name, .font = CUI_FONT_MEDIUM);
    cui_muted(ui, "Objects");
    for (i = 0; i < g->scene->nmeshes; i++) {
        const nora_mesh *me = &g->scene->meshes[i];
        char tr[32];
        if (me->hidden) continue;
        snprintf(tr, sizeof tr, "%d tris", me->tri_count);
        cui_push_id_int(ui, i);
        if (cui_list_item(ui, me->name, .icon = CUI_ICON_BOX, .trailing = tr, .selected = g->sel_mesh == i)) select_mesh(g, i);
        cui_pop_id(ui);
    }
    cui_gap(ui, 6);
    cui_muted(ui, "Materials");
    for (i = 0; i < g->scene->nmats; i++) {
        const nora_material *m = &g->scene->mats[i];
        cui_push_id_int(ui, 10000 + i);
        if (cui_list_item(ui, m->name, .icon = m->shader >= 0 ? CUI_ICON_CODE : CUI_ICON_PALETTE,
                          .trailing = m->shader >= 0 ? "OSL" : nora_mat_type_names[m->type], .selected = g->sel_mat == i))
            select_material(g, i);
        cui_pop_id(ui);
    }
    cui_scroll_end(ui);
}

static cui_color to_ui_color(v3 c) {
    cui_color r;
    r.r = (uint8_t)(nclampf(nora_linear_to_srgb(c.x), 0, 1) * 255.0f + 0.5f);
    r.g = (uint8_t)(nclampf(nora_linear_to_srgb(c.y), 0, 1) * 255.0f + 0.5f);
    r.b = (uint8_t)(nclampf(nora_linear_to_srgb(c.z), 0, 1) * 255.0f + 0.5f);
    r.a = 255;
    return r;
}
static v3 from_ui_color(cui_color c) {
    return v3_make(nora_srgb_to_linear(c.r / 255.0f), nora_srgb_to_linear(c.g / 255.0f), nora_srgb_to_linear(c.b / 255.0f));
}

static int color_edit(cui_ctx *ui, const char *id, v3 *c) {
    cui_color cc = to_ui_color(*c);
    if (cui_color_picker(ui, id, &cc)) { *c = from_ui_color(cc); return 1; }
    return 0;
}


static void transform_fields(nora_gui *g, cui_ctx *ui) {
    nora_mesh *me = &g->scene->meshes[g->sel_mesh];
    static const char *const ax[3] = { "X", "Y", "Z" };
    int k, changed = 0;
    char id[16];
    if (g->field_mesh != g->sel_mesh) {
        g->f_t[0] = me->translate.x; g->f_t[1] = me->translate.y; g->f_t[2] = me->translate.z;
        g->f_r[0] = me->euler_deg.x; g->f_r[1] = me->euler_deg.y; g->f_r[2] = me->euler_deg.z;
        g->f_s[0] = me->scale.x; g->f_s[1] = me->scale.y; g->f_s[2] = me->scale.z;
        g->field_mesh = g->sel_mesh;
    }
    /* one row per axis: the number fields need the width for their value and - / + buttons */
    cui_label(ui, "Location");
    for (k = 0; k < 3; k++) cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER, .width = CUI_FILL) {
        snprintf(id, sizeof id, "t%d", k);
        cui_text(ui, ax[k], .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
        changed |= cui_number(ui, id, &g->f_t[k], -1e6, 1e6, .step = 1, .decimals = 3, .width = CUI_FILL);
    }
    cui_label(ui, "Rotation (degrees, XYZ order)");
    for (k = 0; k < 3; k++) cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER, .width = CUI_FILL) {
        snprintf(id, sizeof id, "r%d", k);
        cui_text(ui, ax[k], .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
        changed |= cui_number(ui, id, &g->f_r[k], -360, 360, .step = 5, .decimals = 2, .width = CUI_FILL);
    }
    cui_label(ui, "Scale");
    for (k = 0; k < 3; k++) cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER, .width = CUI_FILL) {
        snprintf(id, sizeof id, "s%d", k);
        cui_text(ui, ax[k], .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
        changed |= cui_number(ui, id, &g->f_s[k], -1e4, 1e4, .step = 0.1, .decimals = 3, .width = CUI_FILL);
    }
    if (changed) {
        nora_xform x;
        x.translate = v3_make((float)g->f_t[0], (float)g->f_t[1], (float)g->f_t[2]);
        x.euler_deg = v3_make((float)g->f_r[0], (float)g->f_r[1], (float)g->f_r[2]);
        x.rot = quat_from_euler_deg(x.euler_deg);
        x.scale = v3_make((float)g->f_s[0], (float)g->f_s[1], (float)g->f_s[2]);
        field_transform(g, &x);
        g->field_mesh = g->sel_mesh;   /* keep the typed values */
    }
    cui_row(ui, .gap = 6) {
        if (cui_button(ui, "Duplicate", .icon = CUI_ICON_COPY, .size = CUI_SIZE_SM)) do_duplicate(g);
        if (cui_button(ui, "Delete", .icon = CUI_ICON_TRASH_2, .size = CUI_SIZE_SM)) do_delete(g);
        if (cui_button(ui, "Reset", .icon = CUI_ICON_ROTATE_CCW, .size = CUI_SIZE_SM)) {
            nora_xform x;
            x.translate = v3_make(0, 0, 0); x.scale = v3_make(1, 1, 1); x.euler_deg = v3_make(0, 0, 0); x.rot = quat_identity();
            stop_render(g);
            nora_edit_transform(&g->hist, g->scene, g->sel_mesh, &x, "Reset transform");
            g->field_mesh = -2;
            scene_changed(g);
        }
    }
}

static void properties_panel(nora_gui *g, cui_ctx *ui) {
    nora_material *m;
    int changed = 0, type;
    cui_scroll_begin(ui, &(cui_scroll_opts){ ._ = 0, .id = "props_scroll", .width = CUI_FILL, .height = CUI_FILL, .pad = 12, .gap = 10 });
    if (g->sel_mesh >= 0 && g->sel_mesh < g->scene->nmeshes && !g->scene->meshes[g->sel_mesh].hidden) {
        cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER) {
            cui_icon(ui, CUI_ICON_BOX, 16, CUI_COLOR_ACCENT);
            cui_heading(ui, g->scene->meshes[g->sel_mesh].name, 4);
        }
        transform_fields(g, ui);
        cui_separator(ui);
    }
    if (g->sel_mat < 0 || g->sel_mat >= g->scene->nmats) {
        cui_muted(ui, "Select an object or a material in the outliner.");
        cui_scroll_end(ui);
        return;
    }
    m = &g->scene->mats[g->sel_mat];
    cui_heading(ui, m->name, 4);
    if (m->shader >= 0) {
        cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER) {
            cui_badge(ui, "OSL shader", .tone = CUI_TONE_ACCENT);
            cui_text(ui, g->scene->shaders[m->shader].name, .size = 12, .color = CUI_COLOR_TEXT_SECONDARY, .nowrap = true);
        }
        if (cui_button(ui, "Use built-in material", .size = CUI_SIZE_SM, .icon = CUI_ICON_X)) { m->shader = -1; g->graphs[g->sel_mat].used = 0; changed = 1; }
    }
    type = (int)m->type;
    if (cui_select(ui, "mat_type", nora_mat_type_names, NORA_MAT_TYPE_COUNT, &type, .label = "Type")) { m->type = (nora_mat_type)type; changed = 1; }
    cui_label(ui, m->type == NORA_MAT_CONDUCTOR ? "Reflectance" : (m->type == NORA_MAT_DIELECTRIC ? "Tint" : "Base color"));
    changed |= color_edit(ui, "mat_color", &m->color);
    if (m->type == NORA_MAT_CONDUCTOR) { cui_label(ui, "Edge tint"); changed |= color_edit(ui, "mat_edge", &m->edge_tint); }
    if (m->type != NORA_MAT_DIFFUSE) changed |= cui_slider(ui, "Roughness", &m->roughness, 0, 1, .show_value = true, .format = "%.2f");
    if (m->type == NORA_MAT_DIELECTRIC || m->type == NORA_MAT_PLASTIC) changed |= cui_slider(ui, "IOR", &m->ior, 1.0f, 2.5f, .show_value = true, .format = "%.3f");
    if (m->type == NORA_MAT_DIELECTRIC) {
        changed |= cui_slider(ui, "Abbe number (0 = no dispersion)", &m->abbe, 0, 90, .show_value = true, .format = "%.0f");
        cui_muted(ui, "Dispersion is visible in spectral mode.");
    }
    cui_separator(ui);
    cui_label(ui, "Emission");
    changed |= color_edit(ui, "mat_emit", &m->emission);
    changed |= cui_slider(ui, "Strength", &m->emission_strength, 0, 50, .show_value = true, .format = "%.1f");
    {
        bool ts = m->two_sided != 0;
        if (cui_switch(ui, "Two-sided emission", &ts)) { m->two_sided = ts; changed = 1; }
    }
    if (changed) scene_changed(g);
    cui_scroll_end(ui);
}

static void render_panel(nora_gui *g, cui_ctx *ui) {
    static const char *const modes[] = { "RGB", "Spectral" };
    static const char *const ints[] = { "MLT (PSSMLT)", "Path tracer" };
    static const char *const tms[] = { "Clamp", "Reinhard", "ACES" };
    nora_render_stats rs;
    int running = nora_gui_is_rendering(g);
    cui_scroll_begin(ui, &(cui_scroll_opts){ ._ = 0, .id = "render_scroll", .width = CUI_FILL, .height = CUI_FILL, .pad = 12, .gap = 10 });
    cui_row(ui, .gap = 8) {
        if (cui_button(ui, "Render", .variant = CUI_BUTTON_PRIMARY, .icon = CUI_ICON_PLAY, .shortcut = "F12")) nora_gui_command(g, "render");
        if (running && g->render_kind == 2) {
            if (cui_button(ui, "Pause", .icon = CUI_ICON_PAUSE)) pause_render(g);
        } else {
            if (cui_button(ui, "Resume", .icon = CUI_ICON_PLAY, .disabled = !g->render || g->render_kind != 2 || !nora_render_can_resume(g->render))) resume_render(g);
        }
        if (cui_button(ui, "Stop", .icon = CUI_ICON_SQUARE, .disabled = !running)) { if (g->render) nora_render_request_stop(g->render); g->cam_pending = 0; }
        if (cui_button(ui, "Save image", .icon = CUI_ICON_SAVE)) menu_save_image(g);
    }
    cui_input(ui, "save_path", g->save_path, sizeof g->save_path, .label = "Output file (.png, .exr, .pfm)");
    if (g->render) {
        nora_render_get_stats(g->render, &rs);
        cui_progress(ui, nora_render_progress(g->render), .show_value = true);
        if (rs.phase == 1) cui_muted(ui, "MLT bootstrap...");
        else if (nora_render_settings(g->render)->integrator == NORA_INT_MLT && g->render_kind == 2)
            cui_textf(ui, "%.1f s  -  %lld mutations  -  acceptance %.0f%%%s", rs.elapsed, (long long)rs.samples, 100.0 * rs.acceptance, rs.phase == 4 ? "  -  paused" : "");
        else cui_textf(ui, "%.1f s  -  %lld samples%s", rs.elapsed, (long long)rs.samples, rs.phase == 4 ? "  -  paused" : "");
    }
    cui_row(ui, .gap = 8) {
        if (cui_button(ui, "Save checkpoint", .icon = CUI_ICON_SAVE, .size = CUI_SIZE_SM, .disabled = !g->render || g->render_kind != 2)) menu_checkpoint(g, 0);
        if (cui_button(ui, "Resume from checkpoint", .icon = CUI_ICON_HISTORY, .size = CUI_SIZE_SM)) menu_checkpoint(g, 1);
    }
    cui_input(ui, "ckpt_path", g->ckpt_path, sizeof g->ckpt_path, .label = "Checkpoint file (.nrck)");
    cui_separator(ui);
    cui_label(ui, "Mode");
    cui_segmented(ui, "mode", modes, 2, &g->mode_idx, .full_width = true);
    cui_select(ui, "integrator", ints, 2, &g->integrator_idx, .label = "Integrator");
    cui_slider(ui, "Large step probability", &g->p_large, 0.01f, 1.0f, .show_value = true, .format = "%.2f");
    if (cui_number(ui, "spp", &g->spp, 1, 1e6, .label = g->integrator_idx ? "Samples per pixel" : "Mutations per pixel", .step = 16)) {
        if (g->render && g->render_kind == 2 && !nora_render_is_running(g->render)) nora_render_set_spp(g->render, (int)g->spp);
    }
    cui_number(ui, "time", &g->time_limit, 0, 1e5, .label = "Time limit (s, 0 = none)", .step = 10);
    cui_number(ui, "threads", &g->threads, 0, 256, .label = "Threads (0 = all cores)", .step = 1);
    cui_number(ui, "depth", &g->max_depth, 1, 64, .label = "Max path depth", .step = 1);
    cui_row(ui, .gap = 8) {
        cui_number(ui, "w", &g->width, 16, 8192, .label = "Width", .step = 64, .width = CUI_FILL);
        cui_number(ui, "h", &g->height, 16, 8192, .label = "Height", .step = 64, .width = CUI_FILL);
    }
    cui_switch(ui, "Switch to MLT when the camera stops", &g->auto_mlt);
    cui_separator(ui);
    if (cui_slider(ui, "Exposure (EV)", &g->exposure, -5, 5, .show_value = true, .format = "%.1f")) g->tex_dirty = 1;
    if (cui_select(ui, "tonemap", tms, 3, &g->tonemap_idx, .label = "Tone mapping")) g->tex_dirty = 1;
    cui_separator(ui);
    cui_label(ui, "Files");
    if (cui_switch(ui, "Hot reload (watch the scene's files)", &g->hot_reload) && g->hot_reload) watch_update(g);
    cui_textf(ui, "%d watched file%s, polled every 0.5 s", g->ndeps, g->ndeps == 1 ? "" : "s");
    cui_input(ui, "scene_path", g->scene_path, sizeof g->scene_path, .label = "Scene file (.nora / .obj)");
    cui_input(ui, "import_path", g->import_path, sizeof g->import_path, .label = "Import OBJ");
    cui_input(ui, "export_path", g->export_path, sizeof g->export_path, .label = "Export OBJ");
    cui_scroll_end(ui);
}

static void node_value_controls(nora_gui *g, cui_ctx *ui, ng_node *n, int slot, int *changed) {
    const ng_node_def *d = ng_def(n->type);
    const ng_pin_def *pd = &d->in[slot];
    char id[32];
    snprintf(id, sizeof id, "##v%d", slot);
    if (pd->type == NG_PIN_FLOAT) {
        float lo = pd->lo, hi = pd->hi;
        if (hi <= lo) hi = lo + 1;
        *changed |= cui_slider(ui, id, &n->vals[slot][0], lo, hi, .show_value = true, .format = "%.2f");
    } else if (pd->type == NG_PIN_COLOR && pd->default_expr == 0) {
        v3 c = v3_make(n->vals[slot][0], n->vals[slot][1], n->vals[slot][2]);
        if (color_edit(ui, id, &c)) { n->vals[slot][0] = c.x; n->vals[slot][1] = c.y; n->vals[slot][2] = c.z; *changed = 1; }
    }
    (void)g;
}

static void node_editor_panel(nora_gui *g, cui_ctx *ui) {
    static const char *names[64];
    ng_graph *gr;
    int i, changed = 0, k;
    if (g->sel_mat < 0 || g->sel_mat >= g->ngraphs) { cui_muted(ui, "Select a material."); return; }
    gr = &g->graphs[g->sel_mat].g;
    for (i = 0; i < ng_type_count() && i < 64; i++) names[i] = ng_def(i)->title;
    cui_row(ui, .gap = 8, .pad = 6, .align = CUI_ALIGN_CENTER) {
        cui_select(ui, "add_type", names, ng_type_count(), &g->add_type, .width = CUI_PX(170));
        if (cui_button(ui, "Add node", .icon = CUI_ICON_PLUS, .size = CUI_SIZE_SM)) {
            ng_add_node(gr, g->add_type, -g->view.x + 40 + 20 * (gr->nnodes % 5), -g->view.y + 40 + 20 * (gr->nnodes % 5));
            changed = 1;
        }
        if (cui_button(ui, "Reset", .icon = CUI_ICON_ROTATE_CCW, .size = CUI_SIZE_SM)) { ng_default_graph(gr); changed = 1; }
        cui_input(ui, "graph_path", g->graph_path, sizeof g->graph_path, .width = CUI_PX(160), .size = CUI_SIZE_SM);
        if (cui_button(ui, "Load", .icon = CUI_ICON_FOLDER_OPEN, .size = CUI_SIZE_SM)) {
            char path[512];
            if (g->host.file_dialog && g->host.file_dialog(g->host.user, 0, "Shader graphs", "json", path, sizeof path))
                snprintf(g->graph_path, sizeof g->graph_path, "%s", path);
            if (ng_load_file(gr, g->graph_path, g->err, sizeof g->err) == 0) { logf_(g, "loaded graph %s", g->graph_path); changed = 1; }
            else logf_(g, "graph: %s", g->err);
        }
        if (cui_button(ui, "Save", .icon = CUI_ICON_SAVE, .size = CUI_SIZE_SM)) {
            char path[512];
            if (g->host.file_dialog && g->host.file_dialog(g->host.user, 1, "Shader graphs", "json", path, sizeof path))
                snprintf(g->graph_path, sizeof g->graph_path, "%s", path);
            if (ng_save_file(gr, g->graph_path) == 0) logf_(g, "saved graph %s", g->graph_path);
            else logf_(g, "graph: cannot write %s", g->graph_path);
        }
        cui_spacer(ui);
        if (cui_button(ui, "Compile & assign", .variant = CUI_BUTTON_PRIMARY, .icon = CUI_ICON_ZAP, .size = CUI_SIZE_SM)) {
            regenerate_source(g);
            assign_source(g);
        }
    }
    if (cui_node_editor_begin(ui, "graph", &g->view, .height = CUI_FILL, .minimap = true)) {
        for (i = 0; i < gr->nnodes; i++) {
            ng_node *n = &gr->nodes[i];
            const ng_node_def *d = ng_def(n->type);
            cui_push_id_int(ui, n->id);
            cui_node_begin(ui, n->id, &n->x, &n->y, d->title, .width = 190);
            if (d->nops > 0) {
                cui_node_content_begin(ui);
                changed |= cui_select(ui, "op", d->ops, d->nops, &n->op);
                cui_node_content_end(ui);
            }
            if (d->has_text) {
                cui_node_content_begin(ui);
                if (cui_input(ui, "file", n->text, sizeof n->text, .placeholder = "image.png", .size = CUI_SIZE_SM) & CUI_CHANGED) changed = 1;
                cui_node_content_end(ui);
            }
            for (k = 0; k < d->nin; k++) {
                int linked = cui_node_input(ui, ng_pin_id(n->id, k, 0), d->in[k].name, .type = d->in[k].type);
                if (!linked && (d->in[k].type == NG_PIN_FLOAT || (d->in[k].type == NG_PIN_COLOR && d->in[k].default_expr == 0))) {
                    cui_node_content_begin(ui);
                    node_value_controls(g, ui, n, k, &changed);
                    cui_node_content_end(ui);
                }
            }
            for (k = 0; k < d->nout; k++) cui_node_output(ui, ng_pin_id(n->id, k, 1), d->out[k].name, .type = d->out[k].type);
            cui_node_end(ui);
            cui_pop_id(ui);
        }
        for (i = 0; i < gr->nlinks; i++) cui_node_link(ui, gr->links[i].id, gr->links[i].from_pin, gr->links[i].to_pin);
        {
            int from, to, id;
            while (cui_node_link_created(ui, &from, &to)) { if (ng_add_link(gr, from, to) >= 0) changed = 1; }
            while (cui_node_link_removed(ui, &id)) { ng_remove_link(gr, id); changed = 1; }
            while (cui_node_deleted(ui, &id)) { ng_remove_node(gr, id); changed = 1; }
        }
        cui_node_editor_end(ui);
    }
    if (changed) regenerate_source(g);
}

static void source_panel(nora_gui *g, cui_ctx *ui) {
    cui_code_editor_result r;
    cui_row(ui, .gap = 8, .pad = 6, .align = CUI_ALIGN_CENTER) {
        cui_badge(ui, g->src_from_graph ? "generated from graph" : "edited", .tone = g->src_from_graph ? CUI_TONE_INFO : CUI_TONE_WARNING);
        cui_spacer(ui);
        if (cui_button(ui, "Regenerate", .icon = CUI_ICON_REFRESH_CW, .size = CUI_SIZE_SM)) regenerate_source(g);
        if (cui_button(ui, "Assign to material", .variant = CUI_BUTTON_PRIMARY, .icon = CUI_ICON_CHECK, .size = CUI_SIZE_SM, .disabled = !g->compile_ok))
            assign_source(g);
    }
    r = cui_code_editor(ui, "osl_src", g->src, SRC_CAP, .lang = "c", .height = -1);
    if (r.changed) { g->src_from_graph = 0; g->src_needs_check = 1; g->src_edit_time = ntime_now(); }
    if (g->src_needs_check && ntime_now() - g->src_edit_time > 0.3) check_source(g);
    if (g->src_needs_check) cui_request_wakeup(ui, 0.35f);
    cui_text(ui, g->compile_msg, .size = 12, .color = g->compile_ok ? CUI_COLOR_SUCCESS : CUI_COLOR_DANGER);
}

static void log_panel(nora_gui *g, cui_ctx *ui) {
    int i;
    cui_scroll_begin(ui, &(cui_scroll_opts){ ._ = 0, .id = "log_scroll", .width = CUI_FILL, .height = CUI_FILL, .pad = 8, .gap = 2 });
    nmutex_lock(&g->log.mu);
    for (i = 0; i < g->log.count; i++)
        cui_text(ui, g->log.lines[(g->log.head + i) % LOG_LINES], .font = CUI_FONT_MONO, .size = 12);
    nmutex_unlock(&g->log.mu);
    cui_scroll_end(ui);
}

/* ---------------------------------------------------------------- frame */

static const char *const k_panels[7] = { "Viewport", "Node Editor", "Outliner", "Properties", "Render Settings", "OSL Source", "Log" };
static const char *const k_layout =
    "dock1 h0.19(v0.5([*Outliner],[*Properties]),h0.7(v0.6([*Viewport],[*Node Editor]),v0.48([*Render Settings],[*OSL Source,Log])))";

void nora_gui_frame(nora_gui *g, cui_ctx *ui) {
    int i;
    double now = ntime_now();
    g->frame++;
    if (g->frame == 1) cui_set_theme(ui, CUI_THEME_DARK);
    poll_files(g, 0);
    /* switch from the preview to progressive MLT once the camera is still */
    if (g->cam_pending && !g->dragging && now - g->last_cam_change > 0.5) {
        g->cam_pending = 0;
        if (g->auto_mlt && g->view_mode == VIEW_RENDER) start_render(g, 2);
    }
    shortcuts(g, ui);
    update_texture(g, ui, 0);
    if (nora_gui_is_rendering(g)) { cui_request_wakeup(ui, 0.25f); g->was_running = 1; }
    else if (g->was_running) { update_texture(g, ui, 1); g->was_running = 0; }   /* final state of a finished or paused render */
    if (g->hot_reload) cui_request_wakeup(ui, 0.5f);
    if (g->dragging || g->giz_drag >= 0 || g->click_pending) cui_request_wakeup(ui, 0.03f);
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL) {
        menu_bar(g, ui);
        cui_row(ui, .gap = 10, .pad_x = 12, .pad_y = 6, .align = CUI_ALIGN_CENTER, .width = CUI_FILL) {
            cui_icon(ui, CUI_ICON_SPARKLES, 18, CUI_COLOR_ACCENT);
            cui_text(ui, "Nora", .font = CUI_FONT_MEDIUM, .size = 15);
            cui_text(ui, g->scene_file[0] ? nora_path_base(g->scene_file) : g->scene->name, .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
            cui_spacer(ui);
            if (g->render) cui_progress(ui, nora_render_progress(g->render), .width = CUI_PX(160));
            if (cui_button(ui, "Render", .variant = CUI_BUTTON_PRIMARY, .icon = CUI_ICON_PLAY, .size = CUI_SIZE_SM)) nora_gui_command(g, "render");
            if (nora_gui_is_rendering(g) && g->render_kind == 2) {
                if (cui_button(ui, "Pause", .icon = CUI_ICON_PAUSE, .size = CUI_SIZE_SM)) pause_render(g);
            } else if (g->render && g->render_kind == 2 && nora_render_can_resume(g->render)) {
                if (cui_button(ui, "Resume", .icon = CUI_ICON_PLAY, .size = CUI_SIZE_SM)) resume_render(g);
            }
            if (cui_button(ui, "Stop", .icon = CUI_ICON_SQUARE, .size = CUI_SIZE_SM, .disabled = !nora_gui_is_rendering(g))) {
                if (g->render) nora_render_request_stop(g->render);
                g->cam_pending = 0;
            }
            if (cui_button(ui, "Save", .icon = CUI_ICON_SAVE, .size = CUI_SIZE_SM)) save_image(g, g->save_path);
        }
        cui_separator(ui);
        cui_dockspace_begin(ui, "nora_dock", .layout = k_layout);
        for (i = 0; i < 7; i++) {
            if (!cui_dock_panel_begin(ui, k_panels[i], &g->open[i])) continue;
            switch (i) {
            case 0: viewport_panel(g, ui); break;
            case 1: node_editor_panel(g, ui); break;
            case 2: outliner_panel(g, ui); break;
            case 3: properties_panel(g, ui); break;
            case 4: render_panel(g, ui); break;
            case 5: source_panel(g, ui); break;
            default: log_panel(g, ui); break;
            }
            cui_dock_panel_end(ui);
        }
        cui_dockspace_end(ui);
    }
}
