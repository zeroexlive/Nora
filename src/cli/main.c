/* nora-cli - headless renderer.
 *   nora-cli scene.obj --mode spectral --spp 64 -o out.png
 *   nora-cli cornell --integrator pt --mode rgb --spp 256 -o out.exr
 *   nora-cli scene.nora --checkpoint run.nrck --checkpoint-interval 60
 *   nora-cli scene.nora --resume run.nrck --spp 1024 -o out.exr
 * MIT License. */
#include "render.h"
#include "image.h"
#include "scene.h"
#include "scenefile.h"
#include "graph.h"
#include "osl.h"
#include "nthread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

static volatile sig_atomic_t g_interrupted = 0;
static void on_sigint(int sig) { (void)sig; g_interrupted = 1; }

static void usage(void) {
    printf(
        "Nora %s - physically based renderer (Metropolis light transport)\n\n"
        "usage: nora-cli [scene] [options]\n\n"
        "  scene                 cornell | cornell-glass | file.obj | file.nora   (default: cornell)\n"
        "                        a .nora file also sets the render settings; options below override them\n"
        "  -o, --output FILE     output image, .png / .exr / .pfm (repeatable; default nora.png)\n"
        "  --mode rgb|spectral   colour transport (default rgb)\n"
        "  --integrator mlt|pt   PSSMLT or path tracing (default mlt)\n"
        "  --spp N               samples per pixel (pt) or mutations per pixel (mlt), default 64\n"
        "  --time SECONDS        stop after this many seconds\n"
        "  --threads N           worker threads (default: all cores)\n"
        "  --width W --height H  image size (default 512 x 512)\n"
        "  --max-depth N         maximum path length (default 10)\n"
        "  --plarge P            MLT large step probability (default 0.3)\n"
        "  --bootstrap N         MLT bootstrap paths (default 100000)\n"
        "  --chains N            MLT Markov chains (default 64)\n"
        "  --seed N              random seed\n"
        "  --exposure EV         exposure for PNG output, in stops (default 0)\n"
        "  --tonemap clamp|reinhard|aces   PNG tone mapping (default clamp)\n"
        "  --shader FILE.osl     compile with the built-in OSL compiler and assign it ...\n"
        "  --graph FILE.json     ... or generate OSL from a shader graph and assign it ...\n"
        "  --material NAME       ... to this material (default: every material without emission)\n"
        "  --dump-osl            print the OSL generated from --graph and exit\n"
        "  --fov DEG             override the vertical field of view\n"
        "  --aperture R --focus D  thin lens: lens radius and focus distance (scene units)\n"
        "  --env R,G,B           constant environment light\n"
        "  --real-osl            compile and run shaders with liboslcomp/liboslexec (NORA_WITH_OSL builds)\n"
        "  --checkpoint FILE     save a .nrck checkpoint at the end, on Ctrl+C (SIGINT) and every\n"
        "                        --checkpoint-interval seconds\n"
        "  --checkpoint-interval S   seconds between periodic checkpoints (default 0 = off)\n"
        "  --resume FILE         continue a checkpoint (refused if the scene differs); --spp is the\n"
        "                        new total, --time the time for this run; image settings come from the checkpoint\n"
        "  --save-scene FILE     write the scene (with these settings) as a .nora file\n"
        "  --export-obj FILE     write the scene as OBJ/MTL with the object transforms baked in\n"
        "  --no-render           stop after --save-scene / --export-obj\n"
        "  --quiet               less output\n",
        "0.2.0");
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)n + 1);
    if (buf) buf[fread(buf, 1, (size_t)n, f)] = 0;
    fclose(f);
    return buf;
}

static int assign_shader(nora_scene *s, const char *name, const char *src, const char *material) {
    char err[512];
    int slot = nora_scene_add_shader(s, name, src, err, sizeof err), i, n = 0;
    if (slot < 0) { fprintf(stderr, "nora-cli: shader error: %s\n", err); return -1; }
    for (i = 0; i < s->nmats; i++) {
        nora_material *m = &s->mats[i];
        if (material ? strcmp(m->name, material) == 0 : (m->emission_strength <= 0.0f)) { m->shader = slot; n++; }
    }
    if (n == 0) { fprintf(stderr, "nora-cli: no material named '%s'\n", material ? material : "?"); return -1; }
    return 0;
}

