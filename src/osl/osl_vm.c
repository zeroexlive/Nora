/* Nora OSL subset - bytecode VM, closures, noise and program queries. MIT License. */
#include "osl_internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------------------------- noise */

static uint32_t nhash3(int x, int y, int z) {
    uint32_t h = (uint32_t)x * 0x8da6b343u ^ (uint32_t)y * 0xd8163841u ^ (uint32_t)z * 0xcb1ab31fu;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15; h *= 0x27d4eb2du; h ^= h >> 16;
    return h;
}
static float grad3(uint32_t h, float x, float y, float z) {
    switch (h % 12u) {   /* the 12 cube edge directions */
    case 0: return x + y; case 1: return -x + y; case 2: return x - y; case 3: return -x - y;
    case 4: return x + z; case 5: return -x + z; case 6: return x - z; case 7: return -x - z;
    case 8: return y + z; case 9: return -y + z; case 10: return y - z; default: return -y - z;
    }
}
static float fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

float osl_perlin3(float x, float y, float z) {
    float fx = floorf(x), fy = floorf(y), fz = floorf(z);
    int ix = (int)fx, iy = (int)fy, iz = (int)fz;
    float rx = x - fx, ry = y - fy, rz = z - fz;
    float u = fade(rx), v = fade(ry), w = fade(rz);
    float n000 = grad3(nhash3(ix, iy, iz), rx, ry, rz);
    float n100 = grad3(nhash3(ix + 1, iy, iz), rx - 1, ry, rz);
    float n010 = grad3(nhash3(ix, iy + 1, iz), rx, ry - 1, rz);
    float n110 = grad3(nhash3(ix + 1, iy + 1, iz), rx - 1, ry - 1, rz);
    float n001 = grad3(nhash3(ix, iy, iz + 1), rx, ry, rz - 1);
    float n101 = grad3(nhash3(ix + 1, iy, iz + 1), rx - 1, ry, rz - 1);
    float n011 = grad3(nhash3(ix, iy + 1, iz + 1), rx, ry - 1, rz - 1);
    float n111 = grad3(nhash3(ix + 1, iy + 1, iz + 1), rx - 1, ry - 1, rz - 1);
    float r = lerpf(lerpf(lerpf(n000, n100, u), lerpf(n010, n110, u), v),
                    lerpf(lerpf(n001, n101, u), lerpf(n011, n111, u), v), w);
    r *= 0.97f;
    return r < -1.0f ? -1.0f : (r > 1.0f ? 1.0f : r);
}
float osl_cellnoise3(float x, float y, float z) {
    return (float)(nhash3((int)floorf(x), (int)floorf(y), (int)floorf(z)) >> 8) * (1.0f / 16777216.0f);
}

/* ------------------------------------------------------------------ exec */

osl_exec *osl_exec_create(void) {
    osl_exec *e = (osl_exec *)calloc(1, sizeof *e);
    return e;
}
void osl_exec_free(osl_exec *e) {
    if (!e) return;
    free(e->regs);
    free(e->cl);
    free(e);
}
const char *osl_exec_error(const osl_exec *e) { return e ? e->err : ""; }

static int cl_new(osl_exec *e) {
    if (e->ncl >= e->clcap) {
        int nc = e->clcap ? e->clcap * 2 : 64;
        void *np;
        if (nc > 65536) return -1;
        np = realloc(e->cl, sizeof(*e->cl) * (size_t)nc);
        if (!np) return -1;
        e->cl = (struct osl_clnode *)np;
        e->clcap = nc;
    }
    memset(&e->cl[e->ncl], 0, sizeof(e->cl[0]));
    return e->ncl++;
}

enum { CLN_PRIM = 1, CLN_ADD, CLN_MUL };

