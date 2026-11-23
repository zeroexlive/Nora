/* Nora - tests for scene files, editing, hot reload detection, the edit-mode
 * rasterizer and render checkpoints. MIT License. */
#include "test.h"
#include "scene.h"
#include "scenefile.h"
#include "render.h"
#include "edit.h"
#include "watch.h"
#include "raster.h"
#include "fsutil.h"
#include "graph.h"
#include "nthread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef NORA_SOURCE_DIR
#define NORA_SOURCE_DIR "."
#endif

/* save -> load -> save must give identical text */
static void roundtrip(nora_scene *s, const nora_settings *st, const char *label) {
    char *a, *b, err[512];
    nora_scene *s2;
    nora_settings st2;
    int has = 0;
    a = nora_scenefile_write_string(s, st, ".");
    CHECK(a != NULL);
    if (!a) return;
    s2 = nora_scenefile_read_string(a, ".", &st2, &has, err, sizeof err);
    CHECK(s2 != NULL);
    if (!s2) { printf("  %s: %s\n", label, err); free(a); return; }
    CHECK(has == (st != NULL));
    b = nora_scenefile_write_string(s2, st ? &st2 : NULL, ".");
    CHECK(b != NULL && strcmp(a, b) == 0);
    if (b && strcmp(a, b) != 0) printf("  %s: second save differs\n--- first\n%s\n--- second\n%s\n", label, a, b);
    /* same geometry and materials after the round trip */
    if (nora_scene_live_mesh_count(s) == s->nmeshes) CHECK(s2->ntris == s->ntris);
    CHECK(nora_scene_live_mesh_count(s2) == nora_scene_live_mesh_count(s));
    nora_scene_build(s);
    nora_scene_build(s2);
    if (nora_scene_live_mesh_count(s) == s->nmeshes) CHECK(nora_scene_hash(s) == nora_scene_hash(s2));
    free(a);
    free(b);
    nora_scene_destroy(s2);
}

static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (f) { fputs(text, f); fclose(f); }
}

