/* Nora - compares two PFM images: imgcmp a.pfm b.pfm [tolerance]. Passes (exit 0)
 * when sum|a - b| / sum|b| <= tolerance (default 0.01). MIT License. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static float *read_pfm(const char *path, int *w, int *h) {
    FILE *f = fopen(path, "rb");
    char tag[3] = { 0 };
    float scale, *d;
    size_t n;
    if (!f) return NULL;
    if (fscanf(f, "%2s %d %d %f", tag, w, h, &scale) != 4 || strcmp(tag, "PF") != 0 || *w <= 0 || *h <= 0) { fclose(f); return NULL; }
    fgetc(f);
    n = (size_t)*w * (size_t)*h * 3;
    d = (float *)malloc(sizeof(float) * n);
    if (!d || fread(d, sizeof(float), n, f) != n) { free(d); fclose(f); return NULL; }
    fclose(f);
    return d;
}

int main(int argc, char **argv) {
    int wa, ha, wb, hb;
    float *a, *b;
    double tol = argc > 3 ? atof(argv[3]) : 0.01, d = 0, t = 0, rel;
    size_t i, n;
    if (argc < 3) { fprintf(stderr, "usage: imgcmp a.pfm b.pfm [tolerance]\n"); return 2; }
    a = read_pfm(argv[1], &wa, &ha);
    b = read_pfm(argv[2], &wb, &hb);
    if (!a || !b) { fprintf(stderr, "imgcmp: cannot read the images\n"); return 2; }
    if (wa != wb || ha != hb) { fprintf(stderr, "imgcmp: sizes differ (%dx%d vs %dx%d)\n", wa, ha, wb, hb); return 1; }
    n = (size_t)wa * ha * 3;
    for (i = 0; i < n; i++) { d += fabs((double)a[i] - b[i]); t += fabs((double)b[i]); }
    rel = t > 0 ? d / t : d;
    printf("imgcmp: relative difference %.3g (tolerance %.3g)\n", rel, tol);
    free(a); free(b);
    return rel <= tol ? 0 : 1;
}
