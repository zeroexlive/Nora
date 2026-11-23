/* Nora OSL subset - arena, lexer and recursive descent parser. MIT License. */
#include "osl_internal.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---------------------------------------------------------------- arena */
struct osl_arena_block { struct osl_arena_block *next; size_t used, cap; };

void *osl_arena_alloc(osl_arena *a, size_t n) {
    struct osl_arena_block *b = a->head;
    void *p;
    n = (n + 15u) & ~(size_t)15u;
    if (!b || b->used + n > b->cap) {
        size_t cap = n > 65536 ? n : 65536;
        b = (struct osl_arena_block *)malloc(sizeof(struct osl_arena_block) + 16 + cap);
        if (!b) return NULL;
        b->next = a->head; b->used = 0; b->cap = cap;
        a->head = b;
    }
    p = (char *)(b + 1) + 16 + b->used;
    b->used += n;
    memset(p, 0, n);
    return p;
}
char *osl_arena_strndup(osl_arena *a, const char *s, size_t n) {
    char *d = (char *)osl_arena_alloc(a, n + 1);
    if (d) { memcpy(d, s, n); d[n] = 0; }
    return d;
}
void osl_arena_free(osl_arena *a) {
    struct osl_arena_block *b = a->head;
    while (b) { struct osl_arena_block *n = b->next; free(b); b = n; }
    a->head = NULL;
}

