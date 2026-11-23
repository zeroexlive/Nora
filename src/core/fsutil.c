/* Nora - portable file system helpers. MIT License. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#include "fsutil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <io.h>
#else
#include <unistd.h>
#endif

char *nora_read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;
    size_t r;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    r = fread(buf, 1, (size_t)n, f);
    buf[r] = 0;
    fclose(f);
    if (len) *len = r;
    return buf;
}

int nora_write_file_atomic(const char *path, const void *data, size_t len) {
    char tmp[1100];
    FILE *f;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f) return -1;
    if (len && fwrite(data, 1, len, f) != len) { fclose(f); remove(tmp); return -1; }
    if (fclose(f) != 0) { remove(tmp); return -1; }
#ifdef _WIN32
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) { remove(tmp); return -1; }
#else
    if (rename(tmp, path) != 0) { remove(tmp); return -1; }
#endif
    return 0;
}

static int is_sep(char c) { return c == '/' || c == '\\'; }

void nora_path_dir(const char *path, char *out, size_t cap) {
    const char *s = NULL, *p;
    for (p = path; *p; p++) if (is_sep(*p)) s = p;
    if (!s) { snprintf(out, cap, "."); return; }
    if (s == path) { snprintf(out, cap, "/"); return; }
    snprintf(out, cap, "%.*s", (int)(s - path), path);
}

const char *nora_path_base(const char *path) {
    const char *s = path, *p;
    for (p = path; *p; p++) if (is_sep(*p)) s = p + 1;
    return s;
}

const char *nora_path_ext(const char *path) {
    const char *b = nora_path_base(path), *d = strrchr(b, '.');
    return d ? d : "";
}

int nora_path_is_abs(const char *path) {
    if (!path || !path[0]) return 0;
    if (is_sep(path[0])) return 1;
    if (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':') return 1;
    return 0;
}

void nora_path_join(const char *dir, const char *rel, char *out, size_t cap) {
    size_t n;
    if (nora_path_is_abs(rel) || !dir || !dir[0] || !strcmp(dir, ".")) { snprintf(out, cap, "%s", rel); return; }
    n = strlen(dir);
    if (n > 0 && is_sep(dir[n - 1])) snprintf(out, cap, "%s%s", dir, rel);
    else snprintf(out, cap, "%s/%s", dir, rel);
}

/* normalises an absolute path in place: '/' separators, no "." / ".." / "//" */
static void normalize(char *p) {
    char *seg[256];
    size_t nseg = 0, i;
    char prefix[8] = "";
    char *s, *tok;
    char buf[2048];
    for (s = p; *s; s++) if (*s == '\\') *s = '/';
    s = p;
    if (s[0] && s[1] == ':') { snprintf(prefix, sizeof prefix, "%c:", s[0]); s += 2; }
    snprintf(buf, sizeof buf, "%s", s);
    tok = buf;
    while (*tok) {
        char *e = strchr(tok, '/');
        if (e) *e = 0;
        if (!*tok || !strcmp(tok, ".")) { /* skip */ }
        else if (!strcmp(tok, "..")) { if (nseg) nseg--; }
        else if (nseg < 256) seg[nseg++] = tok;
        if (!e) break;
        tok = e + 1;
    }
    {
        char out[2048];
        size_t o = 0;
        o += (size_t)snprintf(out + o, sizeof out - o, "%s", prefix);
        if (nseg == 0) o += (size_t)snprintf(out + o, sizeof out - o, "/");
        for (i = 0; i < nseg && o < sizeof out; i++) o += (size_t)snprintf(out + o, sizeof out - o, "/%s", seg[i]);
        strcpy(p, out);
    }
}

void nora_path_abs(const char *path, char *out, size_t cap) {
    char buf[2048];
    if (nora_path_is_abs(path)) {
        snprintf(buf, sizeof buf, "%s", path);
    } else {
        char cwd[1024];
#ifdef _WIN32
        if (!_getcwd(cwd, sizeof cwd)) strcpy(cwd, ".");
#else
        if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, ".");
#endif
        snprintf(buf, sizeof buf, "%s/%s", cwd, path);
    }
    normalize(buf);
    snprintf(out, cap, "%s", buf);
}

void nora_path_rel(const char *path, const char *dir, char *out, size_t cap) {
    char a[2048], b[2048];
    const char *pa, *pb;
    size_t common = 0, i, o = 0;
    nora_path_abs(path, a, sizeof a);
    nora_path_abs(dir, b, sizeof b);
    /* different drives: keep absolute */
    if ((a[1] == ':') != (b[1] == ':') || (a[1] == ':' && a[0] != b[0])) { snprintf(out, cap, "%s", a); return; }
    if (!strcmp(b, "/")) b[0] = 0;
    /* longest common prefix ending at a separator */
    for (i = 0; a[i] && b[i] && a[i] == b[i]; i++) if (a[i] == '/') common = i;
    if (!b[i] && (a[i] == '/' || !a[i])) common = i;
    pa = a + common; if (*pa == '/') pa++;
    pb = b + common; if (*pb == '/') pb++;
    out[0] = 0;
    if (*pb) {
        o += (size_t)snprintf(out + o, cap > o ? cap - o : 0, "..");
        for (; *pb; pb++) if (*pb == '/') o += (size_t)snprintf(out + o, cap > o ? cap - o : 0, "/..");
        if (*pa) o += (size_t)snprintf(out + o, cap > o ? cap - o : 0, "/");
    }
    snprintf(out + o, cap > o ? cap - o : 0, "%s", *pa ? pa : (o ? "" : "."));
}

int nora_file_stat(const char *path, nora_fstat *st) {
    memset(st, 0, sizeof *st);
#ifdef _WIN32
    {
        WIN32_FILE_ATTRIBUTE_DATA d;
        if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d)) return -1;
        st->exists = 1;
        st->mtime_ns = (int64_t)((((uint64_t)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime) * 100u);
        st->size = (int64_t)(((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow);
    }
#else
    {
        struct stat s;
        if (stat(path, &s) != 0) return -1;
        st->exists = 1;
        st->size = (int64_t)s.st_size;
#if defined(__APPLE__)
        st->mtime_ns = (int64_t)s.st_mtimespec.tv_sec * 1000000000LL + s.st_mtimespec.tv_nsec;
#else
        st->mtime_ns = (int64_t)s.st_mtim.tv_sec * 1000000000LL + s.st_mtim.tv_nsec;
#endif
    }
#endif
    return 0;
}
