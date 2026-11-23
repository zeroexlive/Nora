/* Nora - tiny JSON reader/writer for shader graphs. MIT License. */
#ifndef NORA_JSON_H
#define NORA_JSON_H

#include <stddef.h>

typedef enum { JS_NULL = 0, JS_BOOL, JS_NUM, JS_STR, JS_ARR, JS_OBJ } js_kind;

typedef struct js_value {
    js_kind kind;
    double num;
    char *str;                 /* JS_STR value, or the key when inside an object */
    char *key;
    struct js_value *child;    /* first element / member */
    struct js_value *next;
} js_value;

js_value *js_parse(const char *text, char *err, size_t errcap);
void js_free(js_value *v);
const js_value *js_get(const js_value *obj, const char *key);
const js_value *js_at(const js_value *arr, int i);
int js_len(const js_value *arr);
double js_num(const js_value *v, double def);
const char *js_str(const js_value *v, const char *def);

/* growable output buffer */
typedef struct js_buf { char *p; size_t n, cap; } js_buf;
void jb_puts(js_buf *b, const char *s);
void jb_printf(js_buf *b, const char *fmt, ...);
void jb_str(js_buf *b, const char *s);   /* quoted and escaped */

#endif