void test_scenefile(void) {
    nora_settings st;
    nora_scene *s;
    char err[512];
    nora_settings_default(&st);
    st.width = 320; st.height = 240; st.spp = 128; st.mode = NORA_MODE_SPECTRAL; st.integrator = NORA_INT_PT; st.seed = 42;

    /* Cornell boxes, untouched and with moved / rotated / scaled objects */
    s = nora_scene_cornell(0);
    roundtrip(s, &st, "cornell");
    {
        int tall = -1, i;
        for (i = 0; i < s->nmeshes; i++) if (!strcmp(s->meshes[i].name, "Tall block")) tall = i;
        CHECK(tall >= 0);
        if (tall >= 0) {
            v3 before = nora_mesh_center(&s->meshes[tall]);
            s->meshes[tall].translate = v3_make(-60.5f, 0, 12.25f);
            nora_mesh_set_euler(&s->meshes[tall], v3_make(0, 33.3f, 0));
            s->meshes[tall].scale = v3_make(1, 0.75f, 1);
            nora_scene_apply_transforms(s);
            CHECK_NEAR(nora_mesh_center(&s->meshes[tall]).x, before.x - 60.5f, 1e-3);
            roundtrip(s, &st, "cornell moved");
            roundtrip(s, NULL, "cornell moved, no settings");
        }
    }
    nora_scene_destroy(s);
    s = nora_scene_cornell(1);
    roundtrip(s, &st, "cornell-glass");
    nora_scene_destroy(s);

    /* OBJ with MTL, a texture and an OSL file; plus an inline OSL and an inline graph material */
    {
        char obj[1024], dir[1024];
        snprintf(obj, sizeof obj, "%s/scenes/shaderball.obj", NORA_SOURCE_DIR);
        s = nora_scene_load_obj(obj, err, sizeof err);
        CHECK(s != NULL);
        if (s) {
            int m;
            ng_graph g;
            roundtrip(s, &st, "shaderball.obj");
            m = nora_scene_find_material(s, "Floor");
            CHECK(m >= 0);
            if (m >= 0) {
                int slot = nora_scene_add_shader(s, "inline", "shader inline_test(color c = color(0.2, 0.4, 0.6), output closure color bsdf = 0) { bsdf = c * diffuse(N); }", err, sizeof err);
                CHECK(slot >= 0);
                if (slot >= 0) s->mats[m].shader = slot;
            }
            ng_init(&g);
            ng_default_graph(&g);
            {
                char osl[65536];
                char *json = ng_to_json(&g);
                int slot;
                CHECK(ng_generate_osl(&g, osl, sizeof osl, err, sizeof err) == 0);
                slot = nora_scene_add_shader(s, "graph", osl, err, sizeof err);
                CHECK(slot >= 0 && json != NULL);
                if (slot >= 0 && s->nmats > 1) { s->shaders[slot].graph_json = json; s->mats[1].shader = slot; json = NULL; }
                free(json);
            }
            ng_free(&g);
            s->meshes[0].translate = v3_make(0.1f, 0.2f, 0.3f);
            nora_scene_apply_transforms(s);
            roundtrip(s, &st, "shaderball with inline shaders");
            /* real files: save next to a temporary directory and load from there */
            nora_path_abs("nora_test_scene_dir", dir, sizeof dir);
            CHECK(nora_scenefile_save(s, &st, "nora_test_scene.nora", err, sizeof err) == 0);
            {
                nora_scene *s2 = nora_scenefile_load("nora_test_scene.nora", NULL, NULL, err, sizeof err);
                char deps[16][1024];
                int nd, k, have_obj = 0, have_mtl = 0, have_nora = 0;
                CHECK(s2 != NULL);
                if (!s2) printf("  %s\n", err);
                if (s2) {
                    CHECK(s2->nsources == 1);
                    nd = nora_scene_dependencies(s2, deps, 16);
                    for (k = 0; k < nd; k++) {
                        if (strstr(deps[k], "shaderball.obj")) have_obj = 1;
                        if (strstr(deps[k], "shaderball.mtl")) have_mtl = 1;
                        if (strstr(deps[k], "nora_test_scene.nora")) have_nora = 1;
                    }
                    CHECK(have_obj && have_mtl && have_nora);
                    nora_scene_destroy(s2);
                }
            }
            remove("nora_test_scene.nora");
            nora_scene_destroy(s);
        } else printf("  %s\n", err);
    }

    /* errors */
    CHECK(nora_scenefile_read_string("{\"format\": \"other\"}", ".", NULL, NULL, err, sizeof err) == NULL);
    CHECK(nora_scenefile_read_string("{", ".", NULL, NULL, err, sizeof err) == NULL);
    CHECK(nora_scenefile_read_string("{\"format\": \"nora-scene\", \"version\": 1, \"sources\": [{\"type\": \"obj\", \"path\": \"missing.obj\"}]}", ".", NULL, NULL, err, sizeof err) == NULL);

    /* import merges an OBJ as new objects; export bakes the transforms */
    {
        char obj[1024];
        int first, count = 0, n0;
        snprintf(obj, sizeof obj, "%s/scenes/shaderball.obj", NORA_SOURCE_DIR);
        s = nora_scene_cornell(0);
        n0 = s->nmeshes;
        first = nora_scene_import_obj(s, obj, &count, err, sizeof err);
        CHECK(first == n0 && count > 0);
        CHECK(s->nsources == 2);
        CHECK(nora_scene_find_material(s, "White") >= 0 && nora_scene_find_material(s, "Floor") >= 0);
        if (first >= 0) {
            s->meshes[first].scale = v3_make(80, 80, 80);
            s->meshes[first].translate = v3_make(278, 100, 278);
            nora_scene_apply_transforms(s);
        }
        roundtrip(s, &st, "cornell + imported OBJ");
        CHECK(nora_scene_build(s) == 0);
        CHECK(nora_scene_export_obj(s, "nora_test_export.obj", err, sizeof err) == 0);
        {
            nora_scene *e = nora_scene_load_obj("nora_test_export.obj", err, sizeof err);
            CHECK(e != NULL);
            if (e) {
                v3 a0, a1, b0, b1;
                CHECK(e->ntris == s->ntris);
                CHECK(e->nmeshes == nora_scene_live_mesh_count(s));
                nora_scene_bounds(s, &a0, &a1);
                nora_scene_bounds(e, &b0, &b1);
                CHECK_NEAR(a0.x, b0.x, 1e-2); CHECK_NEAR(a1.y, b1.y, 1e-2); CHECK_NEAR(a1.z, b1.z, 1e-2);
                CHECK(nora_scene_build(e) == 0 && e->nlights == s->nlights);
                nora_scene_destroy(e);
            }
        }
        remove("nora_test_export.obj");
        remove("nora_test_export.mtl");
        nora_scene_destroy(s);
    }
}

