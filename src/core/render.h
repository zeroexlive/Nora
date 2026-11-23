/* Nora - renderer: unidirectional path tracer with next event estimation, and
 * Kelemen-style primary sample space Metropolis light transport over it.
 * Multithreaded and progressive. MIT License. */
#ifndef NORA_RENDER_H
#define NORA_RENDER_H

#include "scene.h"
#include "spectrum.h"
#include <stdint.h>

typedef enum nora_mode { NORA_MODE_RGB = 0, NORA_MODE_SPECTRAL = 1 } nora_mode;
typedef enum nora_integrator { NORA_INT_MLT = 0, NORA_INT_PT = 1 } nora_integrator;

typedef struct nora_settings {
    int width, height;
    nora_mode mode;
    nora_integrator integrator;
    int spp;                   /* PT: samples per pixel. MLT: mutations per pixel */
    float time_limit;          /* seconds; 0 = until spp is reached */
    int threads;               /* 0 = all cores */
    int max_depth;             /* path vertices after the camera (default 10) */
    float p_large;             /* MLT large step probability (default 0.3) */
    int bootstrap;             /* MLT bootstrap paths (default 100000) */
    int chains;                /* MLT Markov chains (default 64) */
    uint64_t seed;
} nora_settings;

void nora_settings_default(nora_settings *s);
const char *nora_mode_name(nora_mode m);
const char *nora_integrator_name(nora_integrator i);

typedef struct nora_render nora_render;

typedef struct nora_render_stats {
    double elapsed;            /* seconds */
    int64_t samples;           /* PT samples or MLT mutations done */
    int64_t target;            /* samples or mutations wanted */
    double acceptance;         /* MLT acceptance rate */
    double b;                  /* MLT normalisation (mean luminance) */
    int phase;                 /* 0 idle, 1 bootstrap, 2 rendering, 3 done, 4 paused */
} nora_render_stats;

/* The scene must be built (nora_scene_build) and must not change while rendering. */
nora_render *nora_render_create(const nora_scene *scene, const nora_settings *st);
int nora_render_start(nora_render *r);          /* asynchronous; 0 on success */
void nora_render_request_stop(nora_render *r);  /* ends the render (phase 3) */
/* Pause keeps everything in memory (film, MLT chains with their sample vectors
 * and RNG state, counters) and blocks until the workers have stopped; resume
 * continues accumulating. Resume returns 1 when there is no work left (raise
 * the target with nora_render_set_spp first). */
int nora_render_pause(nora_render *r);
int nora_render_resume(nora_render *r);
int nora_render_is_paused(nora_render *r);
int nora_render_can_resume(nora_render *r);
/* Only while not running: total samples (PT) or mutations (MLT) per pixel, time limit per run. */
int nora_render_set_spp(nora_render *r, int spp);
void nora_render_set_time_limit(nora_render *r, float seconds);
void nora_render_wait(nora_render *r);
int nora_render_is_running(nora_render *r);
float nora_render_progress(nora_render *r);     /* 0..1 */
void nora_render_get_stats(nora_render *r, nora_render_stats *out);
/* Current estimate as linear sRGB floats (w*h*3). Safe to call while running. */
int nora_render_resolve(nora_render *r, float *rgb);
const nora_settings *nora_render_settings(const nora_render *r);
void nora_render_destroy(nora_render *r);

/* Checkpoints (.nrck, see README): the film, the MLT chains and counters of a
 * paused or finished render. Loading refuses a checkpoint whose scene hash
 * (nora_scene_hash) differs; the returned render is paused, call
 * nora_render_resume (after nora_render_set_spp to render more). threads: worker
 * count for the resumed render (0 = all cores); the image settings come from the
 * checkpoint. */
#define NORA_CHECKPOINT_VERSION 1
int nora_render_save_checkpoint(nora_render *r, uint64_t scene_hash, const char *path, char *err, size_t errcap);
nora_render *nora_render_load_checkpoint(const nora_scene *scene, uint64_t scene_hash, const char *path, int threads,
                                         char *err, size_t errcap);

/* Convenience: render synchronously into rgb (w*h*3). */
int nora_render_blocking(const nora_scene *scene, const nora_settings *st, float *rgb, nora_render_stats *stats);

/* Camera ray for film position (px, py) in pixels (y down) and lens sample. */
ray3 nora_camera_ray(const nora_camera *c, int w, int h, float px, float py, float lu, float lv);

/* Logging hook (used by the GUI log panel and the CLI). */
typedef void (*nora_log_fn)(void *user, const char *msg);
void nora_set_log(nora_log_fn fn, void *user);
void nora_logf(const char *fmt, ...);

#endif
