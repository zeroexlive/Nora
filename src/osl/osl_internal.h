/* Nora OSL subset - internal definitions shared by the front end, compiler and VM. MIT License. */
#ifndef NORA_OSL_INTERNAL_H
#define NORA_OSL_INTERNAL_H

#include "osl.h"
#include <setjmp.h>
#include <stdint.h>

/* ---------------------------------------------------------------- tokens */
enum {
    TK_EOF = 0, TK_IDENT, TK_INT, TK_FLOAT, TK_STRING, TK_PUNCT
};
enum {
    P_LPAREN = 1, P_RPAREN, P_LBRACE, P_RBRACE, P_LBRACK, P_RBRACK, P_SEMI, P_COMMA, P_DOT,
    P_PLUS, P_MINUS, P_STAR, P_SLASH, P_PERCENT, P_ASSIGN, P_PLUSEQ, P_MINUSEQ, P_STAREQ, P_SLASHEQ,
    P_EQ, P_NE, P_LT, P_LE, P_GT, P_GE, P_AND, P_OR, P_NOT, P_QUESTION, P_COLON, P_INC, P_DEC,
    P_META_OPEN, P_META_CLOSE, P_BITAND, P_BITOR, P_XOR, P_TILDE
};

typedef struct osl_token {
    int kind, op;
    double num;
    const char *start;
    int len;
    int line, col;
} osl_token;

/* ------------------------------------------------------------------ AST */
enum {
    N_NUM = 1, N_STR, N_IDENT, N_BINOP, N_UNOP, N_ASSIGN, N_TERNARY, N_CALL, N_INDEX, N_PREINC, N_POSTINC,
    N_CAST,
    N_BLOCK, N_VARDECL, N_EXPRSTMT, N_IF, N_FOR, N_WHILE, N_DO, N_BREAK, N_CONTINUE, N_RETURN, N_EMPTY,
    N_PARAM, N_FUNC, N_SHADER
};

typedef struct osl_node {
    int kind, op;
    int line, col;
    double num;
    int is_int;
    char *name;
    osl_type dtype;            /* declared type (vardecl, param, cast, func return) */
    int is_output;
    int is_closure_decl;
    struct osl_node *a, *b, *c, *d;
    struct osl_node **list;
    int nlist;
} osl_node;

typedef struct osl_arena {
    struct osl_arena_block *head;
} osl_arena;
void *osl_arena_alloc(osl_arena *a, size_t n);
char *osl_arena_strndup(osl_arena *a, const char *s, size_t n);
void osl_arena_free(osl_arena *a);

typedef struct osl_front {
    const char *src;
    osl_token *toks;
    int ntoks, cap;
    int pos;
    osl_arena arena;
    char *err;
    size_t errcap;
    jmp_buf jb;
    osl_node *shader;          /* N_SHADER */
    osl_node **funcs;          /* N_FUNC */
    int nfuncs;
} osl_front;

void osl_front_error(osl_front *f, int line, int col, const char *fmt, ...);
int osl_lex(osl_front *f);
int osl_parse(osl_front *f);

/* --------------------------------------------------------------- program */
enum {
    OP_END = 0, OP_NOP, OP_MOV1, OP_MOV3, OP_BCAST,
    OP_ADD1, OP_SUB1, OP_MUL1, OP_DIV1, OP_MOD1, OP_IDIV, OP_IMOD,
    OP_ADD3, OP_SUB3, OP_MUL3, OP_DIV3,
    OP_NEG1, OP_NEG3, OP_NOT, OP_FTOI,
    OP_LT, OP_LE, OP_GT, OP_GE, OP_EQ1, OP_NE1, OP_EQ3, OP_NE3,
    OP_JMP, OP_JZ, OP_JNZ,
    OP_COMP, OP_COMPK, OP_SETCOMP, OP_SETCOMPK, OP_MAKE3,
    OP_CALL, OP_CLOSURE, OP_CL_ADD, OP_CL_MUL3, OP_CL_MUL1,
    OP_PARAM, OP_COUNT
};

typedef struct osl_insn { int op, d, a, b, c; int line; } osl_insn;

typedef struct osl_param_info {
    char name[64];
    osl_type type;
    int is_output;
    int reg;
} osl_param_info;

/* global registers */
enum { R_P = 0, R_I, R_N, R_NG, R_DPDU, R_DPDV, R_U, R_V, R_TIME, R_CI, R_NGLOBALS };

struct osl_program {
    char name[64];
    osl_insn *code; int ncode;
    int *args; int nargs;
    float *init;               /* initial register image, nregs * 3 */
    int nregs;
    char **strings; int nstrings;
    osl_param_info *params; int nparams;
    char **textures; int ntextures;
    int may_emit;
};

/* builtin function ids (OP_CALL c = id | width << 8) */
enum {
    B_SIN = 1, B_COS, B_TAN, B_ASIN, B_ACOS, B_ATAN, B_SINH, B_COSH, B_TANH, B_SQRT, B_INVSQRT,
    B_EXP, B_EXP2, B_LOG, B_LOG2, B_LOG10, B_ABS, B_FLOOR, B_CEIL, B_ROUND, B_TRUNC, B_SIGN,
    B_RADIANS, B_DEGREES,
    B_POW, B_FMOD, B_MOD, B_MIN, B_MAX, B_ATAN2, B_STEP,
    B_CLAMP, B_MIX, B_SMOOTHSTEP,
    B_DOT, B_CROSS, B_LENGTH, B_DISTANCE, B_NORMALIZE, B_FACEFORWARD, B_REFLECT, B_REFRACT,
    B_LUMINANCE, B_HYPOT, B_NOISE_U, B_NOISE_S, B_NOISE_CELL, B_TEXTURE, B_BACKFACING, B_RAYTYPE,
    B_ISNAN, B_ISINF, B_HSV2RGB, B_NOP, B_FRESNEL_SCHLICK
};

struct osl_exec {
    float *regs; int cap;
    struct osl_clnode { int kind; float w[3]; int a, b; osl_closure prim; } *cl;
    int ncl, clcap;
    char err[256];
};

osl_program *osl_codegen(osl_front *f, char *err, size_t errcap);

#endif
