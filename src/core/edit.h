/* Nora - scene editing helpers: undo/redo history of instance transforms and
 * object visibility (delete, import and duplicate are visibility changes, so
 * every edit is reversible without copying geometry). MIT License. */
#ifndef NORA_EDIT_H
#define NORA_EDIT_H

#include "scene.h"

typedef struct nora_xform { v3 translate, scale, euler_deg; quat rot; } nora_xform;
void nora_xform_get(const nora_mesh *m, nora_xform *x);
void nora_xform_set(nora_mesh *m, const nora_xform *x);
int nora_xform_equal(const nora_xform *a, const nora_xform *b);

typedef enum nora_edit_kind { NORA_EDIT_TRANSFORM = 0, NORA_EDIT_VISIBILITY } nora_edit_kind;

typedef struct nora_edit_op {
    nora_edit_kind kind;
    char label[64];
    int mesh;                      /* transform */
    nora_xform before, after;
    int *meshes, count;            /* visibility */
    int hidden_after;
} nora_edit_op;

#define NORA_HISTORY_MAX 512
typedef struct nora_history {
    nora_edit_op *ops;
    int n, cap;
    int pos;                       /* ops[0..pos) are done; ops[pos..n) can be redone */
} nora_history;

void nora_history_init(nora_history *h);
void nora_history_free(nora_history *h);
void nora_history_clear(nora_history *h);
/* Records an edit that has already been applied to the scene. */
int nora_history_push_transform(nora_history *h, int mesh, const nora_xform *before, const nora_xform *after, const char *label);
int nora_history_push_visibility(nora_history *h, const int *meshes, int count, int hidden_after, const char *label);
/* Undo / redo one step. Return 1 if something changed (label set), 0 otherwise.
 * The caller re-applies the transforms / rebuilds as needed. */
int nora_history_undo(nora_history *h, nora_scene *s, const char **label);
int nora_history_redo(nora_history *h, nora_scene *s, const char **label);
int nora_history_can_undo(const nora_history *h);
int nora_history_can_redo(const nora_history *h);

/* Convenience edits that apply and record. Return 0 on success. */
int nora_edit_transform(nora_history *h, nora_scene *s, int mesh, const nora_xform *after, const char *label);
int nora_edit_delete(nora_history *h, nora_scene *s, int mesh);
int nora_edit_duplicate(nora_history *h, nora_scene *s, int mesh);              /* returns the new mesh or -1 */
int nora_edit_import(nora_history *h, nora_scene *s, const char *path, char *err, size_t errcap);   /* first new mesh or -1 */

#endif
