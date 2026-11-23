/* Nora - interactive GUI. MIT License.
 *
 * Docked layout (CUI dockspace): Viewport, Node Editor, Outliner, Properties,
 * Render Settings, OSL Source and Log. The viewport shows the progressive
 * render as a texture drawn through a CUI canvas. While the camera moves a
 * fast low-resolution path traced preview runs; once the camera has been still
 * for a moment Nora switches to progressive MLT at full resolution.
 *
 * Camera: left drag orbits, Shift+left or middle drag pans, right drag (or
 * Ctrl+left) dollies; + and - zoom, F frames the scene. CUI 1.4 does not expose
 * the mouse wheel to custom canvases, so zoom uses drags, keys and a slider.
 */
#include "gui.h"
#include "render.h"
#include "image.h"
#include "scene.h"
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

typedef struct gui_log {
    nmutex mu;
    char lines[LOG_LINES][200];
    int head, count;
} gui_log;

typedef struct mat_graph { ng_graph g; int used; } mat_graph;

struct nora_gui {
    nora_gui_host host;
    nora_scene *scene;
    char scene_arg[512];

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
    char status[256];

    /* camera */
    float yaw, pitch, dist;
    v3 target;
    double last_cam_change;
    int cam_pending;            /* camera moved: start MLT once it is still */
    int dragging;               /* 0 none, 1 orbit, 2 pan, 3 dolly */
    cui_vec2 drag_last;

    /* selection */
    int sel_mat, sel_mesh;

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

    char save_path[512];
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

/* --------------------------------------------------------------- scene */

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

static void ensure_graphs(nora_gui *g) {
    int i;
    if (g->ngraphs >= g->scene->nmats) return;
    g->graphs = (mat_graph *)realloc(g->graphs, sizeof(mat_graph) * (size_t)g->scene->nmats);
    for (i = g->ngraphs; i < g->scene->nmats; i++) { ng_init(&g->graphs[i].g); ng_default_graph(&g->graphs[i].g); g->graphs[i].used = 0; }
    g->ngraphs = g->scene->nmats;
}

static int load_scene(nora_gui *g, const char *arg) {
    nora_scene *s;
    char err[512];
    if (!arg || !strcmp(arg, "cornell-glass")) s = nora_scene_cornell(1);
    else if (!strcmp(arg, "cornell")) s = nora_scene_cornell(0);
    else {
        s = nora_scene_load_obj(arg, err, sizeof err);
        if (!s) { logf_(g, "error: %s", err); return -1; }
    }
    if (nora_scene_build(s) != 0) { nora_scene_destroy(s); return -1; }
    if (g->scene) nora_scene_destroy(g->scene);
    g->scene = s;
    snprintf(g->scene_arg, sizeof g->scene_arg, "%s", arg ? arg : "cornell-glass");
    cam_from_scene(g);
    g->sel_mat = s->nmats > 0 ? 0 : -1;
    g->sel_mesh = -1;
    ensure_graphs(g);
    logf_(g, "scene: %s, %d triangles, %d materials, %d emissive triangles", s->name, s->ntris, s->nmats, s->nlights);
    return 0;
}

/* ------------------------------------------------------------- rendering */

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

static void start_render(nora_gui *g, int kind) {
    nora_settings st;
    stop_render(g);
    if (!g->scene->built && nora_scene_build(g->scene) != 0) { logf_(g, "error: scene build failed"); return; }
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
    g->render_w = st.width;
    g->render_h = st.height;
    free(g->rgb); free(g->rgba);
    g->rgb = (float *)calloc((size_t)st.width * st.height * 3, sizeof(float));
    g->rgba = (unsigned char *)calloc((size_t)st.width * st.height * 4, 1);
    nora_render_start(g->render);
    g->tex_dirty = 1;
    if (kind == 2)
        logf_(g, "render: %s, %s, %dx%d, %d %s%s", nora_integrator_name(st.integrator), nora_mode_name(st.mode), st.width, st.height, st.spp,
              st.integrator == NORA_INT_MLT ? "mutations/px" : "spp", st.time_limit > 0 ? " (time limited)" : "");
}

static void scene_changed(nora_gui *g) {
    stop_render(g);
    nora_scene_invalidate(g->scene);
    nora_scene_build(g->scene);
    start_render(g, 1);
    g->cam_pending = 1;
    g->last_cam_change = ntime_now();
}

static void camera_changed(nora_gui *g) {
    stop_render(g);
    cam_to_scene(g);
    start_render(g, 1);
    g->cam_pending = 1;
    g->last_cam_change = ntime_now();
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
        start_render(g, 1);
        return -1;
    }
    m->shader = slot;
    g->graphs[g->sel_mat].used = 1;
    logf_(g, "assigned OSL shader to material '%s'", m->name);
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
    g->add_type = ng_type_by_id("diffuse");
    snprintf(g->save_path, sizeof g->save_path, "nora_render.png");
    snprintf(g->graph_path, sizeof g->graph_path, "graph.json");
    for (i = 0; i < 7; i++) g->open[i] = true;
    logf_(g, "Nora 0.1.0 - Metropolis light transport, RGB and spectral");
    if (load_scene(g, scene) != 0 && load_scene(g, "cornell-glass") != 0) { nora_gui_destroy(g); return NULL; }
    select_material(g, 0);
    start_render(g, 1);
    g->cam_pending = 1;
    g->last_cam_change = ntime_now();
    return g;
}

