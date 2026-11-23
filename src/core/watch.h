/* Nora - hot reload: polls the modification time and size of a set of files.
 * MIT License. */
#ifndef NORA_WATCH_H
#define NORA_WATCH_H

#include "fsutil.h"

typedef int (*nora_stat_fn)(const char *path, nora_fstat *st, void *user);

typedef struct nora_watch {
    char (*paths)[1024];
    nora_fstat *st;
    int n, cap;
    double interval;            /* seconds between polls (default 0.5) */
    double last;                /* time of the last poll */
    nora_stat_fn stat;          /* NULL = the file system (nora_file_stat); tests inject their own */
    void *user;
} nora_watch;

void nora_watch_init(nora_watch *w, double interval);
void nora_watch_free(nora_watch *w);
/* Replaces the watched set and records the current state of every file. */
void nora_watch_set(nora_watch *w, char (*paths)[1024], int n, double now);
/* Polls when interval has elapsed since the last poll. Returns how many files
 * changed (appeared, disappeared, or got a new modification time or size) and
 * writes up to cap of their indices. A changed file's new state is recorded, so
 * each change is reported once. */
int nora_watch_poll(nora_watch *w, double now, int *changed, int cap);

#endif
