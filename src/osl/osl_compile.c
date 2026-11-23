/* Nora OSL subset - type checker and bytecode generator. MIT License. */
#include "osl_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

typedef struct cg_sym { const char *name; osl_type type; int reg; int depth; int ro; } cg_sym;
typedef struct cg_patch { int *v; int n, cap; } cg_patch;
typedef struct cg_loop { cg_patch brk, cont; } cg_loop;
typedef struct cg_fn { osl_type ret; int ret_reg; cg_patch rets; } cg_fn;

typedef struct cg {
    osl_front *f;
    osl_program *p;
    int code_cap, args_cap, regs_cap, str_cap, tex_cap, par_cap;
    cg_sym *syms; int nsyms, symcap;
    int depth;
    cg_loop *loops[64]; int nloops;
    cg_fn *fn; int inline_depth;
    int hide_from, hide_to;
    int nglobals_syms;
    cg_patch shader_rets;
    int zero_reg, one_reg;
} cg;

static void cg_error(cg *c, osl_node *n, const char *fmt, ...) {
    char msg[384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    osl_front_error(c->f, n ? n->line : 0, n ? n->col : 0, "%s", msg);
}

static void *grow(cg *c, void *ptr, int *cap, int need, size_t elem) {
    if (need > *cap) {
        int nc = *cap ? *cap : 16;
        void *np;
        while (nc < need) nc *= 2;
        np = realloc(ptr, elem * (size_t)nc);
        if (!np) cg_error(c, NULL, "out of memory");
        *cap = nc;
        return np;
    }
    return ptr;
}

static void patch_add(cg *c, cg_patch *pl, int idx) {
    pl->v = (int *)grow(c, pl->v, &pl->cap, pl->n + 1, sizeof(int));
    pl->v[pl->n++] = idx;
}

static int is_triple(osl_type t) { return t == OSL_T_COLOR || t == OSL_T_POINT || t == OSL_T_VECTOR || t == OSL_T_NORMAL; }
static int is_scalar(osl_type t) { return t == OSL_T_INT || t == OSL_T_FLOAT; }

const char *osl_type_name(osl_type t) {
    switch (t) {
    case OSL_T_VOID: return "void"; case OSL_T_INT: return "int"; case OSL_T_FLOAT: return "float";
    case OSL_T_COLOR: return "color"; case OSL_T_POINT: return "point"; case OSL_T_VECTOR: return "vector";
    case OSL_T_NORMAL: return "normal"; case OSL_T_STRING: return "string"; case OSL_T_CLOSURE: return "closure color";
    default: return "<error>";
    }
}

static int emit(cg *c, int op, int d, int a, int b, int cc, int line) {
    osl_insn *in;
    c->p->code = (osl_insn *)grow(c, c->p->code, &c->code_cap, c->p->ncode + 1, sizeof(osl_insn));
    in = &c->p->code[c->p->ncode];
    in->op = op; in->d = d; in->a = a; in->b = b; in->c = cc; in->line = line;
    return c->p->ncode++;
}
static int here(cg *c) { return c->p->ncode; }
static void patch_to(cg *c, cg_patch *pl, int target) {
    int i;
    for (i = 0; i < pl->n; i++) {
        osl_insn *in = &c->p->code[pl->v[i]];
        if (in->op == OP_JMP) in->a = target; else in->b = target;
    }
    free(pl->v);
    pl->v = NULL; pl->n = pl->cap = 0;
}

static int new_reg(cg *c) {
    osl_program *p = c->p;
    p->init = (float *)grow(c, p->init, &c->regs_cap, (p->nregs + 1) * 3, sizeof(float));
    /* grow() counts floats; keep capacity in floats */
    p->init[p->nregs * 3] = p->init[p->nregs * 3 + 1] = p->init[p->nregs * 3 + 2] = 0.0f;
    return p->nregs++;
}
static int const_reg3(cg *c, float x, float y, float z) {
    int r = new_reg(c);
    c->p->init[r * 3] = x; c->p->init[r * 3 + 1] = y; c->p->init[r * 3 + 2] = z;
    return r;
}
static int const_reg(cg *c, float x) { return const_reg3(c, x, 0.0f, 0.0f); }

static int intern(cg *c, const char *s) {
    osl_program *p = c->p;
    int i;
    for (i = 0; i < p->nstrings; i++) if (strcmp(p->strings[i], s) == 0) return i;
    p->strings = (char **)grow(c, p->strings, &c->str_cap, p->nstrings + 1, sizeof(char *));
    p->strings[p->nstrings] = (char *)malloc(strlen(s) + 1);
    strcpy(p->strings[p->nstrings], s);
    return p->nstrings++;
}

static void add_texture(cg *c, const char *name) {
    osl_program *p = c->p;
    int i;
    for (i = 0; i < p->ntextures; i++) if (strcmp(p->textures[i], name) == 0) return;
    p->textures = (char **)grow(c, p->textures, &c->tex_cap, p->ntextures + 1, sizeof(char *));
    p->textures[p->ntextures] = (char *)malloc(strlen(name) + 1);
    strcpy(p->textures[p->ntextures], name);
    p->ntextures++;
}

static void sym_push(cg *c, const char *name, osl_type t, int reg, int ro) {
    c->syms = (cg_sym *)grow(c, c->syms, &c->symcap, c->nsyms + 1, sizeof(cg_sym));
    c->syms[c->nsyms].name = name;
    c->syms[c->nsyms].type = t;
    c->syms[c->nsyms].reg = reg;
    c->syms[c->nsyms].depth = c->depth;
    c->syms[c->nsyms].ro = ro;
    c->nsyms++;
}
static cg_sym *sym_find(cg *c, const char *name) {
    int i;
    for (i = c->nsyms - 1; i >= 0; i--) {
        if (i >= c->hide_from && i < c->hide_to) continue;
        if (strcmp(c->syms[i].name, name) == 0) return &c->syms[i];
    }
    return NULL;
}
static void scope_push(cg *c) { c->depth++; }
static void scope_pop(cg *c) {
    while (c->nsyms > 0 && c->syms[c->nsyms - 1].depth >= c->depth) c->nsyms--;
    c->depth--;
}

/* --------------------------------------------------------- conversions */

static int convert(cg *c, osl_node *n, int reg, osl_type from, osl_type to) {
    if (from == to) return reg;
    if (to == OSL_T_FLOAT && from == OSL_T_INT) return reg;
    if (to == OSL_T_INT && from == OSL_T_FLOAT) { int r = new_reg(c); emit(c, OP_FTOI, r, reg, 0, 0, n->line); return r; }
    if (is_triple(to) && is_scalar(from)) { int r = new_reg(c); emit(c, OP_BCAST, r, reg, 0, 0, n->line); return r; }
    if (is_triple(to) && is_triple(from)) return reg;
    if (to == OSL_T_CLOSURE && is_scalar(from)) {
        /* only the literal 0 is a valid closure value */
        if (n->kind == N_NUM && n->num == 0.0) return c->zero_reg;
    }
    cg_error(c, n, "cannot convert %s to %s", osl_type_name(from), osl_type_name(to));
    return -1;
}
static int to3(cg *c, osl_node *n, int reg, osl_type t) {
    if (is_triple(t)) return reg;
    if (is_scalar(t)) return convert(c, n, reg, t, OSL_T_VECTOR);
    cg_error(c, n, "expected a number or a color/point/vector/normal, got %s", osl_type_name(t));
    return -1;
}
static int to1(cg *c, osl_node *n, int reg, osl_type t) {
    if (is_scalar(t)) return reg;
    cg_error(c, n, "expected a float, got %s", osl_type_name(t));
    return -1;
}

static int cexpr(cg *c, osl_node *n, osl_type want, osl_type *ty);
static void cstmt(cg *c, osl_node *n);

/* ----------------------------------------------------------- lvalues */

typedef struct lval { int reg; osl_type type; int is_comp; int comp_k; int comp_reg; } lval;

static lval get_lval(cg *c, osl_node *n) {
    lval lv;
    memset(&lv, 0, sizeof lv);
    if (n->kind == N_IDENT) {
        cg_sym *s = sym_find(c, n->name);
        if (!s) cg_error(c, n, "undeclared identifier '%s'", n->name);
        if (s->ro) cg_error(c, n, "'%s' is read-only", n->name);
        lv.reg = s->reg; lv.type = s->type;
        return lv;
    }
    if (n->kind == N_INDEX) {
        lval base = get_lval(c, n->a);
        osl_type it;
        if (!is_triple(base.type) || base.is_comp) cg_error(c, n, "only color/point/vector/normal components can be indexed");
        lv.reg = base.reg; lv.type = OSL_T_FLOAT; lv.is_comp = 1;
        if (n->b->kind == N_NUM) {
            lv.comp_k = (int)n->b->num;
            if (lv.comp_k < 0 || lv.comp_k > 2) cg_error(c, n, "component index %d out of range", lv.comp_k);
            lv.comp_reg = -1;
        } else {
            int r = cexpr(c, n->b, OSL_T_INT, &it);
            lv.comp_reg = to1(c, n->b, r, it);
        }
        return lv;
    }
    cg_error(c, n, "expression is not assignable");
    return lv;
}

static int load_lval(cg *c, osl_node *n, const lval *lv) {
    if (!lv->is_comp) return lv->reg;
    {
        int r = new_reg(c);
        if (lv->comp_reg < 0) emit(c, OP_COMPK, r, lv->reg, 0, lv->comp_k, n->line);
        else emit(c, OP_COMP, r, lv->reg, lv->comp_reg, 0, n->line);
        return r;
    }
}

static void store_lval(cg *c, osl_node *n, const lval *lv, int src, osl_type st) {
    int r = convert(c, n, src, st, lv->type);
    if (lv->is_comp) {
        if (lv->comp_reg < 0) emit(c, OP_SETCOMPK, lv->reg, r, 0, lv->comp_k, n->line);
        else emit(c, OP_SETCOMP, lv->reg, r, lv->comp_reg, 0, n->line);
    } else if (is_triple(lv->type)) {
        if (r != lv->reg) emit(c, OP_MOV3, lv->reg, r, 0, 0, n->line);
    } else {
        if (r != lv->reg) emit(c, OP_MOV1, lv->reg, r, 0, 0, n->line);
    }
}

/* -------------------------------------------------------- arithmetic */

static int arith(cg *c, osl_node *n, int op, int ra, osl_type ta, int rb, osl_type tb, osl_type *ty) {
    int d;
    if (ta == OSL_T_CLOSURE || tb == OSL_T_CLOSURE) {
        if (op == P_PLUS && ta == OSL_T_CLOSURE && tb == OSL_T_CLOSURE) {
            d = new_reg(c); emit(c, OP_CL_ADD, d, ra, rb, 0, n->line); *ty = OSL_T_CLOSURE; return d;
        }
        if (op == P_STAR || op == P_SLASH) {
            int cl = ta == OSL_T_CLOSURE ? ra : rb, w = ta == OSL_T_CLOSURE ? rb : ra;
            osl_type tw = ta == OSL_T_CLOSURE ? tb : ta;
            if (ta == OSL_T_CLOSURE && tb == OSL_T_CLOSURE) cg_error(c, n, "cannot multiply two closures");
            if (op == P_SLASH) {
                if (ta != OSL_T_CLOSURE) cg_error(c, n, "cannot divide by a closure");
                {
                    int one = c->one_reg, inv = new_reg(c);
                    if (is_triple(tw)) { int o3 = new_reg(c); emit(c, OP_BCAST, o3, one, 0, 0, n->line); emit(c, OP_DIV3, inv, o3, w, 0, n->line); }
                    else emit(c, OP_DIV1, inv, one, w, 0, n->line);
                    w = inv;
                }
            }
            d = new_reg(c);
            if (is_triple(tw)) emit(c, OP_CL_MUL3, d, cl, w, 0, n->line);
            else if (is_scalar(tw)) emit(c, OP_CL_MUL1, d, cl, w, 0, n->line);
            else cg_error(c, n, "closures can only be scaled by a float or a color");
            *ty = OSL_T_CLOSURE;
            return d;
        }
        cg_error(c, n, "invalid operation on a closure");
    }
    if (ta == OSL_T_STRING || tb == OSL_T_STRING || ta == OSL_T_VOID || tb == OSL_T_VOID)
        cg_error(c, n, "invalid operands to arithmetic (%s, %s)", osl_type_name(ta), osl_type_name(tb));
    if (is_triple(ta) || is_triple(tb)) {
        int a3 = to3(c, n, ra, ta), b3 = to3(c, n, rb, tb), o;
        osl_type rt = is_triple(ta) ? ta : tb;
        if (is_triple(ta) && is_triple(tb) && ta == OSL_T_POINT && tb == OSL_T_POINT && op == P_MINUS) rt = OSL_T_VECTOR;
        switch (op) {
        case P_PLUS: o = OP_ADD3; break; case P_MINUS: o = OP_SUB3; break;
        case P_STAR: o = OP_MUL3; break; case P_SLASH: o = OP_DIV3; break;
        default: cg_error(c, n, "operator not supported on %s", osl_type_name(rt)); o = 0;
        }
        d = new_reg(c);
        emit(c, o, d, a3, b3, 0, n->line);
        *ty = rt;
        return d;
    }
    {
        int isint = ta == OSL_T_INT && tb == OSL_T_INT, o;
        switch (op) {
        case P_PLUS: o = OP_ADD1; break; case P_MINUS: o = OP_SUB1; break; case P_STAR: o = OP_MUL1; break;
        case P_SLASH: o = isint ? OP_IDIV : OP_DIV1; break;
        case P_PERCENT: o = isint ? OP_IMOD : OP_MOD1; break;
        default: o = 0; cg_error(c, n, "bad operator");
        }
        d = new_reg(c);
        emit(c, o, d, ra, rb, 0, n->line);
        *ty = isint ? OSL_T_INT : OSL_T_FLOAT;
        return d;
    }
}

/* ------------------------------------------------------------- calls */

enum { K_UN = 1, K_BIN, K_TRI, K_SPECIAL };
typedef struct bdef { const char *name; int id; int kind; } bdef;
static const bdef g_builtins[] = {
    { "sin", B_SIN, K_UN }, { "cos", B_COS, K_UN }, { "tan", B_TAN, K_UN }, { "asin", B_ASIN, K_UN },
    { "acos", B_ACOS, K_UN }, { "atan", B_ATAN, K_UN }, { "sinh", B_SINH, K_UN }, { "cosh", B_COSH, K_UN },
    { "tanh", B_TANH, K_UN }, { "sqrt", B_SQRT, K_UN }, { "inversesqrt", B_INVSQRT, K_UN }, { "exp", B_EXP, K_UN },
    { "exp2", B_EXP2, K_UN }, { "log", B_LOG, K_UN }, { "log2", B_LOG2, K_UN }, { "log10", B_LOG10, K_UN },
    { "abs", B_ABS, K_UN }, { "fabs", B_ABS, K_UN }, { "floor", B_FLOOR, K_UN }, { "ceil", B_CEIL, K_UN },
    { "round", B_ROUND, K_UN }, { "trunc", B_TRUNC, K_UN }, { "sign", B_SIGN, K_UN }, { "radians", B_RADIANS, K_UN },
    { "degrees", B_DEGREES, K_UN },
    { "pow", B_POW, K_BIN }, { "fmod", B_FMOD, K_BIN }, { "mod", B_MOD, K_BIN }, { "min", B_MIN, K_BIN },
    { "max", B_MAX, K_BIN }, { "atan2", B_ATAN2, K_BIN }, { "step", B_STEP, K_BIN },
    { "clamp", B_CLAMP, K_TRI }, { "mix", B_MIX, K_TRI }, { "smoothstep", B_SMOOTHSTEP, K_TRI },
    { NULL, 0, 0 }
};

static int compile_args(cg *c, osl_node *n, int *regs, osl_type *types, osl_type want_first) {
    int i;
    if (n->nlist > 16) cg_error(c, n, "too many arguments");
    for (i = 0; i < n->nlist; i++) regs[i] = cexpr(c, n->list[i], i == 0 ? want_first : OSL_T_VOID, &types[i]);
    return n->nlist;
}

static int push_args(cg *c, const int *regs, int n) {
    osl_program *p = c->p;
    int start = p->nargs, i;
    p->args = (int *)grow(c, p->args, &c->args_cap, p->nargs + n + 1, sizeof(int));
    for (i = 0; i < n; i++) p->args[p->nargs++] = regs[i];
    return start;
}

static int emit_call(cg *c, osl_node *n, int id, int width, const int *regs, int nargs) {
    int d = new_reg(c), start = push_args(c, regs, nargs);
    emit(c, OP_CALL, d, start, nargs, id | (width << 8), n->line);
    return d;
}

static void want_args(cg *c, osl_node *n, int lo, int hi) {
    if (n->nlist < lo || n->nlist > hi) {
        if (lo == hi) cg_error(c, n, "%s() takes %d argument%s, %d given", n->name, lo, lo == 1 ? "" : "s", n->nlist);
        cg_error(c, n, "%s() takes %d to %d arguments, %d given", n->name, lo, hi, n->nlist);
    }
}

static const char *const_string(cg *c, osl_node *n) {
    (void)c;
    return n->kind == N_STR ? n->name : NULL;
}

static int compile_closure(cg *c, osl_node *n, int cltype, osl_type *ty) {
    int regs[16], args[8], na = 0, i;
    osl_type t[16];
    int d, start, first = 0;
    *ty = OSL_T_CLOSURE;
    if (cltype == OSL_CL_MICROFACET) {
        const char *dist;
        if (n->nlist != 5 && n->nlist != 7) cg_error(c, n, "microfacet() takes (string distribution, N, alpha, eta, refract) or the 7-argument anisotropic form");
        dist = const_string(c, n->list[0]);
        if (!dist) cg_error(c, n->list[0], "microfacet(): the distribution must be a string literal");
        if (strcmp(dist, "ggx") != 0 && strcmp(dist, "default") != 0 && strcmp(dist, "beckmann") != 0)
            cg_error(c, n->list[0], "microfacet(): unknown distribution \"%s\" (use \"ggx\")", dist);
        first = 1;
    }
    for (i = first; i < n->nlist; i++) regs[i] = cexpr(c, n->list[i], OSL_T_VOID, &t[i]);
    switch (cltype) {
    case OSL_CL_DIFFUSE:
        if (n->nlist < 1 || n->nlist > 2) cg_error(c, n, "%s() takes N%s", n->name, strcmp(n->name, "oren_nayar") == 0 ? " and sigma" : "");
        args[na++] = to3(c, n->list[0], regs[0], t[0]);
        break;
    case OSL_CL_MICROFACET:
        args[na++] = to3(c, n->list[1], regs[1], t[1]);
        if (n->nlist == 5) {
            args[na++] = to1(c, n->list[2], regs[2], t[2]);
            args[na++] = to1(c, n->list[3], regs[3], t[3]);
            args[na++] = to1(c, n->list[4], regs[4], t[4]);
        } else {
            /* (dist, N, U, xalpha, yalpha, eta, refract): isotropic approximation */
            int ax = to1(c, n->list[3], regs[3], t[3]), ay = to1(c, n->list[4], regs[4], t[4]);
            int s = new_reg(c), avg = new_reg(c);
            emit(c, OP_ADD1, s, ax, ay, 0, n->line);
            emit(c, OP_MUL1, avg, s, const_reg(c, 0.5f), 0, n->line);
            args[na++] = avg;
            args[na++] = to1(c, n->list[5], regs[5], t[5]);
            args[na++] = to1(c, n->list[6], regs[6], t[6]);
        }
        break;
    case OSL_CL_REFLECTION:
        want_args(c, n, 1, 2);
        args[na++] = to3(c, n->list[0], regs[0], t[0]);
        args[na++] = n->nlist == 2 ? to1(c, n->list[1], regs[1], t[1]) : c->zero_reg;
        break;
    case OSL_CL_REFRACTION:
        want_args(c, n, 2, 2);
        args[na++] = to3(c, n->list[0], regs[0], t[0]);
        args[na++] = to1(c, n->list[1], regs[1], t[1]);
        break;
    case OSL_CL_EMISSION:
    case OSL_CL_TRANSPARENT:
        want_args(c, n, 0, 0);
        if (cltype == OSL_CL_EMISSION) c->p->may_emit = 1;
        break;
    }
    d = new_reg(c);
    start = push_args(c, args, na);
    emit(c, OP_CLOSURE, d, start, na, cltype, n->line);
    return d;
}

static int inline_function(cg *c, osl_node *call, osl_node *fn, osl_type *ty);

static int ccall(cg *c, osl_node *n, osl_type want, osl_type *ty) {
    const char *name = n->name;
    int regs[16] = { 0 }, i, d;
    osl_type t[16];
    /* type constructors */
    if (!strcmp(name, "color") || !strcmp(name, "point") || !strcmp(name, "vector") || !strcmp(name, "normal")) {
        osl_type rt = !strcmp(name, "color") ? OSL_T_COLOR : !strcmp(name, "point") ? OSL_T_POINT : !strcmp(name, "vector") ? OSL_T_VECTOR : OSL_T_NORMAL;
        int first = 0, is_hsv = 0;
        if (n->nlist >= 1 && n->list[0]->kind == N_STR) {
            const char *sp = n->list[0]->name;
            first = 1;
            if (rt == OSL_T_COLOR) {
                if (!strcmp(sp, "hsv")) is_hsv = 1;
                else if (strcmp(sp, "rgb") != 0) cg_error(c, n->list[0], "color space \"%s\" is not supported (rgb, hsv)", sp);
            }
            /* point/vector/normal spaces: the renderer works in world space only */
        }
        if (n->nlist - first == 1) {
            int r = cexpr(c, n->list[first], rt, &t[0]);
            if (is_triple(t[0])) { *ty = rt; return r; }
            d = convert(c, n->list[first], r, t[0], rt);
            *ty = rt;
            return d;
        }
        if (n->nlist - first != 3) cg_error(c, n, "%s() takes 1 or 3 values", name);
        for (i = 0; i < 3; i++) {
            regs[i] = cexpr(c, n->list[first + i], OSL_T_FLOAT, &t[i]);
            regs[i] = to1(c, n->list[first + i], regs[i], t[i]);
        }
        d = new_reg(c);
        emit(c, OP_MAKE3, d, regs[0], regs[1], regs[2], n->line);
        if (is_hsv) d = emit_call(c, n, B_HSV2RGB, 3, &d, 1);
        *ty = rt;
        return d;
    }
    if (!strcmp(name, "float") || !strcmp(name, "int")) {
        osl_type rt = name[0] == 'f' ? OSL_T_FLOAT : OSL_T_INT;
        want_args(c, n, 1, 1);
        regs[0] = cexpr(c, n->list[0], rt, &t[0]);
        if (!is_scalar(t[0])) cg_error(c, n, "%s() needs a number", name);
        *ty = rt;
        return convert(c, n, regs[0], t[0], rt);
    }
    if (!strcmp(name, "string")) cg_error(c, n, "string() is not supported");
    /* closures */
    if (!strcmp(name, "diffuse") || !strcmp(name, "oren_nayar")) return compile_closure(c, n, OSL_CL_DIFFUSE, ty);
    if (!strcmp(name, "microfacet")) return compile_closure(c, n, OSL_CL_MICROFACET, ty);
    if (!strcmp(name, "reflection")) return compile_closure(c, n, OSL_CL_REFLECTION, ty);
    if (!strcmp(name, "refraction")) return compile_closure(c, n, OSL_CL_REFRACTION, ty);
    if (!strcmp(name, "emission")) return compile_closure(c, n, OSL_CL_EMISSION, ty);
    if (!strcmp(name, "transparent")) return compile_closure(c, n, OSL_CL_TRANSPARENT, ty);
    if (!strcmp(name, "holdout") || !strcmp(name, "background") || !strcmp(name, "debug")) {
        for (i = 0; i < n->nlist; i++) cexpr(c, n->list[i], OSL_T_VOID, &t[0]);
        *ty = OSL_T_CLOSURE;
        return c->zero_reg;
    }
    if (!strcmp(name, "translucent") || !strcmp(name, "phong") || !strcmp(name, "ward") || !strcmp(name, "subsurface"))
        cg_error(c, n, "closure %s() is not supported by the built-in compiler", name);
    /* user functions (inlined) */
    for (i = c->f->nfuncs - 1; i >= 0; i--) {
        osl_node *fn = c->f->funcs[i];
        if (!strcmp(fn->name, name) && fn->nlist == n->nlist) return inline_function(c, n, fn, ty);
    }
    /* math builtins */
    for (i = 0; g_builtins[i].name; i++) {
        const bdef *b = &g_builtins[i];
        int k, triple = 0;
        osl_type rt = OSL_T_FLOAT;
        if (strcmp(b->name, name) != 0) continue;
        want_args(c, n, b->kind == K_UN ? 1 : (b->kind == K_BIN ? 2 : 3), b->kind == K_UN ? 1 : (b->kind == K_BIN ? 2 : 3));
        compile_args(c, n, regs, t, OSL_T_VOID);
        for (k = 0; k < n->nlist; k++) {
            if (is_triple(t[k])) { if (!triple) rt = t[k]; triple = 1; }
            else if (!is_scalar(t[k])) cg_error(c, n->list[k], "%s(): argument %d must be numeric, got %s", name, k + 1, osl_type_name(t[k]));
        }
        if (triple) for (k = 0; k < n->nlist; k++) regs[k] = to3(c, n->list[k], regs[k], t[k]);
        *ty = triple ? rt : OSL_T_FLOAT;
        return emit_call(c, n, b->id, triple ? 3 : 1, regs, n->nlist);
    }
    /* geometric and special builtins */
    if (!strcmp(name, "dot") || !strcmp(name, "distance")) {
        want_args(c, n, 2, 2);
        compile_args(c, n, regs, t, OSL_T_VOID);
        regs[0] = to3(c, n->list[0], regs[0], t[0]); regs[1] = to3(c, n->list[1], regs[1], t[1]);
        *ty = OSL_T_FLOAT;
        return emit_call(c, n, name[0] == 'd' && name[1] == 'o' ? B_DOT : B_DISTANCE, 3, regs, 2);
    }
    if (!strcmp(name, "cross")) {
        want_args(c, n, 2, 2);
        compile_args(c, n, regs, t, OSL_T_VOID);
        regs[0] = to3(c, n->list[0], regs[0], t[0]); regs[1] = to3(c, n->list[1], regs[1], t[1]);
        *ty = OSL_T_VECTOR;
        return emit_call(c, n, B_CROSS, 3, regs, 2);
    }
    if (!strcmp(name, "length") || !strcmp(name, "luminance")) {
        want_args(c, n, 1, 1);
        compile_args(c, n, regs, t, OSL_T_VOID);
        regs[0] = to3(c, n->list[0], regs[0], t[0]);
        *ty = OSL_T_FLOAT;
        return emit_call(c, n, name[0] == 'l' && name[1] == 'e' ? B_LENGTH : B_LUMINANCE, 3, regs, 1);
    }
    if (!strcmp(name, "normalize")) {
        want_args(c, n, 1, 1);
        compile_args(c, n, regs, t, OSL_T_VOID);
        if (!is_triple(t[0])) cg_error(c, n, "normalize() needs a vector");
        *ty = t[0];
        return emit_call(c, n, B_NORMALIZE, 3, regs, 1);
    }
    if (!strcmp(name, "faceforward")) {
        want_args(c, n, 2, 3);
        compile_args(c, n, regs, t, OSL_T_VOID);
        for (i = 0; i < n->nlist; i++) regs[i] = to3(c, n->list[i], regs[i], t[i]);
        *ty = OSL_T_VECTOR;
        return emit_call(c, n, B_FACEFORWARD, 3, regs, n->nlist);
    }
    if (!strcmp(name, "reflect")) {
        want_args(c, n, 2, 2);
        compile_args(c, n, regs, t, OSL_T_VOID);
        regs[0] = to3(c, n->list[0], regs[0], t[0]); regs[1] = to3(c, n->list[1], regs[1], t[1]);
        *ty = OSL_T_VECTOR;
        return emit_call(c, n, B_REFLECT, 3, regs, 2);
    }
    if (!strcmp(name, "refract")) {
        want_args(c, n, 3, 3);
        compile_args(c, n, regs, t, OSL_T_VOID);
        regs[0] = to3(c, n->list[0], regs[0], t[0]); regs[1] = to3(c, n->list[1], regs[1], t[1]);
        regs[2] = to1(c, n->list[2], regs[2], t[2]);
        *ty = OSL_T_VECTOR;
        return emit_call(c, n, B_REFRACT, 3, regs, 3);
    }
    if (!strcmp(name, "hypot")) {
        want_args(c, n, 2, 3);
        compile_args(c, n, regs, t, OSL_T_VOID);
        for (i = 0; i < n->nlist; i++) regs[i] = to1(c, n->list[i], regs[i], t[i]);
        if (n->nlist == 2) regs[2] = c->zero_reg;
        *ty = OSL_T_FLOAT;
        return emit_call(c, n, B_HYPOT, 1, regs, 3);
    }
    if (!strcmp(name, "noise") || !strcmp(name, "snoise") || !strcmp(name, "cellnoise")) {
        int id = !strcmp(name, "snoise") ? B_NOISE_S : (!strcmp(name, "cellnoise") ? B_NOISE_CELL : B_NOISE_U);
        int first = 0, nr = 0, width;
        if (n->nlist >= 1 && n->list[0]->kind == N_STR) {
            const char *ty_s = n->list[0]->name;
            first = 1;
            if (!strcmp(ty_s, "perlin") || !strcmp(ty_s, "snoise") || !strcmp(ty_s, "simplex")) id = B_NOISE_S;
            else if (!strcmp(ty_s, "uperlin") || !strcmp(ty_s, "noise") || !strcmp(ty_s, "usimplex")) id = B_NOISE_U;
            else if (!strcmp(ty_s, "cell") || !strcmp(ty_s, "cellnoise") || !strcmp(ty_s, "hash")) id = B_NOISE_CELL;
            else cg_error(c, n->list[0], "noise type \"%s\" is not supported (perlin, uperlin, cell, simplex)", ty_s);
        } else if (n->nlist >= 1 && n->list[0]->kind != N_STR) {
            osl_type tt;
            (void)tt;
        }
        if (n->nlist - first < 1 || n->nlist - first > 2) cg_error(c, n, "%s() takes a position and an optional time", name);
        for (i = first; i < n->nlist; i++) {
            int r = cexpr(c, n->list[i], OSL_T_VOID, &t[0]);
            if (t[0] == OSL_T_STRING) cg_error(c, n->list[i], "%s(): the noise type must be the first argument", name);
            regs[nr++] = to3(c, n->list[i], r, t[0]);
        }
        if (nr == 1) regs[nr++] = c->zero_reg;
        width = is_triple(want) ? 3 : 1;
        *ty = width == 3 ? (want == OSL_T_POINT || want == OSL_T_NORMAL ? OSL_T_VECTOR : want) : OSL_T_FLOAT;
        if (width == 3 && *ty == OSL_T_VECTOR && want == OSL_T_COLOR) *ty = OSL_T_COLOR;
        return emit_call(c, n, id, width, regs, 2);
    }
    if (!strcmp(name, "texture")) {
        const char *fname;
        int width;
        if (n->nlist < 3 || ((n->nlist - 3) % 2) != 0) cg_error(c, n, "texture() takes (filename, s, t) plus optional name/value pairs");
        compile_args(c, n, regs, t, OSL_T_VOID);
        if (t[0] != OSL_T_STRING) cg_error(c, n->list[0], "texture(): the filename must be a string");
        fname = const_string(c, n->list[0]);
        if (fname) add_texture(c, fname);
        regs[1] = to1(c, n->list[1], regs[1], t[1]);
        regs[2] = to1(c, n->list[2], regs[2], t[2]);
        for (i = 3; i < n->nlist; i += 2) if (t[i] != OSL_T_STRING) cg_error(c, n->list[i], "texture(): optional arguments come as \"name\", value pairs");
        width = want == OSL_T_FLOAT || want == OSL_T_INT ? 1 : 3;
        *ty = width == 3 ? OSL_T_COLOR : OSL_T_FLOAT;
        return emit_call(c, n, B_TEXTURE, width, regs, 3);
    }
    if (!strcmp(name, "backfacing")) {
        want_args(c, n, 0, 0);
        *ty = OSL_T_INT;
        return emit_call(c, n, B_BACKFACING, 1, regs, 0);
    }
    if (!strcmp(name, "raytype")) {
        want_args(c, n, 1, 1);
        compile_args(c, n, regs, t, OSL_T_VOID);
        if (t[0] != OSL_T_STRING) cg_error(c, n, "raytype() takes a string");
        *ty = OSL_T_INT;
        return emit_call(c, n, B_RAYTYPE, 1, regs, 1);
    }
    if (!strcmp(name, "isnan") || !strcmp(name, "isinf")) {
        want_args(c, n, 1, 1);
        compile_args(c, n, regs, t, OSL_T_VOID);
        regs[0] = to1(c, n->list[0], regs[0], t[0]);
        *ty = OSL_T_INT;
        return emit_call(c, n, name[2] == 'n' ? B_ISNAN : B_ISINF, 1, regs, 1);
    }
    if (!strcmp(name, "transform")) {
        /* world space only: transform("space", v) and transform("from", "to", v) are identities */
        want_args(c, n, 2, 3);
        compile_args(c, n, regs, t, OSL_T_VOID);
        if (!is_triple(t[n->nlist - 1])) cg_error(c, n, "transform() needs a point, vector or normal");
        *ty = t[n->nlist - 1];
        return regs[n->nlist - 1];
    }
    if (!strcmp(name, "printf") || !strcmp(name, "warning") || !strcmp(name, "error") || !strcmp(name, "fprintf")) {
        compile_args(c, n, regs, t, OSL_T_VOID);
        *ty = OSL_T_VOID;
        return c->zero_reg;
    }
    if (!strcmp(name, "fresnel_schlick")) {
        /* Nora extension: fresnel_schlick(cos, f0) */
        want_args(c, n, 2, 2);
        compile_args(c, n, regs, t, OSL_T_VOID);
        regs[0] = to1(c, n->list[0], regs[0], t[0]); regs[1] = to1(c, n->list[1], regs[1], t[1]);
        *ty = OSL_T_FLOAT;
        return emit_call(c, n, B_FRESNEL_SCHLICK, 1, regs, 2);
    }
    cg_error(c, n, "unknown function '%s'", name);
    return -1;
}

static int inline_function(cg *c, osl_node *call, osl_node *fn, osl_type *ty) {
    cg_fn ctx, *saved_fn = c->fn;
    int saved_hide_from = c->hide_from, saved_hide_to = c->hide_to, saved_nloops = c->nloops;
    int regs[16], i;
    osl_type t[16];
    if (c->inline_depth > 24) cg_error(c, call, "recursion or nesting too deep calling '%s'", fn->name);
    memset(&ctx, 0, sizeof ctx);
    ctx.ret = fn->dtype;
    ctx.ret_reg = fn->dtype == OSL_T_VOID ? c->zero_reg : new_reg(c);
    /* evaluate arguments in the caller's scope */
    for (i = 0; i < fn->nlist; i++) {
        osl_node *formal = fn->list[i];
        if (formal->is_output) {
            lval lv = get_lval(c, call->list[i]);
            if (lv.is_comp) cg_error(c, call->list[i], "output argument must be a variable");
            if (!(lv.type == formal->dtype || (is_triple(lv.type) && is_triple(formal->dtype))))
                cg_error(c, call->list[i], "output argument type %s does not match %s", osl_type_name(lv.type), osl_type_name(formal->dtype));
            regs[i] = lv.reg;
        } else {
            int r = cexpr(c, call->list[i], formal->dtype, &t[i]), copy = new_reg(c);
            r = convert(c, call->list[i], r, t[i], formal->dtype);
            emit(c, is_triple(formal->dtype) ? OP_MOV3 : OP_MOV1, copy, r, 0, 0, call->line);
            regs[i] = copy;
        }
    }
    c->hide_from = c->nglobals_syms;
    c->hide_to = c->nsyms;
    c->inline_depth++;
    c->fn = &ctx;
    c->nloops = 0;
    scope_push(c);
    for (i = 0; i < fn->nlist; i++) sym_push(c, fn->list[i]->name, fn->list[i]->dtype, regs[i], 0);
    cstmt(c, fn->a);
    scope_pop(c);
    patch_to(c, &ctx.rets, here(c));
    c->fn = saved_fn;
    c->inline_depth--;
    c->hide_from = saved_hide_from;
    c->hide_to = saved_hide_to;
    c->nloops = saved_nloops;
    *ty = fn->dtype;
    return ctx.ret_reg;
}

/* -------------------------------------------------------- expressions */

static int cexpr(cg *c, osl_node *n, osl_type want, osl_type *ty) {
    int ra, rb, d;
    osl_type ta, tb;
    switch (n->kind) {
    case N_NUM:
        *ty = n->is_int ? OSL_T_INT : OSL_T_FLOAT;
        return const_reg(c, (float)n->num);
    case N_STR: {
        /* string literals naming images are preloaded by the host (e.g. a parameter default) */
        size_t L = strlen(n->name);
        static const char *const exts[] = { ".png", ".jpg", ".jpeg", ".hdr", ".tga", ".bmp" };
        int k;
        for (k = 0; k < 6; k++) {
            size_t e = strlen(exts[k]);
            if (L > e && strcmp(n->name + L - e, exts[k]) == 0) add_texture(c, n->name);
        }
        *ty = OSL_T_STRING;
        return const_reg(c, (float)intern(c, n->name));
    }
    case N_IDENT: {
        cg_sym *s = sym_find(c, n->name);
        if (!s) cg_error(c, n, "undeclared identifier '%s'", n->name);
        *ty = s->type;
        return s->reg;
    }
    case N_CAST: {
        ra = cexpr(c, n->a, n->dtype, &ta);
        *ty = n->dtype;
        return convert(c, n, ra, ta, n->dtype);
    }
    case N_UNOP:
        ra = cexpr(c, n->a, want, &ta);
        if (n->op == P_PLUS) { *ty = ta; return ra; }
        if (n->op == P_MINUS) {
            d = new_reg(c);
            if (ta == OSL_T_CLOSURE) { emit(c, OP_CL_MUL1, d, ra, const_reg(c, -1.0f), 0, n->line); *ty = ta; return d; }
            if (is_triple(ta)) emit(c, OP_NEG3, d, ra, 0, 0, n->line);
            else if (is_scalar(ta)) emit(c, OP_NEG1, d, ra, 0, 0, n->line);
            else cg_error(c, n, "cannot negate %s", osl_type_name(ta));
            *ty = ta;
            return d;
        }
        if (n->op == P_NOT) {
            if (!is_scalar(ta)) cg_error(c, n, "'!' needs an int or float");
            d = new_reg(c);
            emit(c, OP_NOT, d, ra, 0, 0, n->line);
            *ty = OSL_T_INT;
            return d;
        }
        cg_error(c, n, "operator not supported");
        return -1;
    case N_BINOP:
        if (n->op == P_COMMA) { cexpr(c, n->a, OSL_T_VOID, &ta); return cexpr(c, n->b, want, ty); }
        if (n->op == P_AND || n->op == P_OR) {
            int jf, jend;
            d = new_reg(c);
            ra = cexpr(c, n->a, OSL_T_INT, &ta);
            if (!is_scalar(ta)) cg_error(c, n->a, "logical operands must be int or float");
            jf = emit(c, n->op == P_AND ? OP_JZ : OP_JNZ, 0, ra, 0, 0, n->line);
            rb = cexpr(c, n->b, OSL_T_INT, &tb);
            if (!is_scalar(tb)) cg_error(c, n->b, "logical operands must be int or float");
            emit(c, OP_NE1, d, rb, c->zero_reg, 0, n->line);
            jend = emit(c, OP_JMP, 0, 0, 0, 0, n->line);
            c->p->code[jf].b = here(c);
            emit(c, OP_MOV1, d, n->op == P_AND ? c->zero_reg : c->one_reg, 0, 0, n->line);
            c->p->code[jend].a = here(c);
            *ty = OSL_T_INT;
            return d;
        }
        ra = cexpr(c, n->a, want, &ta);
        rb = cexpr(c, n->b, want, &tb);
        if (n->op == P_EQ || n->op == P_NE) {
            d = new_reg(c);
            if (ta == OSL_T_STRING || tb == OSL_T_STRING) {
                if (ta != tb) cg_error(c, n, "cannot compare string with %s", osl_type_name(ta == OSL_T_STRING ? tb : ta));
                emit(c, n->op == P_EQ ? OP_EQ1 : OP_NE1, d, ra, rb, 0, n->line);
            } else if (is_triple(ta) || is_triple(tb)) {
                emit(c, n->op == P_EQ ? OP_EQ3 : OP_NE3, d, to3(c, n, ra, ta), to3(c, n, rb, tb), 0, n->line);
            } else if (is_scalar(ta) && is_scalar(tb)) {
                emit(c, n->op == P_EQ ? OP_EQ1 : OP_NE1, d, ra, rb, 0, n->line);
            } else cg_error(c, n, "invalid comparison between %s and %s", osl_type_name(ta), osl_type_name(tb));
            *ty = OSL_T_INT;
            return d;
        }
        if (n->op == P_LT || n->op == P_LE || n->op == P_GT || n->op == P_GE) {
            int o = n->op == P_LT ? OP_LT : n->op == P_LE ? OP_LE : n->op == P_GT ? OP_GT : OP_GE;
            if (!is_scalar(ta) || !is_scalar(tb)) cg_error(c, n, "relational operators need int or float operands");
            d = new_reg(c);
            emit(c, o, d, ra, rb, 0, n->line);
            *ty = OSL_T_INT;
            return d;
        }
        return arith(c, n, n->op, ra, ta, rb, tb, ty);
    case N_ASSIGN: {
        lval lv = get_lval(c, n->a);
        rb = cexpr(c, n->b, lv.type, &tb);
        if (n->op != P_ASSIGN) {
            int bop = n->op == P_PLUSEQ ? P_PLUS : n->op == P_MINUSEQ ? P_MINUS : n->op == P_STAREQ ? P_STAR : P_SLASH;
            int cur = load_lval(c, n, &lv);
            osl_type rt;
            rb = arith(c, n, bop, cur, lv.type, rb, tb, &rt);
            tb = rt;
        }
        if (lv.type == OSL_T_CLOSURE && tb != OSL_T_CLOSURE) {
            if (!(n->b->kind == N_NUM && n->b->num == 0.0)) cg_error(c, n, "cannot assign %s to a closure", osl_type_name(tb));
            rb = c->zero_reg; tb = OSL_T_CLOSURE;
        }
        if (is_triple(tb) && !is_triple(lv.type)) cg_error(c, n, "cannot assign %s to %s", osl_type_name(tb), osl_type_name(lv.type));
        if (tb == OSL_T_STRING && lv.type != OSL_T_STRING) cg_error(c, n, "cannot assign a string to %s", osl_type_name(lv.type));
        if (lv.type == OSL_T_STRING && tb != OSL_T_STRING) cg_error(c, n, "cannot assign %s to a string", osl_type_name(tb));
        store_lval(c, n, &lv, rb, tb);
        *ty = lv.type;
        return load_lval(c, n, &lv);
    }
    case N_TERNARY: {
        int jf, jend, rr;
        osl_type tc;
        ra = cexpr(c, n->a, OSL_T_INT, &tc);
        if (!is_scalar(tc)) cg_error(c, n->a, "condition must be int or float");
        d = new_reg(c);
        jf = emit(c, OP_JZ, 0, ra, 0, 0, n->line);
        rr = cexpr(c, n->b, want, &ta);
        {
            /* decide the result type from both branches: compile the second into a scratch position later */
            int mark = here(c);
            (void)mark;
        }
        /* move the first branch (convert later if the second is wider) */
        emit(c, is_triple(ta) ? OP_MOV3 : OP_MOV1, d, rr, 0, 0, n->line);
        jend = emit(c, OP_JMP, 0, 0, 0, 0, n->line);
        c->p->code[jf].b = here(c);
        rb = cexpr(c, n->c, want, &tb);
        if (is_triple(ta) || is_triple(tb)) {
            rb = to3(c, n->c, rb, tb);
            emit(c, OP_MOV3, d, rb, 0, 0, n->line);
            if (!is_triple(ta)) {
                /* first branch stored a scalar: broadcast it in place */
                int fix = emit(c, OP_JMP, 0, 0, 0, 0, n->line);
                int bc = here(c);
                emit(c, OP_BCAST, d, d, 0, 0, n->line);
                c->p->code[jend].a = bc;
                c->p->code[fix].a = here(c);
                *ty = tb;
                return d;
            }
            *ty = is_triple(ta) ? ta : tb;
        } else {
            if (ta == OSL_T_CLOSURE || tb == OSL_T_CLOSURE) {
                if (ta != tb) cg_error(c, n, "ternary branches have different types");
            } else if (ta == OSL_T_STRING || tb == OSL_T_STRING) {
                if (ta != tb) cg_error(c, n, "ternary branches have different types");
            }
            emit(c, OP_MOV1, d, rb, 0, 0, n->line);
            *ty = (ta == OSL_T_FLOAT || tb == OSL_T_FLOAT) ? OSL_T_FLOAT : ta;
        }
        c->p->code[jend].a = here(c);
        return d;
    }
    case N_CALL:
        return ccall(c, n, want, ty);
    case N_INDEX: {
        osl_type it;
        ra = cexpr(c, n->a, OSL_T_VOID, &ta);
        if (!is_triple(ta)) cg_error(c, n, "only color/point/vector/normal values can be indexed");
        d = new_reg(c);
        if (n->b->kind == N_NUM) {
            int k = (int)n->b->num;
            if (k < 0 || k > 2) cg_error(c, n, "component index %d out of range", k);
            emit(c, OP_COMPK, d, ra, 0, k, n->line);
        } else {
            rb = cexpr(c, n->b, OSL_T_INT, &it);
            emit(c, OP_COMP, d, ra, to1(c, n->b, rb, it), 0, n->line);
        }
        *ty = OSL_T_FLOAT;
        return d;
    }
    case N_PREINC:
    case N_POSTINC: {
        lval lv = get_lval(c, n->a);
        int cur = load_lval(c, n, &lv), old = new_reg(c), nv;
        osl_type rt;
        if (!is_scalar(lv.type)) cg_error(c, n, "++/-- need an int or float");
        emit(c, OP_MOV1, old, cur, 0, 0, n->line);
        nv = arith(c, n, n->op == P_INC ? P_PLUS : P_MINUS, cur, lv.type, c->one_reg, OSL_T_INT, &rt);
        store_lval(c, n, &lv, nv, rt);
        *ty = lv.type;
        return n->kind == N_POSTINC ? old : load_lval(c, n, &lv);
    }
    default:
        cg_error(c, n, "unexpected expression");
    }
    return -1;
}

/* --------------------------------------------------------- statements */

static int ccond(cg *c, osl_node *n) {
    osl_type t;
    int r = cexpr(c, n, OSL_T_INT, &t);
    if (!is_scalar(t)) cg_error(c, n, "condition must be int or float, got %s", osl_type_name(t));
    return r;
}

static void cstmt(cg *c, osl_node *n) {
    int i;
    switch (n->kind) {
    case N_BLOCK:
        scope_push(c);
        for (i = 0; i < n->nlist; i++) cstmt(c, n->list[i]);
        scope_pop(c);
        break;
    case N_EMPTY: break;
    case N_EXPRSTMT: { osl_type t; cexpr(c, n->a, OSL_T_VOID, &t); break; }
    case N_VARDECL:
        for (i = 0; i < n->nlist; i++) {
            osl_node *dn = n->list[i];
            int reg = new_reg(c);
            if (dn->a) {
                osl_type t;
                int r = cexpr(c, dn->a, n->dtype, &t);
                if (n->dtype == OSL_T_CLOSURE && t != OSL_T_CLOSURE) {
                    if (!(dn->a->kind == N_NUM && dn->a->num == 0.0)) cg_error(c, dn, "cannot initialise a closure with %s", osl_type_name(t));
                    r = c->zero_reg; t = OSL_T_CLOSURE;
                }
                if ((n->dtype == OSL_T_STRING) != (t == OSL_T_STRING)) cg_error(c, dn, "cannot initialise %s with %s", osl_type_name(n->dtype), osl_type_name(t));
                if (is_triple(t) && !is_triple(n->dtype)) cg_error(c, dn, "cannot initialise %s with %s", osl_type_name(n->dtype), osl_type_name(t));
                r = convert(c, dn, r, t, n->dtype);
                emit(c, is_triple(n->dtype) ? OP_MOV3 : OP_MOV1, reg, r, 0, 0, dn->line);
            } else {
                emit(c, OP_MOV3, reg, c->zero_reg, 0, 0, dn->line);
            }
            sym_push(c, dn->name, n->dtype, reg, 0);
        }
        break;
    case N_IF: {
        int r = ccond(c, n->a), jf = emit(c, OP_JZ, 0, r, 0, 0, n->line);
        cstmt(c, n->b);
        if (n->c) {
            int je = emit(c, OP_JMP, 0, 0, 0, 0, n->line);
            c->p->code[jf].b = here(c);
            cstmt(c, n->c);
            c->p->code[je].a = here(c);
        } else {
            c->p->code[jf].b = here(c);
        }
        break;
    }
    case N_WHILE:
    case N_DO:
    case N_FOR: {
        cg_loop lp;
        int top, cont_target, jf = -1, r;
        memset(&lp, 0, sizeof lp);
        if (c->nloops >= 64) cg_error(c, n, "loops nested too deeply");
        scope_push(c);
        if (n->kind == N_FOR && n->a) cstmt(c, n->a);
        c->loops[c->nloops++] = &lp;
        top = here(c);
        if (n->kind == N_WHILE) { r = ccond(c, n->a); jf = emit(c, OP_JZ, 0, r, 0, 0, n->line); }
        if (n->kind == N_FOR && n->b) { r = ccond(c, n->b); jf = emit(c, OP_JZ, 0, r, 0, 0, n->line); }
        cstmt(c, n->kind == N_FOR ? n->d : n->b);
        cont_target = here(c);
        if (n->kind == N_FOR && n->c) { osl_type t; cexpr(c, n->c, OSL_T_VOID, &t); }
        if (n->kind == N_DO) { r = ccond(c, n->a); emit(c, OP_JNZ, 0, r, top, 0, n->line); }
        else emit(c, OP_JMP, 0, top, 0, 0, n->line);
        if (jf >= 0) c->p->code[jf].b = here(c);
        patch_to(c, &lp.cont, cont_target);
        patch_to(c, &lp.brk, here(c));
        c->nloops--;
        scope_pop(c);
        break;
    }
    case N_BREAK:
    case N_CONTINUE: {
        int j;
        if (c->nloops == 0) cg_error(c, n, "'%s' outside of a loop", n->kind == N_BREAK ? "break" : "continue");
        j = emit(c, OP_JMP, 0, 0, 0, 0, n->line);
        patch_add(c, n->kind == N_BREAK ? &c->loops[c->nloops - 1]->brk : &c->loops[c->nloops - 1]->cont, j);
        break;
    }
    case N_RETURN:
        if (c->fn) {
            if (n->a) {
                osl_type t;
                int r;
                if (c->fn->ret == OSL_T_VOID) cg_error(c, n, "void function returns a value");
                r = cexpr(c, n->a, c->fn->ret, &t);
                if (c->fn->ret == OSL_T_CLOSURE && t != OSL_T_CLOSURE) {
                    if (!(n->a->kind == N_NUM && n->a->num == 0.0)) cg_error(c, n, "function must return a closure");
                    r = c->zero_reg; t = OSL_T_CLOSURE;
                }
                if (is_triple(t) && !is_triple(c->fn->ret)) cg_error(c, n, "cannot return %s from a %s function", osl_type_name(t), osl_type_name(c->fn->ret));
                r = convert(c, n, r, t, c->fn->ret);
                emit(c, is_triple(c->fn->ret) ? OP_MOV3 : OP_MOV1, c->fn->ret_reg, r, 0, 0, n->line);
            } else if (c->fn->ret != OSL_T_VOID) cg_error(c, n, "function must return a value");
            patch_add(c, &c->fn->rets, emit(c, OP_JMP, 0, 0, 0, 0, n->line));
        } else {
            if (n->a) cg_error(c, n, "a shader cannot return a value");
            patch_add(c, &c->shader_rets, emit(c, OP_JMP, 0, 0, 0, 0, n->line));
        }
        break;
    default:
        cg_error(c, n, "unexpected statement");
    }
}

/* ------------------------------------------------------------- driver */

static void add_global(cg *c, const char *name, osl_type t, int reg, int ro) { sym_push(c, name, t, reg, ro); }

osl_program *osl_codegen(osl_front *f, char *err, size_t errcap) {
    cg c;
    osl_program *volatile p;
    int i;
    osl_node *sh = f->shader;
    (void)err; (void)errcap;
    memset(&c, 0, sizeof c);
    p = (osl_program *)calloc(1, sizeof *p);
    if (!p) return NULL;
    c.f = f;
    c.p = p;
    c.hide_from = c.hide_to = 0;
    if (setjmp(f->jb)) {
        free(c.syms);
        free(c.shader_rets.v);
        osl_program_free(p);
        return NULL;
    }
    strncpy(p->name, sh->name, sizeof p->name - 1);
    for (i = 0; i < R_NGLOBALS; i++) new_reg(&c);
    c.zero_reg = const_reg(&c, 0.0f);
    c.one_reg = const_reg(&c, 1.0f);
    add_global(&c, "P", OSL_T_POINT, R_P, 0);
    add_global(&c, "I", OSL_T_VECTOR, R_I, 0);
    add_global(&c, "N", OSL_T_NORMAL, R_N, 0);
    add_global(&c, "Ng", OSL_T_NORMAL, R_NG, 0);
    add_global(&c, "dPdu", OSL_T_VECTOR, R_DPDU, 0);
    add_global(&c, "dPdv", OSL_T_VECTOR, R_DPDV, 0);
    add_global(&c, "u", OSL_T_FLOAT, R_U, 0);
    add_global(&c, "v", OSL_T_FLOAT, R_V, 0);
    add_global(&c, "time", OSL_T_FLOAT, R_TIME, 0);
    add_global(&c, "Ci", OSL_T_CLOSURE, R_CI, 0);
    add_global(&c, "M_PI", OSL_T_FLOAT, const_reg(&c, 3.14159265358979f), 1);
    add_global(&c, "M_PI_2", OSL_T_FLOAT, const_reg(&c, 1.57079632679490f), 1);
    add_global(&c, "M_PI_4", OSL_T_FLOAT, const_reg(&c, 0.78539816339745f), 1);
    add_global(&c, "M_2PI", OSL_T_FLOAT, const_reg(&c, 6.28318530717959f), 1);
    add_global(&c, "M_2_PI", OSL_T_FLOAT, const_reg(&c, 0.63661977236758f), 1);
    add_global(&c, "M_1_PI", OSL_T_FLOAT, const_reg(&c, 0.31830988618379f), 1);
    add_global(&c, "M_E", OSL_T_FLOAT, const_reg(&c, 2.71828182845905f), 1);
    add_global(&c, "M_SQRT2", OSL_T_FLOAT, const_reg(&c, 1.41421356237310f), 1);
    add_global(&c, "M_SQRT1_2", OSL_T_FLOAT, const_reg(&c, 0.70710678118655f), 1);
    add_global(&c, "M_LN2", OSL_T_FLOAT, const_reg(&c, 0.69314718055995f), 1);
    add_global(&c, "M_LN10", OSL_T_FLOAT, const_reg(&c, 2.30258509299405f), 1);
    c.nglobals_syms = c.nsyms;
    scope_push(&c);
    /* parameters with their default code, skipped when overridden */
    for (i = 0; i < sh->nlist; i++) {
        osl_node *pn = sh->list[i];
        int reg = new_reg(&c), guard, r, k;
        osl_type t;
        osl_param_info *pi;
        for (k = 0; k < p->nparams; k++)
            if (!strcmp(p->params[k].name, pn->name)) cg_error(&c, pn, "duplicate parameter '%s'", pn->name);
        if (pn->dtype == OSL_T_VOID) cg_error(&c, pn, "parameters cannot be void");
        p->params = (osl_param_info *)grow(&c, p->params, &c.par_cap, p->nparams + 1, sizeof(osl_param_info));
        pi = &p->params[p->nparams];
        memset(pi, 0, sizeof *pi);
        strncpy(pi->name, pn->name, sizeof pi->name - 1);
        pi->type = pn->dtype;
        pi->is_output = pn->is_output;
        pi->reg = reg;
        guard = emit(&c, OP_PARAM, 0, p->nparams, 0, 0, pn->line);
        p->nparams++;
        r = cexpr(&c, pn->a, pn->dtype, &t);
        if (pn->dtype == OSL_T_CLOSURE && t != OSL_T_CLOSURE) {
            if (!(pn->a->kind == N_NUM && pn->a->num == 0.0)) cg_error(&c, pn, "closure parameters default to 0");
            r = c.zero_reg; t = OSL_T_CLOSURE;
        }
        if ((pn->dtype == OSL_T_STRING) != (t == OSL_T_STRING)) cg_error(&c, pn, "default of '%s' must be %s", pn->name, osl_type_name(pn->dtype));
        if (is_triple(t) && !is_triple(pn->dtype)) cg_error(&c, pn, "default of '%s' must be %s", pn->name, osl_type_name(pn->dtype));
        r = convert(&c, pn, r, t, pn->dtype);
        emit(&c, is_triple(pn->dtype) ? OP_MOV3 : OP_MOV1, reg, r, 0, 0, pn->line);
        p->code[guard].b = here(&c);
        sym_push(&c, pn->name, pn->dtype, reg, 0);
    }
    cstmt(&c, sh->a);
    patch_to(&c, &c.shader_rets, here(&c));
    emit(&c, OP_END, 0, 0, 0, 0, 0);
    scope_pop(&c);
    free(c.syms);
    return p;
}

void osl_program_free(osl_program *p) {
    int i;
    if (!p) return;
    free(p->code);
    free(p->args);
    free(p->init);
    for (i = 0; i < p->nstrings; i++) free(p->strings[i]);
    free(p->strings);
    free(p->params);
    for (i = 0; i < p->ntextures; i++) free(p->textures[i]);
    free(p->textures);
    free(p);
}

osl_program *osl_compile(const char *source, char *err, size_t errcap) {
    osl_front f;
    osl_program *volatile p = NULL;
    memset(&f, 0, sizeof f);
    f.src = source ? source : "";
    f.err = err;
    f.errcap = errcap;
    if (err && errcap) err[0] = 0;
    if (setjmp(f.jb) == 0) {
        osl_lex(&f);
        osl_parse(&f);
        p = osl_codegen(&f, err, errcap);
    }
    free(f.toks);
    osl_arena_free(&f.arena);
    return p;
}