static void flatten(osl_exec *e, int idx, float w0, float w1, float w2, osl_closure_list *out, int depth) {
    struct osl_clnode *n;
    if (idx <= 0 || idx >= e->ncl || depth > 256) return;
    n = &e->cl[idx];
    if (n->kind == CLN_PRIM) {
        if (out->n >= OSL_MAX_CLOSURES) return;
        if (w0 == 0.0f && w1 == 0.0f && w2 == 0.0f) return;
        out->c[out->n] = n->prim;
        out->c[out->n].w[0] = w0; out->c[out->n].w[1] = w1; out->c[out->n].w[2] = w2;
        out->n++;
    } else if (n->kind == CLN_ADD) {
        int a = n->a, b = n->b;
        flatten(e, a, w0, w1, w2, out, depth + 1);
        flatten(e, b, w0, w1, w2, out, depth + 1);
    } else if (n->kind == CLN_MUL) {
        flatten(e, n->a, w0 * n->w[0], w1 * n->w[1], w2 * n->w[2], out, depth + 1);
    }
}

#define R(i) (regs + (size_t)(i) * 3)

static void hsv2rgb(const float *in, float *out) {
    float h = in[0], s = in[1], v = in[2];
    if (s <= 0.0f) { out[0] = out[1] = out[2] = v; return; }
    h = (h - floorf(h)) * 6.0f;
    {
        int i = (int)floorf(h);
        float f = h - (float)i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
        switch (i) {
        case 0: out[0] = v; out[1] = t; out[2] = p; break;
        case 1: out[0] = q; out[1] = v; out[2] = p; break;
        case 2: out[0] = p; out[1] = v; out[2] = t; break;
        case 3: out[0] = p; out[1] = q; out[2] = v; break;
        case 4: out[0] = t; out[1] = p; out[2] = v; break;
        default: out[0] = v; out[1] = p; out[2] = q; break;
        }
    }
}

static float un1(int id, float x) {
    switch (id) {
    case B_SIN: return sinf(x); case B_COS: return cosf(x); case B_TAN: return tanf(x);
    case B_ASIN: return asinf(x < -1 ? -1 : (x > 1 ? 1 : x)); case B_ACOS: return acosf(x < -1 ? -1 : (x > 1 ? 1 : x));
    case B_ATAN: return atanf(x); case B_SINH: return sinhf(x); case B_COSH: return coshf(x); case B_TANH: return tanhf(x);
    case B_SQRT: return x > 0 ? sqrtf(x) : 0.0f; case B_INVSQRT: return x > 0 ? 1.0f / sqrtf(x) : 0.0f;
    case B_EXP: return expf(x); case B_EXP2: return powf(2.0f, x);
    case B_LOG: return x > 0 ? logf(x) : -1e30f; case B_LOG2: return x > 0 ? logf(x) / 0.69314718f : -1e30f;
    case B_LOG10: return x > 0 ? log10f(x) : -1e30f;
    case B_ABS: return fabsf(x); case B_FLOOR: return floorf(x); case B_CEIL: return ceilf(x);
    case B_ROUND: return floorf(x + 0.5f); case B_TRUNC: return x < 0 ? ceilf(x) : floorf(x);
    case B_SIGN: return x > 0 ? 1.0f : (x < 0 ? -1.0f : 0.0f);
    case B_RADIANS: return x * (3.14159265f / 180.0f); case B_DEGREES: return x * (180.0f / 3.14159265f);
    default: return 0.0f;
    }
}
static float bin1(int id, float a, float b) {
    switch (id) {
    case B_POW: return (a < 0.0f && b != floorf(b)) ? 0.0f : powf(a, b);
    case B_FMOD: return b != 0.0f ? fmodf(a, b) : 0.0f;
    case B_MOD: return b != 0.0f ? a - b * floorf(a / b) : 0.0f;
    case B_MIN: return a < b ? a : b; case B_MAX: return a > b ? a : b;
    case B_ATAN2: return atan2f(a, b);
    case B_STEP: return b < a ? 0.0f : 1.0f;
    default: return 0.0f;
    }
}
static float tri1(int id, float a, float b, float c) {
    switch (id) {
    case B_CLAMP: return a < b ? b : (a > c ? c : a);
    case B_MIX: return a * (1.0f - c) + b * c;
    case B_SMOOTHSTEP: {
        float t;
        if (c < a) return 0.0f;
        if (c >= b) return 1.0f;
        t = (c - a) / (b - a);
        return t * t * (3.0f - 2.0f * t);
    }
    default: return 0.0f;
    }
}

