/* Nora - edit-mode software rasterizer. MIT License. */
#include "raster.h"
#include "image.h"
#include <stdlib.h>
#include <string.h>

void nora_camera_basis(const nora_camera *c, v3 *f, v3 *r, v3 *u) {
    *f = v3_norm(v3_sub(c->target, c->pos));
    *r = v3_norm(v3_cross(*f, c->up));
    *u = v3_cross(*r, *f);
}

typedef struct cam_xf { v3 o, f, r, u; float sx, sy; int w, h; } cam_xf;

static void cam_setup(cam_xf *c, const nora_camera *cam, int w, int h) {
    float th = tanf(0.5f * cam->vfov_deg * NORA_PI / 180.0f);
    nora_camera_basis(cam, &c->f, &c->r, &c->u);
    c->o = cam->pos;
    c->sy = 1.0f / th;
    c->sx = 1.0f / (th * (float)w / (float)h);
    c->w = w; c->h = h;
}

/* camera space: x right, y up, z forward */
static v3 to_cam(const cam_xf *c, v3 p) { v3 d = v3_sub(p, c->o); return v3_make(v3_dot(d, c->r), v3_dot(d, c->u), v3_dot(d, c->f)); }
static void to_screen(const cam_xf *c, v3 q, float *px, float *py) {
    *px = (q.x * c->sx / q.z + 1.0f) * 0.5f * (float)c->w;
    *py = (1.0f - q.y * c->sy / q.z) * 0.5f * (float)c->h;
}

int nora_project(const nora_camera *cam, int w, int h, v3 p, float *px, float *py, float *z) {
    cam_xf c;
    v3 q;
    cam_setup(&c, cam, w, h);
    q = to_cam(&c, p);
    if (z) *z = q.z;
    if (q.z <= 1e-6f) return 0;
    to_screen(&c, q, px, py);
    return 1;
}

typedef struct rctx {
    int w, h;
    unsigned char *rgba;
    float *depth;               /* 1/z, larger is closer; 0 = empty */
    int *ids;
} rctx;

static float edge(float ax, float ay, float bx, float by, float px, float py) { return (bx - ax) * (py - ay) - (by - ay) * (px - ax); }

static void fill_tri(rctx *r, const float sx[3], const float sy[3], const float iz[3], const unsigned char col[3], int id) {
    float area = edge(sx[0], sy[0], sx[1], sy[1], sx[2], sy[2]);
    int x0, x1, y0, y1, x, y;
    if (area == 0.0f || !(area == area)) return;
    x0 = (int)floorf(fminf(sx[0], fminf(sx[1], sx[2])));
    x1 = (int)ceilf(fmaxf(sx[0], fmaxf(sx[1], sx[2])));
    y0 = (int)floorf(fminf(sy[0], fminf(sy[1], sy[2])));
    y1 = (int)ceilf(fmaxf(sy[0], fmaxf(sy[1], sy[2])));
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > r->w - 1) x1 = r->w - 1;
    if (y1 > r->h - 1) y1 = r->h - 1;
    for (y = y0; y <= y1; y++) {
        float py = (float)y + 0.5f;
        for (x = x0; x <= x1; x++) {
            float px = (float)x + 0.5f;
            float w0 = edge(sx[1], sy[1], sx[2], sy[2], px, py) / area;
            float w1 = edge(sx[2], sy[2], sx[0], sy[0], px, py) / area;
            float w2 = 1.0f - w0 - w1;
            float z;
            size_t i;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
            z = w0 * iz[0] + w1 * iz[1] + w2 * iz[2];
            i = (size_t)y * r->w + x;
            if (z <= r->depth[i]) continue;
            r->depth[i] = z;
            r->rgba[i * 4] = col[0]; r->rgba[i * 4 + 1] = col[1]; r->rgba[i * 4 + 2] = col[2]; r->rgba[i * 4 + 3] = 255;
            if (r->ids) r->ids[i] = id;
        }
    }
}

static void draw_line(rctx *r, float ax, float ay, float az, float bx, float by, float bz, const unsigned char col[3]) {
    float dx = bx - ax, dy = by - ay;
    int n = (int)ceilf(fmaxf(fabsf(dx), fabsf(dy))), k;
    if (n <= 0) n = 1;
    if (n > 8192) n = 8192;
    for (k = 0; k <= n; k++) {
        float t = (float)k / (float)n;
        int x = (int)floorf(ax + dx * t), y = (int)floorf(ay + dy * t);
        float z = az + (bz - az) * t;
        size_t i;
        if (x < 0 || y < 0 || x >= r->w || y >= r->h) continue;
        i = (size_t)y * r->w + x;
        if (z < r->depth[i] * 0.995f) continue;   /* hidden behind another surface */
        r->rgba[i * 4] = col[0]; r->rgba[i * 4 + 1] = col[1]; r->rgba[i * 4 + 2] = col[2];
    }
}

static unsigned char to8(float c) { return (unsigned char)(nclampf(nora_linear_to_srgb(nclampf(c, 0, 1)), 0, 1) * 255.0f + 0.5f); }

#define NEAR_Z 1e-3f

/* clips a camera-space triangle against z = near; returns the vertex count (0, 3 or 4) */
static int clip_near(const v3 in[3], v3 out[4], float nearz) {
    int i, n = 0;
    for (i = 0; i < 3; i++) {
        v3 a = in[i], b = in[(i + 1) % 3];
        int ain = a.z >= nearz, bin = b.z >= nearz;
        if (ain) out[n++] = a;
        if (ain != bin) {
            float t = (nearz - a.z) / (b.z - a.z);
            out[n++] = v3_lerp(a, b, t);
        }
    }
    return n;
}

