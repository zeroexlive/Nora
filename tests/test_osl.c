/* Nora - tests for the built-in OSL subset compiler and VM. MIT License. */
#include "test.h"
#include "osl.h"
#include <stdlib.h>
#include <string.h>

#ifndef NORA_SOURCE_DIR
#define NORA_SOURCE_DIR "."
#endif

static osl_exec *g_ex;
static osl_closure_list g_cl;

static int tex_cb(void *user, const char *name, float s, float t, float rgb[3]) {
    (void)user;
    if (strcmp(name, "ramp.png") != 0) return 0;
    rgb[0] = s; rgb[1] = t; rgb[2] = 0.25f;
    return 1;
}

/* compile + run; returns program (caller frees) or NULL */
static osl_program *run(const char *src, int backfacing) {
    char err[512];
    osl_program *p = osl_compile(src, err, sizeof err);
    osl_globals g;
    osl_host h;
    if (!p) { printf("  compile error: %s\n  in: %s\n", err, src); return NULL; }
    memset(&g, 0, sizeof g);
    g.P[0] = 1.5f; g.P[1] = -2.0f; g.P[2] = 0.25f;
    g.N[2] = 1.0f; g.Ng[2] = 1.0f;
    g.I[2] = -1.0f;
    g.u = 0.25f; g.v = 0.75f;
    g.backfacing = backfacing;
    h.texture = tex_cb; h.user = NULL;
    if (osl_execute(p, g_ex, &g, &h, NULL, 0, &g_cl) != 0) { printf("  runtime error: %s\n", osl_exec_error(g_ex)); osl_program_free(p); return NULL; }
    return p;
}

static float outf(osl_program *p, const char *name) {
    float v[3] = { -999, -999, -999 };
    if (osl_exec_read_param(p, g_ex, name, v) != 0) printf("  no param %s\n", name);
    return v[0];
}
static void out3(osl_program *p, const char *name, float v[3]) {
    if (osl_exec_read_param(p, g_ex, name, v) != 0) printf("  no param %s\n", name);
}

static void expect_float(const char *body, double want, double eps) {
    char src[2048];
    osl_program *p;
    snprintf(src, sizeof src, "surface t(output float r = 0) {\n%s\n}\n", body);
    p = run(src, 0);
    CHECK(p != NULL);
    if (!p) return;
    CHECK_NEAR(outf(p, "r"), want, eps);
    osl_program_free(p);
}

static void expect_error(const char *src, const char *fragment) {
    char err[512];
    osl_program *p = osl_compile(src, err, sizeof err);
    CHECK(p == NULL);
    if (p) { osl_program_free(p); printf("  expected an error for: %s\n", src); return; }
    if (!strstr(err, fragment)) { printf("  error '%s' does not mention '%s'\n", err, fragment); CHECK(0); }
    else CHECK(1);
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    long n;
    char *b;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    b = (char *)malloc((size_t)n + 1);
    if (b) b[fread(b, 1, (size_t)n, f)] = 0;
    fclose(f);
    return b;
}

