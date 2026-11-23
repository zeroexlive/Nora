/* Nora - small inline vector math. MIT License. */
#ifndef NORA_NMATH_H
#define NORA_NMATH_H

#include <math.h>
#include <stdint.h>

#ifndef NORA_PI
#define NORA_PI 3.14159265358979323846f
#endif
#define NORA_INV_PI 0.31830988618379067154f
#define NORA_INFF 1e30f

#if defined(_MSC_VER)
#define NORA_INLINE static __inline
#else
#define NORA_INLINE static inline
#endif

typedef struct v3 { float x, y, z; } v3;

NORA_INLINE v3 v3_make(float x, float y, float z) { v3 r; r.x = x; r.y = y; r.z = z; return r; }
NORA_INLINE v3 v3_splat(float s) { return v3_make(s, s, s); }
NORA_INLINE v3 v3_add(v3 a, v3 b) { return v3_make(a.x + b.x, a.y + b.y, a.z + b.z); }
NORA_INLINE v3 v3_sub(v3 a, v3 b) { return v3_make(a.x - b.x, a.y - b.y, a.z - b.z); }
NORA_INLINE v3 v3_mul(v3 a, v3 b) { return v3_make(a.x * b.x, a.y * b.y, a.z * b.z); }
NORA_INLINE v3 v3_scale(v3 a, float s) { return v3_make(a.x * s, a.y * s, a.z * s); }
NORA_INLINE v3 v3_neg(v3 a) { return v3_make(-a.x, -a.y, -a.z); }
NORA_INLINE float v3_dot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
NORA_INLINE v3 v3_cross(v3 a, v3 b) {
    return v3_make(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
NORA_INLINE float v3_len(v3 a) { return sqrtf(v3_dot(a, a)); }
NORA_INLINE v3 v3_norm(v3 a) {
    float l = v3_len(a);
    return l > 0.0f ? v3_scale(a, 1.0f / l) : v3_make(0, 0, 1);
}
NORA_INLINE v3 v3_min(v3 a, v3 b) { return v3_make(fminf(a.x, b.x), fminf(a.y, b.y), fminf(a.z, b.z)); }
NORA_INLINE v3 v3_max(v3 a, v3 b) { return v3_make(fmaxf(a.x, b.x), fmaxf(a.y, b.y), fmaxf(a.z, b.z)); }
NORA_INLINE float v3_get(v3 a, int i) { return i == 0 ? a.x : (i == 1 ? a.y : a.z); }
NORA_INLINE float v3_maxc(v3 a) { return fmaxf(a.x, fmaxf(a.y, a.z)); }
NORA_INLINE v3 v3_lerp(v3 a, v3 b, float t) { return v3_add(v3_scale(a, 1.0f - t), v3_scale(b, t)); }
NORA_INLINE float nclampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }
NORA_INLINE float nsqr(float x) { return x * x; }
NORA_INLINE float nsafe_sqrt(float x) { return x > 0.0f ? sqrtf(x) : 0.0f; }
NORA_INLINE float nlum(v3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

/* Orthonormal basis from a unit normal (Duff et al. 2017). */
NORA_INLINE void onb_from(v3 n, v3 *t, v3 *b) {
    float sign = n.z >= 0.0f ? 1.0f : -1.0f;
    float a = -1.0f / (sign + n.z);
    float bb = n.x * n.y * a;
    *t = v3_make(1.0f + sign * n.x * n.x * a, sign * bb, -sign * n.x);
    *b = v3_make(bb, sign + n.y * n.y * a, -n.y);
}

typedef struct frame3 { v3 t, b, n; } frame3;
NORA_INLINE frame3 frame_from(v3 n) { frame3 f; f.n = n; onb_from(n, &f.t, &f.b); return f; }
NORA_INLINE v3 frame_to_local(const frame3 *f, v3 v) { return v3_make(v3_dot(v, f->t), v3_dot(v, f->b), v3_dot(v, f->n)); }
NORA_INLINE v3 frame_to_world(const frame3 *f, v3 v) {
    return v3_add(v3_add(v3_scale(f->t, v.x), v3_scale(f->b, v.y)), v3_scale(f->n, v.z));
}

typedef struct ray3 { v3 o, d; float tmin, tmax; } ray3;

/* PCG32 random number generator (O'Neill), minimal variant. */
typedef struct pcg32 { uint64_t state, inc; } pcg32;
NORA_INLINE uint32_t pcg32_next(pcg32 *r) {
    uint64_t old = r->state;
    uint32_t xs, rot;
    r->state = old * 6364136223846793005ULL + r->inc;
    xs = (uint32_t)(((old >> 18u) ^ old) >> 27u);
    rot = (uint32_t)(old >> 59u);
    return (xs >> rot) | (xs << ((32u - rot) & 31u));
}
NORA_INLINE void pcg32_seed(pcg32 *r, uint64_t seed, uint64_t seq) {
    r->state = 0u;
    r->inc = (seq << 1u) | 1u;
    pcg32_next(r);
    r->state += seed;
    pcg32_next(r);
}
NORA_INLINE float pcg32_float(pcg32 *r) {
    /* 24 random bits -> [0,1) */
    return (float)(pcg32_next(r) >> 8) * (1.0f / 16777216.0f);
}

NORA_INLINE uint64_t nhash64(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

#endif