void test_edit(void) {
    nora_scene *s = nora_scene_cornell(0);
    nora_history h;
    nora_xform x, orig;
    int tall = 6, dup, nlive, i;
    const char *label = NULL;
    v3 c0;
    nora_history_init(&h);
    CHECK(!strcmp(s->meshes[tall].name, "Tall block"));
    nora_xform_get(&s->meshes[tall], &orig);
    c0 = s->pos[s->meshes[tall].first_pos];
    x = orig;
    x.translate = v3_make(10, 0, 0);
    CHECK(nora_edit_transform(&h, s, tall, &x, "Move") == 0);
    CHECK_NEAR(s->pos[s->meshes[tall].first_pos].x, c0.x + 10, 1e-3);
    x.euler_deg = v3_make(0, 90, 0);
    x.rot = quat_from_euler_deg(x.euler_deg);
    CHECK(nora_edit_transform(&h, s, tall, &x, "Rotate") == 0);
    nlive = nora_scene_live_mesh_count(s);
    dup = nora_edit_duplicate(&h, s, tall);
    CHECK(dup == s->nmeshes - 1 && nora_scene_live_mesh_count(s) == nlive + 1);
    CHECK(!strcmp(s->meshes[dup].name, "Tall block.001"));
    CHECK(nora_edit_delete(&h, s, 0) == 0 && s->meshes[0].hidden);
    CHECK(nora_scene_build(s) == 0);
    /* undo everything */
    CHECK(nora_history_undo(&h, s, &label) == 1 && !strcmp(label, "Delete") && !s->meshes[0].hidden);
    CHECK(nora_history_undo(&h, s, &label) == 1 && !strcmp(label, "Duplicate") && s->meshes[dup].hidden);
    CHECK(nora_history_undo(&h, s, &label) == 1 && !strcmp(label, "Rotate"));
    CHECK(nora_history_undo(&h, s, &label) == 1 && !strcmp(label, "Move"));
    CHECK(nora_history_undo(&h, s, &label) == 0);
    CHECK_NEAR(s->pos[s->meshes[tall].first_pos].x, c0.x, 1e-4);
    {
        nora_xform now;
        nora_xform_get(&s->meshes[tall], &now);
        CHECK(nora_xform_equal(&now, &orig));
    }
    /* redo two, then a new edit drops the redo tail */
    CHECK(nora_history_redo(&h, s, &label) == 1 && !strcmp(label, "Move"));
    CHECK(nora_history_redo(&h, s, &label) == 1 && !strcmp(label, "Rotate"));
    CHECK_NEAR(s->meshes[tall].euler_deg.y, 90, 1e-4);
    CHECK(nora_history_can_redo(&h));
    CHECK(nora_edit_delete(&h, s, 1) == 0);
    CHECK(!nora_history_can_redo(&h));
    /* quaternion <-> Euler */
    for (i = 0; i < 20; i++) {
        v3 e = v3_make((float)(i * 37 % 170) - 85.0f, (float)(i * 53 % 170) - 85.0f, (float)(i * 71 % 340) - 170.0f);
        v3 e2 = quat_to_euler_deg(quat_from_euler_deg(e));
        CHECK_NEAR(e2.x, e.x, 1e-2); CHECK_NEAR(e2.y, e.y, 1e-2); CHECK_NEAR(e2.z, e.z, 1e-2);
    }
    nora_history_free(&h);
    nora_scene_destroy(s);
}

