/* Nora - scene description: triangles, materials, textures, camera, lights. MIT License. */
#ifndef NORA_SCENE_H
#define NORA_SCENE_H

#include "nmath.h"
#include <stddef.h>

struct osl_program;

typedef enum nora_mat_type {
    NORA_MAT_DIFFUSE = 0,
    NORA_MAT_CONDUCTOR,
    NORA_MAT_DIELECTRIC,
    NORA_MAT_PLASTIC,          /* diffuse base under a GGX dielectric coat */
    NORA_MAT_TYPE_COUNT
} nora_mat_type;

extern const char *const nora_mat_type_names[NORA_MAT_TYPE_COUNT];

typedef struct nora_material {
    char name[64];
    nora_mat_type type;
    v3 color;                  /* albedo / conductor reflectance at normal incidence / glass tint */
    int color_tex;             /* texture index or -1 */
    v3 edge_tint;              /* conductor reflectance at grazing angles (Gulbrandsen 2014) */
    float roughness;           /* perceptual roughness, GGX alpha = roughness^2; 0 = smooth */
    float ior;                 /* dielectric / plastic coat index of refraction (at 587.6 nm) */
    float abbe;                /* Abbe number; 0 = no dispersion. Used in spectral mode only */
    v3 emission;               /* emitted radiance colour */
    float emission_strength;
    int two_sided;             /* emission from both faces */
    int shader;                /* OSL shader slot or -1 */
} nora_material;

typedef struct nora_texture {
    char path[512];
    int w, h;
    float *rgb;                /* linear RGB, w*h*3 */
} nora_texture;

typedef struct nora_shader_slot {
    char name[64];
    char *source;              /* OSL source (owned) */
    struct osl_program *prog;  /* compiled program (owned) */
    void *real;                /* NORA_WITH_OSL: shader compiled by the real liboslcomp, or NULL */
    int may_emit;
    /* where the shader came from, for scene files and hot reload (all optional) */
    char file[1024];           /* .osl file (absolute) */
    char graph_file[1024];     /* shader graph .json file (absolute) */
    char *graph_json;          /* inline shader graph (owned), the source was generated from it */
} nora_shader_slot;

typedef struct nora_camera {
    v3 pos, target, up;
    float vfov_deg;
    float aperture;            /* lens radius in scene units; 0 = pinhole */
    float focus_dist;          /* 0 = distance to target */
} nora_camera;

typedef struct nora_tri {
    int v[3];                  /* position indices */
    int n[3];                  /* normal indices or -1 */
    int t[3];                  /* uv indices or -1 */
    int mat;
    int mesh;
} nora_tri;

/* A mesh is one object (an instance) of the scene. Its rest geometry is kept in
 * rest_pos / rest_nrm; the instance transform maps it into pos / nrm:
 *   world = pivot + translate + R(rot) * (scale * (rest - pivot))
 * where pivot is the centre of the rest bounds. rot is the quaternion used
 * internally; euler_deg (XYZ, degrees, R = Rz Ry Rx) is what the user edits and
 * what scene files store. */
typedef struct nora_mesh {
    char name[64];
    int first_tri, tri_count;
    v3 translate, scale, euler_deg;
    quat rot;
    v3 pivot;
    int first_pos, npos, first_nrm, nnrm;   /* owned vertex ranges (after nora_scene_own_vertices) */
    int source, source_mesh;                /* origin: index into scene sources and mesh in it, -1 = none */
    int hidden;                             /* deleted (kept so the deletion can be undone) */
} nora_mesh;

typedef enum nora_source_kind { NORA_SRC_OBJ = 0, NORA_SRC_CORNELL, NORA_SRC_CORNELL_GLASS } nora_source_kind;

#define NORA_MAX_DEPS 16
typedef struct nora_source {
    nora_source_kind kind;
    char path[1024];                        /* absolute path of the OBJ file */
    char deps[NORA_MAX_DEPS][1024];         /* files it pulled in (MTL libraries, OSL), absolute */
    int ndeps;
} nora_source;

typedef struct nora_bvh_node {
    float bmin[3], bmax[3];
    int left_or_first;         /* inner: index of left child (right = left + 1); leaf: first prim */
    int count;                 /* 0 for inner nodes */
} nora_bvh_node;

typedef struct nora_tri_accel { v3 p0, e1, e2; } nora_tri_accel;

typedef struct nora_scene {
    v3 *pos; int npos, cap_pos;
    v3 *nrm; int nnrm, cap_nrm;
    float *uv; int nuv, cap_uv;          /* 2 floats each */
    nora_tri *tris; int ntris, cap_tris;
    nora_material *mats; int nmats, cap_mats;
    nora_texture *texs; int ntexs, cap_texs;
    nora_shader_slot *shaders; int nshaders, cap_shaders;
    nora_mesh *meshes; int nmeshes, cap_meshes;
    nora_source *sources; int nsources, cap_sources;
    v3 *rest_pos, *rest_nrm;             /* rest geometry (when owned) */
    int cap_rest_pos, cap_rest_nrm;
    int owned;                           /* every mesh owns its vertex range */
    char file_path[1024];                /* the .nora file this scene was loaded from or saved to */
    nora_camera cam;
    v3 env_color;                        /* constant environment radiance (0 = none) */
    char name[128];
    char base_dir[512];                  /* for relative texture paths */

    /* built by nora_scene_build */
    nora_bvh_node *nodes; int nnodes;
    int *prim_idx;                       /* triangle order in leaves */
    nora_tri_accel *accel;               /* per-triangle in leaf order */
    v3 bmin, bmax;
    int *light_tris; int nlights;
    float *light_cdf;                    /* nlights + 1 */
    float *tri_light_prob;               /* per triangle selection probability (0 if not a light) */
    float env_prob;                      /* probability of picking the environment in NEE */
    int built;
} nora_scene;