typedef struct vmctx { const osl_program *p; const osl_globals *g; const osl_host *host; } vmctx;

static void noise_eval(int id, const float *pp, float t, float *out, int width) {
    static const float off[3][3] = { { 0, 0, 0 }, { 31.416f, 47.853f, 12.793f }, { -17.11f, 93.71f, 55.33f } };
    int k;
    float x = pp[0] + t * 0.713f, y = pp[1] + t * 0.137f, z = pp[2] + t * 0.571f;
    for (k = 0; k < width; k++) {
        float X = x + off[k][0], Y = y + off[k][1], Z = z + off[k][2], v;
        if (id == B_NOISE_CELL) v = osl_cellnoise3(X, Y, Z);
        else {
            v = osl_perlin3(X, Y, Z);
            if (id == B_NOISE_U) v = 0.5f * (v + 1.0f);
        }
        out[k] = v;
    }
}

static void call_builtin(const vmctx *vc, float *regs, const osl_insn *in) {
    const int *args = vc->p->args + in->a;
    int id = in->c & 255, width = in->c >> 8, k;
    float *d = R(in->d);
    float tmp[3] = { 0, 0, 0 };
    if (id <= B_DEGREES) {
        const float *a = R(args[0]);
        for (k = 0; k < width; k++) tmp[k] = un1(id, a[k]);
    } else if (id <= B_STEP) {
        const float *a = R(args[0]), *b = R(args[1]);
        for (k = 0; k < width; k++) tmp[k] = bin1(id, a[k], b[k]);
    } else if (id <= B_SMOOTHSTEP) {
        const float *a = R(args[0]), *b = R(args[1]), *c = R(args[2]);
        for (k = 0; k < width; k++) tmp[k] = tri1(id, a[k], b[k], c[k]);
    } else {
        switch (id) {
        case B_DOT: { const float *a = R(args[0]), *b = R(args[1]); tmp[0] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; break; }
        case B_CROSS: {
            const float *a = R(args[0]), *b = R(args[1]);
            tmp[0] = a[1] * b[2] - a[2] * b[1]; tmp[1] = a[2] * b[0] - a[0] * b[2]; tmp[2] = a[0] * b[1] - a[1] * b[0];
            break;
        }
        case B_LENGTH: { const float *a = R(args[0]); tmp[0] = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); break; }
        case B_DISTANCE: {
            const float *a = R(args[0]), *b = R(args[1]);
            float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
            tmp[0] = sqrtf(x * x + y * y + z * z);
            break;
        }
        case B_NORMALIZE: {
            const float *a = R(args[0]);
            float l = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
            if (l > 0) { tmp[0] = a[0] / l; tmp[1] = a[1] / l; tmp[2] = a[2] / l; }
            break;
        }
        case B_FACEFORWARD: {
            const float *n = R(args[0]), *i = R(args[1]), *nr = in->b > 2 ? R(args[2]) : n;
            float s = (i[0] * nr[0] + i[1] * nr[1] + i[2] * nr[2]) > 0.0f ? -1.0f : 1.0f;
            for (k = 0; k < 3; k++) tmp[k] = s * n[k];
            break;
        }
        case B_REFLECT: {
            const float *i = R(args[0]), *n = R(args[1]);
            float dn = 2.0f * (i[0] * n[0] + i[1] * n[1] + i[2] * n[2]);
            for (k = 0; k < 3; k++) tmp[k] = i[k] - dn * n[k];
            break;
        }
        case B_REFRACT: {
            const float *i = R(args[0]), *n = R(args[1]);
            float eta = R(args[2])[0], c = n[0] * i[0] + n[1] * i[1] + n[2] * i[2];
            float kk = 1.0f - eta * eta * (1.0f - c * c);
            if (kk >= 0.0f) for (k = 0; k < 3; k++) tmp[k] = eta * i[k] - (eta * c + sqrtf(kk)) * n[k];
            break;
        }
        case B_LUMINANCE: { const float *a = R(args[0]); tmp[0] = 0.2126f * a[0] + 0.7152f * a[1] + 0.0722f * a[2]; break; }
        case B_HYPOT: { float a = R(args[0])[0], b = R(args[1])[0], c = R(args[2])[0]; tmp[0] = sqrtf(a * a + b * b + c * c); break; }
        case B_NOISE_U: case B_NOISE_S: case B_NOISE_CELL:
            noise_eval(id, R(args[0]), R(args[1])[0], tmp, width);
            break;
        case B_TEXTURE: {
            int si = (int)R(args[0])[0];
            const char *name = (si >= 0 && si < vc->p->nstrings) ? vc->p->strings[si] : "";
            float rgb[3] = { 1, 0, 1 };
            if (vc->host && vc->host->texture) vc->host->texture(vc->host->user, name, R(args[1])[0], R(args[2])[0], rgb);
            tmp[0] = rgb[0]; tmp[1] = rgb[1]; tmp[2] = rgb[2];
            break;
        }
        case B_BACKFACING: tmp[0] = vc->g && vc->g->backfacing ? 1.0f : 0.0f; break;
        case B_RAYTYPE: {
            int si = (int)R(args[0])[0];
            const char *name = (si >= 0 && si < vc->p->nstrings) ? vc->p->strings[si] : "";
            tmp[0] = 0.0f;
            (void)name;   /* ray types are not tracked: every query answers 0 */
            break;
        }
        case B_ISNAN: { float x = R(args[0])[0]; tmp[0] = x != x ? 1.0f : 0.0f; break; }
        case B_ISINF: { float x = R(args[0])[0]; tmp[0] = (x == x && (x > 3e38f || x < -3e38f)) ? 1.0f : 0.0f; break; }
        case B_HSV2RGB: hsv2rgb(R(args[0]), tmp); break;
        case B_FRESNEL_SCHLICK: {
            float c = R(args[0])[0], f0 = R(args[1])[0], m = 1.0f - (c < 0 ? 0 : (c > 1 ? 1 : c));
            tmp[0] = f0 + (1.0f - f0) * m * m * m * m * m;
            break;
        }
        default: break;
        }
    }
    d[0] = tmp[0]; d[1] = tmp[1]; d[2] = tmp[2];
}

