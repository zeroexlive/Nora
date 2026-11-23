/* Nora - shader node graph. MIT License. */
#include "graph.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static const char *const math_ops[] = { "Add", "Subtract", "Multiply", "Divide", "Power", "Minimum", "Maximum",
                                        "Sine", "Cosine", "Absolute", "Floor", "Fraction" };
static const char *const mf_ops[] = { "Reflect", "Refract", "Both (glass)" };

#define F(nm, d, lo, hi) { nm, NG_PIN_FLOAT, { d, 0, 0 }, lo, hi, 0 }
#define C(nm, r, g, b) { nm, NG_PIN_COLOR, { r, g, b }, 0, 1, 0 }
#define NRM { "Normal", NG_PIN_COLOR, { 0, 0, 0 }, 0, 1, 1 }
#define VEC(nm) { nm, NG_PIN_COLOR, { 0, 0, 0 }, 0, 1, 2 }
#define CL(nm) { nm, NG_PIN_CLOSURE, { 0, 0, 0 }, 0, 1, 0 }
#define NONE { NULL, 0, { 0, 0, 0 }, 0, 0, 0 }

static const ng_node_def g_defs[] = {
    { "output", "Material Output", "Output", 1, { CL("Surface") }, 0, { NONE }, 0, NULL, 0 },
    { "geometry", "Geometry", "Input", 0, { NONE }, 4, { VEC("Position"), C("Normal", 0, 0, 0), F("U", 0, 0, 1), F("V", 0, 0, 1) }, 0, NULL, 0 },
    { "value", "Value", "Input", 1, { F("Value", 0.5f, 0, 1) }, 1, { F("Value", 0, 0, 1) }, 0, NULL, 0 },
    { "rgb", "RGB", "Input", 1, { C("Color", 0.8f, 0.8f, 0.8f) }, 1, { C("Color", 0, 0, 0) }, 0, NULL, 0 },
    { "math", "Math", "Math", 2, { F("A", 0.5f, 0, 1), F("B", 0.5f, 0, 1) }, 1, { F("Result", 0, 0, 1) }, 12, math_ops, 0 },
    { "mix", "Mix Color", "Color", 3, { F("Factor", 0.5f, 0, 1), C("A", 0.8f, 0.8f, 0.8f), C("B", 0.1f, 0.1f, 0.1f) }, 1, { C("Color", 0, 0, 0) }, 0, NULL, 0 },
    { "image", "Image Texture", "Texture", 1, { F("Scale", 1, 0, 16) }, 2, { C("Color", 0, 0, 0), F("Luminance", 0, 0, 1) }, 0, NULL, 1 },
    { "noise", "Noise Texture", "Texture", 2, { F("Scale", 0.05f, 0, 1), F("Detail", 4, 1, 8) }, 2, { F("Fac", 0, 0, 1), C("Color", 0, 0, 0) }, 0, NULL, 0 },
    { "checker", "Checker Texture", "Texture", 3, { F("Scale", 0.02f, 0, 1), C("Color 1", 0.9f, 0.9f, 0.9f), C("Color 2", 0.15f, 0.15f, 0.15f) }, 1, { C("Color", 0, 0, 0) }, 0, NULL, 0 },
    { "diffuse", "Diffuse BSDF", "Shader", 2, { C("Color", 0.8f, 0.8f, 0.8f), NRM }, 1, { CL("BSDF") }, 0, NULL, 0 },
    { "microfacet", "Microfacet GGX", "Shader", 4, { C("Color", 1, 1, 1), F("Roughness", 0.2f, 0, 1), F("IOR", 1.5f, 1, 3), NRM }, 1, { CL("BSDF") }, 3, mf_ops, 0 },
    { "reflection", "Reflection", "Shader", 3, { C("Color", 1, 1, 1), F("IOR (0 = mirror)", 0, 0, 3), NRM }, 1, { CL("BSDF") }, 0, NULL, 0 },
    { "refraction", "Refraction", "Shader", 3, { C("Color", 1, 1, 1), F("IOR", 1.5f, 1, 3), NRM }, 1, { CL("BSDF") }, 0, NULL, 0 },
    { "emission", "Emission", "Shader", 2, { C("Color", 1, 1, 1), F("Strength", 5, 0, 50) }, 1, { CL("Emission") }, 0, NULL, 0 },
    { "transparent", "Transparent", "Shader", 1, { C("Color", 1, 1, 1) }, 1, { CL("BSDF") }, 0, NULL, 0 },
    { "add_shader", "Add Shader", "Shader", 2, { CL("A"), CL("B") }, 1, { CL("Shader") }, 0, NULL, 0 },
    { "mix_shader", "Mix Shader", "Shader", 3, { F("Factor", 0.5f, 0, 1), CL("A"), CL("B") }, 1, { CL("Shader") }, 0, NULL, 0 },
};
#undef F
#undef C
#undef NRM
#undef VEC
#undef CL
#undef NONE