static int save_ckpt(nora_render *r, uint64_t hash, const char *path, int quiet) {
    char err[512];
    if (nora_render_save_checkpoint(r, hash, path, err, sizeof err) != 0) { fprintf(stderr, "nora-cli: checkpoint: %s\n", err); return -1; }
    if (!quiet) {
        nora_render_stats rs;
        nora_render_get_stats(r, &rs);
        printf("nora: checkpoint %s (%lld %s)\n", path, (long long)rs.samples,
               nora_render_settings(r)->integrator == NORA_INT_MLT ? "mutations" : "samples");
    }
    return 0;
}

int main(int argc, char **argv) {
    nora_settings st, cli;
    int have_spp = 0, have_time = 0, have_threads = 0, have_w = 0, have_h = 0, have_mode = 0, have_int = 0, have_depth = 0;
    int have_plarge = 0, have_boot = 0, have_chains = 0, have_seed = 0, no_render = 0, interrupted = 0;
    const char *ckpt = NULL, *resume = NULL, *save_scene = NULL, *export_obj = NULL;
    double ckpt_interval = 0.0;
    nora_render *r;
    uint64_t hash;
    const char *scene_arg = "cornell", *outputs[8], *shader = NULL, *graph = NULL, *material = NULL;
    int nout = 0, i, quiet = 0, dump_osl = 0;
    float exposure = 0.0f, fov = 0.0f, aperture = -1.0f, focus = -1.0f;
    nora_tonemap tm = NORA_TONEMAP_CLAMP;
    int have_env = 0;
    v3 env = { 0, 0, 0 };
    nora_scene *s;
    float *img;
    nora_render_stats stats;
    nora_settings_default(&cli);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
#define NEXT() (i + 1 < argc ? argv[++i] : (fprintf(stderr, "nora-cli: %s needs a value\n", a), exit(2), ""))
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (!strcmp(a, "-o") || !strcmp(a, "--output")) { if (nout < 8) outputs[nout++] = NEXT(); }
        else if (!strcmp(a, "--mode")) {
            const char *m = NEXT();
            have_mode = 1;
            if (!strcmp(m, "rgb")) cli.mode = NORA_MODE_RGB;
            else if (!strcmp(m, "spectral")) cli.mode = NORA_MODE_SPECTRAL;
            else { fprintf(stderr, "nora-cli: unknown mode '%s'\n", m); return 2; }
        } else if (!strcmp(a, "--integrator")) {
            const char *m = NEXT();
            have_int = 1;
            if (!strcmp(m, "mlt") || !strcmp(m, "pssmlt")) cli.integrator = NORA_INT_MLT;
            else if (!strcmp(m, "pt") || !strcmp(m, "path")) cli.integrator = NORA_INT_PT;
            else { fprintf(stderr, "nora-cli: unknown integrator '%s' (mlt, pt)\n", m); return 2; }
        }
        else if (!strcmp(a, "--spp")) { cli.spp = atoi(NEXT()); have_spp = 1; }
        else if (!strcmp(a, "--time")) { cli.time_limit = (float)atof(NEXT()); have_time = 1; }
        else if (!strcmp(a, "--threads")) { cli.threads = atoi(NEXT()); have_threads = 1; }
        else if (!strcmp(a, "--width")) { cli.width = atoi(NEXT()); have_w = 1; }
        else if (!strcmp(a, "--height")) { cli.height = atoi(NEXT()); have_h = 1; }
        else if (!strcmp(a, "--max-depth")) { cli.max_depth = atoi(NEXT()); have_depth = 1; }
        else if (!strcmp(a, "--plarge")) { cli.p_large = (float)atof(NEXT()); have_plarge = 1; }
        else if (!strcmp(a, "--bootstrap")) { cli.bootstrap = atoi(NEXT()); have_boot = 1; }
        else if (!strcmp(a, "--chains")) { cli.chains = atoi(NEXT()); have_chains = 1; }
        else if (!strcmp(a, "--seed")) { cli.seed = (uint64_t)strtoull(NEXT(), NULL, 10); have_seed = 1; }
        else if (!strcmp(a, "--checkpoint")) ckpt = NEXT();
        else if (!strcmp(a, "--checkpoint-interval")) ckpt_interval = atof(NEXT());
        else if (!strcmp(a, "--resume")) resume = NEXT();
        else if (!strcmp(a, "--save-scene")) save_scene = NEXT();
        else if (!strcmp(a, "--export-obj")) export_obj = NEXT();
        else if (!strcmp(a, "--no-render")) no_render = 1;
        else if (!strcmp(a, "--exposure")) exposure = (float)atof(NEXT());
        else if (!strcmp(a, "--tonemap")) {
            const char *t = NEXT();
            tm = !strcmp(t, "aces") ? NORA_TONEMAP_ACES : (!strcmp(t, "reinhard") ? NORA_TONEMAP_REINHARD : NORA_TONEMAP_CLAMP);
        }
        else if (!strcmp(a, "--shader")) shader = NEXT();
        else if (!strcmp(a, "--graph")) graph = NEXT();
        else if (!strcmp(a, "--material")) material = NEXT();
        else if (!strcmp(a, "--dump-osl")) dump_osl = 1;
        else if (!strcmp(a, "--fov")) fov = (float)atof(NEXT());
        else if (!strcmp(a, "--aperture")) aperture = (float)atof(NEXT());
        else if (!strcmp(a, "--focus")) focus = (float)atof(NEXT());
        else if (!strcmp(a, "--env")) { const char *e = NEXT(); have_env = 1; if (sscanf(e, "%f,%f,%f", &env.x, &env.y, &env.z) == 1) env.y = env.z = env.x; }
        else if (!strcmp(a, "--quiet") || !strcmp(a, "-q")) quiet = 1;
        else if (!strcmp(a, "--real-osl")) {
            if (!nora_use_real_osl(1)) fprintf(stderr, "nora-cli: --real-osl: the real OpenShadingLanguage is not available in this build (configure with -DNORA_WITH_OSL=ON and OSL installed); using the built-in compiler\n");
        }
        else if (a[0] == '-') { fprintf(stderr, "nora-cli: unknown option '%s' (try --help)\n", a); return 2; }
        else scene_arg = a;
#undef NEXT
    }
    if (nout == 0) outputs[nout++] = "nora.png";

    if (graph && dump_osl) {
        ng_graph g;
        char err[512];
        static char src[65536];
        ng_init(&g);
        if (ng_load_file(&g, graph, err, sizeof err) != 0) { fprintf(stderr, "nora-cli: %s\n", err); return 1; }
        if (ng_generate_osl(&g, src, sizeof src, err, sizeof err) != 0) { fprintf(stderr, "nora-cli: %s\n", err); return 1; }
        fputs(src, stdout);
        ng_free(&g);
        return 0;
    }

    spectrum_init();
    {
        char err[512];
        s = nora_scene_open(scene_arg, &st, NULL, err, sizeof err);
        if (!s) { fprintf(stderr, "nora-cli: %s\n", err); return 1; }
    }
    /* command line options override the scene file's render settings */
    if (have_spp) st.spp = cli.spp;
    if (have_time) st.time_limit = cli.time_limit;
    if (have_threads) st.threads = cli.threads;
    if (have_w) st.width = cli.width;
    if (have_h) st.height = cli.height;
    if (have_mode) st.mode = cli.mode;
    if (have_int) st.integrator = cli.integrator;
    if (have_depth) st.max_depth = cli.max_depth;
    if (have_plarge) st.p_large = cli.p_large;
    if (have_boot) st.bootstrap = cli.bootstrap;
    if (have_chains) st.chains = cli.chains;
    if (have_seed) st.seed = cli.seed;
    if (st.width <= 0 || st.height <= 0 || st.width > 16384 || st.height > 16384) { fprintf(stderr, "nora-cli: bad image size\n"); return 2; }
    if (shader) {
        char *src = read_file(shader);
        if (!src) { fprintf(stderr, "nora-cli: cannot read %s\n", shader); return 1; }
        if (assign_shader(s, shader, src, material) != 0) return 1;
        free(src);
    }
    if (graph) {
        ng_graph g;
        char err[512];
        static char src[65536];
        ng_init(&g);
        if (ng_load_file(&g, graph, err, sizeof err) != 0) { fprintf(stderr, "nora-cli: %s\n", err); return 1; }
        if (ng_generate_osl(&g, src, sizeof src, err, sizeof err) != 0) { fprintf(stderr, "nora-cli: %s\n", err); return 1; }
        if (assign_shader(s, graph, src, material) != 0) return 1;
        ng_free(&g);
    }
    if (fov > 0.0f) s->cam.vfov_deg = fov;
    if (aperture >= 0.0f) s->cam.aperture = aperture;
    if (focus > 0.0f) s->cam.focus_dist = focus;
    if (have_env) s->env_color = env;
    if (save_scene) {
        char err[512];
        if (nora_scenefile_save(s, &st, save_scene, err, sizeof err) != 0) { fprintf(stderr, "nora-cli: %s\n", err); return 1; }
        if (!quiet) printf("nora: wrote %s\n", save_scene);
    }
    if (export_obj) {
        char err[512];
        if (nora_scene_export_obj(s, export_obj, err, sizeof err) != 0) { fprintf(stderr, "nora-cli: %s\n", err); return 1; }
        if (!quiet) printf("nora: wrote %s\n", export_obj);
    }
    if (no_render) { nora_scene_destroy(s); return 0; }
    if (nora_scene_build(s) != 0) { fprintf(stderr, "nora-cli: scene build failed\n"); return 1; }
    hash = nora_scene_hash(s);
    if (resume) {
        char err[512];
        r = nora_render_load_checkpoint(s, hash, resume, have_threads ? cli.threads : st.threads, err, sizeof err);
        if (!r) { fprintf(stderr, "nora-cli: cannot resume: %s\n", err); return 1; }
        if (have_spp && nora_render_settings(r)->spp != st.spp) nora_render_set_spp(r, st.spp);
        nora_render_set_time_limit(r, have_time ? st.time_limit : 0.0f);
        st = *nora_render_settings(r);
    } else {
        r = nora_render_create(s, &st);
        if (!r) { fprintf(stderr, "nora-cli: cannot create the renderer\n"); return 1; }
    }
    if (!quiet)
        printf("nora: %s, %d triangles, %d emitters, %dx%d, %s, %s, %d %s\n", s->name, s->ntris, s->nlights, st.width, st.height,
               nora_mode_name(st.mode), nora_integrator_name(st.integrator), st.spp,
               st.integrator == NORA_INT_MLT ? "mutations/pixel" : "spp");
    if (resume && !quiet) {
        nora_render_get_stats(r, &stats);
        printf("nora: resuming %s at %lld %s\n", resume, (long long)stats.samples, st.integrator == NORA_INT_MLT ? "mutations" : "samples");
    }
    img = (float *)malloc(sizeof(float) * 3 * (size_t)st.width * (size_t)st.height);
    if (!img) return 1;
    signal(SIGINT, on_sigint);
    if (resume) nora_render_resume(r);
    else nora_render_start(r);
    {
        double last = ntime_now();
        while (nora_render_is_running(r)) {
            nsleep_ms(20);
            if (g_interrupted) {
                nora_render_pause(r);
                interrupted = 1;
                fprintf(stderr, "nora-cli: interrupted%s\n", ckpt ? ", saving the checkpoint" : "");
                break;
            }
            if (ckpt && ckpt_interval > 0.0 && ntime_now() - last >= ckpt_interval) {
                nora_render_pause(r);
                save_ckpt(r, hash, ckpt, quiet);
                last = ntime_now();
                if (nora_render_can_resume(r)) nora_render_resume(r);
            }
        }
        nora_render_wait(r);
    }
    if (ckpt && save_ckpt(r, hash, ckpt, quiet) != 0) return 1;
    nora_render_resolve(r, img);
    nora_render_get_stats(r, &stats);
    nora_render_destroy(r);
    if (!quiet) {
        printf("nora: %.2f s, %lld %s", stats.elapsed, (long long)stats.samples, st.integrator == NORA_INT_MLT ? "mutations" : "samples");
        if (st.integrator == NORA_INT_MLT) printf(", acceptance %.1f%%, b = %.5g", 100.0 * stats.acceptance, stats.b);
        printf("\n");
    }
    for (i = 0; i < nout; i++) {
        if (nora_write_image(outputs[i], img, st.width, st.height, exposure, tm) != 0) {
            fprintf(stderr, "nora-cli: cannot write %s\n", outputs[i]);
            return 1;
        }
        if (!quiet) printf("nora: wrote %s\n", outputs[i]);
    }
    free(img);
    nora_scene_destroy(s);
    return interrupted ? 130 : 0;
}
