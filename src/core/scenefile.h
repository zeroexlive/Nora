/* Nora - .nora scene files (JSON), scene opening, OBJ export and the list of
 * files a scene depends on (for hot reload). MIT License.
 *
 * A .nora file references OBJ files (paths relative to the .nora file) or the
 * built-in Cornell boxes, and stores per object the instance transform
 * (translation, Euler XYZ rotation in degrees, scale), the materials with their
 * OSL source or shader graph, the camera and the render settings. See README. */
#ifndef NORA_SCENEFILE_H
#define NORA_SCENEFILE_H

#include "scene.h"
#include "render.h"

#define NORA_SCENEFILE_VERSION 1

/* Serialises the scene. Paths are written relative to dir (the directory of the
 * .nora file). st may be NULL (no "render" block). Returns malloc'd text. */
char *nora_scenefile_write_string(nora_scene *s, const nora_settings *st, const char *dir);
int nora_scenefile_save(nora_scene *s, const nora_settings *st, const char *path, char *err, size_t errcap);

/* Parses a .nora document; relative paths are resolved against dir. If st is not
 * NULL it receives the render settings (defaults for missing keys) and *has_st
 * says whether the file had a "render" block. */
nora_scene *nora_scenefile_read_string(const char *text, const char *dir, nora_settings *st, int *has_st, char *err, size_t errcap);
nora_scene *nora_scenefile_load(const char *path, nora_settings *st, int *has_st, char *err, size_t errcap);

/* "cornell", "cornell-glass", a .obj or a .nora file. */
nora_scene *nora_scene_open(const char *arg, nora_settings *st, int *has_st, char *err, size_t errcap);
int nora_is_scene_file(const char *path);   /* ends in .nora */

/* Files the scene was built from: the .nora file itself, the OBJ files and their
 * MTL libraries, .osl files and shader graph files. Returns the count (<= cap). */
int nora_scene_dependencies(const nora_scene *s, char (*out)[1024], int cap);

#endif
