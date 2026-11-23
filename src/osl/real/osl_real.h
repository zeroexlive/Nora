/* Nora - optional backend that runs shaders through the real OpenShadingLanguage
 * (liboslcomp + liboslexec) behind a small C ABI. Built only with -DNORA_WITH_OSL=ON.
 * When OSL is not found at configure time the shim compiles as a stub whose
 * nora_real_osl_available() returns 0. MIT License. */
#ifndef NORA_OSL_REAL_H
#define NORA_OSL_REAL_H

#include "osl.h"

#ifdef __cplusplus
extern "C" {
#endif

int nora_real_osl_available(void);
const char *nora_real_osl_version(void);
/* Compiles OSL source with liboslcomp and loads it into the shading system. NULL on failure. */
void *nora_real_osl_compile(const char *source, char *err, size_t errcap);
/* Executes the shader with liboslexec and flattens Ci into Nora closures. 0 on success. */
int nora_real_osl_execute(void *shader, const osl_globals *g, osl_closure_list *out);
void nora_real_osl_free(void *shader);

#ifdef __cplusplus
}
#endif

#endif