void osl_front_error(osl_front *f, int line, int col, const char *fmt, ...) {
    char msg[384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (f->err && f->errcap) snprintf(f->err, f->errcap, "%d:%d: error: %s", line, col, msg);
    longjmp(f->jb, 1);
}

/* ---------------------------------------------------------------- lexer */
static void push_tok(osl_front *f, int kind, int op, double num, const char *start, int len, int line, int col) {
    osl_token *t;
    if (f->ntoks >= f->cap) {
        int nc = f->cap ? f->cap * 2 : 256;
        osl_token *nt = (osl_token *)realloc(f->toks, sizeof(osl_token) * (size_t)nc);
        if (!nt) osl_front_error(f, line, col, "out of memory");
        f->toks = nt; f->cap = nc;
    }
    t = &f->toks[f->ntoks++];
    t->kind = kind; t->op = op; t->num = num; t->start = start; t->len = len; t->line = line; t->col = col;
}

int osl_lex(osl_front *f) {
    const char *p = f->src;
    int line = 1;
    const char *line_start = p;
    while (*p) {
        int col = (int)(p - line_start) + 1;
        char c = *p;
        if (c == '\n') { line++; p++; line_start = p; continue; }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') { p++; continue; }
        if (c == '/' && p[1] == '/') { while (*p && *p != '\n') p++; continue; }
        if (c == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) { if (*p == '\n') { line++; line_start = p + 1; } p++; }
            if (!*p) osl_front_error(f, line, col, "unterminated comment");
            p += 2;
            continue;
        }
        if (c == '#') {   /* preprocessor directive: ignored (e.g. #include "stdosl.h") */
            while (*p && *p != '\n') { if (*p == '\\' && p[1] == '\n') { p++; line++; line_start = p + 1; } p++; }
            continue;
        }
        if (isalpha((unsigned char)c) || c == '_') {
            const char *s = p;
            while (isalnum((unsigned char)*p) || *p == '_') p++;
            push_tok(f, TK_IDENT, 0, 0, s, (int)(p - s), line, col);
            continue;
        }
        if (isdigit((unsigned char)c) || (c == '.' && isdigit((unsigned char)p[1]))) {
            const char *s = p;
            int is_float = 0;
            double v;
            if (c == '0' && (p[1] == 'x' || p[1] == 'X')) {
                v = (double)strtol(p, (char **)&p, 16);
            } else {
                while (isdigit((unsigned char)*p)) p++;
                if (*p == '.') { is_float = 1; p++; while (isdigit((unsigned char)*p)) p++; }
                if (*p == 'e' || *p == 'E') {
                    const char *q = p + 1;
                    if (*q == '+' || *q == '-') q++;
                    if (isdigit((unsigned char)*q)) { is_float = 1; p = q; while (isdigit((unsigned char)*p)) p++; }
                }
                v = strtod(s, NULL);
                if (*p == 'f' || *p == 'F') { is_float = 1; p++; }
            }
            push_tok(f, is_float ? TK_FLOAT : TK_INT, 0, v, s, (int)(p - s), line, col);
            continue;
        }
        if (c == '"') {
            const char *s = ++p;
            while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; if (*p == '\n') osl_front_error(f, line, col, "newline in string"); p++; }
            if (!*p) osl_front_error(f, line, col, "unterminated string");
            push_tok(f, TK_STRING, 0, 0, s, (int)(p - s), line, col);
            p++;
            continue;
        }
        {
            int op = 0, len = 1;
            char d = p[1];
#define TWO(a, b, o) if (c == a && d == b) { op = o; len = 2; } else
            TWO('+', '=', P_PLUSEQ) TWO('-', '=', P_MINUSEQ) TWO('*', '=', P_STAREQ) TWO('/', '=', P_SLASHEQ)
            TWO('=', '=', P_EQ) TWO('!', '=', P_NE) TWO('<', '=', P_LE) TWO('>', '=', P_GE)
            TWO('&', '&', P_AND) TWO('|', '|', P_OR) TWO('+', '+', P_INC) TWO('-', '-', P_DEC)
            {
                switch (c) {
                case '(': op = P_LPAREN; break; case ')': op = P_RPAREN; break;
                case '{': op = P_LBRACE; break; case '}': op = P_RBRACE; break;
                case '[': op = P_LBRACK; break; case ']': op = P_RBRACK; break;
                case ';': op = P_SEMI; break; case ',': op = P_COMMA; break; case '.': op = P_DOT; break;
                case '+': op = P_PLUS; break; case '-': op = P_MINUS; break; case '*': op = P_STAR; break;
                case '/': op = P_SLASH; break; case '%': op = P_PERCENT; break; case '=': op = P_ASSIGN; break;
                case '<': op = P_LT; break; case '>': op = P_GT; break; case '!': op = P_NOT; break;
                case '?': op = P_QUESTION; break; case ':': op = P_COLON; break; case '&': op = P_BITAND; break;
                case '|': op = P_BITOR; break; case '^': op = P_XOR; break; case '~': op = P_TILDE; break;
                default: osl_front_error(f, line, col, "unexpected character '%c'", c);
                }
            }
#undef TWO
            push_tok(f, TK_PUNCT, op, 0, p, len, line, col);
            p += len;
        }
    }
    push_tok(f, TK_EOF, 0, 0, p, 0, line, (int)(p - line_start) + 1);
    return 0;
}

/* --------------------------------------------------------------- parser */
static osl_token *peek(osl_front *f) { return &f->toks[f->pos]; }
static osl_token *peekn(osl_front *f, int n) { int i = f->pos + n; if (i >= f->ntoks) i = f->ntoks - 1; return &f->toks[i]; }
static osl_token *next(osl_front *f) { osl_token *t = &f->toks[f->pos]; if (t->kind != TK_EOF) f->pos++; return t; }
static int is_punct(osl_token *t, int op) { return t->kind == TK_PUNCT && t->op == op; }
static int is_word(osl_token *t, const char *w) { return t->kind == TK_IDENT && (int)strlen(w) == t->len && strncmp(t->start, w, (size_t)t->len) == 0; }
static int accept(osl_front *f, int op) { if (is_punct(peek(f), op)) { f->pos++; return 1; } return 0; }

static const char *punct_text(int op) {
    static const char *names[] = { "", "(", ")", "{", "}", "[", "]", ";", ",", ".", "+", "-", "*", "/", "%", "=", "+=", "-=",
                                   "*=", "/=", "==", "!=", "<", "<=", ">", ">=", "&&", "||", "!", "?", ":", "++", "--",
                                   "[[", "]]", "&", "|", "^", "~" };
    return (op > 0 && op < (int)(sizeof names / sizeof names[0])) ? names[op] : "?";
}

