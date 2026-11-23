/* Nora - edit-mode viewport: a small software rasterizer (solid shading with a
 * z-buffer, optional wireframe, object ID buffer for picking). It never touches
 * the path tracer. MIT License. */
#ifndef NORA_RASTER_H
#define NORA_RASTER_H

#include "scene.h"

typedef struct nora_raster_opts {
    int wireframe;              /* draw triangle edges over the shading */
    int selected;               /* mesh to highlight, -1 = none */
} nora_raster_opts;

/* Renders the live meshes (with their current pos/nrm, so call
 * nora_scene_apply_transforms after edits). rgba: w*h*4. depth (w*h floats) and
 * ids (w*h ints, mesh index or -1) may be NULL. Returns the triangle count drawn. */
int nora_raster(const nora_scene *s, const nora_camera *cam, int w, int h, const nora_raster_opts *o,
                unsigned char *rgba, float *depth, int *ids);

/* Projects a world point with the same camera model as the renderer. Returns 1
 * when the point is in front of the camera; px, py in pixels (y down). */
int nora_project(const nora_camera *cam, int w, int h, v3 p, float *px, float *py, float *z);

/* The camera basis used by both (forward, right, up). */
void nora_camera_basis(const nora_camera *cam, v3 *f, v3 *r, v3 *u);

#endif
