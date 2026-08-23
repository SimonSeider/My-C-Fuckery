#include "b3d.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int rd32be(const unsigned char *p)
{
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
           ((unsigned int)p[2] << 8) | (unsigned int)p[3];
}

static unsigned char *fslurp(FILE *f, size_t *n)
{
    if (fseek(f, 0, SEEK_END) != 0)
        return NULL;
    long sz = ftell(f);
    if (sz < 0)
        return NULL;
    rewind(f);
    unsigned char *b = malloc((size_t)sz + 1);
    if (!b)
        return NULL;
    size_t got = fread(b, 1, (size_t)sz, f);
    b[got] = 0;
    *n = got;
    return b;
}

static int dims_ok(long w, long h)
{
    return w > 0 && h > 0 && w <= 16384 && h <= 16384;
}

static unsigned char *trim_alpha(unsigned char *a, size_t px)
{
    if (!a)
        return NULL;
    size_t i = 0;
    while (i < px && a[i] == 255)
        i++;
    if (i == px)
    {
        free(a);
        return NULL;
    }
    return a;
}

static int ppm_int(FILE *f)
{
    int c;
    for (;;)
    {
        c = fgetc(f);
        if (c == '#')
        {
            do
            {
                c = fgetc(f);
            } while (c != EOF && c != '\n');
        }
        else if (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
                 c == '\v' || c == '\f')
        {
            continue;
        }
        else
        {
            break;
        }
    }
    if (c < '0' || c > '9')
        return -1;
    int v = 0;
    while (c >= '0' && c <= '9')
    {
        v = v * 10 + (c - '0');
        if (v > (1 << 28))
            return -1;
        c = fgetc(f);
    }
    return v;
}

static int pbm_bit(FILE *f)
{
    int c;
    for (;;)
    {
        c = fgetc(f);
        if (c == EOF)
            return -1;
        if (c == '#')
        {
            do
            {
                c = fgetc(f);
            } while (c != EOF && c != '\n');
            continue;
        }
        if (c == '0' || c == '1')
            return c - '0';
    }
}

static unsigned char *load_pam(FILE *f, int *ow, int *oh, unsigned char **alpha)
{
    int w = 0, h = 0, depth = 0, maxv = 255;
    char line[256];
    int seen_end = 0;
    while (fgets(line, sizeof line, f))
    {
        if (line[0] == '#')
            continue;
        if (!strncmp(line, "ENDHDR", 6))
        {
            seen_end = 1;
            break;
        }
        sscanf(line, "WIDTH %d", &w);
        sscanf(line, "HEIGHT %d", &h);
        sscanf(line, "DEPTH %d", &depth);
        sscanf(line, "MAXVAL %d", &maxv);
    }
    if (!seen_end || !dims_ok(w, h) || depth < 1 || depth > 4 || maxv < 1 || maxv > 65535)
        return NULL;
    size_t px = (size_t)w * h;
    int bytes = maxv > 255 ? 2 : 1;
    unsigned char *rgb = malloc(px * 3);
    unsigned char *a = (depth == 2 || depth == 4) ? malloc(px) : NULL;
    unsigned char *row = malloc((size_t)depth * bytes);
    if (!rgb || !row || ((depth == 2 || depth == 4) && !a))
    {
        free(rgb);
        free(a);
        free(row);
        return NULL;
    }
    for (size_t i = 0; i < px; i++)
    {
        if (fread(row, 1, (size_t)depth * bytes, f) != (size_t)depth * bytes)
        {
            free(rgb);
            free(a);
            free(row);
            return NULL;
        }
        int s[4];
        for (int k = 0; k < depth; k++)
        {
            int v = bytes == 2 ? ((row[k * 2] << 8) | row[k * 2 + 1]) : row[k];
            s[k] = v * 255 / maxv;
        }
        if (depth <= 2)
        {
            rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = (unsigned char)s[0];
            if (a)
                a[i] = (unsigned char)s[1];
        }
        else
        {
            rgb[i * 3] = (unsigned char)s[0];
            rgb[i * 3 + 1] = (unsigned char)s[1];
            rgb[i * 3 + 2] = (unsigned char)s[2];
            if (a)
                a[i] = (unsigned char)s[3];
        }
    }
    free(row);
    *ow = w;
    *oh = h;
    if (alpha)
        *alpha = trim_alpha(a, px);
    else
        free(a);
    return rgb;
}

