/* obj - upstream https://github.com/schmarta/obj (MIT, Copyright (c) 2024 Lucas Marta).
 * Vendored into Nora with local changes (see README.nora.md):
 *  - portable headers (<stdlib.h> instead of <malloc.h>), longer lines
 *  - faces of any vertex count (fan triangulated) and every index form:
 *    v, v/vt, v//vn, v/vt/vn, with negative (relative) indices
 *  - usemtl / mtllib / o / g recorded per triangle
 *  - fixed: uninitialised name, index buffer overrun, leaks on error and in obj_free
 */
#include "obj.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void obj_print(obj_t *obj) {
    size_t i;
    printf("vertices {\n");
    for (i = 0; i < obj->vertex_count; i++) {
        printf("\tv %lu: x: %f, y: %f, z: %f\n", (unsigned long)i, obj->vertices[i].x, obj->vertices[i].y, obj->vertices[i].z);
    }
    printf("}\n");
}

static char *obj_strdup(const char *s) {
    size_t n = strlen(s);
    char *d = (char *)malloc(n + 1);
    if (d) memcpy(d, s, n + 1);
    return d;
}

static void trim(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
}

/* returns index in list, adding the name when new */
static int name_index(char ***list, size_t *count, const char *name) {
    size_t i;
    char **nl;
    for (i = 0; i < *count; i++) if (strcmp((*list)[i], name) == 0) return (int)i;
    nl = (char **)realloc(*list, sizeof(char *) * (*count + 1));
    if (!nl) return -1;
    *list = nl;
    nl[*count] = obj_strdup(name);
    return (int)(*count)++;
}

static int count_face_verts(const char *line) {
    int n = 0;
    const char *p = line + 1;
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n' || *p == '\r' || *p == '#') break;
        n++;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
    }
    return n;
}

static int resolve(long idx, size_t count) {
    if (idx > 0) return (int)idx;
    if (idx < 0) return (int)((long)count + idx + 1);
    return 0;
}

/* parse one corner token "v", "v/vt", "v//vn" or "v/vt/vn" */
static const char *parse_corner(const char *p, face_t *f, size_t nv, size_t nt, size_t nn) {
    char *end;
    long v = 0, t = 0, n = 0;
    v = strtol(p, &end, 10);
    p = end;
    if (*p == '/') {
        p++;
        if (*p != '/') { t = strtol(p, &end, 10); p = end; }
        if (*p == '/') { p++; n = strtol(p, &end, 10); p = end; }
    }
    f->x = resolve(v, nv);
    f->y = resolve(t, nt);
    f->z = resolve(n, nn);
    return p;
}

