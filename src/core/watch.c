/* Nora - file change polling for hot reload. MIT License. */
#include "watch.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void nora_watch_init(nora_watch *w, double interval) {
    memset(w, 0, sizeof *w);
    w->interval = interval > 0 ? interval : 0.5;
    w->last = -1e30;
}

void nora_watch_free(nora_watch *w) { free(w->paths); free(w->st); w->paths = NULL; w->st = NULL; w->n = w->cap = 0; }

static void do_stat(nora_watch *w, int i, nora_fstat *st) {
    int rc = w->stat ? w->stat(w->paths[i], st, w->user) : nora_file_stat(w->paths[i], st);
    if (rc != 0) memset(st, 0, sizeof *st);
}

void nora_watch_set(nora_watch *w, char (*paths)[1024], int n, double now) {
    int i;
    if (n > w->cap) {
        void *np = realloc(w->paths, sizeof(*w->paths) * (size_t)n);
        void *ns = realloc(w->st, sizeof(nora_fstat) * (size_t)n);
        if (np) w->paths = (char (*)[1024])np;
        if (ns) w->st = (nora_fstat *)ns;
        if (!np || !ns) { w->n = 0; return; }
        w->cap = n;
    }
    w->n = n;
    for (i = 0; i < n; i++) {
        snprintf(w->paths[i], sizeof w->paths[i], "%s", paths[i]);
        do_stat(w, i, &w->st[i]);
    }
    w->last = now;
}

int nora_watch_poll(nora_watch *w, double now, int *changed, int cap) {
    int i, nc = 0;
    if (now - w->last < w->interval) return 0;
    w->last = now;
    for (i = 0; i < w->n; i++) {
        nora_fstat st;
        do_stat(w, i, &st);
        if (st.exists != w->st[i].exists || st.mtime_ns != w->st[i].mtime_ns || st.size != w->st[i].size) {
            w->st[i] = st;
            if (changed && nc < cap) changed[nc] = i;
            nc++;
        }
    }
    return nc;
}
