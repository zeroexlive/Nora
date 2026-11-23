/* Nora - built-in Open Shading Language subset: lexer, parser, type checker,
 * bytecode compiler and VM. MIT License.
 *
 * Supported: surface/shader declarations with parameters and defaults, output
 * parameters, types float/int/color/point/vector/normal/string/closure color,
 * arithmetic, comparisons, logic, ternary, component access c[i], if/else,
 * for/while/do loops with break/continue, a common subset of the standard
 * library and the closures diffuse, oren_nayar (as diffuse), microfacet,
 * reflection, refraction, emission, transparent, holdout (ignored). */
#ifndef NORA_OSL_H
#define NORA_OSL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum osl_type {
    OSL_T_VOID = 0, OSL_T_INT, OSL_T_FLOAT, OSL_T_COLOR, OSL_T_POINT, OSL_T_VECTOR, OSL_T_NORMAL,
    OSL_T_STRING, OSL_T_CLOSURE, OSL_T_ERROR
} osl_type;

typedef enum osl_closure_type {
    OSL_CL_DIFFUSE = 0,
    OSL_CL_MICROFACET,        /* refract: 0 reflect, 1 refract, 2 both */
    OSL_CL_REFLECTION,        /* eta 0 = perfect mirror */
    OSL_CL_REFRACTION,
    OSL_CL_EMISSION,
    OSL_CL_TRANSPARENT,
    OSL_CL_COUNT
} osl_closure_type;

typedef struct osl_closure {
    int type;
    float w[3];               /* RGB weight */
    float N[3];
    float alpha;              /* GGX roughness (alpha) */
    float eta;
    int refract;
} osl_closure;

#define OSL_MAX_CLOSURES 16
typedef struct osl_closure_list { int n; osl_closure c[OSL_MAX_CLOSURES]; } osl_closure_list;

typedef struct osl_globals {
    float P[3], N[3], Ng[3], I[3], dPdu[3], dPdv[3];
    float u, v, time;
    int backfacing;
} osl_globals;

typedef int (*osl_texture_fn)(void *user, const char *name, float s, float t, float rgb[3]);
typedef struct osl_host {
    osl_texture_fn texture;   /* optional; NULL makes texture() return magenta */
    void *user;
} osl_host;

typedef struct osl_override { int param; float v[3]; } osl_override;

typedef struct osl_program osl_program;
typedef struct osl_exec osl_exec;

/* Compile source. On failure returns NULL and writes "line:col: message" to err. */
osl_program *osl_compile(const char *source, char *err, size_t errcap);
void osl_program_free(osl_program *p);
const char *osl_program_name(const osl_program *p);
int osl_program_may_emit(const osl_program *p);
int osl_program_instruction_count(const osl_program *p);
int osl_program_texture_count(const osl_program *p);
const char *osl_program_texture_name(const osl_program *p, int i);
/* Human readable bytecode listing (for debugging); returns bytes written. */
size_t osl_program_disassemble(const osl_program *p, char *buf, size_t cap);

int osl_param_count(const osl_program *p);
const char *osl_param_name(const osl_program *p, int i);
osl_type osl_param_type(const osl_program *p, int i);
int osl_param_is_output(const osl_program *p, int i);
int osl_find_param(const osl_program *p, const char *name);
const char *osl_type_name(osl_type t);

osl_exec *osl_exec_create(void);
void osl_exec_free(osl_exec *e);
/* Runs the shader. Returns 0 on success (closures in out), nonzero on a runtime error. */
int osl_execute(const osl_program *p, osl_exec *e, const osl_globals *g, const osl_host *host,
                const osl_override *ov, int nov, osl_closure_list *out);
/* After osl_execute: read any parameter (e.g. an output) by name. Returns 0 if found. */
int osl_exec_read_param(const osl_program *p, const osl_exec *e, const char *name, float out[3]);
const char *osl_exec_error(const osl_exec *e);

/* Noise used by noise()/snoise() (exposed for tests). Perlin gradient noise in [-1,1]. */
float osl_perlin3(float x, float y, float z);
float osl_cellnoise3(float x, float y, float z);

#ifdef __cplusplus
}
#endif

#endif
