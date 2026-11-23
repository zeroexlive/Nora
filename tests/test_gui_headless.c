/* Nora - drives the GUI headless with CUI's software renderer: builds every
 * panel, runs a preview and an MLT render, exercises the shader graph and the
 * OSL source path, then the scene editing workflow (Edit mode, picking by mouse
 * click, gizmo drags, undo/redo, duplicate/delete, import, scene files, hot
 * reload, export, pause/resume and checkpoints), and writes screenshots.
 * MIT License. */
#include "gui.h"
#include "scene.h"
#include "fsutil.h"
#include <cui/cui_render.h>
#include "nthread.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cui_sw_renderer *sw;
static cui_ctx *ui;

#ifndef NORA_SOURCE_DIR
#define NORA_SOURCE_DIR "."
#endif

static void frames(nora_gui *g, int n, int sleep_ms) {
    int i;
    for (i = 0; i < n; i++) {
        cui_begin_frame(ui, 1.0f / 30.0f);
        nora_gui_frame(g, ui);
        cui_end_frame(ui);
        if (sleep_ms) nsleep_ms(sleep_ms);
    }
}

static void mouse_to(nora_gui *g, float x, float y) { cui_input_mouse_move(ui, x, y); frames(g, 1, 0); }
static void mouse_button(nora_gui *g, int down) { cui_input_mouse_button(ui, CUI_MOUSE_LEFT, down != 0); frames(g, 1, 0); }
static void click(nora_gui *g, float x, float y) { mouse_to(g, x, y); mouse_button(g, 1); mouse_button(g, 0); frames(g, 1, 0); }
static void key(nora_gui *g, cui_key k, int mods) {
    cui_input_key(ui, k, true, mods); frames(g, 1, 0);
    cui_input_key(ui, k, false, mods); frames(g, 1, 0);
}
static int mesh_named(nora_gui *g, const char *name) {
    nora_scene *s = nora_gui_scene(g);
    int i;
    for (i = 0; i < s->nmeshes; i++) if (!s->meshes[i].hidden && !strcmp(s->meshes[i].name, name)) return i;
    return -1;
}
static void screenshot(const char *path) {
    const cui_draw_data *dd = cui_get_draw_data(ui);
    unsigned char *px = (unsigned char *)malloc((size_t)dd->width_px * dd->height_px * 4);
    cui_sw_render(sw, dd, px, dd->width_px, dd->height_px, dd->width_px * 4, CUI_SW_RGBA);
    if (cui_write_png(path, px, dd->width_px, dd->height_px, dd->width_px * 4)) printf("screenshot: %s\n", path);
    free(px);
}