int nora_raster(const nora_scene *s, const nora_camera *cam, int w, int h, const nora_raster_opts *o,
                unsigned char *rgba, float *depth, int *ids) {
    rctx r;
    cam_xf c;
    int t, k, drawn = 0, sel = o ? o->selected : -1, wire = o ? o->wireframe : 0;
    float *own_depth = NULL, nearz;
    size_t npix = (size_t)w * h, i;
    v3 bmn, bmx;
    if (w <= 0 || h <= 0 || !rgba) return 0;
    if (!depth) { own_depth = (float *)malloc(sizeof(float) * npix); depth = own_depth; if (!depth) return 0; }
    r.w = w; r.h = h; r.rgba = rgba; r.depth = depth; r.ids = ids;
    cam_setup(&c, cam, w, h);
    /* background: vertical gradient */
    for (k = 0; k < h; k++) {
        float tt = (float)k / (float)(h > 1 ? h - 1 : 1);
        unsigned char g = (unsigned char)(46 - 22 * tt), b2 = (unsigned char)(52 - 24 * tt);
        for (t = 0; t < w; t++) {
            unsigned char *p = rgba + ((size_t)k * w + t) * 4;
            p[0] = g; p[1] = g; p[2] = b2; p[3] = 255;
        }
    }
    for (i = 0; i < npix; i++) depth[i] = 0.0f;
    if (ids) for (i = 0; i < npix; i++) ids[i] = -1;
    nora_scene_bounds(s, &bmn, &bmx);
    nearz = fmaxf(NEAR_Z, 1e-5f * v3_len(v3_sub(bmx, bmn)));
    for (t = 0; t < s->ntris; t++) {
        const nora_tri *tr = &s->tris[t];
        v3 p[3], q[3], poly[4], n, cen;
        float lum, ndl;
        v3 base;
        unsigned char col[3];
        int m = tr->mesh, nv;
        if (m >= 0 && m < s->nmeshes && s->meshes[m].hidden) continue;
        for (k = 0; k < 3; k++) { p[k] = s->pos[tr->v[k]]; q[k] = to_cam(&c, p[k]); }
        if (q[0].z < nearz && q[1].z < nearz && q[2].z < nearz) continue;
        n = v3_norm(v3_cross(v3_sub(p[1], p[0]), v3_sub(p[2], p[0])));
        cen = v3_scale(v3_add(p[0], v3_add(p[1], p[2])), 1.0f / 3.0f);
        ndl = fabsf(v3_dot(n, v3_norm(v3_sub(c.o, cen))));
        /* headlight plus a little key light from above */
        lum = 0.22f + 0.62f * ndl + 0.16f * fmaxf(0.0f, n.y);
        if (tr->mat >= 0 && tr->mat < s->nmats) {
            const nora_material *mt = &s->mats[tr->mat];
            base = mt->color;
            if (mt->shader >= 0) base = v3_make(0.62f, 0.62f, 0.66f);
            if (mt->type == NORA_MAT_DIELECTRIC) base = v3_make(0.55f, 0.7f, 0.8f);
            if (mt->emission_strength > 0.0f && v3_maxc(mt->emission) > 0.0f) {
                base = v3_scale(mt->emission, 1.0f / v3_maxc(mt->emission));
                lum = 1.0f;
            }
        } else {
            base = v3_make(0.7f, 0.7f, 0.7f);
        }
        base = v3_scale(base, lum);
        if (m == sel) base = v3_lerp(base, v3_make(1.0f, 0.55f, 0.12f), 0.35f);
        col[0] = to8(base.x); col[1] = to8(base.y); col[2] = to8(base.z);
        nv = clip_near(q, poly, nearz);
        for (k = 1; k + 1 < nv; k++) {
            float sx[3], sy[3], iz[3];
            to_screen(&c, poly[0], &sx[0], &sy[0]); iz[0] = 1.0f / poly[0].z;
            to_screen(&c, poly[k], &sx[1], &sy[1]); iz[1] = 1.0f / poly[k].z;
            to_screen(&c, poly[k + 1], &sx[2], &sy[2]); iz[2] = 1.0f / poly[k + 1].z;
            fill_tri(&r, sx, sy, iz, col, m);
        }
        drawn++;
    }
    /* edges: everything in wireframe mode, the selection always */
    if (wire || sel >= 0) {
        static const unsigned char wcol[3] = { 200, 205, 215 }, scol[3] = { 255, 150, 40 };
        for (t = 0; t < s->ntris; t++) {
            const nora_tri *tr = &s->tris[t];
            int m = tr->mesh;
            v3 q[3];
            if (m >= 0 && m < s->nmeshes && s->meshes[m].hidden) continue;
            if (!wire && m != sel) continue;
            for (k = 0; k < 3; k++) q[k] = to_cam(&c, s->pos[tr->v[k]]);
            for (k = 0; k < 3; k++) {
                v3 a = q[k], b = q[(k + 1) % 3];
                float ax, ay, bx, by;
                if (a.z < nearz && b.z < nearz) continue;
                if (a.z < nearz) a = v3_lerp(a, b, (nearz - a.z) / (b.z - a.z));
                if (b.z < nearz) b = v3_lerp(b, a, (nearz - b.z) / (a.z - b.z));
                to_screen(&c, a, &ax, &ay);
                to_screen(&c, b, &bx, &by);
                draw_line(&r, ax, ay, 1.0f / a.z, bx, by, 1.0f / b.z, m == sel ? scol : wcol);
            }
        }
    }
    free(own_depth);
    return drawn;
}
