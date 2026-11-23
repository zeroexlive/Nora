/* Nora - built-in Cornell box (geometry after the Cornell University
 * measurements; reflectances and light as commonly used in research renderers).
 * MIT License. */
#include "scene.h"
#include <string.h>
#include <stdlib.h>

static void add_quad(nora_scene *s, int mesh, int mat, const float q[4][3]) {
    int b = s->npos, i;
    nora_tri t;
    for (i = 0; i < 4; i++) nora_scene_add_pos(s, v3_make(q[i][0], q[i][1], q[i][2]));
    memset(&t, 0, sizeof t);
    t.mat = mat; t.mesh = mesh;
    t.n[0] = t.n[1] = t.n[2] = -1;
    t.t[0] = t.t[1] = t.t[2] = -1;
    t.v[0] = b; t.v[1] = b + 1; t.v[2] = b + 2;
    nora_scene_add_tri(s, &t);
    t.v[0] = b; t.v[1] = b + 2; t.v[2] = b + 3;
    nora_scene_add_tri(s, &t);
}

static int begin_mesh(nora_scene *s, const char *name) { return nora_scene_add_mesh(s, name, s->ntris, 0); }
static void end_mesh(nora_scene *s, int m) { s->meshes[m].tri_count = s->ntris - s->meshes[m].first_tri; }

/* icosphere with smooth normals */
static void add_sphere(nora_scene *s, int mesh, int mat, v3 c, float radius, int subdiv) {
    const float t = 1.6180339887f;
    float vv[12][3] = { { -1, t, 0 }, { 1, t, 0 }, { -1, -t, 0 }, { 1, -t, 0 }, { 0, -1, t }, { 0, 1, t },
                        { 0, -1, -t }, { 0, 1, -t }, { t, 0, -1 }, { t, 0, 1 }, { -t, 0, -1 }, { -t, 0, 1 } };
    int ff[20][3] = { { 0, 11, 5 }, { 0, 5, 1 }, { 0, 1, 7 }, { 0, 7, 10 }, { 0, 10, 11 }, { 1, 5, 9 }, { 5, 11, 4 },
                      { 11, 10, 2 }, { 10, 7, 6 }, { 7, 1, 8 }, { 3, 9, 4 }, { 3, 4, 2 }, { 3, 2, 6 }, { 3, 6, 8 },
                      { 3, 8, 9 }, { 4, 9, 5 }, { 2, 4, 11 }, { 6, 2, 10 }, { 8, 6, 7 }, { 9, 8, 1 } };
    int ntri = 20, i, lvl;
    v3 *tris = (v3 *)malloc(sizeof(v3) * 3 * 20);
    for (i = 0; i < 20; i++) {
        int k;
        for (k = 0; k < 3; k++) tris[i * 3 + k] = v3_norm(v3_make(vv[ff[i][k]][0], vv[ff[i][k]][1], vv[ff[i][k]][2]));
    }
    for (lvl = 0; lvl < subdiv; lvl++) {
        v3 *nt = (v3 *)malloc(sizeof(v3) * 3 * (size_t)ntri * 4);
        for (i = 0; i < ntri; i++) {
            v3 a = tris[i * 3], b = tris[i * 3 + 1], cc = tris[i * 3 + 2];
            v3 ab = v3_norm(v3_add(a, b)), bc = v3_norm(v3_add(b, cc)), ca = v3_norm(v3_add(cc, a));
            v3 *o = nt + (size_t)i * 12;
            o[0] = a; o[1] = ab; o[2] = ca;
            o[3] = b; o[4] = bc; o[5] = ab;
            o[6] = cc; o[7] = ca; o[8] = bc;
            o[9] = ab; o[10] = bc; o[11] = ca;
        }
        free(tris);
        tris = nt;
        ntri *= 4;
    }
    for (i = 0; i < ntri; i++) {
        nora_tri tr;
        int k;
        memset(&tr, 0, sizeof tr);
        tr.mat = mat; tr.mesh = mesh;
        for (k = 0; k < 3; k++) {
            v3 n = tris[i * 3 + k];
            tr.v[k] = nora_scene_add_pos(s, v3_add(c, v3_scale(n, radius)));
            tr.n[k] = nora_scene_add_nrm(s, n);
            tr.t[k] = -1;
        }
        nora_scene_add_tri(s, &tr);
    }
    free(tris);
}