enum { T_OUTPUT = 0, T_GEOMETRY, T_VALUE, T_RGB, T_MATH, T_MIX, T_IMAGE, T_NOISE, T_CHECKER, T_DIFFUSE, T_MICROFACET,
       T_REFLECTION, T_REFRACTION, T_EMISSION, T_TRANSPARENT, T_ADD, T_MIXSH, T_COUNT };

int ng_type_count(void) { return (int)(sizeof g_defs / sizeof g_defs[0]); }
const ng_node_def *ng_def(int type) { return (type >= 0 && type < ng_type_count()) ? &g_defs[type] : NULL; }
int ng_type_by_id(const char *id) {
    int i;
    for (i = 0; i < ng_type_count(); i++) if (!strcmp(g_defs[i].id, id)) return i;
    return -1;
}

void ng_init(ng_graph *g) { memset(g, 0, sizeof *g); g->next_node_id = 1; g->next_link_id = 1; }
void ng_free(ng_graph *g) { free(g->nodes); free(g->links); ng_init(g); }
void ng_clear(ng_graph *g) { g->nnodes = 0; g->nlinks = 0; g->next_node_id = 1; g->next_link_id = 1; }

int ng_pin_id(int node_id, int slot, int is_output) { return node_id * NG_PIN_SLOTS + (is_output ? 8 + slot : slot); }
int ng_pin_node(int pin) { return pin / NG_PIN_SLOTS; }
int ng_pin_slot(int pin) { int s = pin % NG_PIN_SLOTS; return s >= 8 ? s - 8 : s; }
int ng_pin_is_output(int pin) { return (pin % NG_PIN_SLOTS) >= 8; }

ng_node *ng_find(ng_graph *g, int id) {
    int i;
    for (i = 0; i < g->nnodes; i++) if (g->nodes[i].id == id) return &g->nodes[i];
    return NULL;
}
const ng_node *ng_find_const(const ng_graph *g, int id) {
    int i;
    for (i = 0; i < g->nnodes; i++) if (g->nodes[i].id == id) return &g->nodes[i];
    return NULL;
}

int ng_pin_type(const ng_graph *g, int pin) {
    const ng_node *n = ng_find_const(g, ng_pin_node(pin));
    const ng_node_def *d;
    int s = ng_pin_slot(pin);
    if (!n) return 0;
    d = ng_def(n->type);
    if (ng_pin_is_output(pin)) return s < d->nout ? d->out[s].type : 0;
    return s < d->nin ? d->in[s].type : 0;
}

static int add_node_with_id(ng_graph *g, int type, int id, float x, float y) {
    ng_node *n;
    const ng_node_def *d = ng_def(type);
    int i;
    if (!d) return -1;
    if (g->nnodes >= g->cap_nodes) {
        int nc = g->cap_nodes ? g->cap_nodes * 2 : 16;
        ng_node *nn = (ng_node *)realloc(g->nodes, sizeof(ng_node) * (size_t)nc);
        if (!nn) return -1;
        g->nodes = nn; g->cap_nodes = nc;
    }
    n = &g->nodes[g->nnodes++];
    memset(n, 0, sizeof *n);
    n->id = id; n->type = type; n->x = x; n->y = y;
    for (i = 0; i < d->nin; i++) memcpy(n->vals[i], d->in[i].def, sizeof n->vals[i]);
    if (id >= g->next_node_id) g->next_node_id = id + 1;
    return id;
}

int ng_add_node(ng_graph *g, int type, float x, float y) { return add_node_with_id(g, type, g->next_node_id, x, y); }

