/* Nora - drives the GUI headless with CUI's software renderer: builds every
 * panel, runs a preview and an MLT render, exercises the shader graph and the
 * OSL source path, and writes a screenshot. MIT License. */
#include "gui.h"
#include <cui/cui_render.h>
#include "nthread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cui_sw_renderer *sw;
static cui_ctx *ui;

static void frames(nora_gui *g, int n, int sleep_ms) {
    int i;
    for (i = 0; i < n; i++) {
        cui_begin_frame(ui, 1.0f / 30.0f);
        nora_gui_frame(g, ui);
        cui_end_frame(ui);
        if (sleep_ms) nsleep_ms(sleep_ms);
    }
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