nora_scene *nora_scene_cornell(int variant) {
    static const float floor_q[4][3] = { { 552.8f, 0, 0 }, { 0, 0, 0 }, { 0, 0, 559.2f }, { 549.6f, 0, 559.2f } };
    static const float ceil_q[4][3] = { { 556, 548.8f, 0 }, { 556, 548.8f, 559.2f }, { 0, 548.8f, 559.2f }, { 0, 548.8f, 0 } };
    static const float back_q[4][3] = { { 549.6f, 0, 559.2f }, { 0, 0, 559.2f }, { 0, 548.8f, 559.2f }, { 556, 548.8f, 559.2f } };
    static const float right_q[4][3] = { { 0, 0, 559.2f }, { 0, 0, 0 }, { 0, 548.8f, 0 }, { 0, 548.8f, 559.2f } };
    static const float left_q[4][3] = { { 552.8f, 0, 0 }, { 549.6f, 0, 559.2f }, { 556, 548.8f, 559.2f }, { 556, 548.8f, 0 } };
    static const float light_q[4][3] = { { 343, 548.7f, 227 }, { 343, 548.7f, 332 }, { 213, 548.7f, 332 }, { 213, 548.7f, 227 } };
    static const float short_q[5][4][3] = {
        { { 130, 165, 65 }, { 82, 165, 225 }, { 240, 165, 272 }, { 290, 165, 114 } },
        { { 290, 0, 114 }, { 290, 165, 114 }, { 240, 165, 272 }, { 240, 0, 272 } },
        { { 130, 0, 65 }, { 130, 165, 65 }, { 290, 165, 114 }, { 290, 0, 114 } },
        { { 82, 0, 225 }, { 82, 165, 225 }, { 130, 165, 65 }, { 130, 0, 65 } },
        { { 240, 0, 272 }, { 240, 165, 272 }, { 82, 165, 225 }, { 82, 0, 225 } } };
    static const float tall_q[5][4][3] = {
        { { 423, 330, 247 }, { 265, 330, 296 }, { 314, 330, 456 }, { 472, 330, 406 } },
        { { 423, 0, 247 }, { 423, 330, 247 }, { 472, 330, 406 }, { 472, 0, 406 } },
        { { 472, 0, 406 }, { 472, 330, 406 }, { 314, 330, 456 }, { 314, 0, 456 } },
        { { 314, 0, 456 }, { 314, 330, 456 }, { 265, 330, 296 }, { 265, 0, 296 } },
        { { 265, 0, 296 }, { 265, 330, 296 }, { 423, 330, 247 }, { 423, 0, 247 } } };
    nora_scene *s = nora_scene_create();
    nora_material m;
    int white, red, green, light, glass, mesh, i;
    if (!s) return NULL;
    strcpy(s->name, variant ? "Cornell box (glass)" : "Cornell box");
    nora_material_default(&m, "White"); m.color = v3_make(0.725f, 0.71f, 0.68f); white = nora_scene_add_material(s, &m);
    nora_material_default(&m, "Red"); m.color = v3_make(0.63f, 0.065f, 0.05f); red = nora_scene_add_material(s, &m);
    nora_material_default(&m, "Green"); m.color = v3_make(0.14f, 0.45f, 0.091f); green = nora_scene_add_material(s, &m);
    nora_material_default(&m, "Light"); m.color = v3_make(0.78f, 0.78f, 0.78f);
    m.emission = v3_make(17.0f, 12.0f, 4.0f); m.emission_strength = 1.0f; light = nora_scene_add_material(s, &m);
    nora_material_default(&m, "Glass"); m.type = NORA_MAT_DIELECTRIC; m.color = v3_make(1, 1, 1);
    m.ior = 1.6f; m.abbe = 18.0f; glass = nora_scene_add_material(s, &m);

    mesh = begin_mesh(s, "Floor"); add_quad(s, mesh, white, floor_q); end_mesh(s, mesh);
    mesh = begin_mesh(s, "Ceiling"); add_quad(s, mesh, white, ceil_q); end_mesh(s, mesh);
    mesh = begin_mesh(s, "Back wall"); add_quad(s, mesh, white, back_q); end_mesh(s, mesh);
    mesh = begin_mesh(s, "Green wall"); add_quad(s, mesh, green, right_q); end_mesh(s, mesh);
    mesh = begin_mesh(s, "Red wall"); add_quad(s, mesh, red, left_q); end_mesh(s, mesh);
    mesh = begin_mesh(s, "Light"); add_quad(s, mesh, light, light_q); end_mesh(s, mesh);
    mesh = begin_mesh(s, "Tall block");
    for (i = 0; i < 5; i++) add_quad(s, mesh, white, tall_q[i]);
    end_mesh(s, mesh);
    if (variant == 0) {
        mesh = begin_mesh(s, "Short block");
        for (i = 0; i < 5; i++) add_quad(s, mesh, white, short_q[i]);
        end_mesh(s, mesh);
    } else {
        mesh = begin_mesh(s, "Glass sphere");
        add_sphere(s, mesh, glass, v3_make(186.0f, 100.0f, 169.0f), 100.0f, 3);
        end_mesh(s, mesh);
    }
    s->cam.pos = v3_make(278, 273, -800);
    s->cam.target = v3_make(278, 273, 0);
    s->cam.up = v3_make(0, 1, 0);
    s->cam.vfov_deg = 39.3077f;
    s->cam.aperture = 0.0f;
    s->cam.focus_dist = 0.0f;
    (void)glass;
    nora_scene_add_source(s, variant ? NORA_SRC_CORNELL_GLASS : NORA_SRC_CORNELL, NULL);
    for (i = 0; i < s->nmeshes; i++) { s->meshes[i].source = 0; s->meshes[i].source_mesh = i; }
    nora_scene_own_vertices(s);
    return s;
}
