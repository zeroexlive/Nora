/* nora-cli - headless renderer.
 *   nora-cli scene.obj --mode spectral --spp 64 -o out.png
 *   nora-cli cornell --integrator pt --mode rgb --spp 256 -o out.exr
 * MIT License. */
#include "render.h"
#include "image.h"
#include "scene.h"
#include "graph.h"
#include "osl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void) {
    printf(
        "Nora %s - physically based renderer (Metropolis light transport)\n\n"
        "usage: nora-cli [scene] [options]\n\n"
        "  scene                 cornell | cornell-glass | path/to/file.obj   (default: cornell)\n"
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
        "  --quiet               less output\n",
        "0.1.0");
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

int main(int argc, char **argv) {
    nora_settings st;
    const char *scene_arg = "cornell", *outputs[8], *shader = NULL, *graph = NULL, *material = NULL;
    int nout = 0, i, quiet = 0, dump_osl = 0;
    float exposure = 0.0f, fov = 0.0f, aperture = -1.0f, focus = -1.0f;
    nora_tonemap tm = NORA_TONEMAP_CLAMP;
    int have_env = 0;
    v3 env = { 0, 0, 0 };
    nora_scene *s;
    float *img;
    nora_render_stats stats;
    nora_settings_default(&st);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
#define NEXT() (i + 1 < argc ? argv[++i] : (fprintf(stderr, "nora-cli: %s needs a value\n", a), exit(2), ""))
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (!strcmp(a, "-o") || !strcmp(a, "--output")) { if (nout < 8) outputs[nout++] = NEXT(); }
        else if (!strcmp(a, "--mode")) {
            const char *m = NEXT();
            if (!strcmp(m, "rgb")) st.mode = NORA_MODE_RGB;
            else if (!strcmp(m, "spectral")) st.mode = NORA_MODE_SPECTRAL;
            else { fprintf(stderr, "nora-cli: unknown mode '%s'\n", m); return 2; }
        } else if (!strcmp(a, "--integrator")) {
            const char *m = NEXT();
            if (!strcmp(m, "mlt") || !strcmp(m, "pssmlt")) st.integrator = NORA_INT_MLT;
            else if (!strcmp(m, "pt") || !strcmp(m, "path")) st.integrator = NORA_INT_PT;
            else { fprintf(stderr, "nora-cli: unknown integrator '%s' (mlt, pt)\n", m); return 2; }
        }
        else if (!strcmp(a, "--spp")) st.spp = atoi(NEXT());
        else if (!strcmp(a, "--time")) st.time_limit = (float)atof(NEXT());
        else if (!strcmp(a, "--threads")) st.threads = atoi(NEXT());
        else if (!strcmp(a, "--width")) st.width = atoi(NEXT());
        else if (!strcmp(a, "--height")) st.height = atoi(NEXT());
        else if (!strcmp(a, "--max-depth")) st.max_depth = atoi(NEXT());
        else if (!strcmp(a, "--plarge")) st.p_large = (float)atof(NEXT());
        else if (!strcmp(a, "--bootstrap")) st.bootstrap = atoi(NEXT());
        else if (!strcmp(a, "--chains")) st.chains = atoi(NEXT());
        else if (!strcmp(a, "--seed")) st.seed = (uint64_t)strtoull(NEXT(), NULL, 10);
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
            if (!nora_use_real_osl(1)) fprintf(stderr, "nora-cli: --real-osl: this build has no OpenShadingLanguage backend (NORA_WITH_OSL); using the built-in compiler\n");
        }
        else if (a[0] == '-') { fprintf(stderr, "nora-cli: unknown option '%s' (try --help)\n", a); return 2; }
        else scene_arg = a;
#undef NEXT
    }
    if (nout == 0) outputs[nout++] = "nora.png";
    if (st.width <= 0 || st.height <= 0 || st.width > 16384 || st.height > 16384) { fprintf(stderr, "nora-cli: bad image size\n"); return 2; }

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
    if (!strcmp(scene_arg, "cornell")) s = nora_scene_cornell(0);
    else if (!strcmp(scene_arg, "cornell-glass")) s = nora_scene_cornell(1);
    else {
        char err[512];
        s = nora_scene_load_obj(scene_arg, err, sizeof err);
        if (!s) { fprintf(stderr, "nora-cli: %s\n", err); return 1; }
    }
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
    if (nora_scene_build(s) != 0) { fprintf(stderr, "nora-cli: scene build failed\n"); return 1; }
    if (!quiet)
        printf("nora: %s, %d triangles, %d emitters, %dx%d, %s, %s, %d %s\n", s->name, s->ntris, s->nlights, st.width, st.height,
               nora_mode_name(st.mode), nora_integrator_name(st.integrator), st.spp,
               st.integrator == NORA_INT_MLT ? "mutations/pixel" : "spp");
    img = (float *)malloc(sizeof(float) * 3 * (size_t)st.width * (size_t)st.height);
    if (!img) return 1;
    if (nora_render_blocking(s, &st, img, &stats) != 0) { fprintf(stderr, "nora-cli: render failed\n"); return 1; }
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
    return 0;
}
