/* Nora - BSDF lobes (Lambert, conductor, dielectric, GGX) and shading. MIT License. */
#ifndef NORA_BSDF_H
#define NORA_BSDF_H

#include "nmath.h"
#include "spectrum.h"
#include "scene.h"

struct osl_exec;

typedef enum nora_lobe_type {
    LOBE_DIFFUSE = 0,
    LOBE_CONDUCTOR,      /* smooth when alpha == 0, else GGX */
    LOBE_DIELECTRIC,     /* smooth when alpha == 0, else GGX (Walter et al. 2007) */
    LOBE_TRANSPARENT     /* straight pass-through */
} nora_lobe_type;

enum { DIEL_BOTH = 0, DIEL_REFLECT = 1, DIEL_TRANSMIT = 2 };

typedef struct nora_lobe {
    int type;
    sp4 w;               /* weight / tint */
    v3 N;                /* lobe normal (world) */
    float alpha;         /* GGX alpha */
    float eta;           /* dielectric relative IOR (outside/inside); 0 = Fresnel 1 (mirror) */
    int mode;            /* DIEL_* */
    int two_sided;       /* flip N toward wo (reflection-only lobes) */
    sp4 cond_n, cond_k;  /* conductor complex IOR per channel / wavelength */
} nora_lobe;

#define NORA_MAX_LOBES 8
typedef struct nora_bsdf {
    int n;
    nora_lobe l[NORA_MAX_LOBES];
    float sel[NORA_MAX_LOBES];
    v3 ng;               /* geometric normal (unit, as modelled) */
    sp4 Le;              /* emitted radiance toward wo */
    int dispersive;      /* spectral: wavelength dependent IOR, terminate secondaries */
} nora_bsdf;

typedef struct nora_bsdf_sample {
    v3 wi;
    sp4 weight;          /* f * |cos| / pdf */
    float pdf;           /* solid angle pdf (non-delta) */
    int delta;           /* specular / pass-through */
    int transparent;     /* pass-through without scattering */
} nora_bsdf_sample;

typedef struct nora_isect {
    v3 p, ng, ns, dpdu, dpdv;
    float u, v;
    int tri, mat;
    int front;
} nora_isect;

void nora_isect_fill(const nora_scene *s, const ray3 *r, const nora_hit *h, nora_isect *it);

/* Builds the BSDF (and emission toward wo) at a surface point: built-in material or OSL shader. */
void nora_shade(const nora_scene *s, struct osl_exec *osl, const nora_isect *it, v3 wo, const swl *w, nora_bsdf *out);

sp4 nora_bsdf_eval(const nora_bsdf *b, v3 wo, v3 wi, float *pdf);
int nora_bsdf_sample_dir(const nora_bsdf *b, v3 wo, float u0, float u1, float u2, float u3, nora_bsdf_sample *out);
int nora_bsdf_has_nondelta(const nora_bsdf *b);

/* Helpers exposed for tests. */
float nora_fresnel_dielectric(float cos_i, float eta);
float nora_fresnel_conductor(float cos_i, float n, float k);
void nora_gulbrandsen(float r, float g, float *n, float *k);
float nora_ggx_D(float alpha, v3 m);
float nora_ggx_G1(float alpha, v3 w);

#endif