static unsigned char *pnm_load(FILE *f, int *ow, int *oh, unsigned char **alpha)
{
    if (alpha)
        *alpha = NULL;
    int p0 = fgetc(f), p1 = fgetc(f);
    if (p0 != 'P' || p1 < '1' || p1 > '7')
        return NULL;
    int kind = p1 - '0';
    if (kind == 7)
        return load_pam(f, ow, oh, alpha);

    int w = ppm_int(f), h = ppm_int(f);
    int maxv = (kind == 1 || kind == 4) ? 1 : ppm_int(f);
    if (!dims_ok(w, h) || maxv < 1 || maxv > 65535)
        return NULL;
    int chan = (kind == 3 || kind == 6) ? 3 : 1;
    int binary = kind >= 4;
    size_t px = (size_t)w * h;
    unsigned char *rgb = malloc(px * 3);
    if (!rgb)
        return NULL;

    if (kind == 4)
    {
        size_t rowb = (size_t)(w + 7) / 8;
        unsigned char *row = malloc(rowb);
        if (!row)
        {
            free(rgb);
            return NULL;
        }
        for (int y = 0; y < h; y++)
        {
            if (fread(row, 1, rowb, f) != rowb)
            {
                free(row);
                free(rgb);
                return NULL;
            }
            for (int x = 0; x < w; x++)
            {
                unsigned char v = (row[x >> 3] >> (7 - (x & 7))) & 1 ? 0 : 255;
                unsigned char *d = rgb + ((size_t)y * w + x) * 3;
                d[0] = d[1] = d[2] = v;
            }
        }
        free(row);
    }
    else if (kind == 1)
    {
        for (size_t i = 0; i < px; i++)
        {
            int b = pbm_bit(f);
            if (b < 0)
            {
                free(rgb);
                return NULL;
            }
            unsigned char v = b ? 0 : 255;
            rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = v;
        }
    }
    else
    {
        size_t n = px * (size_t)chan;
        int bytes = (binary && maxv > 255) ? 2 : 1;
        for (size_t i = 0; i < n; i++)
        {
            int v;
            if (binary)
            {
                int b0 = fgetc(f);
                if (b0 < 0)
                {
                    free(rgb);
                    return NULL;
                }
                v = bytes == 2 ? ((b0 << 8) | (fgetc(f) & 0xFF)) : b0;
            }
            else
            {
                v = ppm_int(f);
                if (v < 0)
                {
                    free(rgb);
                    return NULL;
                }
            }
            if (v > maxv)
                v = maxv;
            unsigned char s = (unsigned char)(v * 255 / maxv);
            if (chan == 3)
            {
                rgb[i] = s;
            }
            else
            {
                rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = s;
            }
        }
    }
    *ow = w;
    *oh = h;
    return rgb;
}

unsigned char *qoi_load(FILE *f, int *ow, int *oh, unsigned char **alpha)
{
    if (alpha)
        *alpha = NULL;
    size_t n = 0;
    unsigned char *b = fslurp(f, &n);
    if (!b || n < 14 || memcmp(b, "qoif", 4))
    {
        free(b);
        return NULL;
    }
    long w = rd32be(b + 4), h = rd32be(b + 8);
    int chan = b[12];
    if (!dims_ok(w, h) || (chan != 3 && chan != 4))
    {
        free(b);
        return NULL;
    }
    size_t px = (size_t)w * h;
    unsigned char *rgb = malloc(px * 3);
    unsigned char *a = chan == 4 ? malloc(px) : NULL;
    if (!rgb || (chan == 4 && !a))
    {
        free(rgb);
        free(a);
        free(b);
        return NULL;
    }

    unsigned char idx[64][4];
    memset(idx, 0, sizeof idx);
    unsigned char c[4] = {0, 0, 0, 255};
    size_t p = 14, o = 0;
    while (o < px && p < n)
    {
        int op = b[p++];
        if (op == 0xFE && p + 3 <= n)
        {
            c[0] = b[p];
            c[1] = b[p + 1];
            c[2] = b[p + 2];
            p += 3;
        }
        else if (op == 0xFF && p + 4 <= n)
        {
            memcpy(c, b + p, 4);
            p += 4;
        }
        else if ((op & 0xC0) == 0x00)
        {
            memcpy(c, idx[op & 63], 4);
        }
        else if ((op & 0xC0) == 0x40)
        {
            c[0] += (unsigned char)(((op >> 4) & 3) - 2);
            c[1] += (unsigned char)(((op >> 2) & 3) - 2);
            c[2] += (unsigned char)((op & 3) - 2);
        }
        else if ((op & 0xC0) == 0x80)
        {
            if (p >= n)
                break;
            int b2 = b[p++];
            int dg = (op & 63) - 32;
            c[0] += (unsigned char)(dg - 8 + ((b2 >> 4) & 15));
            c[1] += (unsigned char)dg;
            c[2] += (unsigned char)(dg - 8 + (b2 & 15));
        }
        else
        {
            int run = (op & 63) + 1;
            for (int i = 0; i < run && o < px; i++, o++)
            {
                memcpy(rgb + o * 3, c, 3);
                if (a)
                    a[o] = c[3];
            }
            continue;
        }
        memcpy(idx[(c[0] * 3 + c[1] * 5 + c[2] * 7 + c[3] * 11) & 63], c, 4);
        memcpy(rgb + o * 3, c, 3);
        if (a)
            a[o] = c[3];
        o++;
    }
    free(b);
    *ow = (int)w;
    *oh = (int)h;
    if (alpha)
        *alpha = trim_alpha(a, px);
    else
        free(a);
    return rgb;
}