/* fake file system for the watch test */
typedef struct fakefs { const char *names[4]; nora_fstat st[4]; int calls; } fakefs;
static int fake_stat(const char *path, nora_fstat *st, void *user) {
    fakefs *f = (fakefs *)user;
    int i;
    f->calls++;
    for (i = 0; i < 4; i++)
        if (f->names[i] && !strcmp(f->names[i], path)) { *st = f->st[i]; return f->st[i].exists ? 0 : -1; }
    return -1;
}

void test_watch(void) {
    nora_watch w;
    fakefs fs;
    char paths[3][1024];
    int changed[4], n;
    memset(&fs, 0, sizeof fs);
    fs.names[0] = "scene.nora"; fs.names[1] = "model.obj"; fs.names[2] = "shader.osl";
    fs.st[0].exists = fs.st[1].exists = fs.st[2].exists = 1;
    fs.st[0].mtime_ns = 100; fs.st[1].mtime_ns = 200; fs.st[2].mtime_ns = 300;
    fs.st[0].size = 10; fs.st[1].size = 20; fs.st[2].size = 30;
    snprintf(paths[0], 1024, "scene.nora"); snprintf(paths[1], 1024, "model.obj"); snprintf(paths[2], 1024, "shader.osl");
    nora_watch_init(&w, 0.5);
    w.stat = fake_stat;
    w.user = &fs;
    nora_watch_set(&w, paths, 3, 10.0);
    CHECK(nora_watch_poll(&w, 10.6, changed, 4) == 0);         /* nothing changed */
    fs.st[1].mtime_ns = 250;                                    /* model.obj touched */
    CHECK(nora_watch_poll(&w, 10.8, changed, 4) == 0);         /* within 500 ms of the last poll: not polled */
    n = nora_watch_poll(&w, 11.2, changed, 4);
    CHECK(n == 1 && changed[0] == 1);
    CHECK(nora_watch_poll(&w, 11.8, changed, 4) == 0);         /* reported once */
    fs.st[2].size = 31;                                         /* same time, new size */
    fs.st[0].exists = 0;                                        /* scene file deleted */
    n = nora_watch_poll(&w, 12.4, changed, 4);
    CHECK(n == 2 && changed[0] == 0 && changed[1] == 2);
    fs.st[0].exists = 1; fs.st[0].mtime_ns = 999;               /* written again */
    n = nora_watch_poll(&w, 13.0, changed, 4);
    CHECK(n == 1 && changed[0] == 0);
    nora_watch_free(&w);

    /* the real file system */
    {
        nora_fstat a, b;
        FILE *f = fopen("nora_test_watch.txt", "w");
        if (f) { fputs("one", f); fclose(f); }
        CHECK(nora_file_stat("nora_test_watch.txt", &a) == 0 && a.exists && a.size == 3);
        f = fopen("nora_test_watch.txt", "w");
        if (f) { fputs("three", f); fclose(f); }
        CHECK(nora_file_stat("nora_test_watch.txt", &b) == 0 && b.size == 5);
        snprintf(paths[0], 1024, "nora_test_watch.txt");
        nora_watch_init(&w, 0.5);
        nora_watch_set(&w, paths, 1, 0.0);
        CHECK(nora_watch_poll(&w, 1.0, changed, 4) == 0);
        f = fopen("nora_test_watch.txt", "w");
        if (f) { fputs("changed!", f); fclose(f); }
        CHECK(nora_watch_poll(&w, 2.0, changed, 4) == 1);
        remove("nora_test_watch.txt");
        CHECK(nora_watch_poll(&w, 3.0, changed, 4) == 1);
        CHECK(nora_file_stat("nora_test_watch.txt", &a) != 0);
        nora_watch_free(&w);
    }
    /* paths */
    {
        char out[1024];
        nora_path_rel("/a/b/scenes/x.obj", "/a/b/out", out, sizeof out);
        CHECK(!strcmp(out, "../scenes/x.obj"));
        nora_path_rel("/a/b/x.obj", "/a/b", out, sizeof out);
        CHECK(!strcmp(out, "x.obj"));
        nora_path_abs("/a/./b/../c//d.txt", out, sizeof out);
        CHECK(!strcmp(out, "/a/c/d.txt"));
    }
}