nora_scene *nora_scene_create(void);
void nora_scene_destroy(nora_scene *s);
int nora_scene_add_pos(nora_scene *s, v3 p);
int nora_scene_add_nrm(nora_scene *s, v3 n);
int nora_scene_add_uv(nora_scene *s, float u, float v);
int nora_scene_add_tri(nora_scene *s, const nora_tri *t);
int nora_scene_add_material(nora_scene *s, const nora_material *m);
int nora_scene_add_mesh(nora_scene *s, const char *name, int first_tri, int tri_count);
int nora_scene_find_material(const nora_scene *s, const char *name);
int nora_scene_load_texture(nora_scene *s, const char *path);  /* cached by path, -1 on failure */
void nora_material_default(nora_material *m, const char *name);

/* Shader slots. Compiles with the built-in OSL compiler; returns slot or -1 (err filled). */
int nora_scene_add_shader(nora_scene *s, const char *name, const char *source, char *err, size_t errcap);
/* NORA_WITH_OSL builds: also compile shaders with the real OSL and execute them with
 * liboslexec (falls back to the built-in VM when that fails). Returns 0 if unavailable. */
int nora_use_real_osl(int on);
int nora_scene_replace_shader(nora_scene *s, int slot, const char *source, char *err, size_t errcap);

/* Instances. nora_scene_own_vertices gives every mesh its own contiguous vertex
 * range (duplicating shared vertices) and records the rest geometry; loaders call
 * it. nora_scene_apply_transforms recomputes pos/nrm from the rest geometry and
 * the instance transforms (nora_scene_build calls it). */
int nora_scene_own_vertices(nora_scene *s);
void nora_scene_apply_transforms(nora_scene *s);
void nora_mesh_set_euler(nora_mesh *m, v3 euler_deg);    /* sets euler_deg and rot */
void nora_mesh_set_rot(nora_mesh *m, quat q);            /* sets rot and euler_deg */
v3 nora_mesh_xform_point(const nora_mesh *m, v3 rest);
v3 nora_mesh_center(const nora_mesh *m);                 /* world position of the pivot */
void nora_mesh_world_bounds(const nora_scene *s, int mesh, v3 *bmin, v3 *bmax);
int nora_scene_live_mesh_count(const nora_scene *s);
int nora_scene_add_source(nora_scene *s, nora_source_kind kind, const char *path);
/* Copies mesh m (geometry, material, transform) as a new mesh; returns its index or -1. */
int nora_scene_duplicate_mesh(nora_scene *s, int m);
/* Appends meshes of src to dst (all live meshes when list is NULL). Materials are
 * merged by name (dst wins), shaders and textures are copied. With list == NULL
 * the sources are appended too; with a list the new meshes get source -1 and the
 * caller sets it. src must be owned. Returns the index of the first new mesh, or -1. */
int nora_scene_append(nora_scene *dst, const nora_scene *src, const int *list, int n);
/* "Import Object": loads an OBJ (with its MTL) and merges it as new objects.
 * Returns the first new mesh (count in *count), -1 on failure (err filled). */
int nora_scene_import_obj(nora_scene *s, const char *path, int *count, char *err, size_t errcap);
/* 64-bit hash of everything that changes the rendered image (geometry after the
 * transforms, materials, shaders, textures, camera, environment). Uses the
 * current pos/nrm: call it after nora_scene_build or nora_scene_apply_transforms. */
uint64_t nora_scene_hash(nora_scene *s);

/* Builds the BVH and the light distribution. Call after edits, before rendering. */
int nora_scene_build(nora_scene *s);
void nora_scene_invalidate(nora_scene *s);

/* Frame the camera on the scene bounds (used for OBJ files without a camera). */
void nora_scene_frame_camera(nora_scene *s);
void nora_scene_bounds(const nora_scene *s, v3 *bmin, v3 *bmax);

/* Ray queries (BVH). */
typedef struct nora_hit {
    float t, b1, b2;           /* barycentrics of v1 and v2 */
    int tri;
} nora_hit;
int nora_intersect(const nora_scene *s, const ray3 *r, nora_hit *hit);
int nora_occluded(const nora_scene *s, const ray3 *r);

/* Texture sampling (bilinear, wrapping). t = 0 is the top row. */
v3 nora_texture_sample(const nora_texture *t, float s, float tt);

/* Built-in scenes. variant: 0 = classic boxes, 1 = boxes + dispersive glass sphere. */
nora_scene *nora_scene_cornell(int variant);

/* OBJ/MTL import (third_party/obj, MIT). Returns NULL and fills err on failure. */
nora_scene *nora_scene_load_obj(const char *path, char *err, size_t errcap);
/* OBJ/MTL export of the live meshes with their transforms baked in. */
int nora_scene_export_obj(nora_scene *s, const char *path, char *err, size_t errcap);

#endif
