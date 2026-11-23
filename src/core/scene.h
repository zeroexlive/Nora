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

typedef struct nora_mesh {
    char name[64];
    int first_tri, tri_count;
} nora_mesh;

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

#endif