void test_raster(void) {
    nora_scene *s = nora_scene_cornell(0);
    const int W = 96, H = 96;
    unsigned char *rgba = (unsigned char *)malloc((size_t)W * H * 4);
    float *depth = (float *)malloc(sizeof(float) * W * H);
    int *ids = (int *)malloc(sizeof(int) * W * H);
    nora_raster_opts o;
    int drawn, back = 2, tall = 6, i, cnt = 0;
    float px, py;
    memset(&o, 0, sizeof o);
    o.selected = -1;
    drawn = nora_raster(s, &s->cam, W, H, &o, rgba, depth, ids);
    CHECK(drawn == s->ntris);
    /* the centre of the image sees the back wall; the tall block's centre projects onto it */
    CHECK(ids[(H / 2) * W + W / 2] == back || ids[(H / 2) * W + W / 2] == tall);
    CHECK(nora_project(&s->cam, W, H, nora_mesh_center(&s->meshes[tall]), &px, &py, NULL));
    CHECK(ids[(int)py * W + (int)px] == tall);
    for (i = 0; i < W * H; i++) if (ids[i] < 0) cnt++;
    CHECK(cnt < W * H / 2);   /* the box fills most of the frame */
    /* moving the block moves its pixels; hiding it reveals what is behind */
    s->meshes[tall].hidden = 1;
    nora_raster(s, &s->cam, W, H, &o, rgba, depth, ids);
    CHECK(ids[(int)py * W + (int)px] != tall);
    s->meshes[tall].hidden = 0;
    o.wireframe = 1;
    o.selected = tall;
    CHECK(nora_raster(s, &s->cam, W, H, &o, rgba, NULL, NULL) == s->ntris);
    /* camera inside the box: near-plane clipping keeps it drawable */
    {
        nora_camera c = s->cam;
        c.pos = v3_make(100, 400, 100);    /* left of the tall block, above the short one */
        c.target = v3_make(100, 400, 559);
        nora_raster(s, &c, W, H, &o, rgba, depth, ids);
        CHECK(ids[(H / 2) * W + W / 2] == back);
    }
    free(rgba); free(depth); free(ids);
    nora_scene_destroy(s);
}

static double rel_diff(const float *a, const float *b, int n) {
    double d = 0, t = 0;
    int i;
    for (i = 0; i < n; i++) { d += fabs((double)a[i] - b[i]); t += fabs((double)b[i]); }
    return t > 0 ? d / t : d;
}
static double mean(const float *a, int n) { double s = 0; int i; for (i = 0; i < n; i++) s += a[i]; return s / n; }

