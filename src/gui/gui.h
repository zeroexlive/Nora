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

/* scene: "cornell", "cornell-glass" or an OBJ path (NULL = cornell-glass). */
nora_gui *nora_gui_create(const char *scene, const nora_gui_host *host);
void nora_gui_frame(nora_gui *g, cui_ctx *ui);
void nora_gui_destroy(nora_gui *g);

/* Scripting hook used by tests and the command line of the GUI:
 * "render", "stop", "preview", "mode rgb|spectral", "integrator mlt|pt", "spp N",
 * "size W H", "select MATERIAL", "graph default|PATH", "assign", "save PATH", "orbit DX DY".
 * Returns 0 on success. */
int nora_gui_command(nora_gui *g, const char *cmd);
int nora_gui_is_rendering(nora_gui *g);
float nora_gui_progress(nora_gui *g);
const char *nora_gui_last_error(nora_gui *g);

#endif
