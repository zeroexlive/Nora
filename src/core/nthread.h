/* Nora - portable threads, atomics and timing (pthreads or Win32). MIT License. */
#ifndef NORA_NTHREAD_H
#define NORA_NTHREAD_H

#include <stdint.h>

typedef void (*nthread_fn)(void *arg);

typedef struct nthread {
    void *handle;   /* pthread_t* or HANDLE */
    nthread_fn fn;
    void *arg;
} nthread;

int nthread_start(nthread *t, nthread_fn fn, void *arg);   /* 0 on success */
void nthread_join(nthread *t);
int nthread_cpu_count(void);

typedef struct nmutex { void *impl; } nmutex;
void nmutex_init(nmutex *m);
void nmutex_destroy(nmutex *m);
void nmutex_lock(nmutex *m);
void nmutex_unlock(nmutex *m);

int64_t natomic_add64(volatile int64_t *p, int64_t v);   /* returns new value */
int64_t natomic_load64(volatile int64_t *p);
void natomic_store64(volatile int64_t *p, int64_t v);
int natomic_load_int(volatile int *p);
void natomic_store_int(volatile int *p, int v);

double ntime_now(void);          /* seconds, monotonic */
void nsleep_ms(int ms);

#endif