void test_checkpoint(void) {
    nora_scene *s = nora_scene_cornell(0);
    nora_settings st;
    const int N = 4, M = 6;
    float *direct, *resumed, *other;
    nora_render *r;
    uint64_t hash;
    char err[512];
    int n;
    nora_settings_default(&st);
    st.width = 48; st.height = 48; st.bootstrap = 4000; st.threads = 2; st.chains = 16;
    n = st.width * st.height * 3;
    direct = (float *)malloc(sizeof(float) * n);
    resumed = (float *)malloc(sizeof(float) * n);
    other = (float *)malloc(sizeof(float) * n);
    CHECK(nora_scene_build(s) == 0);
    hash = nora_scene_hash(s);

    /* MLT: N + M directly */
    st.spp = N + M;
    CHECK(nora_render_blocking(s, &st, direct, NULL) == 0);
    /* MLT: N, checkpoint, then M more in a fresh renderer */
    st.spp = N;
    r = nora_render_create(s, &st);
    CHECK(r != NULL);
    nora_render_start(r);
    nora_render_wait(r);
    CHECK(nora_render_pause(r) == 0);
    CHECK(nora_render_save_checkpoint(r, hash, "nora_test.nrck", err, sizeof err) == 0);
    nora_render_destroy(r);
    r = nora_render_load_checkpoint(s, hash, "nora_test.nrck", 2, err, sizeof err);
    CHECK(r != NULL);
    if (r) {
        nora_render_stats rs;
        CHECK(nora_render_resume(r) == 1);            /* nothing left at N */
        CHECK(nora_render_set_spp(r, N + M) == 0);
        CHECK(nora_render_resume(r) == 0);
        nora_render_wait(r);
        nora_render_get_stats(r, &rs);
        CHECK(rs.samples == (int64_t)(N + M) * st.width * st.height);
        nora_render_resolve(r, resumed);
        nora_render_destroy(r);
        /* the chains continue exactly: equal up to float summation order */
        CHECK(rel_diff(resumed, direct, n) < 1e-3);
        CHECK_NEAR(mean(resumed, n), mean(direct, n), 1e-3 * mean(direct, n) + 1e-6);
    } else printf("  %s\n", err);
    /* a statistically independent render (other seed) is much further away, so the
     * tolerance above is meaningful, yet the same in expectation */
    st.spp = N + M;
    st.seed = 99;
    CHECK(nora_render_blocking(s, &st, other, NULL) == 0);
    CHECK(rel_diff(other, direct, n) > 1e-2);
    CHECK_NEAR(mean(other, n), mean(direct, n), 0.35 * mean(direct, n));   /* tiny bootstrap: b varies */
    st.seed = 1;

    /* pause in the middle (time based), resume in memory: same total, same result */
    st.spp = 40;
    CHECK(nora_render_blocking(s, &st, direct, NULL) == 0);
    r = nora_render_create(s, &st);
    if (r) {
        int pauses = 0;
        nora_render_start(r);
        while (nora_render_is_running(r) && pauses < 50) {
            nsleep_ms(5);
            nora_render_pause(r);
            pauses++;
            if (pauses == 3) {
                /* checkpoint mid-render and continue from it in another renderer */
                nora_render *r2;
                CHECK(nora_render_save_checkpoint(r, hash, "nora_test.nrck", err, sizeof err) == 0);
                r2 = nora_render_load_checkpoint(s, hash, "nora_test.nrck", 3, err, sizeof err);
                CHECK(r2 != NULL);
                if (r2) {
                    CHECK(nora_render_is_paused(r2) || !nora_render_can_resume(r2));
                    nora_render_resume(r2);
                    nora_render_wait(r2);
                    nora_render_resolve(r2, other);
                    CHECK(rel_diff(other, direct, n) < 1e-3);
                    nora_render_destroy(r2);
                }
            }
            nora_render_resume(r);
        }
        nora_render_wait(r);
        nora_render_resolve(r, resumed);
        CHECK(rel_diff(resumed, direct, n) < 1e-3);
        nora_render_destroy(r);
    }

    /* PT checkpoints */
    st.integrator = NORA_INT_PT;
    st.spp = 6;
    CHECK(nora_render_blocking(s, &st, direct, NULL) == 0);
    st.spp = 2;
    r = nora_render_create(s, &st);
    if (r) {
        nora_render_start(r);
        nora_render_wait(r);
        CHECK(nora_render_save_checkpoint(r, hash, "nora_test.nrck", err, sizeof err) == 0);
        nora_render_destroy(r);
    }
    r = nora_render_load_checkpoint(s, hash, "nora_test.nrck", 2, err, sizeof err);
    CHECK(r != NULL);
    if (r) {
        nora_render_set_spp(r, 6);
        nora_render_resume(r);
        nora_render_wait(r);
        nora_render_resolve(r, resumed);
        CHECK(rel_diff(resumed, direct, n) < 1e-4);
        nora_render_destroy(r);
    }

    /* a different scene is refused */
    s->meshes[6].translate = v3_make(5, 0, 0);
    CHECK(nora_scene_build(s) == 0);
    CHECK(nora_scene_hash(s) != hash);
    r = nora_render_load_checkpoint(s, nora_scene_hash(s), "nora_test.nrck", 2, err, sizeof err);
    CHECK(r == NULL && strstr(err, "different scene") != NULL);
    nora_render_destroy(r);
    /* garbage is refused */
    write_text("nora_test_bad.nrck", "NRCK garbage");
    CHECK(nora_render_load_checkpoint(s, nora_scene_hash(s), "nora_test_bad.nrck", 2, err, sizeof err) == NULL);
    remove("nora_test_bad.nrck");
    remove("nora_test.nrck");
    free(direct); free(resumed); free(other);
    nora_scene_destroy(s);
}