static void expect(osl_front *f, int op) {
    osl_token *t = peek(f);
    if (!is_punct(t, op)) {
        if (t->kind == TK_EOF) osl_front_error(f, t->line, t->col, "expected '%s' before end of file", punct_text(op));
        osl_front_error(f, t->line, t->col, "expected '%s' but found '%.*s'", punct_text(op), t->len, t->start);
    }
    f->pos++;
}

static osl_node *mk(osl_front *f, int kind, osl_token *t) {
    osl_node *n = (osl_node *)osl_arena_alloc(&f->arena, sizeof(osl_node));
    if (!n) osl_front_error(f, t->line, t->col, "out of memory");
    n->kind = kind;
    n->line = t->line;
    n->col = t->col;
    return n;
}

static void list_push(osl_front *f, osl_node *parent, osl_node *child, int *cap) {
    if (parent->nlist >= *cap) {
        int nc = *cap ? *cap * 2 : 8;
        osl_node **nl = (osl_node **)osl_arena_alloc(&f->arena, sizeof(osl_node *) * (size_t)nc);
        if (parent->nlist) memcpy(nl, parent->list, sizeof(osl_node *) * (size_t)parent->nlist);
        parent->list = nl;
        *cap = nc;
    }
    parent->list[parent->nlist++] = child;
}

static char *tok_str(osl_front *f, osl_token *t) { return osl_arena_strndup(&f->arena, t->start, (size_t)t->len); }

/* type keyword at token (not consuming). Returns OSL_T_ERROR when not a type. */
static osl_type type_of_tok(osl_token *t) {
    if (t->kind != TK_IDENT) return OSL_T_ERROR;
    if (is_word(t, "float")) return OSL_T_FLOAT;
    if (is_word(t, "int")) return OSL_T_INT;
    if (is_word(t, "color")) return OSL_T_COLOR;
    if (is_word(t, "point")) return OSL_T_POINT;
    if (is_word(t, "vector")) return OSL_T_VECTOR;
    if (is_word(t, "normal")) return OSL_T_NORMAL;
    if (is_word(t, "string")) return OSL_T_STRING;
    if (is_word(t, "void")) return OSL_T_VOID;
    if (is_word(t, "closure")) return OSL_T_CLOSURE;
    return OSL_T_ERROR;
}

static int starts_type(osl_front *f) {
    osl_token *t = peek(f);
    if (is_word(t, "matrix") || is_word(t, "struct")) osl_front_error(f, t->line, t->col, "'%.*s' is not supported by the built-in OSL compiler", t->len, t->start);
    return type_of_tok(t) != OSL_T_ERROR;
}

static osl_type parse_type(osl_front *f) {
    osl_token *t = next(f);
    osl_type ty = type_of_tok(t);
    if (ty == OSL_T_ERROR) osl_front_error(f, t->line, t->col, "expected a type");
    if (ty == OSL_T_CLOSURE) {
        osl_token *c = next(f);
        if (!is_word(c, "color")) osl_front_error(f, c->line, c->col, "expected 'color' after 'closure'");
    }
    return ty;
}

static void skip_metadata(osl_front *f) {
    while (is_punct(peek(f), P_LBRACK) && is_punct(peekn(f, 1), P_LBRACK)) {
        int depth = 0;
        do {
            osl_token *t = next(f);
            if (t->kind == TK_EOF) osl_front_error(f, t->line, t->col, "unterminated metadata");
            if (is_punct(t, P_LBRACK)) depth++;
            else if (is_punct(t, P_RBRACK)) depth--;
        } while (depth > 0);
    }
}

static osl_node *parse_expr(osl_front *f);
static osl_node *parse_assign(osl_front *f);
static osl_node *parse_stmt(osl_front *f);

static osl_node *parse_args(osl_front *f, osl_node *call) {
    int cap = 0;
    if (!accept(f, P_RPAREN)) {
        do { list_push(f, call, parse_assign(f), &cap); } while (accept(f, P_COMMA));
        expect(f, P_RPAREN);
    }
    return call;
}