void ng_remove_node(ng_graph *g, int id) {
    int i, j = 0;
    for (i = 0; i < g->nlinks; i++) {
        if (ng_pin_node(g->links[i].from_pin) == id || ng_pin_node(g->links[i].to_pin) == id) continue;
        g->links[j++] = g->links[i];
    }
    g->nlinks = j;
    for (i = 0; i < g->nnodes; i++) {
        if (g->nodes[i].id == id) {
            memmove(&g->nodes[i], &g->nodes[i + 1], sizeof(ng_node) * (size_t)(g->nnodes - i - 1));
            g->nnodes--;
            return;
        }
    }
}

const ng_link *ng_link_into(const ng_graph *g, int to_pin) {
    int i;
    for (i = 0; i < g->nlinks; i++) if (g->links[i].to_pin == to_pin) return &g->links[i];
    return NULL;
}

int ng_add_link(ng_graph *g, int from_pin, int to_pin) {
    int i, ta, tb;
    if (!ng_pin_is_output(from_pin) && ng_pin_is_output(to_pin)) { int t = from_pin; from_pin = to_pin; to_pin = t; }
    if (!ng_pin_is_output(from_pin) || ng_pin_is_output(to_pin)) return -1;
    if (ng_pin_node(from_pin) == ng_pin_node(to_pin)) return -1;
    ta = ng_pin_type(g, from_pin); tb = ng_pin_type(g, to_pin);
    if (!ta || !tb || ta != tb) return -1;
    for (i = 0; i < g->nlinks; i++) {
        if (g->links[i].to_pin == to_pin) { g->links[i].from_pin = from_pin; return g->links[i].id; }
    }
    if (g->nlinks >= g->cap_links) {
        int nc = g->cap_links ? g->cap_links * 2 : 16;
        ng_link *nl = (ng_link *)realloc(g->links, sizeof(ng_link) * (size_t)nc);
        if (!nl) return -1;
        g->links = nl; g->cap_links = nc;
    }
    g->links[g->nlinks].id = g->next_link_id++;
    g->links[g->nlinks].from_pin = from_pin;
    g->links[g->nlinks].to_pin = to_pin;
    return g->links[g->nlinks++].id;
}

void ng_remove_link(ng_graph *g, int link_id) {
    int i;
    for (i = 0; i < g->nlinks; i++) {
        if (g->links[i].id == link_id) {
            memmove(&g->links[i], &g->links[i + 1], sizeof(ng_link) * (size_t)(g->nlinks - i - 1));
            g->nlinks--;
            return;
        }
    }
}

void ng_default_graph(ng_graph *g) {
    int out, dif, mf, add, col;
    ng_clear(g);
    out = ng_add_node(g, T_OUTPUT, 760, 140);
    add = ng_add_node(g, T_ADD, 540, 140);
    dif = ng_add_node(g, T_DIFFUSE, 300, 40);
    mf = ng_add_node(g, T_MICROFACET, 300, 260);
    col = ng_add_node(g, T_RGB, 40, 40);
    ng_find(g, col)->vals[0][0] = 0.75f; ng_find(g, col)->vals[0][1] = 0.35f; ng_find(g, col)->vals[0][2] = 0.12f;
    ng_find(g, mf)->vals[0][0] = ng_find(g, mf)->vals[0][1] = ng_find(g, mf)->vals[0][2] = 1.0f;
    ng_find(g, mf)->vals[1][0] = 0.15f;
    ng_add_link(g, ng_pin_id(col, 0, 1), ng_pin_id(dif, 0, 0));
    ng_add_link(g, ng_pin_id(dif, 0, 1), ng_pin_id(add, 0, 0));
    ng_add_link(g, ng_pin_id(mf, 0, 1), ng_pin_id(add, 1, 0));
    ng_add_link(g, ng_pin_id(add, 0, 1), ng_pin_id(out, 0, 0));
}

/* ---------------------------------------------------------------- codegen */

typedef struct gen {
    const ng_graph *g;
    char *buf; size_t cap, n;
    int *state;                /* 0 unvisited, 1 in progress, 2 done (indexed by node array index) */
    char *err; size_t errcap;
    int failed;
} gen;

static void gputs(gen *G, const char *s) {
    size_t n = strlen(s);
    if (G->n + n + 1 > G->cap) { G->failed = 1; if (G->err) snprintf(G->err, G->errcap, "generated source too long"); return; }
    memcpy(G->buf + G->n, s, n + 1);
    G->n += n;
}
static void gprintf(gen *G, const char *fmt, ...) {
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    gputs(G, tmp);
}