obj_t *obj_load(const char *file) {
    FILE *handle = fopen(file, "r");
    obj_t *obj;
    char *line;
    size_t vertex_index = 0, texcoord_index = 0, normal_index = 0, face_index = 0, tri_index = 0;
    int cur_mat = -1, cur_group = 0;

    if (handle == NULL) {
        return NULL;
    }

    obj = (obj_t *)calloc(1, sizeof(obj_t));
    line = (char *)malloc(MAX_LEN);
    if (!obj || !line) { free(obj); free(line); fclose(handle); return NULL; }

    // first pass, populate count's
    while (fgets(line, MAX_LEN, handle)) {
        if (line[0] == 'o' && (line[1] == ' ' || line[1] == '\t')) {
            char tmp[MAX_LEN];
            if (sscanf(line, "o %4095s", tmp) == 1 && obj->name == NULL) obj->name = obj_strdup(tmp);
        } else if (line[0] == 'v') {
            if (line[1] == ' ' || line[1] == '\t') {
                obj->vertex_count++;
            } else if (line[1] == 't') {
                obj->texcoord_count++;
            } else if (line[1] == 'n') {
                obj->normal_count++;
            }
        } else if (line[0] == 'f' && (line[1] == ' ' || line[1] == '\t')) {
            int n = count_face_verts(line);
            if (n >= 3) obj->face_count += (size_t)(n - 2) * 3;
        }
    }

    obj->vertices = (vertex_t *)malloc(sizeof(vertex_t) * (obj->vertex_count + 1));
    obj->texcoords = (texcoord_t *)malloc(sizeof(texcoord_t) * (obj->texcoord_count + 1));
    obj->normals = (normal_t *)malloc(sizeof(normal_t) * (obj->normal_count + 1));
    obj->faces = (face_t *)malloc(sizeof(face_t) * (obj->face_count + 1));
    obj->face_materials = (int *)malloc(sizeof(int) * (obj->face_count / 3 + 1));
    obj->face_groups = (int *)malloc(sizeof(int) * (obj->face_count / 3 + 1));
    if (!obj->vertices || !obj->texcoords || !obj->normals || !obj->faces || !obj->face_materials || !obj->face_groups) {
        free(line); fclose(handle); obj_free(obj); return NULL;
    }
    name_index(&obj->group_names, &obj->group_count, obj->name ? obj->name : "default");

    rewind(handle);
    while (fgets(line, MAX_LEN, handle)) {
        if (line[0] == 'v') {
            if (line[1] == ' ' || line[1] == '\t') {
                vertex_t *v = &obj->vertices[vertex_index];
                if (sscanf(line + 1, "%f %f %f", &v->x, &v->y, &v->z) != 3) {
                    fprintf(stderr, "Invalid vertex format\n");
                    free(line); fclose(handle); obj_free(obj);
                    return NULL;
                }
                vertex_index++;
            } else if (line[1] == 't') {
                texcoord_t *t = &obj->texcoords[texcoord_index];
                t->v = 0.0f;
                if (sscanf(line + 2, "%f %f", &t->u, &t->v) < 1) {
                    fprintf(stderr, "Invalid texcoord format\n");
                    free(line); fclose(handle); obj_free(obj);
                    return NULL;
                }
                texcoord_index++;
            } else if (line[1] == 'n') {
                normal_t *n = &obj->normals[normal_index];
                if (sscanf(line + 2, "%f %f %f", &n->x, &n->y, &n->z) != 3) {
                    fprintf(stderr, "Invalid normal format\n");
                    free(line); fclose(handle); obj_free(obj);
                    return NULL;
                }
                normal_index++;
            }
        } else if (line[0] == 'f' && (line[1] == ' ' || line[1] == '\t')) {
            face_t corners[64];
            int n = 0, k;
            const char *p = line + 1;
            for (;;) {
                while (*p == ' ' || *p == '\t') p++;
                if (!*p || *p == '\n' || *p == '\r' || *p == '#') break;
                if (n < 64) p = parse_corner(p, &corners[n++], vertex_index, texcoord_index, normal_index);
                while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
            }
            for (k = 0; k < n; k++) {
                if (corners[k].x < 1 || (size_t)corners[k].x > vertex_index) {
                    fprintf(stderr, "Invalid face format\n");
                    free(line); fclose(handle); obj_free(obj);
                    return NULL;
                }
            }
            for (k = 1; k + 1 < n; k++) {   /* fan triangulation */
                if (face_index + 3 > obj->face_count) break;
                obj->faces[face_index + 0] = corners[0];
                obj->faces[face_index + 1] = corners[k];
                obj->faces[face_index + 2] = corners[k + 1];
                obj->face_materials[tri_index] = cur_mat;
                obj->face_groups[tri_index] = cur_group;
                face_index += 3;
                tri_index++;
            }
        } else if (strncmp(line, "usemtl", 6) == 0 && (line[6] == ' ' || line[6] == '\t')) {
            char *nm = line + 7;
            while (*nm == ' ' || *nm == '\t') nm++;
            trim(nm);
            cur_mat = name_index(&obj->material_names, &obj->material_count, nm);
        } else if (strncmp(line, "mtllib", 6) == 0 && (line[6] == ' ' || line[6] == '\t')) {
            char *nm = line + 7;
            while (*nm == ' ' || *nm == '\t') nm++;
            trim(nm);
            name_index(&obj->mtllibs, &obj->mtllib_count, nm);
        } else if ((line[0] == 'o' || line[0] == 'g') && (line[1] == ' ' || line[1] == '\t')) {
            char *nm = line + 2;
            while (*nm == ' ' || *nm == '\t') nm++;
            trim(nm);
            if (*nm) cur_group = name_index(&obj->group_names, &obj->group_count, nm);
        }
    }
    fclose(handle);
    free(line);
    obj->face_count = face_index;

    {
        /* upstream's flattened vertex list (one entry per position) and index buffer */
        complete_vertex_t *vertex_data = (complete_vertex_t *)malloc((obj->vertex_count + 1) * sizeof(complete_vertex_t));
        size_t i;
        obj->indices = (unsigned int *)malloc((obj->face_count + 1) * sizeof(unsigned int));
        if (!vertex_data || !obj->indices) { free(vertex_data); obj_free(obj); return NULL; }
        for (i = 0; i < obj->vertex_count; ++i) {
            vertex_data[i].vertex = obj->vertices[i];
            vertex_data[i].texcoord.u = 0.0f;
            vertex_data[i].texcoord.v = 0.0f;
            vertex_data[i].normal.x = vertex_data[i].normal.y = vertex_data[i].normal.z = 0.0f;
        }
        for (i = 0; i < obj->face_count; ++i) {
            const face_t *f = &obj->faces[i];
            size_t vi = (size_t)(f->x - 1);
            if (f->y > 0) vertex_data[vi].texcoord = obj->texcoords[f->y - 1];
            if (f->z > 0) vertex_data[vi].normal = obj->normals[f->z - 1];
            obj->indices[i] = (unsigned int)vi;
        }
        obj->complete_vertices = vertex_data;
        obj->complete_vertex_count = obj->vertex_count;
        obj->index_count = obj->face_count;
    }

    return obj;
}

static void free_names(char **l, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) free(l[i]);
    free(l);
}

void obj_free(obj_t *obj) {
    if (!obj) return;
    free(obj->name);
    free(obj->vertices);
    free(obj->texcoords);
    free(obj->normals);
    free(obj->faces);
    free(obj->complete_vertices);
    free(obj->indices);
    free(obj->face_materials);
    free(obj->face_groups);
    free_names(obj->material_names, obj->material_count);
    free_names(obj->group_names, obj->group_count);
    free_names(obj->mtllibs, obj->mtllib_count);
    free(obj);
}
