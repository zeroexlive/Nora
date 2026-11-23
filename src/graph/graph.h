/* Nora - shader node graph: node catalogue, OSL code generation, JSON save/load.
 * The GUI draws it with CUI's node editor; this module has no GUI dependency. MIT License. */
#ifndef NORA_GRAPH_H
#define NORA_GRAPH_H

#include <stddef.h>

enum { NG_PIN_FLOAT = 1, NG_PIN_COLOR = 2, NG_PIN_CLOSURE = 3 };   /* also CUI pin types */
#define NG_MAX_IN 6
#define NG_MAX_OUT 4
#define NG_PIN_SLOTS 16    /* pin id = node id * 16 + slot; inputs 0..7, outputs 8..15 */

typedef struct ng_pin_def {
    const char *name;
    int type;
    float def[3];
    float lo, hi;          /* slider range for floats */
    int default_expr;      /* unlinked value is a global: 1 = N, 2 = P */
} ng_pin_def;

typedef struct ng_node_def {
    const char *id;        /* stable name used in JSON */
    const char *title;
    const char *category;  /* Input, Math, Color, Texture, Shader, Output */
    int nin;
    ng_pin_def in[NG_MAX_IN];
    int nout;
    ng_pin_def out[NG_MAX_OUT];
    int nops;              /* > 0: the node has an operation choice */
    const char *const *ops;
    int has_text;          /* filename */
} ng_node_def;

typedef struct ng_node {
    int id;
    int type;
    float x, y;
    float vals[NG_MAX_IN][3];
    int op;
    char text[256];
} ng_node;

typedef struct ng_link { int id, from_pin, to_pin; } ng_link;

typedef struct ng_graph {
    ng_node *nodes; int nnodes, cap_nodes;
    ng_link *links; int nlinks, cap_links;
    int next_node_id, next_link_id;
} ng_graph;

int ng_type_count(void);
const ng_node_def *ng_def(int type);
int ng_type_by_id(const char *id);

void ng_init(ng_graph *g);
void ng_free(ng_graph *g);
void ng_clear(ng_graph *g);
int ng_add_node(ng_graph *g, int type, float x, float y);   /* returns node id */
void ng_remove_node(ng_graph *g, int node_id);
ng_node *ng_find(ng_graph *g, int node_id);
const ng_node *ng_find_const(const ng_graph *g, int node_id);

int ng_pin_id(int node_id, int slot, int is_output);
int ng_pin_node(int pin_id);
int ng_pin_slot(int pin_id);     /* index into in[] or out[] */
int ng_pin_is_output(int pin_id);
int ng_pin_type(const ng_graph *g, int pin_id);

/* Link an output pin to an input pin (types must match; replaces the input's link).
 * Returns the link id or -1. */
int ng_add_link(ng_graph *g, int from_pin, int to_pin);
void ng_remove_link(ng_graph *g, int link_id);
const ng_link *ng_link_into(const ng_graph *g, int to_pin);

/* A small default graph: Principled-like plastic (diffuse + GGX) into the output. */
void ng_default_graph(ng_graph *g);

/* Generate OSL source for the graph. Returns 0, or -1 with err filled (missing output, cycle). */
int ng_generate_osl(const ng_graph *g, char *buf, size_t cap, char *err, size_t errcap);

/* JSON. ng_to_json returns a malloc'd string. */
char *ng_to_json(const ng_graph *g);
int ng_from_json(ng_graph *g, const char *text, char *err, size_t errcap);
int ng_save_file(const ng_graph *g, const char *path);
int ng_load_file(ng_graph *g, const char *path, char *err, size_t errcap);

#endif