static const char *out_var(const ng_node *n, int slot, char *tmp, size_t cap) {
    const ng_node_def *d = ng_def(n->type);
    char name[64];
    size_t i, j = 0;
    for (i = 0; d->out[slot].name[i] && j < sizeof name - 1; i++) {
        char c = d->out[slot].name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) name[j++] = (char)(c | 0x20);
    }
    name[j] = 0;
    snprintf(tmp, cap, "n%d_%s", n->id, name);
    return tmp;
}

static void emit_node(gen *G, int idx);

static int node_index(const ng_graph *g, int id) {
    int i;
    for (i = 0; i < g->nnodes; i++) if (g->nodes[i].id == id) return i;
    return -1;
}

/* expression for input slot of node n; also emits the source node first */
static const char *input_expr(gen *G, const ng_node *n, int slot, char *tmp, size_t cap) {
    const ng_node_def *d = ng_def(n->type);
    const ng_link *l = ng_link_into(G->g, ng_pin_id(n->id, slot, 0));
    if (l) {
        int si = node_index(G->g, ng_pin_node(l->from_pin));
        if (si >= 0) {
            emit_node(G, si);
            return out_var(&G->g->nodes[si], ng_pin_slot(l->from_pin), tmp, cap);
        }
    }
    switch (d->in[slot].type) {
    case NG_PIN_FLOAT: snprintf(tmp, cap, "%g", (double)n->vals[slot][0]); break;
    case NG_PIN_COLOR:
        if (d->in[slot].default_expr == 1) snprintf(tmp, cap, "N");
        else if (d->in[slot].default_expr == 2) snprintf(tmp, cap, "P");
        else snprintf(tmp, cap, "color(%g, %g, %g)", (double)n->vals[slot][0], (double)n->vals[slot][1], (double)n->vals[slot][2]);
        break;
    default: snprintf(tmp, cap, "0"); break;
    }
    return tmp;
}
static int input_linked(gen *G, const ng_node *n, int slot) { return ng_link_into(G->g, ng_pin_id(n->id, slot, 0)) != NULL; }

