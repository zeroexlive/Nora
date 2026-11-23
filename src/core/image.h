/* Nora - image output (PNG via stb_image_write, PFM, OpenEXR) and input. MIT License. */
#ifndef NORA_IMAGE_H
#define NORA_IMAGE_H

#include <stdint.h>

typedef enum nora_tonemap { NORA_TONEMAP_CLAMP = 0, NORA_TONEMAP_REINHARD, NORA_TONEMAP_ACES } nora_tonemap;

/* Linear RGB float (w*h*3) -> 8-bit sRGB RGBA (w*h*4), with exposure in stops. */
void nora_image_to_srgb8(const float *rgb, int w, int h, float exposure, nora_tonemap tm, uint8_t *rgba);

int nora_write_png(const char *path, const float *rgb, int w, int h, float exposure, nora_tonemap tm);
int nora_write_pfm(const char *path, const float *rgb, int w, int h);
int nora_write_exr(const char *path, const float *rgb, int w, int h);   /* uncompressed, 32-bit float */
/* Picks the format from the extension (.png, .pfm, .exr). Returns 0 on success. */
int nora_write_image(const char *path, const float *rgb, int w, int h, float exposure, nora_tonemap tm);

/* Loads PNG/JPEG/HDR via stb_image into linear RGB floats (8-bit files are sRGB decoded). */
float *nora_load_image_linear(const char *path, int *w, int *h);

float nora_srgb_to_linear(float c);
float nora_linear_to_srgb(float c);

#endif