static osl_node *parse_primary(osl_front *f) {
    osl_token *t = peek(f);
    osl_node *n;
    if (t->kind == TK_INT || t->kind == TK_FLOAT) {
        next(f);
        n = mk(f, N_NUM, t);
        n->num = t->num;
        n->is_int = t->kind == TK_INT;
        return n;
    }
    if (t->kind == TK_STRING) {
        /* adjacent literals concatenate */
        size_t total = 0, off = 0;
        int i = f->pos;
        char *s;
        while (f->toks[i].kind == TK_STRING) { total += (size_t)f->toks[i].len; i++; }
        s = (char *)osl_arena_alloc(&f->arena, total + 1);
        while (peek(f)->kind == TK_STRING) {
            osl_token *st = next(f);
            int k;
            for (k = 0; k < st->len; k++) {
                char ch = st->start[k];
                if (ch == '\\' && k + 1 < st->len) {
                    char e = st->start[++k];
                    ch = e == 'n' ? '\n' : (e == 't' ? '\t' : e);
                }
                s[off++] = ch;
            }
        }
        s[off] = 0;
        n = mk(f, N_STR, t);
        n->name = s;
        return n;
    }
    if (t->kind == TK_IDENT) {
        osl_type ty = type_of_tok(t);
        if (ty != OSL_T_ERROR && ty != OSL_T_CLOSURE && ty != OSL_T_VOID) {
            next(f);
            if (!is_punct(peek(f), P_LPAREN)) osl_front_error(f, t->line, t->col, "expected '(' after type name '%.*s'", t->len, t->start);
            next(f);
            n = mk(f, N_CALL, t);
            n->name = tok_str(f, t);
            return parse_args(f, n);
        }
        next(f);
        if (is_punct(peek(f), P_LPAREN)) {
            next(f);
            n = mk(f, N_CALL, t);
            n->name = tok_str(f, t);
            return parse_args(f, n);
        }
        n = mk(f, N_IDENT, t);
        n->name = tok_str(f, t);
        return n;
    }
    if (is_punct(t, P_LPAREN)) {
        next(f);
        n = parse_expr(f);
        expect(f, P_RPAREN);
        return n;
    }
    if (t->kind == TK_EOF) osl_front_error(f, t->line, t->col, "unexpected end of file in expression");
    osl_front_error(f, t->line, t->col, "unexpected '%.*s' in expression", t->len, t->start);
    return NULL;
}

static osl_node *parse_postfix(osl_front *f) {
    osl_node *n = parse_primary(f);
    for (;;) {
        osl_token *t = peek(f);
        if (is_punct(t, P_LBRACK) && !is_punct(peekn(f, 1), P_LBRACK)) {
            osl_node *ix = mk(f, N_INDEX, t);
            next(f);
            ix->a = n;
            ix->b = parse_expr(f);
            expect(f, P_RBRACK);
            n = ix;
        } else if (is_punct(t, P_INC) || is_punct(t, P_DEC)) {
            osl_node *pn = mk(f, N_POSTINC, t);
            next(f);
            pn->op = t->op;
            pn->a = n;
            n = pn;
        } else if (is_punct(t, P_DOT)) {
            osl_front_error(f, t->line, t->col, "member access is not supported (use c[0], c[1], c[2])");
        } else break;
    }
    return n;
}

static osl_node *parse_unary(osl_front *f) {
    osl_token *t = peek(f);
    if (is_punct(t, P_MINUS) || is_punct(t, P_PLUS) || is_punct(t, P_NOT) || is_punct(t, P_TILDE)) {
        osl_node *n = mk(f, N_UNOP, t);
        next(f);
        n->op = t->op;
        n->a = parse_unary(f);
        return n;
    }
    if (is_punct(t, P_INC) || is_punct(t, P_DEC)) {
        osl_node *n = mk(f, N_PREINC, t);
        next(f);
        n->op = t->op;
        n->a = parse_unary(f);
        return n;
    }
    if (is_punct(t, P_LPAREN) && type_of_tok(peekn(f, 1)) != OSL_T_ERROR && is_punct(peekn(f, 2), P_RPAREN)) {
        osl_node *n = mk(f, N_CAST, t);
        next(f);
        n->dtype = parse_type(f);
        expect(f, P_RPAREN);
        n->a = parse_unary(f);
        return n;
    }
    return parse_postfix(f);
}