static unsigned char *ff_load(FILE *f, int *ow, int *oh, unsigned char **alpha)
{
    if (alpha)
        *alpha = NULL;
    unsigned char hd[16];
    if (fread(hd, 1, 16, f) != 16 || memcmp(hd, "farbfeld", 8))
        return NULL;
    long w = rd32be(hd + 8), h = rd32be(hd + 12);
    if (!dims_ok(w, h))
        return NULL;
    size_t px = (size_t)w * h;
    unsigned char *rgb = malloc(px * 3);
    unsigned char *a = malloc(px);
    if (!rgb || !a)
    {
        free(rgb);
        free(a);
        return NULL;
    }
    unsigned char p[8];
    for (size_t i = 0; i < px; i++)
    {
        if (fread(p, 1, 8, f) != 8)
        {
            free(rgb);
            free(a);
            return NULL;
        }
        rgb[i * 3] = p[0];
        rgb[i * 3 + 1] = p[2];
        rgb[i * 3 + 2] = p[4];
        a[i] = p[6];
    }
    *ow = (int)w;
    *oh = (int)h;
    if (alpha)
        *alpha = trim_alpha(a, px);
    else
        free(a);
    return rgb;
}

static unsigned char *pnm_any(FILE *f, int *ow, int *oh, unsigned char **alpha)
{
    unsigned char *rgb = stb_load(f, ow, oh, alpha);
    if (rgb)
        return rgb;
    rewind(f);
    return pnm_load(f, ow, oh, alpha);
}

const struct b3d_imgfmt B3D_IMGFMTS[] = {
    {"PNG", "png", {137, 'P', 'N', 'G'}, 4, stb_load},
    {"JPEG", "jpg", {0xFF, 0xD8, 0xFF}, 3, stb_load},
    {"GIF", "gif", {'G', 'I', 'F', '8'}, 4, stb_load},
    {"BMP", "bmp", {'B', 'M'}, 2, stb_load},
    {"PSD", "psd", {'8', 'B', 'P', 'S'}, 4, stb_load},
    {"Softimage", "pic", {0x53, 0x80, 0xF6, 0x34}, 4, stb_load},
    {"DDS", "dds", {'D', 'D', 'S', ' '}, 4, dds_load},
    {"QOI", "qoi", {'q', 'o', 'i', 'f'}, 4, qoi_load},
    {"Radiance", "hdr", {'#', '?'}, 2, stb_load_hdr},
    {"farbfeld", "ff", {'f', 'a', 'r', 'b', 'f', 'e', 'l', 'd'}, 8, ff_load},
    {"Netpbm", "ppm", {'P'}, 1, pnm_any},
    {"Targa", "tga", {0}, 0, stb_load},
};
const int B3D_NIMGFMTS = (int)(sizeof B3D_IMGFMTS / sizeof *B3D_IMGFMTS);

static struct texture *tex_store(const char *key, unsigned char *rgb,
                                 unsigned char *a, int w, int h)
{
    if (g.ntexs >= g.captexs)
    {
        g.captexs = g.captexs ? g.captexs * 2 : 4;
        g.texs = xrealloc(g.texs, (size_t)g.captexs * sizeof *g.texs);
    }
    struct texture *t = calloc(1, sizeof *t);
    if (!t)
        die("out of memory");
    snprintf(t->path, sizeof t->path, "%s", key);
    t->w = w;
    t->h = h;
    t->rgb = rgb;
    t->alpha = a;
    double sum = 0.0;
    size_t n = (size_t)w * h;
    for (size_t i = 0; i < n; i++)
        sum += 0.2126 * rgb[i * 3] + 0.7152 * rgb[i * 3 + 1] + 0.0722 * rgb[i * 3 + 2];
    t->avg_lum = n ? sum / ((double)n * 255.0) : 1.0;
    g.texs[g.ntexs++] = t;
    return t;
}

