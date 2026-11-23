/* Nora - tiny JSON reader/writer. MIT License. */
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

typedef struct jp { const char *s; const char *start; char *err; size_t errcap; int failed; } jp;

static void jfail(jp *p, const char *msg) {
    if (!p->failed && p->err && p->errcap) {
        int line = 1;
        const char *q;
        for (q = p->start; q < p->s; q++) if (*q == '\n') line++;
        snprintf(p->err, p->errcap, "JSON line %d: %s", line, msg);
    }
    p->failed = 1;
}
static void ws(jp *p) { while (*p->s && isspace((unsigned char)*p->s)) p->s++; }

static js_value *jnew(js_kind k) {
    js_value *v = (js_value *)calloc(1, sizeof *v);
    if (v) v->kind = k;
    return v;
}

static char *jstring(jp *p) {
    size_t cap = 32, n = 0;
    char *out = (char *)malloc(cap);
    if (*p->s != '"') { jfail(p, "expected a string"); free(out); return NULL; }
    p->s++;
    while (*p->s && *p->s != '"') {
        char c = *p->s++;
        if (c == '\\') {
            char e = *p->s++;
            switch (e) {
            case 'n': c = '\n'; break; case 't': c = '\t'; break; case 'r': c = '\r'; break;
            case 'b': c = '\b'; break; case 'f': c = '\f'; break;
            case 'u': {
                unsigned v = 0;
                int k;
                for (k = 0; k < 4 && isxdigit((unsigned char)*p->s); k++) {
                    char h = *p->s++;
                    v = v * 16 + (unsigned)(isdigit((unsigned char)h) ? h - '0' : (tolower((unsigned char)h) - 'a' + 10));
                }
                c = v < 128 ? (char)v : '?';
                break;
            }
            default: c = e;
            }
        }
        if (n + 2 > cap) { cap *= 2; out = (char *)realloc(out, cap); }
        out[n++] = c;
    }
    if (*p->s != '"') { jfail(p, "unterminated string"); free(out); return NULL; }
    p->s++;
    out[n] = 0;
    return out;
}

static js_value *jvalue(jp *p, int depth) {
    js_value *v;
    ws(p);
    if (depth > 64) { jfail(p, "nesting too deep"); return NULL; }
    if (*p->s == '{') {
        js_value **tail;
        v = jnew(JS_OBJ);
        tail = &v->child;
        p->s++;
        ws(p);
        if (*p->s == '}') { p->s++; return v; }
        for (;;) {
            char *key;
            js_value *m;
            ws(p);
            key = jstring(p);
            if (!key) { js_free(v); return NULL; }
            ws(p);
            if (*p->s != ':') { jfail(p, "expected ':'"); free(key); js_free(v); return NULL; }
            p->s++;
            m = jvalue(p, depth + 1);
            if (!m) { free(key); js_free(v); return NULL; }
            m->key = key;
            *tail = m;
            tail = &m->next;
            ws(p);
            if (*p->s == ',') { p->s++; continue; }
            if (*p->s == '}') { p->s++; return v; }
            jfail(p, "expected ',' or '}'");
            js_free(v);
            return NULL;
        }
    }
    if (*p->s == '[') {
        js_value **tail;
        v = jnew(JS_ARR);
        tail = &v->child;
        p->s++;
        ws(p);
        if (*p->s == ']') { p->s++; return v; }
        for (;;) {
            js_value *e = jvalue(p, depth + 1);
            if (!e) { js_free(v); return NULL; }
            *tail = e;
            tail = &e->next;
            ws(p);
            if (*p->s == ',') { p->s++; continue; }
            if (*p->s == ']') { p->s++; return v; }
            jfail(p, "expected ',' or ']'");
            js_free(v);
            return NULL;
        }
    }
    if (*p->s == '"') {
        v = jnew(JS_STR);
        v->str = jstring(p);
        if (!v->str) { js_free(v); return NULL; }
        return v;
    }
    if (!strncmp(p->s, "true", 4)) { p->s += 4; v = jnew(JS_BOOL); v->num = 1; return v; }
    if (!strncmp(p->s, "false", 5)) { p->s += 5; v = jnew(JS_BOOL); v->num = 0; return v; }
    if (!strncmp(p->s, "null", 4)) { p->s += 4; return jnew(JS_NULL); }
    if (*p->s == '-' || isdigit((unsigned char)*p->s)) {
        char *end;
        v = jnew(JS_NUM);
        v->num = strtod(p->s, &end);
        p->s = end;
        return v;
    }
    jfail(p, "unexpected character");
    return NULL;
}

js_value *js_parse(const char *text, char *err, size_t errcap) {
    jp p;
    js_value *v;
    p.s = p.start = text ? text : "";
    p.err = err; p.errcap = errcap; p.failed = 0;
    v = jvalue(&p, 0);
    if (v) {
        ws(&p);
        if (*p.s) { jfail(&p, "trailing characters"); js_free(v); return NULL; }
    }
    return v;
}

void js_free(js_value *v) {
    while (v) {
        js_value *n = v->next;
        js_free(v->child);
        free(v->str);
        free(v->key);
        free(v);
        v = n;
    }
}

const js_value *js_get(const js_value *o, const char *key) {
    const js_value *c;
    if (!o || o->kind != JS_OBJ) return NULL;
    for (c = o->child; c; c = c->next) if (c->key && !strcmp(c->key, key)) return c;
    return NULL;
}
const js_value *js_at(const js_value *a, int i) {
    const js_value *c;
    if (!a || a->kind != JS_ARR) return NULL;
    for (c = a->child; c && i > 0; c = c->next) i--;
    return c;
}
int js_len(const js_value *a) {
    int n = 0;
    const js_value *c;
    if (!a || a->kind != JS_ARR) return 0;
    for (c = a->child; c; c = c->next) n++;
    return n;
}
double js_num(const js_value *v, double def) { return (v && (v->kind == JS_NUM || v->kind == JS_BOOL)) ? v->num : def; }
const char *js_str(const js_value *v, const char *def) { return (v && v->kind == JS_STR) ? v->str : def; }

void jb_puts(js_buf *b, const char *s) {
    size_t n = strlen(s);
    if (b->n + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 256;
        while (nc < b->n + n + 1) nc *= 2;
        b->p = (char *)realloc(b->p, nc);
        b->cap = nc;
    }
    memcpy(b->p + b->n, s, n + 1);
    b->n += n;
}
void jb_printf(js_buf *b, const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    jb_puts(b, tmp);
}
void jb_str(js_buf *b, const char *s) {
    char tmp[8];
    jb_puts(b, "\"");
    for (; s && *s; s++) {
        char c = *s;
        if (c == '"' || c == '\\') { tmp[0] = '\\'; tmp[1] = c; tmp[2] = 0; jb_puts(b, tmp); }
        else if (c == '\n') jb_puts(b, "\\n");
        else if (c == '\t') jb_puts(b, "\\t");
        else if ((unsigned char)c < 32) { snprintf(tmp, sizeof tmp, "\\u%04x", (unsigned char)c); jb_puts(b, tmp); }
        else { tmp[0] = c; tmp[1] = 0; jb_puts(b, tmp); }
    }
    jb_puts(b, "\"");
}