static void emit_node(gen *G, int idx) {
    const ng_node *n = &G->g->nodes[idx];
    char a[256], b[256], c[256], d4[256], o[128], o2[128];
    if (G->failed) return;
    if (G->state[idx] == 2) return;
    if (G->state[idx] == 1) { G->failed = 1; if (G->err) snprintf(G->err, G->errcap, "the graph has a cycle through node %d", n->id); return; }
    G->state[idx] = 1;
    switch (n->type) {
    case T_GEOMETRY:
        gprintf(G, "    point %s = P;\n", out_var(n, 0, o, sizeof o));
        gprintf(G, "    normal %s = N;\n", out_var(n, 1, o, sizeof o));
        gprintf(G, "    float %s = u;\n", out_var(n, 2, o, sizeof o));
        gprintf(G, "    float %s = v;\n", out_var(n, 3, o, sizeof o));
        break;
    case T_VALUE: gprintf(G, "    float %s = %g;\n", out_var(n, 0, o, sizeof o), (double)n->vals[0][0]); break;
    case T_RGB:
        gprintf(G, "    color %s = color(%g, %g, %g);\n", out_var(n, 0, o, sizeof o), (double)n->vals[0][0], (double)n->vals[0][1], (double)n->vals[0][2]);
        break;
    case T_MATH: {
        static const char *const fmt[] = { "%s + %s", "%s - %s", "%s * %s", "%s / %s", "pow(%s, %s)", "min(%s, %s)", "max(%s, %s)",
                                           "sin(%s)", "cos(%s)", "abs(%s)", "floor(%s)", "%s - floor(%s)" };
        char expr[600];
        int op = n->op >= 0 && n->op < 12 ? n->op : 0;
        input_expr(G, n, 0, a, sizeof a);
        input_expr(G, n, 1, b, sizeof b);
        if (op == 11) snprintf(expr, sizeof expr, fmt[op], a, a);
        else if (op >= 7) snprintf(expr, sizeof expr, fmt[op], a);
        else snprintf(expr, sizeof expr, fmt[op], a, b);
        gprintf(G, "    float %s = %s;\n", out_var(n, 0, o, sizeof o), expr);
        break;
    }
    case T_MIX:
        input_expr(G, n, 0, a, sizeof a); input_expr(G, n, 1, b, sizeof b); input_expr(G, n, 2, c, sizeof c);
        gprintf(G, "    color %s = mix(%s, %s, clamp(%s, 0.0, 1.0));\n", out_var(n, 0, o, sizeof o), b, c, a);
        break;
    case T_IMAGE: {
        char fname[300];
        size_t i, j = 0;
        for (i = 0; n->text[i] && j < sizeof fname - 2; i++) { if (n->text[i] == '"' || n->text[i] == '\\') fname[j++] = '\\'; fname[j++] = n->text[i]; }
        fname[j] = 0;
        input_expr(G, n, 0, a, sizeof a);
        gprintf(G, "    color %s = texture(\"%s\", u * %s, (1.0 - v) * %s);\n", out_var(n, 0, o, sizeof o), fname, a, a);
        gprintf(G, "    float %s = luminance(%s);\n", out_var(n, 1, o2, sizeof o2), o);
        break;
    }
    case T_NOISE:
        input_expr(G, n, 0, a, sizeof a); input_expr(G, n, 1, b, sizeof b);
        out_var(n, 0, o, sizeof o); out_var(n, 1, o2, sizeof o2);
        gprintf(G, "    float %s = 0;\n    color %s = 0;\n", o, o2);
        gprintf(G, "    {\n        point _p = P * %s;\n        float _amp = 0.5, _norm = 0;\n", a);
        gprintf(G, "        for (int _i = 0; _i < int(%s); _i++) {\n", b);
        gprintf(G, "            %s += _amp * noise(\"perlin\", _p);\n", o);
        gprintf(G, "            %s += _amp * noise(\"uperlin\", _p + vector(17.3, 4.1, 9.7));\n", o2);
        gprintf(G, "            _norm += _amp; _p *= 2.0; _amp *= 0.5;\n        }\n");
        gprintf(G, "        if (_norm > 0) { %s = 0.5 + 0.5 * %s / _norm; %s = %s / _norm; }\n    }\n", o, o, o2, o2);
        break;
    case T_CHECKER:
        input_expr(G, n, 0, a, sizeof a); input_expr(G, n, 1, b, sizeof b); input_expr(G, n, 2, c, sizeof c);
        out_var(n, 0, o, sizeof o);
        gprintf(G, "    color %s = 0;\n    {\n        point _q = P * %s;\n", o, a);
        gprintf(G, "        %s = mod(floor(_q[0]) + floor(_q[1]) + floor(_q[2]), 2.0) < 1.0 ? %s : %s;\n    }\n", o, b, c);
        break;
    case T_DIFFUSE:
        input_expr(G, n, 0, a, sizeof a); input_expr(G, n, 1, b, sizeof b);
        gprintf(G, "    closure color %s = %s * diffuse(%s);\n", out_var(n, 0, o, sizeof o), a, b);
        break;
    case T_MICROFACET: {
        int op = n->op >= 0 && n->op < 3 ? n->op : 0;
        input_expr(G, n, 0, a, sizeof a); input_expr(G, n, 1, b, sizeof b); input_expr(G, n, 2, c, sizeof c);
        input_expr(G, n, 3, d4, sizeof d4);
        if (op == 0)
            gprintf(G, "    closure color %s = %s * microfacet(\"ggx\", %s, %s * %s, %s, 0);\n", out_var(n, 0, o, sizeof o), a, d4, b, b, c);
        else
            gprintf(G, "    closure color %s = %s * microfacet(\"ggx\", %s, %s * %s, backfacing() ? 1.0 / %s : %s, %d);\n",
                    out_var(n, 0, o, sizeof o), a, d4, b, b, c, c, op);
        break;
    }
    case T_REFLECTION:
        input_expr(G, n, 0, a, sizeof a); input_expr(G, n, 1, b, sizeof b); input_expr(G, n, 2, c, sizeof c);
        gprintf(G, "    closure color %s = %s * reflection(%s, %s);\n", out_var(n, 0, o, sizeof o), a, c, b);
        break;
    case T_REFRACTION:
        input_expr(G, n, 0, a, sizeof a); input_expr(G, n, 1, b, sizeof b); input_expr(G, n, 2, c, sizeof c);
        gprintf(G, "    closure color %s = %s * refraction(%s, backfacing() ? 1.0 / %s : %s);\n", out_var(n, 0, o, sizeof o), a, c, b, b);
        break;
    case T_EMISSION:
        input_expr(G, n, 0, a, sizeof a); input_expr(G, n, 1, b, sizeof b);
        gprintf(G, "    closure color %s = (%s * %s) * emission();\n", out_var(n, 0, o, sizeof o), a, b);
        break;
    case T_TRANSPARENT:
        input_expr(G, n, 0, a, sizeof a);
        gprintf(G, "    closure color %s = %s * transparent();\n", out_var(n, 0, o, sizeof o), a);
        break;
    case T_ADD: {
        int la = input_linked(G, n, 0), lb = input_linked(G, n, 1);
        input_expr(G, n, 0, a, sizeof a); input_expr(G, n, 1, b, sizeof b);
        out_var(n, 0, o, sizeof o);
        if (la && lb) gprintf(G, "    closure color %s = %s + %s;\n", o, a, b);
        else if (la || lb) gprintf(G, "    closure color %s = %s;\n", o, la ? a : b);
        else gprintf(G, "    closure color %s = 0;\n", o);
        break;
    }
    case T_MIXSH: {
        int la = input_linked(G, n, 1), lb = input_linked(G, n, 2);
        input_expr(G, n, 0, c, sizeof c); input_expr(G, n, 1, a, sizeof a); input_expr(G, n, 2, b, sizeof b);
        out_var(n, 0, o, sizeof o);
        if (la && lb) gprintf(G, "    closure color %s = (1.0 - clamp(%s, 0.0, 1.0)) * %s + clamp(%s, 0.0, 1.0) * %s;\n", o, c, a, c, b);
        else if (la) gprintf(G, "    closure color %s = (1.0 - clamp(%s, 0.0, 1.0)) * %s;\n", o, c, a);
        else if (lb) gprintf(G, "    closure color %s = clamp(%s, 0.0, 1.0) * %s;\n", o, c, b);
        else gprintf(G, "    closure color %s = 0;\n", o);
        break;
    }
    default: break;
    }
    G->state[idx] = 2;
}