int osl_execute(const osl_program *p, osl_exec *e, const osl_globals *g, const osl_host *host,
                const osl_override *ov, int nov, osl_closure_list *out) {
    float *regs;
    int pc = 0;
    long budget = 20000000L;
    vmctx vc;
    if (out) out->n = 0;
    if (!p || !e) return -1;
    e->err[0] = 0;
    if (p->nregs * 3 > e->cap) {
        float *nr = (float *)realloc(e->regs, sizeof(float) * (size_t)p->nregs * 3);
        if (!nr) return -1;
        e->regs = nr;
        e->cap = p->nregs * 3;
    }
    regs = e->regs;
    memcpy(regs, p->init, sizeof(float) * (size_t)p->nregs * 3);
    if (g) {
        memcpy(R(R_P), g->P, 12); memcpy(R(R_I), g->I, 12); memcpy(R(R_N), g->N, 12); memcpy(R(R_NG), g->Ng, 12);
        memcpy(R(R_DPDU), g->dPdu, 12); memcpy(R(R_DPDV), g->dPdv, 12);
        R(R_U)[0] = g->u; R(R_V)[0] = g->v; R(R_TIME)[0] = g->time;
    }
    R(R_CI)[0] = 0.0f;
    e->ncl = 0;
    cl_new(e);   /* index 0 = empty closure */
    vc.p = p; vc.g = g; vc.host = host;
    for (;;) {
        const osl_insn *in = &p->code[pc];
        float *d;
        if (--budget <= 0) { snprintf(e->err, sizeof e->err, "line %d: instruction budget exceeded (infinite loop?)", in->line); return 1; }
        d = R(in->d);
        switch (in->op) {
        case OP_END: goto done;
        case OP_NOP: break;
        case OP_MOV1: d[0] = R(in->a)[0]; break;
        case OP_MOV3: { const float *a = R(in->a); d[0] = a[0]; d[1] = a[1]; d[2] = a[2]; break; }
        case OP_BCAST: { float v = R(in->a)[0]; d[0] = d[1] = d[2] = v; break; }
        case OP_ADD1: d[0] = R(in->a)[0] + R(in->b)[0]; break;
        case OP_SUB1: d[0] = R(in->a)[0] - R(in->b)[0]; break;
        case OP_MUL1: d[0] = R(in->a)[0] * R(in->b)[0]; break;
        case OP_DIV1: { float b = R(in->b)[0]; d[0] = b != 0.0f ? R(in->a)[0] / b : 0.0f; break; }
        case OP_MOD1: { float a = R(in->a)[0], b = R(in->b)[0]; d[0] = b != 0.0f ? fmodf(a, b) : 0.0f; break; }
        case OP_IDIV: { int a = (int)R(in->a)[0], b = (int)R(in->b)[0]; d[0] = b != 0 ? (float)(a / b) : 0.0f; break; }
        case OP_IMOD: { int a = (int)R(in->a)[0], b = (int)R(in->b)[0]; d[0] = b != 0 ? (float)(a % b) : 0.0f; break; }
        case OP_ADD3: { const float *a = R(in->a), *b = R(in->b); float x = a[0] + b[0], y = a[1] + b[1], z = a[2] + b[2]; d[0] = x; d[1] = y; d[2] = z; break; }
        case OP_SUB3: { const float *a = R(in->a), *b = R(in->b); float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2]; d[0] = x; d[1] = y; d[2] = z; break; }
        case OP_MUL3: { const float *a = R(in->a), *b = R(in->b); float x = a[0] * b[0], y = a[1] * b[1], z = a[2] * b[2]; d[0] = x; d[1] = y; d[2] = z; break; }
        case OP_DIV3: {
            const float *a = R(in->a), *b = R(in->b);
            float x = b[0] != 0 ? a[0] / b[0] : 0, y = b[1] != 0 ? a[1] / b[1] : 0, z = b[2] != 0 ? a[2] / b[2] : 0;
            d[0] = x; d[1] = y; d[2] = z;
            break;
        }
        case OP_NEG1: d[0] = -R(in->a)[0]; break;
        case OP_NEG3: { const float *a = R(in->a); d[0] = -a[0]; d[1] = -a[1]; d[2] = -a[2]; break; }
        case OP_NOT: d[0] = R(in->a)[0] == 0.0f ? 1.0f : 0.0f; break;
        case OP_FTOI: { float a = R(in->a)[0]; d[0] = a < 0 ? ceilf(a) : floorf(a); break; }
        case OP_LT: d[0] = R(in->a)[0] < R(in->b)[0] ? 1.0f : 0.0f; break;
        case OP_LE: d[0] = R(in->a)[0] <= R(in->b)[0] ? 1.0f : 0.0f; break;
        case OP_GT: d[0] = R(in->a)[0] > R(in->b)[0] ? 1.0f : 0.0f; break;
        case OP_GE: d[0] = R(in->a)[0] >= R(in->b)[0] ? 1.0f : 0.0f; break;
        case OP_EQ1: d[0] = R(in->a)[0] == R(in->b)[0] ? 1.0f : 0.0f; break;
        case OP_NE1: d[0] = R(in->a)[0] != R(in->b)[0] ? 1.0f : 0.0f; break;
        case OP_EQ3: { const float *a = R(in->a), *b = R(in->b); d[0] = (a[0] == b[0] && a[1] == b[1] && a[2] == b[2]) ? 1.0f : 0.0f; break; }
        case OP_NE3: { const float *a = R(in->a), *b = R(in->b); d[0] = (a[0] == b[0] && a[1] == b[1] && a[2] == b[2]) ? 0.0f : 1.0f; break; }
        case OP_JMP: pc = in->a; continue;
        case OP_JZ: if (R(in->a)[0] == 0.0f) { pc = in->b; continue; } break;
        case OP_JNZ: if (R(in->a)[0] != 0.0f) { pc = in->b; continue; } break;
        case OP_COMP: { int k = (int)R(in->b)[0]; if (k < 0 || k > 2) { snprintf(e->err, sizeof e->err, "line %d: component index %d out of range", in->line, k); return 1; } d[0] = R(in->a)[k]; break; }
        case OP_COMPK: d[0] = R(in->a)[in->c]; break;
        case OP_SETCOMP: { int k = (int)R(in->b)[0]; if (k < 0 || k > 2) { snprintf(e->err, sizeof e->err, "line %d: component index %d out of range", in->line, k); return 1; } d[k] = R(in->a)[0]; break; }
        case OP_SETCOMPK: d[in->c] = R(in->a)[0]; break;
        case OP_MAKE3: { float x = R(in->a)[0], y = R(in->b)[0], z = R(in->c)[0]; d[0] = x; d[1] = y; d[2] = z; break; }
        case OP_CALL: call_builtin(&vc, regs, in); break;
        case OP_CLOSURE: {
            const int *args = p->args + in->a;
            int idx = cl_new(e);
            struct osl_clnode *n;
            if (idx < 0) { snprintf(e->err, sizeof e->err, "line %d: too many closures", in->line); return 1; }
            n = &e->cl[idx];
            n->kind = CLN_PRIM;
            n->prim.type = in->c;
            n->prim.w[0] = n->prim.w[1] = n->prim.w[2] = 1.0f;
            if (in->b >= 1) memcpy(n->prim.N, R(args[0]), 12);
            switch (in->c) {
            case OSL_CL_MICROFACET:
                n->prim.alpha = R(args[1])[0]; n->prim.eta = R(args[2])[0]; n->prim.refract = (int)R(args[3])[0];
                break;
            case OSL_CL_REFLECTION: case OSL_CL_REFRACTION:
                n->prim.eta = R(args[1])[0];
                break;
            default: break;
            }
            d[0] = (float)idx;
            break;
        }
        case OP_CL_ADD: {
            int a = (int)R(in->a)[0], b = (int)R(in->b)[0];
            if (a == 0) d[0] = (float)b;
            else if (b == 0) d[0] = (float)a;
            else {
                int idx = cl_new(e);
                if (idx < 0) { snprintf(e->err, sizeof e->err, "line %d: too many closures", in->line); return 1; }
                e->cl[idx].kind = CLN_ADD; e->cl[idx].a = a; e->cl[idx].b = b;
                d[0] = (float)idx;
            }
            break;
        }
        case OP_CL_MUL3:
        case OP_CL_MUL1: {
            int a = (int)R(in->a)[0];
            const float *w = R(in->b);
            if (a == 0) { d[0] = 0.0f; break; }
            {
                int idx = cl_new(e);
                if (idx < 0) { snprintf(e->err, sizeof e->err, "line %d: too many closures", in->line); return 1; }
                e->cl[idx].kind = CLN_MUL; e->cl[idx].a = a;
                if (in->op == OP_CL_MUL3) { e->cl[idx].w[0] = w[0]; e->cl[idx].w[1] = w[1]; e->cl[idx].w[2] = w[2]; }
                else { e->cl[idx].w[0] = e->cl[idx].w[1] = e->cl[idx].w[2] = w[0]; }
                d[0] = (float)idx;
            }
            break;
        }
        case OP_PARAM: {
            int i, found = 0;
            for (i = 0; i < nov && ov; i++) {
                if (ov[i].param == in->a) {
                    const osl_param_info *pi = &p->params[in->a];
                    float *pr = R(pi->reg);
                    if (pi->type == OSL_T_STRING || pi->type == OSL_T_CLOSURE) break;
                    pr[0] = ov[i].v[0]; pr[1] = ov[i].v[1]; pr[2] = ov[i].v[2];
                    if (pi->type == OSL_T_INT) pr[0] = floorf(pr[0]);
                    found = 1;
                    break;
                }
            }
            if (found) { pc = in->b; continue; }
            break;
        }
        default:
            snprintf(e->err, sizeof e->err, "bad opcode %d", in->op);
            return 1;
        }
        pc++;
    }
