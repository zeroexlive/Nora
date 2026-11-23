/* Nora - undo/redo history. MIT License. */
#include "edit.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void nora_xform_get(const nora_mesh *m, nora_xform *x) {
    x->translate = m->translate; x->scale = m->scale; x->euler_deg = m->euler_deg; x->rot = m->rot;
}
void nora_xform_set(nora_mesh *m, const nora_xform *x) {
    m->translate = x->translate; m->scale = x->scale; m->euler_deg = x->euler_deg; m->rot = x->rot;
}
int nora_xform_equal(const nora_xform *a, const nora_xform *b) {
    return a->translate.x == b->translate.x && a->translate.y == b->translate.y && a->translate.z == b->translate.z &&
           a->scale.x == b->scale.x && a->scale.y == b->scale.y && a->scale.z == b->scale.z &&
           a->euler_deg.x == b->euler_deg.x && a->euler_deg.y == b->euler_deg.y && a->euler_deg.z == b->euler_deg.z &&
           a->rot.x == b->rot.x && a->rot.y == b->rot.y && a->rot.z == b->rot.z && a->rot.w == b->rot.w;
}

void nora_history_init(nora_history *h) { memset(h, 0, sizeof *h); }

static void op_free(nora_edit_op *op) { free(op->meshes); op->meshes = NULL; }

void nora_history_clear(nora_history *h) {
    int i;
    for (i = 0; i < h->n; i++) op_free(&h->ops[i]);
    h->n = h->pos = 0;
}
void nora_history_free(nora_history *h) { nora_history_clear(h); free(h->ops); memset(h, 0, sizeof *h); }

static nora_edit_op *push(nora_history *h) {
    int i;
    for (i = h->pos; i < h->n; i++) op_free(&h->ops[i]);   /* drop the redo tail */
    h->n = h->pos;
    if (h->n >= NORA_HISTORY_MAX) {                        /* forget the oldest step */
        op_free(&h->ops[0]);
        memmove(h->ops, h->ops + 1, sizeof(nora_edit_op) * (size_t)(h->n - 1));
        h->n--;
    }
    if (h->n >= h->cap) {
        int nc = h->cap ? h->cap * 2 : 32;
        nora_edit_op *no = (nora_edit_op *)realloc(h->ops, sizeof(nora_edit_op) * (size_t)nc);
        if (!no) return NULL;
        h->ops = no; h->cap = nc;
    }
    memset(&h->ops[h->n], 0, sizeof(nora_edit_op));
    h->pos = ++h->n;
    return &h->ops[h->n - 1];
}

int nora_history_push_transform(nora_history *h, int mesh, const nora_xform *before, const nora_xform *after, const char *label) {
    nora_edit_op *op;
    if (nora_xform_equal(before, after)) return 0;
    op = push(h);
    if (!op) return -1;
    op->kind = NORA_EDIT_TRANSFORM;
    op->mesh = mesh;
    op->before = *before;
    op->after = *after;
    snprintf(op->label, sizeof op->label, "%s", label ? label : "Transform");
    return 0;
}

int nora_history_push_visibility(nora_history *h, const int *meshes, int count, int hidden_after, const char *label) {
    nora_edit_op *op;
    if (count <= 0) return 0;
    op = push(h);
    if (!op) return -1;
    op->kind = NORA_EDIT_VISIBILITY;
    op->meshes = (int *)malloc(sizeof(int) * (size_t)count);
    if (!op->meshes) { h->n--; h->pos = h->n; return -1; }
    memcpy(op->meshes, meshes, sizeof(int) * (size_t)count);
    op->count = count;
    op->hidden_after = hidden_after;
    snprintf(op->label, sizeof op->label, "%s", label ? label : "Edit");
    return 0;
}

static void apply_op(nora_scene *s, const nora_edit_op *op, int forward) {
    int i;
    if (op->kind == NORA_EDIT_TRANSFORM) {
        if (op->mesh >= 0 && op->mesh < s->nmeshes) nora_xform_set(&s->meshes[op->mesh], forward ? &op->after : &op->before);
    } else {
        for (i = 0; i < op->count; i++)
            if (op->meshes[i] >= 0 && op->meshes[i] < s->nmeshes) s->meshes[op->meshes[i]].hidden = forward ? op->hidden_after : !op->hidden_after;
    }
    nora_scene_apply_transforms(s);
    s->built = 0;
}

int nora_history_undo(nora_history *h, nora_scene *s, const char **label) {
    if (h->pos <= 0) return 0;
    h->pos--;
    apply_op(s, &h->ops[h->pos], 0);
    if (label) *label = h->ops[h->pos].label;
    return 1;
}
int nora_history_redo(nora_history *h, nora_scene *s, const char **label) {
    if (h->pos >= h->n) return 0;
    apply_op(s, &h->ops[h->pos], 1);
    if (label) *label = h->ops[h->pos].label;
    h->pos++;
    return 1;
}
int nora_history_can_undo(const nora_history *h) { return h->pos > 0; }
int nora_history_can_redo(const nora_history *h) { return h->pos < h->n; }

int nora_edit_transform(nora_history *h, nora_scene *s, int mesh, const nora_xform *after, const char *label) {
    nora_xform before;
    if (mesh < 0 || mesh >= s->nmeshes) return -1;
    nora_xform_get(&s->meshes[mesh], &before);
    nora_xform_set(&s->meshes[mesh], after);
    nora_scene_apply_transforms(s);
    return nora_history_push_transform(h, mesh, &before, after, label);
}

int nora_edit_delete(nora_history *h, nora_scene *s, int mesh) {
    if (mesh < 0 || mesh >= s->nmeshes || s->meshes[mesh].hidden) return -1;
    s->meshes[mesh].hidden = 1;
    s->built = 0;
    return nora_history_push_visibility(h, &mesh, 1, 1, "Delete");
}

int nora_edit_duplicate(nora_history *h, nora_scene *s, int mesh) {
    int nm;
    if (mesh < 0 || mesh >= s->nmeshes || s->meshes[mesh].hidden) return -1;
    nm = nora_scene_duplicate_mesh(s, mesh);
    if (nm < 0) return -1;
    nora_history_push_visibility(h, &nm, 1, 0, "Duplicate");
    return nm;
}

int nora_edit_import(nora_history *h, nora_scene *s, const char *path, char *err, size_t errcap) {
    int count = 0, i, *list;
    int first = nora_scene_import_obj(s, path, &count, err, errcap);
    if (first < 0) return -1;
    list = (int *)malloc(sizeof(int) * (size_t)count);
    if (list) {
        for (i = 0; i < count; i++) list[i] = first + i;
        nora_history_push_visibility(h, list, count, 0, "Import");
        free(list);
    }
    return first;
}
