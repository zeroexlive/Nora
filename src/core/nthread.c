/* Nora - portable threads, atomics and timing. MIT License. */
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "nthread.h"
#include <stdlib.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>

static DWORD WINAPI nthread_tramp(LPVOID p) {
    nthread *t = (nthread *)p;
    t->fn(t->arg);
    return 0;
}
int nthread_start(nthread *t, nthread_fn fn, void *arg) {
    t->fn = fn; t->arg = arg;
    t->handle = (void *)CreateThread(NULL, 0, nthread_tramp, t, 0, NULL);
    return t->handle ? 0 : -1;
}
void nthread_join(nthread *t) {
    if (!t->handle) return;
    WaitForSingleObject((HANDLE)t->handle, INFINITE);
    CloseHandle((HANDLE)t->handle);
    t->handle = NULL;
}
int nthread_cpu_count(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
}
void nmutex_init(nmutex *m) {
    CRITICAL_SECTION *cs = (CRITICAL_SECTION *)malloc(sizeof *cs);
    InitializeCriticalSection(cs);
    m->impl = cs;
}
void nmutex_destroy(nmutex *m) {
    if (!m->impl) return;
    DeleteCriticalSection((CRITICAL_SECTION *)m->impl);
    free(m->impl); m->impl = NULL;
}
void nmutex_lock(nmutex *m) { EnterCriticalSection((CRITICAL_SECTION *)m->impl); }
void nmutex_unlock(nmutex *m) { LeaveCriticalSection((CRITICAL_SECTION *)m->impl); }
int64_t natomic_add64(volatile int64_t *p, int64_t v) { return InterlockedExchangeAdd64((volatile LONG64 *)p, v) + v; }
int64_t natomic_load64(volatile int64_t *p) { return InterlockedCompareExchange64((volatile LONG64 *)p, 0, 0); }
void natomic_store64(volatile int64_t *p, int64_t v) { InterlockedExchange64((volatile LONG64 *)p, v); }
int natomic_load_int(volatile int *p) { return (int)InterlockedCompareExchange((volatile LONG *)p, 0, 0); }
void natomic_store_int(volatile int *p, int v) { InterlockedExchange((volatile LONG *)p, v); }
double ntime_now(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}
void nsleep_ms(int ms) { Sleep((DWORD)ms); }

#else /* POSIX */
#include <pthread.h>
#include <time.h>
#include <unistd.h>

static void *nthread_tramp(void *p) {
    nthread *t = (nthread *)p;
    t->fn(t->arg);
    return NULL;
}
int nthread_start(nthread *t, nthread_fn fn, void *arg) {
    pthread_t *h = (pthread_t *)malloc(sizeof *h);
    t->fn = fn; t->arg = arg;
    if (!h) return -1;
    if (pthread_create(h, NULL, nthread_tramp, t) != 0) { free(h); t->handle = NULL; return -1; }
    t->handle = h;
    return 0;
}
void nthread_join(nthread *t) {
    if (!t->handle) return;
    pthread_join(*(pthread_t *)t->handle, NULL);
    free(t->handle);
    t->handle = NULL;
}
int nthread_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}
void nmutex_init(nmutex *m) {
    pthread_mutex_t *mx = (pthread_mutex_t *)malloc(sizeof *mx);
    pthread_mutex_init(mx, NULL);
    m->impl = mx;
}
void nmutex_destroy(nmutex *m) {
    if (!m->impl) return;
    pthread_mutex_destroy((pthread_mutex_t *)m->impl);
    free(m->impl); m->impl = NULL;
}
void nmutex_lock(nmutex *m) { pthread_mutex_lock((pthread_mutex_t *)m->impl); }
void nmutex_unlock(nmutex *m) { pthread_mutex_unlock((pthread_mutex_t *)m->impl); }
int64_t natomic_add64(volatile int64_t *p, int64_t v) { return __atomic_add_fetch(p, v, __ATOMIC_SEQ_CST); }
int64_t natomic_load64(volatile int64_t *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
void natomic_store64(volatile int64_t *p, int64_t v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
int natomic_load_int(volatile int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
void natomic_store_int(volatile int *p, int v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
double ntime_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
void nsleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
#endif