static unsigned char *decode_stream(FILE *f, const char *what, int *w, int *h,
                                    unsigned char **a)
{
    unsigned char magic[8] = {0};
    size_t got = fread(magic, 1, sizeof magic, f);
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < B3D_NIMGFMTS; i++)
        {
            const struct b3d_imgfmt *fm = &B3D_IMGFMTS[i];
            if (pass == 0)
            {
                if (!fm->nmagic || got < (size_t)fm->nmagic)
                    continue;
                if (memcmp(magic, fm->magic, (size_t)fm->nmagic))
                    continue;
            }
            else if (fm->nmagic)
            {
                continue;
            }
            rewind(f);
            unsigned char *rgb = fm->load(f, w, h, a);
            if (rgb)
                return rgb;
            if (pass == 0)
                fprintf(stderr, "B-3D: '%s' looks like %s but could not be decoded\n",
                        what, fm->name);
            return NULL;
        }
    return NULL;
}

struct texture *tex_cache_get(const char *path)
{
    for (int i = 0; i < g.ntexs; i++)
        if (!strcmp(g.texs[i]->path, path))
            return g.texs[i];
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        fprintf(stderr, "B-3D: cannot read texture '%s'\n", path);
        return NULL;
    }
    int w = 0, h = 0;
    unsigned char *a = NULL;
    unsigned char *rgb = decode_stream(f, path, &w, &h, &a);
    fclose(f);
    if (!rgb)
    {
        fprintf(stderr, "B-3D: unsupported texture '%s' (see --list)\n", path);
        return NULL;
    }
    return tex_store(path, rgb, a, w, h);
}

struct texture *tex_from_memory(const char *name, const unsigned char *buf, size_t n)
{
    for (int i = 0; i < g.ntexs; i++)
        if (!strcmp(g.texs[i]->path, name))
            return g.texs[i];
    FILE *f = fmemopen((void *)buf, n, "rb");
    if (!f)
        return NULL;
    int w = 0, h = 0;
    unsigned char *a = NULL;
    unsigned char *rgb = decode_stream(f, name, &w, &h, &a);
    fclose(f);
    if (!rgb)
    {
        fprintf(stderr, "B-3D: unsupported embedded texture '%s'\n", name);
        return NULL;
    }
    return tex_store(name, rgb, a, w, h);
}

static void texel(const struct texture *t, int x, int y, double c[3])
{
    const unsigned char *p = t->rgb + ((size_t)y * t->w + x) * 3;
    c[0] = p[0] / 255.0;
    c[1] = p[1] / 255.0;
    c[2] = p[2] / 255.0;
}

void tex_sample(const struct texture *t, double u, double v, double c[3])
{
    u -= floor(u);
    v -= floor(v);
    if (g.opt.filter == TEX_LINEAR && t->w > 1 && t->h > 1)
    {
        double fx = u * t->w - 0.5, fy = v * t->h - 0.5;
        int x0 = (int)floor(fx), y0 = (int)floor(fy);
        double tx = fx - x0, ty = fy - y0;
        int x1 = (x0 + 1) % t->w, y1 = (y0 + 1) % t->h;
        x0 = ((x0 % t->w) + t->w) % t->w;
        y0 = ((y0 % t->h) + t->h) % t->h;
        x1 = ((x1 % t->w) + t->w) % t->w;
        y1 = ((y1 % t->h) + t->h) % t->h;
        double a[3], b[3], d[3], e[3];
        texel(t, x0, y0, a);
        texel(t, x1, y0, b);
        texel(t, x0, y1, d);
        texel(t, x1, y1, e);
        for (int k = 0; k < 3; k++)
            c[k] = (a[k] * (1 - tx) + b[k] * tx) * (1 - ty) +
                   (d[k] * (1 - tx) + e[k] * tx) * ty;
        return;
    }
    int x = (int)(u * t->w);
    int y = (int)(v * t->h);
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x >= t->w)
        x = t->w - 1;
    if (y >= t->h)
        y = t->h - 1;
    texel(t, x, y, c);
}

double tex_alpha(const struct texture *t, double u, double v)
{
    if (!t || !t->alpha)
        return 1.0;
    u -= floor(u);
    v -= floor(v);
    int x = (int)(u * t->w);
    int y = (int)(v * t->h);
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x >= t->w)
        x = t->w - 1;
    if (y >= t->h)
        y = t->h - 1;
    return t->alpha[(size_t)y * t->w + x] / 255.0;
}