static osl_node *binop(osl_front *f, osl_token *t, int op, osl_node *a, osl_node *b) {
    osl_node *n = mk(f, N_BINOP, t);
    n->op = op; n->a = a; n->b = b;
    return n;
}

static osl_node *parse_mul(osl_front *f) {
    osl_node *n = parse_unary(f);
    for (;;) {
        osl_token *t = peek(f);
        if (is_punct(t, P_STAR) || is_punct(t, P_SLASH) || is_punct(t, P_PERCENT)) { next(f); n = binop(f, t, t->op, n, parse_unary(f)); }
        else return n;
    }
}
static osl_node *parse_add(osl_front *f) {
    osl_node *n = parse_mul(f);
    for (;;) {
        osl_token *t = peek(f);
        if (is_punct(t, P_PLUS) || is_punct(t, P_MINUS)) { next(f); n = binop(f, t, t->op, n, parse_mul(f)); }
        else return n;
    }
}
static osl_node *parse_rel(osl_front *f) {
    osl_node *n = parse_add(f);
    for (;;) {
        osl_token *t = peek(f);
        if (is_punct(t, P_LT) || is_punct(t, P_LE) || is_punct(t, P_GT) || is_punct(t, P_GE)) { next(f); n = binop(f, t, t->op, n, parse_add(f)); }
        else return n;
    }
}
static osl_node *parse_eq(osl_front *f) {
    osl_node *n = parse_rel(f);
    for (;;) {
        osl_token *t = peek(f);
        if (is_punct(t, P_EQ) || is_punct(t, P_NE)) { next(f); n = binop(f, t, t->op, n, parse_rel(f)); }
        else return n;
    }
}
static osl_node *parse_and(osl_front *f) {
    osl_node *n = parse_eq(f);
    for (;;) {
        osl_token *t = peek(f);
        if (is_punct(t, P_AND)) { next(f); n = binop(f, t, P_AND, n, parse_eq(f)); }
        else return n;
    }
}
static osl_node *parse_or(osl_front *f) {
    osl_node *n = parse_and(f);
    for (;;) {
        osl_token *t = peek(f);
        if (is_punct(t, P_OR)) { next(f); n = binop(f, t, P_OR, n, parse_and(f)); }
        else return n;
    }
}
static osl_node *parse_ternary(osl_front *f) {
    osl_node *c = parse_or(f);
    osl_token *t = peek(f);
    if (is_punct(t, P_QUESTION)) {
        osl_node *n = mk(f, N_TERNARY, t);
        next(f);
        n->a = c;
        n->b = parse_expr(f);
        expect(f, P_COLON);
        n->c = parse_ternary(f);
        return n;
    }
    return c;
}
static osl_node *parse_assign(osl_front *f) {
    osl_node *lhs = parse_ternary(f);
    osl_token *t = peek(f);
    if (is_punct(t, P_ASSIGN) || is_punct(t, P_PLUSEQ) || is_punct(t, P_MINUSEQ) || is_punct(t, P_STAREQ) || is_punct(t, P_SLASHEQ)) {
        osl_node *n = mk(f, N_ASSIGN, t);
        next(f);
        n->op = t->op;
        n->a = lhs;
        n->b = parse_assign(f);
        return n;
    }
    return lhs;
}
static osl_node *parse_expr(osl_front *f) {
    osl_node *n = parse_assign(f);
    osl_token *t = peek(f);
    if (is_punct(t, P_COMMA)) {
        /* comma operator: evaluate left, value of right */
        osl_node *blk = mk(f, N_BINOP, t);
        int cap = 0;
        (void)cap;
        next(f);
        blk->op = P_COMMA;
        blk->a = n;
        blk->b = parse_expr(f);
        return blk;
    }
    return n;
}