done:
    if (out) {
        int ci = (int)R(R_CI)[0];
        flatten(e, ci, 1.0f, 1.0f, 1.0f, out, 0);
    }
    return 0;
}

int osl_exec_read_param(const osl_program *p, const osl_exec *e, const char *name, float out[3]) {
    int i;
    for (i = 0; i < p->nparams; i++) {
        if (strcmp(p->params[i].name, name) == 0) {
            const float *r = e->regs + (size_t)p->params[i].reg * 3;
            out[0] = r[0]; out[1] = r[1]; out[2] = r[2];
            return 0;
        }
    }
    return -1;
}

/* --------------------------------------------------------------- queries */

const char *osl_program_name(const osl_program *p) { return p ? p->name : ""; }
int osl_program_may_emit(const osl_program *p) { return p ? p->may_emit : 0; }
int osl_program_instruction_count(const osl_program *p) { return p ? p->ncode : 0; }
int osl_program_texture_count(const osl_program *p) { return p ? p->ntextures : 0; }
const char *osl_program_texture_name(const osl_program *p, int i) { return (p && i >= 0 && i < p->ntextures) ? p->textures[i] : ""; }
int osl_param_count(const osl_program *p) { return p ? p->nparams : 0; }
const char *osl_param_name(const osl_program *p, int i) { return (p && i >= 0 && i < p->nparams) ? p->params[i].name : ""; }
osl_type osl_param_type(const osl_program *p, int i) { return (p && i >= 0 && i < p->nparams) ? p->params[i].type : OSL_T_ERROR; }
int osl_param_is_output(const osl_program *p, int i) { return (p && i >= 0 && i < p->nparams) ? p->params[i].is_output : 0; }
int osl_find_param(const osl_program *p, const char *name) {
    int i;
    for (i = 0; p && i < p->nparams; i++) if (strcmp(p->params[i].name, name) == 0) return i;
    return -1;
}

static const char *const g_opnames[OP_COUNT] = {
    "end", "nop", "mov1", "mov3", "bcast", "add1", "sub1", "mul1", "div1", "mod1", "idiv", "imod",
    "add3", "sub3", "mul3", "div3", "neg1", "neg3", "not", "ftoi", "lt", "le", "gt", "ge", "eq1", "ne1", "eq3", "ne3",
    "jmp", "jz", "jnz", "comp", "compk", "setcomp", "setcompk", "make3", "call", "closure", "cl_add", "cl_mul3", "cl_mul1", "param"
};

size_t osl_program_disassemble(const osl_program *p, char *buf, size_t cap) {
    size_t n = 0;
    int i;
    if (!p || !buf || cap == 0) return 0;
    buf[0] = 0;
    for (i = 0; i < p->ncode && n + 80 < cap; i++) {
        const osl_insn *in = &p->code[i];
        int w = snprintf(buf + n, cap - n, "%4d  %-8s d=%d a=%d b=%d c=%d   ; line %d\n", i,
                         (in->op >= 0 && in->op < OP_COUNT) ? g_opnames[in->op] : "?", in->d, in->a, in->b, in->c, in->line);
        if (w < 0) break;
        n += (size_t)w;
    }
    return n;
}