void nora_gui_destroy(nora_gui *g) {
    int i;
    if (!g) return;
    stop_render(g);
    nora_set_log(NULL, NULL);
    if (g_log_target == g) g_log_target = NULL;
    for (i = 0; i < g->ngraphs; i++) ng_free(&g->graphs[i].g);
    free(g->graphs);
    if (g->scene) nora_scene_destroy(g->scene);
    free(g->rgb); free(g->rgba); free(g->src);
    nmutex_destroy(&g->log.mu);
    free(g);
}

int nora_gui_is_rendering(nora_gui *g) { return g && g->render && nora_render_is_running(g->render); }
float nora_gui_progress(nora_gui *g) { return g && g->render ? nora_render_progress(g->render) : 0.0f; }
const char *nora_gui_last_error(nora_gui *g) { return g ? g->err : ""; }

int nora_gui_command(nora_gui *g, const char *cmd) {
    char a[512];
    int x, y;
    float fx, fy;
    if (!g || !cmd) return -1;
    if (!strcmp(cmd, "render")) { g->cam_pending = 0; start_render(g, 2); return g->render ? 0 : -1; }
    if (!strcmp(cmd, "stop")) { if (g->render) nora_render_request_stop(g->render); return 0; }
    if (!strcmp(cmd, "preview")) { start_render(g, 1); return 0; }
    if (!strcmp(cmd, "assign")) return assign_source(g);
    if (sscanf(cmd, "mode %511s", a) == 1) { g->mode_idx = !strcmp(a, "spectral"); return 0; }
    if (sscanf(cmd, "integrator %511s", a) == 1) { g->integrator_idx = !strcmp(a, "pt"); return 0; }
    if (sscanf(cmd, "spp %d", &x) == 1) { g->spp = x; return 0; }
    if (sscanf(cmd, "size %d %d", &x, &y) == 2) { g->width = x; g->height = y; return 0; }
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
    if (sscanf(cmd, "save %511[^\n]", a) == 1) return save_image(g, a);
    if (sscanf(cmd, "orbit %f %f", &fx, &fy) == 2) { g->yaw += fx; g->pitch = nclampf(g->pitch + fy, -1.5f, 1.5f); camera_changed(g); return 0; }
    return -1;
}

/* ------------------------------------------------------------- panels */

typedef struct vp_paint { cui_texture tex; int w, h; char overlay[160]; } vp_paint;

static void paint_viewport(cui_ctx *ui, cui_rect r, void *user) {
    vp_paint *p = (vp_paint *)user;
    cui_draw_rect(ui, r, (cui_corners){ 0, 0, 0, 0 }, CUI_RGB(0x16181c));
    if (p->tex && p->w > 0 && p->h > 0) {
        float s = fminf(r.w / (float)p->w, r.h / (float)p->h);
        cui_rect img = { r.x + (r.w - p->w * s) * 0.5f, r.y + (r.h - p->h * s) * 0.5f, p->w * s, p->h * s };
        cui_draw_image(ui, p->tex, img, (cui_rect){ 0, 0, 1, 1 }, (cui_corners){ 0, 0, 0, 0 }, CUI_RGB(0xffffff));
    }
    if (p->overlay[0])
        cui_draw_text(ui, CUI_FONT_MONO, 12, (cui_vec2){ r.x + 10, r.y + 8 }, CUI_RGBA(0xffffffd0), p->overlay, -1);
}