static osl_node *parse_vardecl(osl_front *f) {
    osl_token *t = peek(f);
    osl_node *n = mk(f, N_VARDECL, t);
    int cap = 0;
    n->dtype = parse_type(f);
    if (n->dtype == OSL_T_VOID) osl_front_error(f, t->line, t->col, "variables cannot be void");
    do {
        osl_token *id = next(f);
        osl_node *d;
        if (id->kind != TK_IDENT) osl_front_error(f, id->line, id->col, "expected a variable name");
        d = mk(f, N_IDENT, id);
        d->name = tok_str(f, id);
        if (is_punct(peek(f), P_LBRACK)) osl_front_error(f, id->line, id->col, "arrays are not supported by the built-in OSL compiler");
        if (accept(f, P_ASSIGN)) d->a = parse_assign(f);
        list_push(f, n, d, &cap);
    } while (accept(f, P_COMMA));
    return n;
}

static osl_node *parse_block(osl_front *f) {
    osl_token *t = peek(f);
    osl_node *n = mk(f, N_BLOCK, t);
    int cap = 0;
    expect(f, P_LBRACE);
    while (!is_punct(peek(f), P_RBRACE)) {
        if (peek(f)->kind == TK_EOF) osl_front_error(f, peek(f)->line, peek(f)->col, "expected '}' before end of file");
        list_push(f, n, parse_stmt(f), &cap);
    }
    next(f);
    return n;
}

static osl_node *parse_stmt(osl_front *f) {
    osl_token *t = peek(f);
    osl_node *n;
    if (is_punct(t, P_LBRACE)) return parse_block(f);
    if (is_punct(t, P_SEMI)) { next(f); return mk(f, N_EMPTY, t); }
    if (is_word(t, "if")) {
        next(f);
        n = mk(f, N_IF, t);
        expect(f, P_LPAREN); n->a = parse_expr(f); expect(f, P_RPAREN);
        n->b = parse_stmt(f);
        if (is_word(peek(f), "else")) { next(f); n->c = parse_stmt(f); }
        return n;
    }
    if (is_word(t, "while")) {
        next(f);
        n = mk(f, N_WHILE, t);
        expect(f, P_LPAREN); n->a = parse_expr(f); expect(f, P_RPAREN);
        n->b = parse_stmt(f);
        return n;
    }
    if (is_word(t, "do")) {
        next(f);
        n = mk(f, N_DO, t);
        n->b = parse_stmt(f);
        if (!is_word(peek(f), "while")) osl_front_error(f, peek(f)->line, peek(f)->col, "expected 'while' after do-body");
        next(f);
        expect(f, P_LPAREN); n->a = parse_expr(f); expect(f, P_RPAREN); expect(f, P_SEMI);
        return n;
    }
    if (is_word(t, "for")) {
        next(f);
        n = mk(f, N_FOR, t);
        expect(f, P_LPAREN);
        if (!accept(f, P_SEMI)) {
            if (starts_type(f)) n->a = parse_vardecl(f);
            else { n->a = mk(f, N_EXPRSTMT, peek(f)); n->a->a = parse_expr(f); }
            expect(f, P_SEMI);
        }
        if (!is_punct(peek(f), P_SEMI)) n->b = parse_expr(f);
        expect(f, P_SEMI);
        if (!is_punct(peek(f), P_RPAREN)) n->c = parse_expr(f);
        expect(f, P_RPAREN);
        n->d = parse_stmt(f);
        return n;
    }
    if (is_word(t, "break")) { next(f); expect(f, P_SEMI); return mk(f, N_BREAK, t); }
    if (is_word(t, "continue")) { next(f); expect(f, P_SEMI); return mk(f, N_CONTINUE, t); }
    if (is_word(t, "return")) {
        next(f);
        n = mk(f, N_RETURN, t);
        if (!is_punct(peek(f), P_SEMI)) n->a = parse_expr(f);
        expect(f, P_SEMI);
        return n;
    }
    if (starts_type(f) && !(type_of_tok(t) != OSL_T_CLOSURE && is_punct(peekn(f, 1), P_LPAREN))) {
        n = parse_vardecl(f);
        expect(f, P_SEMI);
        return n;
    }
    n = mk(f, N_EXPRSTMT, t);
    n->a = parse_expr(f);
    expect(f, P_SEMI);
    return n;
}