/* the scene editing workflow; returns the number of failures */
static int editing(nora_gui *g, const char *outdir) {
    int fails = 0, tall, n0, m;
    float x, y, vx, vy, vw, vh;
    nora_scene *s;
    char path[1024], cmd[1200], obj[1024];
    v3 t0;
#define EXPECT(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
    snprintf(obj, sizeof obj, "%s/scenes/shaderball.obj", NORA_SOURCE_DIR);
    EXPECT(nora_gui_command(g, "open cornell") == 0);
    EXPECT(nora_gui_command(g, "view edit") == 0);
    frames(g, 3, 0);
    EXPECT(nora_gui_view_mode(g) == 0);
    EXPECT(!nora_gui_is_rendering(g));                      /* no path tracing in Edit mode */
    EXPECT(nora_gui_viewport_rect(g, &vx, &vy, &vw, &vh) == 0 && vw > 100 && vh > 100);
    /* pick the tall block with a mouse click on its centre */
    s = nora_gui_scene(g);
    tall = mesh_named(g, "Tall block");
    EXPECT(tall >= 0);
    EXPECT(nora_gui_project(g, tall, &x, &y) == 0);
    click(g, x, y);
    EXPECT(nora_gui_selected_object(g) == tall);
    /* clicking the background deselects */
    click(g, vx + 3, vy + 3);
    EXPECT(nora_gui_selected_object(g) == -1);
    click(g, x, y);
    EXPECT(nora_gui_selected_object(g) == tall);
    frames(g, 2, 0);
    /* drag the X handle of the move gizmo */
    t0 = s->meshes[tall].translate;
    key(g, CUI_KEY_W, 0);
    EXPECT(nora_gui_gizmo_handle(g, 0, &x, &y) == 0);
    mouse_to(g, x, y);
    mouse_button(g, 1);
    mouse_to(g, x - 25, y);
    mouse_to(g, x - 50, y + 2);
    mouse_button(g, 0);
    frames(g, 2, 0);
    EXPECT(fabsf(s->meshes[tall].translate.x - t0.x) > 1.0f);
    EXPECT(fabsf(s->meshes[tall].translate.y - t0.y) < 1e-3f && fabsf(s->meshes[tall].translate.z - t0.z) < 1e-3f);
    EXPECT(nora_gui_selected_object(g) == tall);            /* a drag is not a click */
    /* Ctrl+Z undoes the drag, Ctrl+Y redoes it */
    key(g, CUI_KEY_Z, CUI_MOD_PRIMARY);
    EXPECT(fabsf(s->meshes[tall].translate.x - t0.x) < 1e-4f);
    key(g, CUI_KEY_Y, CUI_MOD_PRIMARY);
    EXPECT(fabsf(s->meshes[tall].translate.x - t0.x) > 1.0f);
    /* rotate gizmo (E) around Y */
    key(g, CUI_KEY_E, 0);
    frames(g, 1, 0);
    EXPECT(nora_gui_gizmo_handle(g, 1, &x, &y) == 0);
    mouse_to(g, x, y);
    mouse_button(g, 1);
    mouse_to(g, x + 20, y + 15);
    mouse_to(g, x + 40, y + 30);
    mouse_button(g, 0);
    EXPECT(fabsf(s->meshes[tall].euler_deg.y) > 1.0f);
    /* scale gizmo (R) along Y */
    key(g, CUI_KEY_R, 0);
    frames(g, 1, 0);
    EXPECT(nora_gui_gizmo_handle(g, 1, &x, &y) == 0);
    mouse_to(g, x, y);
    mouse_button(g, 1);
    mouse_to(g, x, y - 30);
    mouse_button(g, 0);
    EXPECT(fabsf(s->meshes[tall].scale.y - 1.0f) > 0.01f);
    /* numeric transforms, undo and redo by command */
    EXPECT(nora_gui_command(g, "move -40 0 -60") == 0);
    EXPECT(nora_gui_command(g, "rotate 0 30 0") == 0);
    EXPECT(nora_gui_command(g, "scale 1 1 1") == 0);
    EXPECT(fabsf(s->meshes[tall].euler_deg.y - 30.0f) < 1e-4f);
    EXPECT(nora_gui_command(g, "undo") == 0 && nora_gui_command(g, "undo") == 0);
    EXPECT(fabsf(s->meshes[tall].euler_deg.y - 30.0f) > 1e-3f);
    EXPECT(nora_gui_command(g, "redo") == 0 && nora_gui_command(g, "redo") == 0);
    EXPECT(fabsf(s->meshes[tall].euler_deg.y - 30.0f) < 1e-4f && fabsf(s->meshes[tall].scale.y - 1.0f) < 1e-6f);
    /* duplicate (Shift+D), delete (Del), undo */
    n0 = nora_scene_live_mesh_count(s);
    key(g, CUI_KEY_D, CUI_MOD_SHIFT);
    EXPECT(nora_scene_live_mesh_count(s) == n0 + 1);
    m = nora_gui_selected_object(g);
    EXPECT(m >= 0 && !strcmp(s->meshes[m].name, "Tall block.001"));
    key(g, CUI_KEY_DELETE, 0);
    EXPECT(nora_scene_live_mesh_count(s) == n0 && nora_gui_selected_object(g) == -1);
    EXPECT(nora_gui_command(g, "undo") == 0 && nora_scene_live_mesh_count(s) == n0 + 1);
    EXPECT(nora_gui_command(g, "undo") == 0 && nora_scene_live_mesh_count(s) == n0);
    /* import an OBJ as new objects, undo it, redo it */
    EXPECT(nora_gui_command(g, "selectobj Tall block") == 0);
    snprintf(cmd, sizeof cmd, "import %s", obj);
    EXPECT(nora_gui_command(g, cmd) == 0);
    EXPECT(nora_scene_live_mesh_count(s) > n0);
    EXPECT(nora_gui_command(g, "undo") == 0 && nora_scene_live_mesh_count(s) == n0);
    EXPECT(nora_gui_command(g, "redo") == 0 && nora_scene_live_mesh_count(s) > n0);
    EXPECT(nora_gui_command(g, "undo") == 0);
    EXPECT(nora_gui_command(g, "wireframe on") == 0);
    EXPECT(nora_gui_command(g, "selectobj Tall block") == 0);
    EXPECT(nora_gui_command(g, "gizmo move") == 0);
    frames(g, 3, 0);
    snprintf(path, sizeof path, "%s/gui_headless_edit.png", outdir);
    screenshot(path);
    EXPECT(nora_gui_command(g, "wireframe off") == 0);
    /* scene file: save, edit it on disk, hot reload keeps the selection */
    snprintf(path, sizeof path, "%s/gui_headless_scene.nora", outdir);
    snprintf(cmd, sizeof cmd, "savescene %s", path);
    EXPECT(nora_gui_command(g, cmd) == 0);
    {
        char *text = nora_read_file(path, NULL);
        char *at = text ? strstr(text, "\"translation\": [-40, 0, -60]") : NULL;
        EXPECT(at != NULL);
        if (at) {
            memcpy(at, "\"translation\": [-90, 0, -60]", strlen("\"translation\": [-90, 0, -60]"));
            nsleep_ms(20);
            EXPECT(nora_write_file_atomic(path, text, strlen(text)) == 0);
        }
        free(text);
    }
    EXPECT(nora_gui_command(g, "poll") == 0);
    s = nora_gui_scene(g);
    tall = mesh_named(g, "Tall block");
    EXPECT(tall >= 0 && fabsf(s->meshes[tall].translate.x + 90.0f) < 1e-4f);
    EXPECT(nora_gui_selected_object(g) == tall);
    /* hot reload off: changes are ignored */
    EXPECT(nora_gui_command(g, "hotreload off") == 0);
    {
        char *text = nora_read_file(path, NULL);
        char *at = text ? strstr(text, "[-90, 0, -60]") : NULL;
        if (at) { memcpy(at, "[-10, 0, -60]", 13); nsleep_ms(20); nora_write_file_atomic(path, text, strlen(text)); }
        free(text);
    }
    EXPECT(nora_gui_command(g, "poll") == 0);
    EXPECT(fabsf(nora_gui_scene(g)->meshes[tall].translate.x + 90.0f) < 1e-4f);
    EXPECT(nora_gui_command(g, "hotreload on") == 0);
    /* open the .nora file, export OBJ */
    snprintf(cmd, sizeof cmd, "open %s", path);
    EXPECT(nora_gui_command(g, cmd) == 0);
    s = nora_gui_scene(g);
    tall = mesh_named(g, "Tall block");
    EXPECT(tall >= 0 && fabsf(s->meshes[tall].translate.x + 10.0f) < 1e-4f && fabsf(s->meshes[tall].euler_deg.y - 30.0f) < 1e-4f);
    snprintf(cmd, sizeof cmd, "exportobj %s/gui_headless_export.obj", outdir);
    EXPECT(nora_gui_command(g, cmd) == 0);
    /* back to Render mode: BVH rebuilt, render, pause, checkpoint, resume */
    EXPECT(nora_gui_command(g, "view render") == 0);
    EXPECT(nora_gui_view_mode(g) == 1 && s->built);
    nora_gui_command(g, "integrator mlt");
    nora_gui_command(g, "mode rgb");
    nora_gui_command(g, "size 96 96");
    nora_gui_command(g, "spp 400");
    EXPECT(nora_gui_command(g, "render") == 0);
    {
        double t0w = ntime_now();
        long long before = 0, after = 0, paused_at = 0;
        while (ntime_now() - t0w < 0.4) frames(g, 1, 20);
        EXPECT(nora_gui_command(g, "pause") == 0);
        EXPECT(nora_gui_is_paused(g) && !nora_gui_is_rendering(g));
        nora_gui_render_samples(g, &paused_at);
        frames(g, 3, 20);
        nora_gui_render_samples(g, &before);
        EXPECT(before == paused_at && before > 0);           /* nothing moves while paused */
        snprintf(cmd, sizeof cmd, "checkpoint %s/gui_headless.nrck", outdir);
        EXPECT(nora_gui_command(g, cmd) == 0);
        EXPECT(nora_gui_command(g, "resume") == 0);
        frames(g, 3, 30);
        nora_gui_render_samples(g, &after);
        EXPECT(after > before);                              /* kept accumulating */
        EXPECT(nora_gui_command(g, "stop") == 0);
        frames(g, 3, 10);
        snprintf(cmd, sizeof cmd, "loadcheckpoint %s/gui_headless.nrck", outdir);
        EXPECT(nora_gui_command(g, cmd) == 0);
        nora_gui_render_samples(g, &after);
        EXPECT(after >= before);
        t0w = ntime_now();
        while (nora_gui_is_rendering(g) && ntime_now() - t0w < 60.0) frames(g, 1, 30);
        EXPECT(nora_gui_progress(g) > 0.99f);
    }
    /* a checkpoint of another scene is refused */
    EXPECT(nora_gui_command(g, "open cornell-glass") == 0);
    snprintf(cmd, sizeof cmd, "loadcheckpoint %s/gui_headless.nrck", outdir);
    EXPECT(nora_gui_command(g, cmd) != 0);
#undef EXPECT
    return fails;
}

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : "gui_headless.png";
    nora_gui *g;
    const cui_draw_data *dd;
    unsigned char *px;
    int fails = 0;
    double t0;
    sw = cui_sw_create();
    ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(sw), .theme = CUI_THEME_DARK });
    cui_set_viewport(ui, 1600, 960, 1);
    g = nora_gui_create("cornell-glass", NULL);
    if (!g) { printf("FAIL: gui create\n"); return 1; }
    nora_gui_command(g, "size 160 160");
    nora_gui_command(g, "spp 16");
    frames(g, 20, 10);
    /* a camera move restarts the preview, then MLT starts once the camera is still */
    if (nora_gui_command(g, "orbit 0.1 0.05") != 0) { printf("FAIL: orbit\n"); fails++; }
    frames(g, 5, 10);
    nsleep_ms(600);
    frames(g, 5, 10);
    /* shader graph: default graph on the White material, compile and assign */
    if (nora_gui_command(g, "select White") != 0) { printf("FAIL: select\n"); fails++; }
    if (nora_gui_command(g, "graph default") != 0) { printf("FAIL: graph\n"); fails++; }
    frames(g, 3, 0);
    if (nora_gui_command(g, "assign") != 0) { printf("FAIL: assign (%s)\n", nora_gui_last_error(g)); fails++; }
    /* final spectral MLT render */
    nora_gui_command(g, "mode spectral");
    nora_gui_command(g, "integrator mlt");
    if (nora_gui_command(g, "render") != 0) { printf("FAIL: render\n"); fails++; }
    t0 = ntime_now();
    while (nora_gui_is_rendering(g) && ntime_now() - t0 < 60.0) frames(g, 1, 50);
    frames(g, 4, 0);
    if (nora_gui_progress(g) < 0.99f) { printf("FAIL: render did not finish (%.2f)\n", nora_gui_progress(g)); fails++; }
    if (nora_gui_command(g, "save gui_headless_render.png") != 0) { printf("FAIL: save\n"); fails++; }
    nora_gui_command(g, "select Glass");
    frames(g, 6, 0);
    {
        char dir[1024];
        nora_path_dir(out, dir, sizeof dir);
        fails += editing(g, dir);
        nora_gui_command(g, "view render");
        nora_gui_command(g, "select Glass");
        frames(g, 6, 0);
    }
    dd = cui_get_draw_data(ui);
    px = (unsigned char *)malloc((size_t)dd->width_px * dd->height_px * 4);
    cui_sw_render(sw, dd, px, dd->width_px, dd->height_px, dd->width_px * 4, CUI_SW_RGBA);
    if (!cui_write_png(out, px, dd->width_px, dd->height_px, dd->width_px * 4)) { printf("FAIL: screenshot\n"); fails++; }
    else printf("screenshot: %s (%dx%d)\n", out, dd->width_px, dd->height_px);
    free(px);
    nora_gui_destroy(g);
    cui_destroy(ui);
    cui_sw_destroy(sw);
    printf("%s\n", fails ? "FAILED" : "OK");
    return fails ? 1 : 0;
}
