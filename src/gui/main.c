/* nora - interactive GUI.  usage: nora [cornell | cornell-glass | scene.obj | scene.nora]
 * MIT License. */
#include "gui.h"
#include <cui/cui_app.h>
#include <stdio.h>
#include <string.h>

static bool file_dialog(void *user, int save, const char *filter_name, const char *filter, char *out, size_t cap) {
    cui_file_dialog_opts o;
    (void)user;
    memset(&o, 0, sizeof o);
    o.kind = save ? CUI_FILE_SAVE : CUI_FILE_OPEN;
    o.filter_name = filter_name;
    o.filter = filter;
    o.default_name = NULL;
    if (save) {
        if (strstr(filter, "nora")) o.default_name = "scene.nora";
        else if (strstr(filter, "obj")) o.default_name = "export.obj";
        else if (strstr(filter, "nrck")) o.default_name = "render.nrck";
        else if (strstr(filter, "json")) o.default_name = "graph.json";
        else o.default_name = "nora_render.png";
    }
    return cui_app_file_dialog(NULL, &o, out, cap);
}

static void frame(cui_ctx *ui, void *user) { nora_gui_frame((nora_gui *)user, ui); }

int main(int argc, char **argv) {
    nora_gui_host host;
    nora_gui *g;
    cui_app_desc desc;
    int rc;
    memset(&host, 0, sizeof host);
    host.file_dialog = file_dialog;
    g = nora_gui_create(argc > 1 ? argv[1] : NULL, &host);
    if (!g) { fprintf(stderr, "nora: cannot start\n"); return 1; }
    memset(&desc, 0, sizeof desc);
    desc.title = "Nora";
    desc.width = 1600;
    desc.height = 960;
    desc.theme = CUI_THEME_DARK;
    desc.app_id = "io.github.zeroexlive.Nora";
    rc = cui_app_run(&desc, frame, g);
    nora_gui_destroy(g);
    return rc;
}
