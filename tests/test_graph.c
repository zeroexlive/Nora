/* Nora - shader graph tests: links, OSL generation, JSON round trip. MIT License. */
#include "test.h"
#include "graph.h"
#include "osl.h"
#include <stdlib.h>
#include <string.h>

#ifndef NORA_SOURCE_DIR
#define NORA_SOURCE_DIR "."
#endif

static int compiles(const char *src, osl_closure_list *cl) {
    char err[512];
    osl_program *p = osl_compile(src, err, sizeof err);
    osl_exec *e;
    osl_globals g;
    int rc;
    if (!p) { printf("  generated OSL does not compile: %s\n%s\n", err, src); return 0; }
    e = osl_exec_create();
    memset(&g, 0, sizeof g);
    g.N[2] = g.Ng[2] = 1; g.I[2] = -1; g.P[0] = 3.3f; g.P[1] = 1.7f;
    rc = osl_execute(p, e, &g, NULL, NULL, 0, cl);
    osl_exec_free(e);
    osl_program_free(p);
    return rc == 0;
}

void test_graph(void) {
    ng_graph g, h;
    static char src[65536], src2[65536];
    char err[256], *json;
    osl_closure_list cl;
    int i, t, out, dif, val, rgb, mix;
    ng_init(&g);
    ng_init(&h);
    /* catalogue */
    CHECK(ng_type_count() >= 17);
    CHECK(ng_type_by_id("diffuse") >= 0 && ng_type_by_id("nope") < 0);
    /* default graph generates OSL that compiles and gives two lobes */
    ng_default_graph(&g);
    CHECK(ng_generate_osl(&g, src, sizeof src, err, sizeof err) == 0);
    CHECK(strstr(src, "surface nora_graph") != NULL);
    CHECK(compiles(src, &cl));
    CHECK(cl.n == 2);
    /* JSON round trip reproduces the same OSL */
    json = ng_to_json(&g);
    CHECK(json != NULL);
    CHECK(ng_from_json(&h, json, err, sizeof err) == 0);
    CHECK(h.nnodes == g.nnodes && h.nlinks == g.nlinks);
    CHECK(ng_generate_osl(&h, src2, sizeof src2, err, sizeof err) == 0);
    CHECK(strcmp(src, src2) == 0);
    free(json);
    /* links: type checking, replacement, removal */
    ng_clear(&g);
    out = ng_add_node(&g, ng_type_by_id("output"), 0, 0);
    dif = ng_add_node(&g, ng_type_by_id("diffuse"), 0, 0);
    val = ng_add_node(&g, ng_type_by_id("value"), 0, 0);
    rgb = ng_add_node(&g, ng_type_by_id("rgb"), 0, 0);
    mix = ng_add_node(&g, ng_type_by_id("mix"), 0, 0);
    CHECK(ng_add_link(&g, ng_pin_id(val, 0, 1), ng_pin_id(dif, 0, 0)) < 0);   /* float -> color: rejected */
    CHECK(ng_add_link(&g, ng_pin_id(rgb, 0, 1), ng_pin_id(dif, 0, 0)) > 0);
    CHECK(ng_add_link(&g, ng_pin_id(mix, 0, 1), ng_pin_id(dif, 0, 0)) > 0);   /* replaces the rgb link */
    CHECK(g.nlinks == 1);
    CHECK(ng_add_link(&g, ng_pin_id(dif, 0, 1), ng_pin_id(out, 0, 0)) > 0);
    CHECK(ng_add_link(&g, ng_pin_id(out, 0, 0), ng_pin_id(dif, 0, 1)) > 0);   /* reversed arguments are accepted */
    CHECK(g.nlinks == 2);
    CHECK(ng_add_link(&g, ng_pin_id(val, 0, 1), ng_pin_id(mix, 0, 0)) > 0);
    CHECK(ng_generate_osl(&g, src, sizeof src, err, sizeof err) == 0);
    CHECK(compiles(src, &cl));
    CHECK(cl.n == 1);
    ng_remove_node(&g, mix);
    CHECK(g.nlinks == 1);   /* links into and out of the removed node are gone */
    /* every node type generates code that compiles when wired to the output */
    for (t = 0; t < ng_type_count(); t++) {
        const ng_node_def *d = ng_def(t);
        int n, o, sh, k;
        if (!strcmp(d->id, "output")) continue;
        ng_clear(&g);
        o = ng_add_node(&g, ng_type_by_id("output"), 0, 0);
        n = ng_add_node(&g, t, 0, 0);
        if (!strcmp(d->id, "image")) strcpy(ng_find(&g, n)->text, "grid.png");
        if (d->out[0].type == NG_PIN_CLOSURE) {
            sh = n;
            /* feed closure inputs with diffuse nodes */
            for (k = 0; k < d->nin; k++) if (d->in[k].type == NG_PIN_CLOSURE) {
                int df = ng_add_node(&g, ng_type_by_id("diffuse"), 0, 0);
                CHECK(ng_add_link(&g, ng_pin_id(df, 0, 1), ng_pin_id(n, k, 0)) > 0);
            }
        } else {
            /* route the first output into a diffuse/emission shader */
            sh = ng_add_node(&g, ng_type_by_id(d->out[0].type == NG_PIN_COLOR ? "diffuse" : "emission"), 0, 0);
            CHECK(ng_add_link(&g, ng_pin_id(n, 0, 1), ng_pin_id(sh, d->out[0].type == NG_PIN_COLOR ? 0 : 1, 0)) > 0);
        }
        CHECK(ng_add_link(&g, ng_pin_id(sh, 0, 1), ng_pin_id(o, 0, 0)) > 0);
        for (k = 0; k < (d->nops > 0 ? d->nops : 1); k++) {
            ng_find(&g, n)->op = k;
            CHECK(ng_generate_osl(&g, src, sizeof src, err, sizeof err) == 0);
            if (!compiles(src, &cl)) { printf("  node type %s op %d\n", d->id, k); CHECK(0); }
        }
    }
    /* cycles are reported */
    ng_clear(&g);
    out = ng_add_node(&g, ng_type_by_id("output"), 0, 0);
    i = ng_add_node(&g, ng_type_by_id("add_shader"), 0, 0);
    t = ng_add_node(&g, ng_type_by_id("add_shader"), 0, 0);
    ng_add_link(&g, ng_pin_id(i, 0, 1), ng_pin_id(t, 0, 0));
    ng_add_link(&g, ng_pin_id(t, 0, 1), ng_pin_id(i, 0, 0));
    ng_add_link(&g, ng_pin_id(i, 0, 1), ng_pin_id(out, 0, 0));
    CHECK(ng_generate_osl(&g, src, sizeof src, err, sizeof err) != 0);
    CHECK(strstr(err, "cycle") != NULL);
    /* missing output */
    ng_clear(&g);
    ng_add_node(&g, ng_type_by_id("diffuse"), 0, 0);
    CHECK(ng_generate_osl(&g, src, sizeof src, err, sizeof err) != 0);
    /* bad JSON is rejected with a message */
    CHECK(ng_from_json(&g, "{ \"format\": \"nora-shader-graph\", \"nodes\": [ { \"type\": \"bogus\" } ] }", err, sizeof err) != 0);
    CHECK(strstr(err, "bogus") != NULL);
    CHECK(ng_from_json(&g, "{ \"format\": ", err, sizeof err) != 0);
    /* shipped example graphs load and compile */
    {
        static const char *const names[] = { "copper.json", "noisy_plastic.json" };
        for (i = 0; i < 2; i++) {
            char path[1024];
            snprintf(path, sizeof path, "%s/examples/graphs/%s", NORA_SOURCE_DIR, names[i]);
            CHECK(ng_load_file(&g, path, err, sizeof err) == 0);
            CHECK(ng_generate_osl(&g, src, sizeof src, err, sizeof err) == 0);
            CHECK(compiles(src, &cl));
            CHECK(cl.n >= 1);
        }
    }
    /* save to a file and load it back */
    ng_default_graph(&g);
    CHECK(ng_save_file(&g, "nora_test_graph.json") == 0);
    CHECK(ng_load_file(&h, "nora_test_graph.json", err, sizeof err) == 0);
    CHECK(h.nnodes == g.nnodes);
    remove("nora_test_graph.json");
    ng_free(&g);
    ng_free(&h);
}
