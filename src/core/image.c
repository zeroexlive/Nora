/* Nora - image IO. MIT License.
 * PNG writing and image loading use the stb headers that ship with CUI
 * (public domain / MIT, see THIRD_PARTY in the CUI repository). */
#include "image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_GIF
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

float nora_srgb_to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}
float nora_linear_to_srgb(float c) {
    if (c <= 0.0f) return 0.0f;
    if (c >= 1.0f) return 1.0f;
    return c <= 0.0031308f ? 12.92f * c : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

static float tm_channel(float x, nora_tonemap tm) {
    if (!(x == x) || x < 0.0f) x = 0.0f;
    switch (tm) {
    case NORA_TONEMAP_REINHARD: return x / (1.0f + x);
    case NORA_TONEMAP_ACES: {   /* Narkowicz 2015 fit */
        float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
        x *= 0.6f;
        return (x * (a * x + b)) / (x * (c * x + d) + e);
    }
    default: return x;
    }
}

void nora_image_to_srgb8(const float *rgb, int w, int h, float exposure, nora_tonemap tm, uint8_t *rgba) {
    float k = powf(2.0f, exposure);
    long i, n = (long)w * h;
    for (i = 0; i < n; i++) {
        int c;
        for (c = 0; c < 3; c++) {
            float v = nora_linear_to_srgb(tm_channel(rgb[i * 3 + c] * k, tm));
            int q = (int)(v * 255.0f + 0.5f);
            rgba[i * 4 + c] = (uint8_t)(q < 0 ? 0 : (q > 255 ? 255 : q));
        }
        rgba[i * 4 + 3] = 255;
    }
}

int nora_write_png(const char *path, const float *rgb, int w, int h, float exposure, nora_tonemap tm) {
    uint8_t *buf = (uint8_t *)malloc((size_t)w * h * 4);
    int ok;
    if (!buf) return -1;
    nora_image_to_srgb8(rgb, w, h, exposure, tm, buf);
    ok = stbi_write_png(path, w, h, 4, buf, w * 4);
    free(buf);
    return ok ? 0 : -1;
}

int nora_write_pfm(const char *path, const float *rgb, int w, int h) {
    FILE *f = fopen(path, "wb");
    int y;
    if (!f) return -1;
    fprintf(f, "PF\n%d %d\n-1.0\n", w, h);   /* negative scale: little endian */
    for (y = h - 1; y >= 0; y--) {            /* PFM stores rows bottom to top */
        int x;
        for (x = 0; x < w; x++) {
            float px[3];
            unsigned char b[12];
            int c, k;
            for (c = 0; c < 3; c++) px[c] = rgb[((size_t)y * w + x) * 3 + c];
            for (c = 0; c < 3; c++) {
                uint32_t u;
                memcpy(&u, &px[c], 4);
                for (k = 0; k < 4; k++) b[c * 4 + k] = (unsigned char)((u >> (8 * k)) & 0xff);
            }
            fwrite(b, 1, 12, f);
        }
    }
    fclose(f);
    return 0;
}

/* --- minimal OpenEXR writer: single part scanline, no compression, FLOAT B,G,R --- */
typedef struct bytebuf { unsigned char *p; size_t n, cap; } bytebuf;
static void bb_put(bytebuf *b, const void *d, size_t n) {
    if (b->n + n > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 1024;
        while (nc < b->n + n) nc *= 2;
        b->p = (unsigned char *)realloc(b->p, nc);
        b->cap = nc;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void bb_u32(bytebuf *b, uint32_t v) {
    unsigned char c[4];
    c[0] = (unsigned char)v; c[1] = (unsigned char)(v >> 8); c[2] = (unsigned char)(v >> 16); c[3] = (unsigned char)(v >> 24);
    bb_put(b, c, 4);
}
static void bb_u64(bytebuf *b, uint64_t v) { bb_u32(b, (uint32_t)v); bb_u32(b, (uint32_t)(v >> 32)); }
static void bb_f32(bytebuf *b, float f) { uint32_t u; memcpy(&u, &f, 4); bb_u32(b, u); }
static void bb_str(bytebuf *b, const char *s) { bb_put(b, s, strlen(s) + 1); }
static void bb_attr(bytebuf *b, const char *name, const char *type, uint32_t size) {
    bb_str(b, name); bb_str(b, type); bb_u32(b, size);
}

int nora_write_exr(const char *path, const float *rgb, int w, int h) {
    bytebuf b = { 0, 0, 0 };
    const char *chans[3] = { "B", "G", "R" };
    const int chan_idx[3] = { 2, 1, 0 };
    int c, y;
    size_t offsets_pos, line_bytes;
    FILE *f;
    bb_u32(&b, 20000630u);   /* magic 0x762f3101 */
    bb_u32(&b, 2u);          /* version 2, single part scanline */
    bb_attr(&b, "channels", "chlist", 3 * (2 + 16) + 1);
    for (c = 0; c < 3; c++) {
        unsigned char z[4] = { 0, 0, 0, 0 };
        bb_str(&b, chans[c]);
        bb_u32(&b, 2u);      /* FLOAT */
        bb_put(&b, z, 4);    /* pLinear + reserved */
        bb_u32(&b, 1u); bb_u32(&b, 1u);
    }
    { unsigned char z = 0; bb_put(&b, &z, 1); }
    bb_attr(&b, "compression", "compression", 1);
    { unsigned char z = 0; bb_put(&b, &z, 1); }
    bb_attr(&b, "dataWindow", "box2i", 16);
    bb_u32(&b, 0); bb_u32(&b, 0); bb_u32(&b, (uint32_t)(w - 1)); bb_u32(&b, (uint32_t)(h - 1));
    bb_attr(&b, "displayWindow", "box2i", 16);
    bb_u32(&b, 0); bb_u32(&b, 0); bb_u32(&b, (uint32_t)(w - 1)); bb_u32(&b, (uint32_t)(h - 1));
    bb_attr(&b, "lineOrder", "lineOrder", 1);
    { unsigned char z = 0; bb_put(&b, &z, 1); }
    bb_attr(&b, "pixelAspectRatio", "float", 4);
    bb_f32(&b, 1.0f);
    bb_attr(&b, "screenWindowCenter", "v2f", 8);
    bb_f32(&b, 0.0f); bb_f32(&b, 0.0f);
    bb_attr(&b, "screenWindowWidth", "float", 4);
    bb_f32(&b, 1.0f);
    { unsigned char z = 0; bb_put(&b, &z, 1); }   /* end of header */
    offsets_pos = b.n;
    line_bytes = (size_t)w * 3 * 4;
    for (y = 0; y < h; y++) bb_u64(&b, (uint64_t)(offsets_pos + (size_t)h * 8 + (size_t)y * (8 + line_bytes)));
    for (y = 0; y < h; y++) {
        bb_u32(&b, (uint32_t)y);
        bb_u32(&b, (uint32_t)line_bytes);
        for (c = 0; c < 3; c++) {
            int x;
            for (x = 0; x < w; x++) bb_f32(&b, rgb[((size_t)y * w + x) * 3 + chan_idx[c]]);
        }
    }
    f = fopen(path, "wb");
    if (!f) { free(b.p); return -1; }
    fwrite(b.p, 1, b.n, f);
    fclose(f);
    free(b.p);
    return 0;
}

static int ends_with(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf), i;
    if (b > a) return 0;
    for (i = 0; i < b; i++) {
        char c = s[a - b + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != suf[i]) return 0;
    }
    return 1;
}

int nora_write_image(const char *path, const float *rgb, int w, int h, float exposure, nora_tonemap tm) {
    if (ends_with(path, ".pfm")) return nora_write_pfm(path, rgb, w, h);
    if (ends_with(path, ".exr")) return nora_write_exr(path, rgb, w, h);
    return nora_write_png(path, rgb, w, h, exposure, tm);
}

float *nora_load_image_linear(const char *path, int *w, int *h) {
    int n = 0;
    float *out;
    long i, cnt;
    if (stbi_is_hdr(path)) {
        float *d = stbi_loadf(path, w, h, &n, 3);
        return d;   /* stbi allocations use malloc: free() is fine */
    } else {
        unsigned char *d = stbi_load(path, w, h, &n, 3);
        if (!d) return NULL;
        cnt = (long)(*w) * (*h) * 3;
        out = (float *)malloc(sizeof(float) * (size_t)cnt);
        if (out) for (i = 0; i < cnt; i++) out[i] = nora_srgb_to_linear((float)d[i] / 255.0f);
        stbi_image_free(d);
        return out;
    }
}