void test_osl(void) {
    osl_program *p;
    float v[3];
    int i;
    g_ex = osl_exec_create();

    /* arithmetic, precedence, ints */
    expect_float("r = 1 + 2 * 3 - 4 / 2;", 5, 1e-6);
    expect_float("r = 7 / 2;", 3, 1e-6);
    expect_float("r = 7.0 / 2;", 3.5, 1e-6);
    expect_float("r = 7 % 3 + -2;", -1, 1e-6);
    expect_float("int i = 2.9; r = i;", 2, 1e-6);
    expect_float("float x = 2; x *= 3; x -= 1; x /= 2; r = x;", 2.5, 1e-6);
    expect_float("r = (1 < 2) + (2 <= 1) * 10 + (3 == 3) * 100 + (1 != 1) * 1000;", 101, 1e-6);
    expect_float("r = (1 && 0) + (1 || 0) * 2 + !0 * 4;", 6, 1e-6);
    expect_float("float a = 1; r = (a > 0) ? 10 : 20;", 10, 1e-6);
    expect_float("int i = 0; r = i++ + i++ + ++i;", 0 + 1 + 3, 1e-6);
    expect_float("r = M_PI;", 3.14159265, 1e-5);
    /* control flow */
    expect_float("for (int i = 0; i < 10; i++) { if (i == 3) continue; if (i == 6) break; r += i; }", 0 + 1 + 2 + 4 + 5, 1e-6);
    expect_float("int i = 0; while (i < 5) { i++; r += 2; }", 10, 1e-6);
    expect_float("int i = 0; do { r += 1; i++; } while (i < 3);", 3, 1e-6);
    expect_float("if (r == 0) r = 1; else r = 2;", 1, 1e-6);
    expect_float("for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { if (j == 1) break; r += 1; }", 3, 1e-6);
    /* triples */
    expect_float("color c = color(1, 2, 3) * 2; c[1] = 0.5; r = c[0] + c[1] + c[2];", 2 + 0.5 + 6, 1e-6);
    expect_float("vector a = vector(1, 0, 0), b = vector(0, 1, 0); r = dot(cross(a, b), vector(0, 0, 1));", 1, 1e-6);
    expect_float("r = length(normalize(vector(3, 4, 12)));", 1, 1e-5);
    expect_float("r = distance(point(0, 0, 0), point(3, 4, 0));", 5, 1e-5);
    expect_float("vector d = reflect(vector(1, -1, 0), vector(0, 1, 0)); r = d[1];", 1, 1e-6);
    expect_float("r = luminance(color(1, 1, 1));", 1, 1e-4);
    expect_float("color c = abs(color(-1, 2, -3)); r = c[0] + c[1] + c[2];", 6, 1e-6);
    expect_float("color c = max(color(0.2, 0.8, 0.5), 0.6); r = c[0] + c[1] + c[2];", 0.6 + 0.8 + 0.6, 1e-6);
    expect_float("int k = 2; color c = color(1, 2, 3); r = c[k];", 3, 1e-6);
    expect_float("point p = point(1, 1, 1); point q = point(0, 1, 0); vector d = p - q; r = d[0] + d[2];", 2, 1e-6);
    expect_float("color c = 0.5; r = c[2];", 0.5, 1e-6);
    expect_float("color c = (1 > 0) ? color(1, 2, 3) : 0.5; r = c[1];", 2, 1e-6);
    expect_float("color c = (1 < 0) ? color(1, 2, 3) : 0.5; r = c[1];", 0.5, 1e-6);
    expect_float("color c = color(\"hsv\", 0, 1, 1); r = c[0] + c[1] * 10;", 1, 1e-5);
    /* math library */
    expect_float("r = mix(1.0, 3.0, 0.25);", 1.5, 1e-6);
    expect_float("r = clamp(5, 0, 1) + clamp(-1, 0, 1);", 1, 1e-6);
    expect_float("r = smoothstep(0, 1, 0.5);", 0.5, 1e-6);
    expect_float("r = pow(2, 10) + sqrt(16) + floor(-1.5) + ceil(1.2);", 1024 + 4 - 2 + 2, 1e-4);
    expect_float("r = mod(-1, 3) + fmod(7, 4) + step(0.5, 0.7);", 2 + 3 + 1, 1e-5);
    expect_float("r = sin(M_PI / 2) + cos(0) + atan2(1, 1) * 4 / M_PI;", 3, 1e-5);
    expect_float("r = hypot(3, 4) + min(2, 1) + max(2, 1);", 8, 1e-5);
    expect_float("r = radians(180) - M_PI + degrees(M_PI);", 180, 1e-3);
    expect_float("r = sign(-3) + exp(0) + log(1) + exp2(3) + log2(8);", -1 + 1 + 0 + 8 + 3, 1e-5);
    expect_float("color c = mix(color(0), color(1, 2, 4), 0.5); r = c[2];", 2, 1e-6);
    expect_float("r = backfacing();", 0, 1e-6);
    expect_float("r = transform(\"world\", point(1, 2, 3))[1];", 2, 1e-6);
    /* user functions */
    expect_float("r = 0;", 0, 0);
    {
        p = run("float sq(float x) { return x * x; }\n"
                "void twice(output float a) { a *= 2; }\n"
                "color tint(color c, float k) { if (k > 1) return c * k; return c; }\n"
                "surface t(output float r = 0, output color c = 0) { r = sq(3) + sq(sq(2)); twice(r); c = tint(color(1, 2, 3), 2); }\n", 0);
        CHECK(p != NULL);
        if (p) {
            CHECK_NEAR(outf(p, "r"), (9 + 16) * 2, 1e-6);
            out3(p, "c", v);
            CHECK_NEAR(v[2], 6, 1e-6);
            osl_program_free(p);
        }
    }
    /* globals */
    p = run("surface t(output float r = 0, output color n = 0) { r = u * 10 + v + P[1]; n = N; }", 0);
    CHECK(p != NULL);
    if (p) { CHECK_NEAR(outf(p, "r"), 2.5 + 0.75 - 2.0, 1e-6); out3(p, "n", v); CHECK_NEAR(v[2], 1, 1e-6); osl_program_free(p); }
    p = run("surface t(output float r = 0) { r = backfacing() ? 1 : 2; }", 1);
    CHECK(p != NULL);
    if (p) { CHECK_NEAR(outf(p, "r"), 1, 1e-6); osl_program_free(p); }
    /* parameter defaults may use globals and earlier parameters */
    p = run("surface t(float a = 2, float b = a * 3, normal nn = N, output float r = 0) { r = a + b + nn[2]; }", 0);
    CHECK(p != NULL);
    if (p) {
        osl_globals g;
        osl_override ov;
        CHECK_NEAR(outf(p, "r"), 2 + 6 + 1, 1e-6);
        CHECK(osl_param_count(p) == 4);
        CHECK(osl_find_param(p, "b") == 1);
        CHECK(osl_param_type(p, 2) == OSL_T_NORMAL);
        CHECK(osl_param_is_output(p, 3));
        memset(&g, 0, sizeof g);
        ov.param = 0; ov.v[0] = 10; ov.v[1] = ov.v[2] = 0;
        CHECK(osl_execute(p, g_ex, &g, NULL, &ov, 1, &g_cl) == 0);
        CHECK_NEAR(outf(p, "r"), 10 + 30 + 0, 1e-6);   /* b's default follows the overridden a */
        osl_program_free(p);
    }
    /* noise */
    p = run("surface t(output float lo = 10, output float hi = -10, output float slo = 10, output float shi = -10, output float c1 = 0, output float c2 = 0) {\n"
            "  for (int i = 0; i < 400; i++) { point q = point(i * 0.37, i * 0.11 - 5, i * 0.53);\n"
            "    float n = noise(q); float s = snoise(q);\n"
            "    lo = min(lo, n); hi = max(hi, n); slo = min(slo, s); shi = max(shi, s); }\n"
            "  c1 = cellnoise(point(1.2, 3.3, 4.4)); c2 = noise(\"cell\", point(1.9, 3.1, 4.0)); }", 0);
    CHECK(p != NULL);
    if (p) {
        CHECK(outf(p, "lo") >= 0.0f && outf(p, "hi") <= 1.0f && outf(p, "hi") > outf(p, "lo") + 0.3f);
        CHECK(outf(p, "slo") >= -1.0f && outf(p, "shi") <= 1.0f && outf(p, "slo") < 0.0f && outf(p, "shi") > 0.0f);
        CHECK_NEAR(outf(p, "c1"), outf(p, "c2"), 1e-7);
        osl_program_free(p);
    }
    p = run("surface t(output color c = 0) { c = noise(P * 3.1); }", 0);
    CHECK(p != NULL);
    if (p) { out3(p, "c", v); CHECK(v[0] != v[1] || v[1] != v[2]); osl_program_free(p); }
    /* textures via the host */
    p = run("surface t(output color c = 0, output float f = 0) { c = texture(\"ramp.png\", u, v); f = texture(\"ramp.png\", 0.5, 0.1); }", 0);
    CHECK(p != NULL);
    if (p) {
        out3(p, "c", v);
        CHECK_NEAR(v[0], 0.25, 1e-6); CHECK_NEAR(v[1], 0.75, 1e-6); CHECK_NEAR(v[2], 0.25, 1e-6);
        CHECK_NEAR(outf(p, "f"), 0.5, 1e-6);
        CHECK(osl_program_texture_count(p) == 1);
        CHECK(!strcmp(osl_program_texture_name(p, 0), "ramp.png"));
        osl_program_free(p);
    }
    /* closures */
    p = run("surface t(color base = color(0.8, 0.4, 0.2)) {\n"
            "  closure color spec = microfacet(\"ggx\", N, 0.25, 1.45, 0);\n"
            "  Ci = base * diffuse(N) + 0.5 * spec;\n"
            "  Ci += color(1, 2, 3) * emission();\n"
            "  Ci = Ci * 0.5;\n"
            "}", 0);
    CHECK(p != NULL);
    if (p) {
        int nd = 0, nm = 0, ne = 0;
        CHECK(osl_program_may_emit(p));
        CHECK(g_cl.n == 3);
        for (i = 0; i < g_cl.n; i++) {
            const osl_closure *c = &g_cl.c[i];
            if (c->type == OSL_CL_DIFFUSE) { nd++; CHECK_NEAR(c->w[0], 0.4, 1e-6); CHECK_NEAR(c->w[2], 0.1, 1e-6); CHECK_NEAR(c->N[2], 1, 1e-6); }
            if (c->type == OSL_CL_MICROFACET) { nm++; CHECK_NEAR(c->w[1], 0.25, 1e-6); CHECK_NEAR(c->alpha, 0.25, 1e-6); CHECK_NEAR(c->eta, 1.45, 1e-6); CHECK(c->refract == 0); }
            if (c->type == OSL_CL_EMISSION) { ne++; CHECK_NEAR(c->w[2], 1.5, 1e-6); }
        }
        CHECK(nd == 1 && nm == 1 && ne == 1);
        osl_program_free(p);
    }
    p = run("surface t() { Ci = reflection(N) + 0.3 * refraction(N, 1.33) + transparent() * color(0.5) + holdout(); }", 0);
    CHECK(p != NULL);
    if (p) {
        CHECK(g_cl.n == 3);
        CHECK(!osl_program_may_emit(p));
        if (g_cl.n == 3) {
            CHECK(g_cl.c[0].type == OSL_CL_REFLECTION && g_cl.c[0].eta == 0.0f);
            CHECK(g_cl.c[1].type == OSL_CL_REFRACTION); CHECK_NEAR(g_cl.c[1].eta, 1.33, 1e-6);
            CHECK(g_cl.c[2].type == OSL_CL_TRANSPARENT); CHECK_NEAR(g_cl.c[2].w[0], 0.5, 1e-6);
        }
        osl_program_free(p);
    }
    p = run("surface t(float k = 0) { closure color c = 0; if (k > 0) c = diffuse(N); Ci = c; }", 0);
    CHECK(p != NULL);
    if (p) { CHECK(g_cl.n == 0); osl_program_free(p); }
    /* runtime safety: infinite loops stop with an error */
    {
        char err[256];
        osl_globals g;
        p = osl_compile("surface t() { float x = 0; while (1) { x += 1; } }", err, sizeof err);
        CHECK(p != NULL);
        memset(&g, 0, sizeof g);
        if (p) { CHECK(osl_execute(p, g_ex, &g, NULL, NULL, 0, &g_cl) != 0); osl_program_free(p); }
    }
    /* diagnostics */
    expect_error("surface t() { float x = 1 }", "expected ';'");
    expect_error("surface t() { y = 1; }", "undeclared identifier 'y'");
    expect_error("surface t() { color c = 1; float f = c; }", "cannot initialise float with color");
    expect_error("surface t() { Ci = diffuse(N) * diffuse(N); }", "cannot multiply two closures");
    expect_error("surface t() { Ci = frobnicate(N); }", "unknown function 'frobnicate'");
    expect_error("surface t() { break; }", "outside of a loop");
    expect_error("surface t() { M_PI = 3; }", "read-only");
    expect_error("surface t(float a) { }", "needs a default value");
    expect_error("surface t() { float f = \"x\"; }", "cannot initialise float with string");
    expect_error("surface t() { Ci = microfacet(\"phong\", N, 0.1, 1.5, 0); }", "unknown distribution");
    expect_error("float f(float x) { return f(x); } surface t() { float y = f(1); }", "too deep");
    expect_error("surface t() { float a[3]; }", "arrays are not supported");
    expect_error("surface t() {\n  float x = 1;\n  x = x +;\n}", "3:");
    expect_error("", "no surface shader");
    expect_error("displacement d() { }", "only surface shaders");
    /* the example shaders shipped with Nora compile */
    {
        static const char *const names[] = { "plastic.osl", "glass.osl", "checker.osl", "emissive.osl", "marble.osl", "brushed_metal.osl", "textured.osl" };
        for (i = 0; i < (int)(sizeof names / sizeof names[0]); i++) {
            char path[1024], err[512];
            char *src;
            snprintf(path, sizeof path, "%s/examples/shaders/%s", NORA_SOURCE_DIR, names[i]);
            src = read_file(path);
            CHECK(src != NULL);
            if (!src) { printf("  missing %s\n", path); continue; }
            p = osl_compile(src, err, sizeof err);
            if (!p) printf("  %s: %s\n", names[i], err);
            CHECK(p != NULL);
            if (p) {
                osl_globals g;
                memset(&g, 0, sizeof g);
                g.N[2] = g.Ng[2] = 1; g.I[2] = -1;
                CHECK(osl_execute(p, g_ex, &g, NULL, NULL, 0, &g_cl) == 0);
                CHECK(g_cl.n > 0);
                osl_program_free(p);
            }
            free(src);
        }
    }
    osl_exec_free(g_ex);
}
