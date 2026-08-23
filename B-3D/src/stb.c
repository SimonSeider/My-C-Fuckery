
#include "b3d.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define STBI_MAX_DIMENSIONS 16384
#define STBI_NO_THREAD_LOCALS
#define STB_IMAGE_IMPLEMENTATION

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wcast-qual"
#include "stb_image.h"
#pragma GCC diagnostic pop

static unsigned char *stb_trim_alpha(unsigned char *a, size_t px)
{
    size_t i = 0;
    while (i < px && a[i] == 255)
        i++;
    if (i == px) {
        free(a);
        return NULL;
    }
    return a;
}

static unsigned char *split_planes(unsigned char *src, int w, int h, int n,
                                   unsigned char **alpha)
{
    size_t px = (size_t)w * (size_t)h;
    unsigned char *rgb = malloc(px * 3);
    unsigned char *a = NULL;
    if (!rgb) {
        stbi_image_free(src);
        return NULL;
    }
    if (n == 2 || n == 4) {
        a = malloc(px);
        if (!a) {
            free(rgb);
            stbi_image_free(src);
            return NULL;
        }
    }
    for (size_t i = 0; i < px; i++) {
        const unsigned char *p = src + i * (size_t)n;
        if (n >= 3) {
            rgb[i * 3] = p[0];
            rgb[i * 3 + 1] = p[1];
            rgb[i * 3 + 2] = p[2];
        } else {
            rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = p[0];
        }
        if (a)
            a[i] = p[n - 1];
    }
    stbi_image_free(src);
    if (a)
        a = stb_trim_alpha(a, px);
    if (alpha)
        *alpha = a;
    else
        free(a);
    return rgb;
}

unsigned char *stb_load(FILE *f, int *ow, int *oh, unsigned char **alpha)
{
    if (alpha)
        *alpha = NULL;
    int w = 0, h = 0, n = 0;
    unsigned char *src = stbi_load_from_file(f, &w, &h, &n, 0);
    if (!src)
        return NULL;
    if (w <= 0 || h <= 0 || n < 1 || n > 4) {
        stbi_image_free(src);
        return NULL;
    }
    *ow = w;
    *oh = h;
    return split_planes(src, w, h, n, alpha);
}

unsigned char *stb_load_hdr(FILE *f, int *ow, int *oh, unsigned char **alpha)
{
    if (alpha)
        *alpha = NULL;
    int w = 0, h = 0, n = 0;
    float *src = stbi_loadf_from_file(f, &w, &h, &n, 3);
    if (!src)
        return NULL;
    if (w <= 0 || h <= 0) {
        stbi_image_free(src);
        return NULL;
    }
    size_t px = (size_t)w * (size_t)h;
    unsigned char *rgb = malloc(px * 3);
    if (!rgb) {
        stbi_image_free(src);
        return NULL;
    }
    for (size_t i = 0; i < px * 3; i++) {
        double v = src[i];
        if (!(v > 0.0))
            v = 0.0;
        v = v / (1.0 + v);
        v = pow(v, 1.0 / 2.2);
        int c = (int)(v * 255.0 + 0.5);
        rgb[i] = (unsigned char)(c < 0 ? 0 : c > 255 ? 255 : c);
    }
    stbi_image_free(src);
    *ow = w;
    *oh = h;
    return rgb;
}
