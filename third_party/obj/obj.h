/* obj - "Insanely fast obj importer written from scratch in ANSI C"
 * Upstream: https://github.com/schmarta/obj (MIT License, Copyright (c) 2024 Lucas Marta).
 * Vendored into Nora with local changes, see README.nora.md in this directory. */
#ifndef OBJ_H
#define OBJ_H

#include <stddef.h>
#include <stdint.h>

#define MAX_LEN 4096   /* Nora: was 256 */

typedef struct vec3 {
    float x, y, z;
} vec3_t;

typedef vec3_t vertex_t;

typedef struct texcoord {
    float u, v;
} texcoord_t;

typedef vec3_t normal_t;

/* One triangle corner: x = position index, y = texcoord index, z = normal index.
 * Indices are 1-based; 0 means "not given" (Nora: negative indices are resolved). */
typedef struct face {
    int x, y, z;
} face_t;

typedef struct complete_vertex {
    vertex_t vertex;
    texcoord_t texcoord;
    normal_t normal;
} complete_vertex_t;

typedef struct obj {
    char *name;

    size_t vertex_count;
    size_t texcoord_count;
    size_t normal_count;
    size_t face_count;          /* number of triangle corners (3 per triangle) */

    vertex_t *vertices;
    texcoord_t *texcoords;
    normal_t *normals;
    face_t *faces;

    size_t complete_vertex_count;
    complete_vertex_t *complete_vertices;

    size_t index_count;
    unsigned int *indices;

    /* Nora additions: materials and groups per triangle */
    int *face_materials;        /* per triangle, index into material_names or -1 */
    int *face_groups;           /* per triangle, index into group_names */
    char **material_names; size_t material_count;
    char **group_names; size_t group_count;
    char **mtllibs; size_t mtllib_count;
} obj_t;

void obj_print(obj_t *obj);

obj_t *obj_load(const char *file);
void obj_free(obj_t *obj);

#endif // OBJ_H