static void viewport_panel(nora_gui *g, cui_ctx *ui) {
    vp_paint vp;
    cui_canvas_result cr;
    cui_vec2 m = cui_mouse_pos(ui);
    int mods = cui_key_mods(ui);
    nora_render_stats rs;
    memset(&vp, 0, sizeof vp);
    vp.tex = g->tex; vp.w = g->tex_w; vp.h = g->tex_h;
    if (g->render) {
        nora_render_get_stats(g->render, &rs);
        snprintf(vp.overlay, sizeof vp.overlay, "%s  %s  %dx%d  %.0f%%  %.1fs%s", g->render_kind == 1 ? "preview (PT)" : nora_integrator_name(nora_render_settings(g->render)->integrator),
                 nora_mode_name(nora_render_settings(g->render)->mode), g->render_w, g->render_h, 100.0 * nora_render_progress(g->render), rs.elapsed,
                 rs.phase == 1 ? "  bootstrapping" : "");
    }
    cr = cui_canvas(ui, "viewport", CUI_FILL, CUI_FILL, paint_viewport, &vp, sizeof vp);
    /* camera controls */
    if (!g->dragging && cr.hovered) {
        if (cui_mouse_down(ui, CUI_MOUSE_LEFT)) g->dragging = (mods & CUI_MOD_SHIFT) ? 2 : ((mods & CUI_MOD_CTRL) ? 3 : 1);
        else if (cui_mouse_down(ui, CUI_MOUSE_MIDDLE)) g->dragging = 2;
        else if (cui_mouse_down(ui, CUI_MOUSE_RIGHT)) g->dragging = 3;
        if (g->dragging) g->drag_last = m;
    }
    if (g->dragging) {
        int held = cui_mouse_down(ui, CUI_MOUSE_LEFT) || cui_mouse_down(ui, CUI_MOUSE_MIDDLE) || cui_mouse_down(ui, CUI_MOUSE_RIGHT);
        float dx = m.x - g->drag_last.x, dy = m.y - g->drag_last.y;
        if (!held) g->dragging = 0;
        else if (dx != 0 || dy != 0) {
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
    if (cr.hovered && !cui_wants_keyboard(ui)) {
        if (cui_key_pressed(ui, CUI_KEY_EQUAL)) { g->dist *= 0.9f; camera_changed(g); }
        if (cui_key_pressed(ui, CUI_KEY_MINUS)) { g->dist /= 0.9f; camera_changed(g); }
        if (cui_key_pressed(ui, CUI_KEY_LEFT)) { g->yaw += 0.08f; camera_changed(g); }
        if (cui_key_pressed(ui, CUI_KEY_RIGHT)) { g->yaw -= 0.08f; camera_changed(g); }
        if (cui_key_pressed(ui, CUI_KEY_UP)) { g->pitch = nclampf(g->pitch + 0.08f, -1.5f, 1.5f); camera_changed(g); }
        if (cui_key_pressed(ui, CUI_KEY_DOWN)) { g->pitch = nclampf(g->pitch - 0.08f, -1.5f, 1.5f); camera_changed(g); }
        if (cui_key_pressed(ui, CUI_KEY_F)) { nora_scene_frame_camera(g->scene); cam_from_scene(g); camera_changed(g); }
    }
}

static void outliner_panel(nora_gui *g, cui_ctx *ui) {
    int i;
    cui_scroll_begin(ui, &(cui_scroll_opts){ ._ = 0, .id = "outliner_scroll", .width = CUI_FILL, .height = CUI_FILL, .pad = 8, .gap = 2 });
    cui_text(ui, g->scene->name, .font = CUI_FONT_MEDIUM);
    cui_muted(ui, "Objects");
    for (i = 0; i < g->scene->nmeshes; i++) {
        const nora_mesh *me = &g->scene->meshes[i];
        int mat = me->tri_count > 0 ? g->scene->tris[me->first_tri].mat : -1;
        char tr[32];
        snprintf(tr, sizeof tr, "%d tris", me->tri_count);
        cui_push_id_int(ui, i);
        if (cui_list_item(ui, me->name, .icon = CUI_ICON_BOX, .trailing = tr, .selected = g->sel_mesh == i)) {
            g->sel_mesh = i;
            if (mat >= 0) select_material(g, mat);
        }
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

static void properties_panel(nora_gui *g, cui_ctx *ui) {
    nora_material *m;
    int changed = 0, type;
    cui_scroll_begin(ui, &(cui_scroll_opts){ ._ = 0, .id = "props_scroll", .width = CUI_FILL, .height = CUI_FILL, .pad = 12, .gap = 10 });
    if (g->sel_mat < 0 || g->sel_mat >= g->scene->nmats) {
        cui_muted(ui, "Select a material in the outliner.");
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
    cui_scroll_begin(ui, &(cui_scroll_opts){ ._ = 0, .id = "render_scroll", .width = CUI_FILL, .height = CUI_FILL, .pad = 12, .gap = 10 });
    cui_row(ui, .gap = 8) {
        if (cui_button(ui, "Render", .variant = CUI_BUTTON_PRIMARY, .icon = CUI_ICON_PLAY, .shortcut = "F12")) { g->cam_pending = 0; start_render(g, 2); }
        if (cui_button(ui, "Stop", .icon = CUI_ICON_SQUARE, .disabled = !nora_gui_is_rendering(g))) { if (g->render) nora_render_request_stop(g->render); g->cam_pending = 0; }
        if (cui_button(ui, "Save image", .icon = CUI_ICON_SAVE)) {
            char path[512];
            if (g->host.file_dialog && g->host.file_dialog(g->host.user, 1, "Images", "png;exr;pfm", path, sizeof path))
                snprintf(g->save_path, sizeof g->save_path, "%s", path);
            save_image(g, g->save_path);
        }
    }
    cui_input(ui, "save_path", g->save_path, sizeof g->save_path, .label = "Output file (.png, .exr, .pfm)");
    if (g->render) {
        nora_render_get_stats(g->render, &rs);
        cui_progress(ui, nora_render_progress(g->render), .show_value = true);
        if (rs.phase == 1) cui_muted(ui, "MLT bootstrap...");
        else if (nora_render_settings(g->render)->integrator == NORA_INT_MLT && g->render_kind == 2)
            cui_textf(ui, "%.1f s  -  %lld mutations  -  acceptance %.0f%%", rs.elapsed, (long long)rs.samples, 100.0 * rs.acceptance);
        else cui_textf(ui, "%.1f s  -  %lld samples", rs.elapsed, (long long)rs.samples);
    }
    cui_separator(ui);
    cui_label(ui, "Mode");
    cui_segmented(ui, "mode", modes, 2, &g->mode_idx, .full_width = true);
    cui_select(ui, "integrator", ints, 2, &g->integrator_idx, .label = "Integrator");
    cui_slider(ui, "Large step probability", &g->p_large, 0.01f, 1.0f, .show_value = true, .format = "%.2f");
    cui_number(ui, "spp", &g->spp, 1, 1e6, .label = g->integrator_idx ? "Samples per pixel" : "Mutations per pixel", .step = 16);
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
    /* switch from the preview to progressive MLT once the camera is still */
    if (g->cam_pending && !g->dragging && now - g->last_cam_change > 0.5) {
        g->cam_pending = 0;
        if (g->auto_mlt) start_render(g, 2);
    }
    if (!cui_wants_keyboard(ui) && cui_key_pressed(ui, CUI_KEY_F12)) { g->cam_pending = 0; start_render(g, 2); }
    update_texture(g, ui, 0);
    if (nora_gui_is_rendering(g)) { cui_request_wakeup(ui, 0.25f); g->was_running = 1; }
    else if (g->was_running) { update_texture(g, ui, 1); g->was_running = 0; }   /* final state of a finished render */
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL) {
        cui_row(ui, .gap = 10, .pad_x = 12, .pad_y = 6, .align = CUI_ALIGN_CENTER, .width = CUI_FILL) {
            cui_icon(ui, CUI_ICON_SPARKLES, 18, CUI_COLOR_ACCENT);
            cui_text(ui, "Nora", .font = CUI_FONT_MEDIUM, .size = 15);
            cui_text(ui, g->scene->name, .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
            cui_spacer(ui);
            if (g->render) cui_progress(ui, nora_render_progress(g->render), .width = CUI_PX(160));
            if (cui_button(ui, "Render", .variant = CUI_BUTTON_PRIMARY, .icon = CUI_ICON_PLAY, .size = CUI_SIZE_SM)) { g->cam_pending = 0; start_render(g, 2); }
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