int ng_generate_osl(const ng_graph *g, char *buf, size_t cap, char *err, size_t errcap) {
    gen G;
    int i, out = -1;
    char a[256];
    memset(&G, 0, sizeof G);
    G.g = g; G.buf = buf; G.cap = cap; G.err = err; G.errcap = errcap;
    if (!buf || cap == 0) return -1;
    buf[0] = 0;
    for (i = 0; i < g->nnodes; i++) if (g->nodes[i].type == T_OUTPUT) { out = i; break; }
    if (out < 0) { if (err) snprintf(err, errcap, "the graph has no Material Output node"); return -1; }
    G.state = (int *)calloc((size_t)g->nnodes + 1, sizeof(int));
    gputs(&G, "// Generated by the Nora shader graph editor.\n");
    gputs(&G, "surface nora_graph()\n{\n");
    if (input_linked(&G, &g->nodes[out], 0)) {
        input_expr(&G, &g->nodes[out], 0, a, sizeof a);
        gprintf(&G, "    Ci = %s;\n", a);
    } else {
        gputs(&G, "    Ci = 0;   // nothing is connected to the output\n");
    }
    gputs(&G, "}\n");
    free(G.state);
    return G.failed ? -1 : 0;
}

/* ------------------------------------------------------------------- JSON */

