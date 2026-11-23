/* Nora - interactive GUI on CUI (immediate mode). The GUI logic lives here and
 * only uses the CUI core, so it also runs headless with CUI's software renderer
 * (see tests/test_gui_headless.c); src/gui/main.c adds the window. MIT License. */
#ifndef NORA_GUI_H
#define NORA_GUI_H

#include <cui/cui.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct nora_gui nora_gui;

typedef struct nora_gui_host {
    /* Native file dialog; NULL to use the path fields only. save: 1 for a save dialog. */
    bool (*file_dialog)(void *user, int save, const char *filter_name, const char *filter, char *out, size_t cap);
    void *user;
} nora_gui_host;

struct nora_scene;

/* scene: "cornell", "cornell-glass", an OBJ or a .nora path (NULL = cornell-glass). */
nora_gui *nora_gui_create(const char *scene, const nora_gui_host *host);
void nora_gui_frame(nora_gui *g, cui_ctx *ui);
void nora_gui_destroy(nora_gui *g);

/* Scripting hook used by tests:
 *   render, stop, pause, resume, preview, mode rgb|spectral, integrator mlt|pt, spp N,
 *   size W H, select MATERIAL, graph default|PATH, assign, save IMAGE, orbit DX DY,
 *   view edit|render, wireframe on|off, gizmo move|rotate|scale, selectobj NAME,
 *   move X Y Z, rotate X Y Z (degrees), scale X Y Z, undo, redo, delete, duplicate,
 *   open PATH, import PATH, savescene [PATH], exportobj PATH, checkpoint PATH,
 *   loadcheckpoint PATH, hotreload on|off, poll.
 * Returns 0 on success. */
int nora_gui_command(nora_gui *g, const char *cmd);
int nora_gui_is_rendering(nora_gui *g);
int nora_gui_is_paused(nora_gui *g);
float nora_gui_progress(nora_gui *g);
int nora_gui_render_samples(nora_gui *g, long long *samples);
const char *nora_gui_last_error(nora_gui *g);
struct nora_scene *nora_gui_scene(nora_gui *g);
int nora_gui_selected_object(nora_gui *g);        /* mesh index or -1 */
int nora_gui_view_mode(nora_gui *g);              /* 0 edit, 1 render */
/* Screen geometry (UI pixels) of the last frame, for driving the GUI with mouse events. */
int nora_gui_viewport_rect(nora_gui *g, float *x, float *y, float *w, float *h);
int nora_gui_project(nora_gui *g, int mesh, float *x, float *y);   /* object centre (edit mode) */
int nora_gui_gizmo_handle(nora_gui *g, int axis, float *x, float *y);

#endif