static osl_node *parse_param(osl_front *f, int is_formal) {
    osl_token *t = peek(f);
    osl_node *p = mk(f, N_PARAM, t);
    osl_token *id;
    if (is_word(t, "output")) { next(f); p->is_output = 1; }
    p->dtype = parse_type(f);
    id = next(f);
    if (id->kind != TK_IDENT) osl_front_error(f, id->line, id->col, "expected a parameter name");
    p->name = tok_str(f, id);
    p->line = id->line; p->col = id->col;
    if (is_punct(peek(f), P_LBRACK)) osl_front_error(f, id->line, id->col, "array parameters are not supported");
    if (accept(f, P_ASSIGN)) p->a = parse_assign(f);
    else if (!is_formal) osl_front_error(f, id->line, id->col, "shader parameter '%s' needs a default value", p->name);
    skip_metadata(f);
    return p;
}

int osl_parse(osl_front *f) {
    f->pos = 0;
    while (peek(f)->kind != TK_EOF) {
        osl_token *t = peek(f);
        if (is_word(t, "surface") || is_word(t, "shader") || is_word(t, "displacement") || is_word(t, "volume") || is_word(t, "light")) {
            osl_node *s;
            osl_token *id;
            int cap = 0;
            if (f->shader) osl_front_error(f, t->line, t->col, "only one shader per file is supported");
            if (is_word(t, "displacement") || is_word(t, "volume") || is_word(t, "light"))
                osl_front_error(f, t->line, t->col, "only surface shaders are supported");
            next(f);
            s = mk(f, N_SHADER, t);
            id = next(f);
            if (id->kind != TK_IDENT) osl_front_error(f, id->line, id->col, "expected the shader name");
            s->name = tok_str(f, id);
            skip_metadata(f);
            expect(f, P_LPAREN);
            if (!accept(f, P_RPAREN)) {
                do {
                    if (is_punct(peek(f), P_RPAREN)) break;   /* tolerate a trailing comma */
                    list_push(f, s, parse_param(f, 0), &cap);
                } while (accept(f, P_COMMA));
                expect(f, P_RPAREN);
            }
            skip_metadata(f);
            s->a = parse_block(f);
            f->shader = s;
        } else if (starts_type(f)) {
            osl_node *fn = mk(f, N_FUNC, t);
            osl_token *id;
            int cap = 0;
            fn->dtype = parse_type(f);
            id = next(f);
            if (id->kind != TK_IDENT) osl_front_error(f, id->line, id->col, "expected a function name");
            fn->name = tok_str(f, id);
            expect(f, P_LPAREN);
            if (!accept(f, P_RPAREN)) {
                do { list_push(f, fn, parse_param(f, 1), &cap); } while (accept(f, P_COMMA));
                expect(f, P_RPAREN);
            }
            skip_metadata(f);
            fn->a = parse_block(f);
            {
                osl_node **nf = (osl_node **)osl_arena_alloc(&f->arena, sizeof(osl_node *) * (size_t)(f->nfuncs + 1));
                if (f->nfuncs) memcpy(nf, f->funcs, sizeof(osl_node *) * (size_t)f->nfuncs);
                nf[f->nfuncs++] = fn;
                f->funcs = nf;
            }
        } else if (is_punct(t, P_SEMI)) {
            next(f);
        } else {
            osl_front_error(f, t->line, t->col, "expected a shader or function declaration, found '%.*s'", t->len, t->start);
        }
    }
    if (!f->shader) osl_front_error(f, peek(f)->line, peek(f)->col, "no surface shader found");
    return 0;
}