char *ng_to_json(const ng_graph *g) {
    js_buf b = { 0, 0, 0 };
    int i, k;
    jb_puts(&b, "{\n  \"format\": \"nora-shader-graph\",\n  \"version\": 1,\n  \"nodes\": [");
    for (i = 0; i < g->nnodes; i++) {
        const ng_node *n = &g->nodes[i];
        const ng_node_def *d = ng_def(n->type);
        jb_printf(&b, "%s\n    { \"id\": %d, \"type\": ", i ? "," : "", n->id);
        jb_str(&b, d->id);
        jb_printf(&b, ", \"x\": %.1f, \"y\": %.1f, \"op\": %d, \"text\": ", (double)n->x, (double)n->y, n->op);
        jb_str(&b, n->text);
        jb_puts(&b, ", \"values\": [");
        for (k = 0; k < d->nin; k++)
            jb_printf(&b, "%s[%g, %g, %g]", k ? ", " : "", (double)n->vals[k][0], (double)n->vals[k][1], (double)n->vals[k][2]);
        jb_puts(&b, "] }");
    }
    jb_puts(&b, "\n  ],\n  \"links\": [");
    for (i = 0; i < g->nlinks; i++) {
        const ng_link *l = &g->links[i];
        jb_printf(&b, "%s\n    { \"from\": [%d, %d], \"to\": [%d, %d] }", i ? "," : "", ng_pin_node(l->from_pin), ng_pin_slot(l->from_pin),
                  ng_pin_node(l->to_pin), ng_pin_slot(l->to_pin));
    }
    jb_puts(&b, "\n  ]\n}\n");
    return b.p;
}

int ng_from_json(ng_graph *g, const char *text, char *err, size_t errcap) {
    js_value *root = js_parse(text, err, errcap);
    const js_value *nodes, *links;
    int i, n;
    if (!root) return -1;
    if (strcmp(js_str(js_get(root, "format"), ""), "nora-shader-graph") != 0) {
        if (err) snprintf(err, errcap, "not a Nora shader graph (missing \"format\": \"nora-shader-graph\")");
        js_free(root);
        return -1;
    }
    ng_clear(g);
    nodes = js_get(root, "nodes");
    n = js_len(nodes);
    for (i = 0; i < n; i++) {
        const js_value *jn = js_at(nodes, i), *vals;
        int type = ng_type_by_id(js_str(js_get(jn, "type"), "")), id, k, nv;
        ng_node *node;
        if (type < 0) {
            if (err) snprintf(err, errcap, "unknown node type \"%s\"", js_str(js_get(jn, "type"), "?"));
            js_free(root);
            return -1;
        }
        id = (int)js_num(js_get(jn, "id"), g->next_node_id);
        if (id <= 0 || ng_find(g, id)) { if (err) snprintf(err, errcap, "bad or duplicate node id %d", id); js_free(root); return -1; }
        add_node_with_id(g, type, id, (float)js_num(js_get(jn, "x"), 0), (float)js_num(js_get(jn, "y"), 0));
        node = ng_find(g, id);
        node->op = (int)js_num(js_get(jn, "op"), 0);
        strncpy(node->text, js_str(js_get(jn, "text"), ""), sizeof node->text - 1);
        vals = js_get(jn, "values");
        nv = js_len(vals);
        for (k = 0; k < nv && k < ng_def(type)->nin; k++) {
            const js_value *v = js_at(vals, k);
            node->vals[k][0] = (float)js_num(js_at(v, 0), 0);
            node->vals[k][1] = (float)js_num(js_at(v, 1), 0);
            node->vals[k][2] = (float)js_num(js_at(v, 2), 0);
        }
    }
    links = js_get(root, "links");
    n = js_len(links);
    for (i = 0; i < n; i++) {
        const js_value *l = js_at(links, i), *fr = js_get(l, "from"), *to = js_get(l, "to");
        int fp = ng_pin_id((int)js_num(js_at(fr, 0), 0), (int)js_num(js_at(fr, 1), 0), 1);
        int tp = ng_pin_id((int)js_num(js_at(to, 0), 0), (int)js_num(js_at(to, 1), 0), 0);
        if (ng_add_link(g, fp, tp) < 0) {
            if (err) snprintf(err, errcap, "invalid link %d", i);
            js_free(root);
            return -1;
        }
    }
    js_free(root);
    return 0;
}

int ng_save_file(const ng_graph *g, const char *path) {
    char *s = ng_to_json(g);
    FILE *f;
    if (!s) return -1;
    f = fopen(path, "wb");
    if (!f) { free(s); return -1; }
    fputs(s, f);
    fclose(f);
    free(s);
    return 0;
}

int ng_load_file(ng_graph *g, const char *path, char *err, size_t errcap) {
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;
    int rc;
    if (!f) { if (err) snprintf(err, errcap, "cannot open %s", path); return -1; }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return -1; }
    buf[fread(buf, 1, (size_t)n, f)] = 0;
    fclose(f);
    rc = ng_from_json(g, buf, err, errcap);
    free(buf);
    return rc;
}
