#include "b3d.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FCC(a, b, c, d) ((unsigned)(a) | ((unsigned)(b) << 8) | \
                         ((unsigned)(c) << 16) | ((unsigned)(d) << 24))

#define DDPF_ALPHAPIXELS 0x00000001u
#define DDPF_ALPHA 0x00000002u
#define DDPF_FOURCC 0x00000004u
#define DDPF_RGB 0x00000040u
#define DDPF_LUMINANCE 0x00020000u

#define DDSD_PITCH 0x00000008u

enum
{
    FMT_BAD,
    FMT_BC1,
    FMT_BC2,
    FMT_BC3,
    FMT_BC4,
    FMT_BC5,
    FMT_BC7,
    FMT_RAW
};
enum
{
    RAW_MASK,
    RAW_U16,
    RAW_F16,
    RAW_F32
};

struct desc
{
    int fmt;
    int sign;
    int raw;
    int bytes;
    int nch;
    int gray;
    unsigned mr, mg, mb, ma;
    const char *name;
};

static unsigned rd32(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static const unsigned char BC7_P2[64][16] = {
    {0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1},
    {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1},
    {0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1},
    {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 1},
    {0, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1},
    {0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1},
    {0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1},
    {0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1},
    {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1},
    {0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 1},
    {0, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0},
    {0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0},
    {0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0},
    {0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1},
    {0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0},
    {0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0},
    {0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0},
    {0, 0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 0, 0},
    {0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0},
    {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0},
    {0, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0},
    {0, 0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0},
    {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1},
    {0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1},
    {0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0},
    {0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0},
    {0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0},
    {0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0},
    {0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1},
    {0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1},
    {0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1, 0},
    {0, 0, 0, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 0, 0, 0},
    {0, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 0, 0},
    {0, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 0, 0},
    {0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0},
    {0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1},
    {0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1},
    {0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0},
    {0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0},
    {0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0},
    {0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0},
    {0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1},
    {0, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1},
    {0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0},
    {0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1, 0},
    {0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 1},
    {0, 1, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1},
    {0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 1},
    {0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1},
    {0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1},
    {0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0},
    {0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0},
    {0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1},
};
static const unsigned char BC7_P3[64][16] = {
    {0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 1, 2, 2, 2, 2},
    {0, 0, 0, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 2, 1},
    {0, 0, 0, 0, 2, 0, 0, 1, 2, 2, 1, 1, 2, 2, 1, 1},
    {0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 1, 0, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2},
    {0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 2, 2},
    {0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1},
    {0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2},
    {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2},
    {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2},
    {0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2},
    {0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2},
    {0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2},
    {0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2, 1, 2, 2, 2},
    {0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0, 2, 2, 2, 0},
    {0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2},
    {0, 1, 1, 1, 0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0},
    {0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2},
    {0, 0, 2, 2, 0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1},
    {0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2, 0, 2, 2, 2},
    {0, 0, 0, 1, 0, 0, 0, 1, 2, 2, 2, 1, 2, 2, 2, 1},
    {0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2},
    {0, 0, 0, 0, 1, 1, 0, 0, 2, 2, 1, 0, 2, 2, 1, 0},
    {0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1, 0, 0, 0, 0},
    {0, 0, 1, 2, 0, 0, 1, 2, 1, 1, 2, 2, 2, 2, 2, 2},
    {0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1, 0, 1, 1, 0},
    {0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1},
    {0, 0, 2, 2, 1, 1, 0, 2, 1, 1, 0, 2, 0, 0, 2, 2},
    {0, 1, 1, 0, 0, 1, 1, 0, 2, 0, 0, 2, 2, 2, 2, 2},
    {0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1},
    {0, 0, 0, 0, 2, 0, 0, 0, 2, 2, 1, 1, 2, 2, 2, 1},
    {0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 2, 2, 2},
    {0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 2, 0, 0, 1, 1},
    {0, 0, 1, 1, 0, 0, 1, 2, 0, 0, 2, 2, 0, 2, 2, 2},
    {0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0},
    {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0},
    {0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0},
    {0, 1, 2, 0, 2, 0, 1, 2, 1, 2, 0, 1, 0, 1, 2, 0},
    {0, 0, 1, 1, 2, 2, 0, 0, 1, 1, 2, 2, 0, 0, 1, 1},
    {0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0, 1, 1},
    {0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2},
    {0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1},
    {0, 0, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2, 1, 1, 2, 2},
    {0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 1, 1},
    {0, 2, 2, 0, 1, 2, 2, 1, 0, 2, 2, 0, 1, 2, 2, 1},
    {0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 0, 1, 0, 1},
    {0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1},
    {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2},
    {0, 2, 2, 2, 0, 1, 1, 1, 0, 2, 2, 2, 0, 1, 1, 1},
    {0, 0, 0, 2, 1, 1, 1, 2, 0, 0, 0, 2, 1, 1, 1, 2},
    {0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2},
    {0, 2, 2, 2, 0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2},
    {0, 0, 0, 2, 1, 1, 1, 2, 1, 1, 1, 2, 0, 0, 0, 2},
    {0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2},
    {0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2},
    {0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2, 2, 2, 2, 2},
    {0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2},
    {0, 0, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2},
    {0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 1},
    {0, 2, 2, 2, 1, 2, 2, 2, 0, 2, 2, 2, 1, 2, 2, 2},
    {0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2},
    {0, 1, 1, 1, 2, 0, 1, 1, 2, 2, 0, 1, 2, 2, 2, 0},
};
static const unsigned char BC7_A2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2,
    8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2, 15, 15, 6, 8, 2, 8, 15, 15,
    2, 8, 2, 2, 2, 15, 15, 6, 6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15,
    2, 2, 15};
static const unsigned char BC7_A3A[64] = {
    3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3, 3, 3, 8, 15, 3, 3,
    6, 10, 5, 8, 8, 6, 8, 5, 15, 15, 8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15,
    5, 15, 15, 15, 15, 3, 15, 5, 5, 5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3, 3};
static const unsigned char BC7_A3B[64] = {
    15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8, 15, 8, 15, 3,
    15, 8, 15, 8, 3, 15, 6, 10, 15, 15, 10, 8, 15, 3, 15, 10, 10, 8, 9, 10,
    6, 15, 8, 15, 3, 6, 6, 8, 15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 3, 15, 15, 8};

static const unsigned char BC_W2[4] = {0, 21, 43, 64};
static const unsigned char BC_W3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
static const unsigned char BC_W4[16] = {0, 4, 9, 13, 17, 21, 26, 30,
                                        34, 38, 43, 47, 51, 55, 60, 64};

static const struct
{
    unsigned char ns, pb, rb, isb, cb, ab, epb, spb, ib, ib2;
} BC7_MODE[8] = {
    {3, 4, 0, 0, 4, 0, 1, 0, 3, 0},
    {2, 6, 0, 0, 6, 0, 0, 1, 3, 0},
    {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
    {2, 6, 0, 0, 7, 0, 1, 0, 2, 0},
    {1, 0, 2, 1, 5, 6, 0, 0, 2, 3},
    {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
    {1, 0, 0, 0, 7, 7, 1, 0, 4, 0},
    {2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
};

static int lerp8(int a, int b, int w) { return ((64 - w) * a + w * b + 32) >> 6; }

static void expand565(unsigned c, unsigned char *o)
{
    int r = (int)(c >> 11) & 31, g = (int)(c >> 5) & 63, b = (int)c & 31;
    o[0] = (unsigned char)((r << 3) | (r >> 2));
    o[1] = (unsigned char)((g << 2) | (g >> 4));
    o[2] = (unsigned char)((b << 3) | (b >> 2));
}

static void bc1_block(const unsigned char *b, unsigned char *out, int opaque)
{
    unsigned c0 = (unsigned)b[0] | ((unsigned)b[1] << 8);
    unsigned c1 = (unsigned)b[2] | ((unsigned)b[3] << 8);
    unsigned char c[4][4];
    expand565(c0, c[0]);
    expand565(c1, c[1]);
    c[0][3] = c[1][3] = c[2][3] = 255;
    if (c0 > c1 || opaque)
    {
        for (int k = 0; k < 3; k++)
        {
            c[2][k] = (unsigned char)((2 * c[0][k] + c[1][k] + 1) / 3);
            c[3][k] = (unsigned char)((c[0][k] + 2 * c[1][k] + 1) / 3);
        }
        c[3][3] = 255;
    }
    else
    {
        for (int k = 0; k < 3; k++)
        {
            c[2][k] = (unsigned char)((c[0][k] + c[1][k]) / 2);
            c[3][k] = 0;
        }
        c[3][3] = 0;
    }
    unsigned bits = rd32(b + 4);
    for (int i = 0; i < 16; i++)
        memcpy(out + i * 4, c[(bits >> (2 * i)) & 3], 4);
}

static void bc4_chan(const unsigned char *b, int sign, unsigned char *dst, int stride)
{
    int a[8];
    if (sign)
    {
        int e0 = (signed char)b[0], e1 = (signed char)b[1];
        if (e0 == -128)
            e0 = -127;
        if (e1 == -128)
            e1 = -127;
        a[0] = e0;
        a[1] = e1;
        if (e0 > e1)
        {
            for (int i = 0; i < 6; i++)
                a[2 + i] = ((6 - i) * e0 + (1 + i) * e1) / 7;
        }
        else
        {
            for (int i = 0; i < 4; i++)
                a[2 + i] = ((4 - i) * e0 + (1 + i) * e1) / 5;
            a[6] = -127;
            a[7] = 127;
        }
        for (int i = 0; i < 8; i++)
            a[i] += 128;
    }
    else
    {
        a[0] = b[0];
        a[1] = b[1];
        if (a[0] > a[1])
        {
            for (int i = 0; i < 6; i++)
                a[2 + i] = ((6 - i) * a[0] + (1 + i) * a[1] + 3) / 7;
        }
        else
        {
            for (int i = 0; i < 4; i++)
                a[2 + i] = ((4 - i) * a[0] + (1 + i) * a[1] + 2) / 5;
            a[6] = 0;
            a[7] = 255;
        }
    }
    unsigned long long bits = 0;
    for (int i = 0; i < 6; i++)
        bits |= (unsigned long long)b[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++)
    {
        int v = a[(bits >> (3 * i)) & 7];
        dst[i * stride] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255
                                                              : v);
    }
}

static void bc2_block(const unsigned char *b, unsigned char *out)
{
    bc1_block(b + 8, out, 1);
    for (int i = 0; i < 16; i++)
    {
        int v = (b[i / 2] >> ((i & 1) * 4)) & 15;
        out[i * 4 + 3] = (unsigned char)(v * 17);
    }
}

static void bc3_block(const unsigned char *b, unsigned char *out)
{
    bc1_block(b + 8, out, 1);
    bc4_chan(b, 0, out + 3, 4);
}

static void bc4_block(const unsigned char *b, unsigned char *out, int sign)
{
    bc4_chan(b, sign, out, 4);
    for (int i = 0; i < 16; i++)
    {
        out[i * 4 + 1] = out[i * 4 + 2] = out[i * 4];
        out[i * 4 + 3] = 255;
    }
}

static void bc5_block(const unsigned char *b, unsigned char *out, int sign)
{
    bc4_chan(b, sign, out, 4);
    bc4_chan(b + 8, sign, out + 1, 4);
    for (int i = 0; i < 16; i++)
    {
        out[i * 4 + 2] = 0;
        out[i * 4 + 3] = 255;
    }
}

struct bits
{
    const unsigned char *p;
    int pos;
};

static unsigned bits_get(struct bits *r, int n)
{
    unsigned v = 0;
    for (int i = 0; i < n; i++, r->pos++)
        v |= (unsigned)((r->p[r->pos >> 3] >> (r->pos & 7)) & 1) << i;
    return v;
}

static int unquant(int v, int bits)
{
    return (v << (8 - bits)) | (v >> (2 * bits - 8));
}

static void bc7_block(const unsigned char *blk, unsigned char *out)
{
    struct bits r = {blk, 0};
    int mode = 0;
    while (mode < 8 && bits_get(&r, 1) == 0)
        mode++;
    if (mode == 8)
    {
        memset(out, 0, 64);
        for (int i = 0; i < 16; i++)
            out[i * 4 + 3] = 255;
        return;
    }
    int ns = BC7_MODE[mode].ns, cb = BC7_MODE[mode].cb, ab = BC7_MODE[mode].ab;
    int ib = BC7_MODE[mode].ib, ib2 = BC7_MODE[mode].ib2;
    int rot = BC7_MODE[mode].rb ? (int)bits_get(&r, BC7_MODE[mode].rb) : 0;
    int isb = BC7_MODE[mode].isb ? (int)bits_get(&r, BC7_MODE[mode].isb) : 0;
    int part = BC7_MODE[mode].pb ? (int)bits_get(&r, BC7_MODE[mode].pb) : 0;
    int ne = ns * 2, ep[6][4], i, c, s;

    for (c = 0; c < 3; c++)
        for (i = 0; i < ne; i++)
            ep[i][c] = (int)bits_get(&r, cb);
    for (i = 0; i < ne; i++)
        ep[i][3] = ab ? (int)bits_get(&r, ab) : 255;

    if (BC7_MODE[mode].epb)
    {
        for (i = 0; i < ne; i++)
        {
            int pb = (int)bits_get(&r, 1);
            for (c = 0; c < 3; c++)
                ep[i][c] = (ep[i][c] << 1) | pb;
            if (ab)
                ep[i][3] = (ep[i][3] << 1) | pb;
        }
        cb++;
        if (ab)
            ab++;
    }
    else if (BC7_MODE[mode].spb)
    {
        for (s = 0; s < ns; s++)
        {
            int pb = (int)bits_get(&r, 1);
            for (int k = 0; k < 2; k++)
            {
                i = s * 2 + k;
                for (c = 0; c < 3; c++)
                    ep[i][c] = (ep[i][c] << 1) | pb;
                if (ab)
                    ep[i][3] = (ep[i][3] << 1) | pb;
            }
        }
        cb++;
        if (ab)
            ab++;
    }
    for (i = 0; i < ne; i++)
    {
        for (c = 0; c < 3; c++)
            ep[i][c] = unquant(ep[i][c], cb);
        if (ab)
            ep[i][3] = unquant(ep[i][3], ab);
    }

    const unsigned char *pt = ns == 2 ? BC7_P2[part] : ns == 3 ? BC7_P3[part]
                                                               : NULL;
    int anchor[3] = {0, 0, 0};
    if (ns == 2)
    {
        anchor[1] = BC7_A2[part];
    }
    else if (ns == 3)
    {
        anchor[1] = BC7_A3A[part];
        anchor[2] = BC7_A3B[part];
    }

    int idx[16], idx2[16] = {0};
    for (i = 0; i < 16; i++)
    {
        s = pt ? pt[i] : 0;
        idx[i] = (int)bits_get(&r, ib - (i == anchor[s]));
    }
    for (i = 0; i < 16 && ib2; i++)
        idx2[i] = (int)bits_get(&r, ib2 - (i == 0));

    const unsigned char *w1 = ib == 2 ? BC_W2 : ib == 3 ? BC_W3
                                                        : BC_W4;
    const unsigned char *w2 = ib2 == 2 ? BC_W2 : ib2 == 3 ? BC_W3
                                                          : BC_W4;
    const unsigned char *cw = !ib2 ? w1 : isb ? w2
                                              : w1;
    const unsigned char *aw = !ib2 ? w1 : isb ? w1
                                              : w2;
    const int *ci_src = (ib2 && isb) ? idx2 : idx;
    const int *ai_src = !ib2 ? idx : isb ? idx
                                         : idx2;

    for (i = 0; i < 16; i++)
    {
        int ci = ci_src[i], ai = ai_src[i];
        s = pt ? pt[i] : 0;
        unsigned char px[4];
        for (c = 0; c < 3; c++)
            px[c] = (unsigned char)lerp8(ep[s * 2][c], ep[s * 2 + 1][c], cw[ci]);
        px[3] = BC7_MODE[mode].ab
                    ? (unsigned char)lerp8(ep[s * 2][3], ep[s * 2 + 1][3], aw[ai])
                    : 255;
        if (rot)
        {
            unsigned char t = px[3];
            px[3] = px[rot - 1];
            px[rot - 1] = t;
        }
        memcpy(out + i * 4, px, 4);
    }
}

static void as_mask(struct desc *d, int bytes, unsigned r, unsigned g,
                    unsigned b, unsigned a)
{
    d->fmt = FMT_RAW;
    d->raw = RAW_MASK;
    d->bytes = bytes;
    d->mr = r;
    d->mg = g;
    d->mb = b;
    d->ma = a;
}

static void as_chan(struct desc *d, int kind, int nch, int bytes)
{
    d->fmt = FMT_RAW;
    d->raw = kind;
    d->nch = nch;
    d->bytes = bytes * nch;
    d->gray = nch == 1;
}

static void as_block(struct desc *d, int fmt, int sign, const char *name)
{
    d->fmt = fmt;
    d->sign = sign;
    d->name = name;
}

static int desc_dxgi(unsigned f, struct desc *d)
{
    switch (f)
    {
    case 70:
    case 71:
    case 72:
        as_block(d, FMT_BC1, 0, "BC1");
        return 1;
    case 73:
    case 74:
    case 75:
        as_block(d, FMT_BC2, 0, "BC2");
        return 1;
    case 76:
    case 77:
    case 78:
        as_block(d, FMT_BC3, 0, "BC3");
        return 1;
    case 79:
    case 80:
        as_block(d, FMT_BC4, 0, "BC4");
        return 1;
    case 81:
        as_block(d, FMT_BC4, 1, "BC4");
        return 1;
    case 82:
    case 83:
        as_block(d, FMT_BC5, 0, "BC5");
        return 1;
    case 84:
        as_block(d, FMT_BC5, 1, "BC5");
        return 1;
    case 97:
    case 98:
    case 99:
        as_block(d, FMT_BC7, 0, "BC7");
        return 1;
    case 2:
        as_chan(d, RAW_F32, 4, 4);
        return 1;
    case 6:
        as_chan(d, RAW_F32, 3, 4);
        return 1;
    case 10:
        as_chan(d, RAW_F16, 4, 2);
        return 1;
    case 11:
        as_chan(d, RAW_U16, 4, 2);
        return 1;
    case 34:
        as_chan(d, RAW_F16, 2, 2);
        return 1;
    case 35:
        as_chan(d, RAW_U16, 2, 2);
        return 1;
    case 41:
        as_chan(d, RAW_F32, 1, 4);
        return 1;
    case 54:
        as_chan(d, RAW_F16, 1, 2);
        return 1;
    case 56:
        as_chan(d, RAW_U16, 1, 2);
        return 1;
    case 24:
        as_mask(d, 4, 0x3FFu, 0xFFC00u, 0x3FF00000u, 0xC0000000u);
        return 1;
    case 27:
    case 28:
    case 29:
        as_mask(d, 4, 0xFFu, 0xFF00u, 0xFF0000u, 0xFF000000u);
        return 1;
    case 49:
        as_mask(d, 2, 0xFFu, 0xFF00u, 0, 0);
        return 1;
    case 60:
    case 61:
        as_mask(d, 1, 0xFFu, 0, 0, 0);
        d->gray = 1;
        return 1;
    case 65:
        as_mask(d, 1, 0xFFu, 0, 0, 0);
        d->gray = 1;
        return 1;
    case 85:
        as_mask(d, 2, 0xF800u, 0x7E0u, 0x1Fu, 0);
        return 1;
    case 86:
        as_mask(d, 2, 0x7C00u, 0x3E0u, 0x1Fu, 0x8000u);
        return 1;
    case 87:
    case 90:
    case 91:
        as_mask(d, 4, 0xFF0000u, 0xFF00u, 0xFFu, 0xFF000000u);
        return 1;
    case 88:
    case 92:
    case 93:
        as_mask(d, 4, 0xFF0000u, 0xFF00u, 0xFFu, 0);
        return 1;
    case 115:
        as_mask(d, 2, 0xF00u, 0xF0u, 0xFu, 0xF000u);
        return 1;
    default:
        return 0;
    }
}

static int desc_legacy(const unsigned char *h, struct desc *d)
{
    unsigned pf = rd32(h + 80), cc = rd32(h + 84);
    int bits = (int)rd32(h + 88);
    if (pf & DDPF_FOURCC)
    {
        switch (cc)
        {
        case FCC('D', 'X', 'T', '1'):
            as_block(d, FMT_BC1, 0, "DXT1");
            return 1;
        case FCC('D', 'X', 'T', '2'):
        case FCC('D', 'X', 'T', '3'):
            as_block(d, FMT_BC2, 0, "DXT3");
            return 1;
        case FCC('D', 'X', 'T', '4'):
        case FCC('D', 'X', 'T', '5'):
            as_block(d, FMT_BC3, 0, "DXT5");
            return 1;
        case FCC('A', 'T', 'I', '1'):
        case FCC('B', 'C', '4', 'U'):
            as_block(d, FMT_BC4, 0, "BC4");
            return 1;
        case FCC('B', 'C', '4', 'S'):
            as_block(d, FMT_BC4, 1, "BC4");
            return 1;
        case FCC('A', 'T', 'I', '2'):
        case FCC('B', 'C', '5', 'U'):
            as_block(d, FMT_BC5, 0, "BC5");
            return 1;
        case FCC('B', 'C', '5', 'S'):
            as_block(d, FMT_BC5, 1, "BC5");
            return 1;
        case 36:
            as_chan(d, RAW_U16, 4, 2);
            return 1;
        case 111:
            as_chan(d, RAW_F16, 1, 2);
            return 1;
        case 112:
            as_chan(d, RAW_F16, 2, 2);
            return 1;
        case 113:
            as_chan(d, RAW_F16, 4, 2);
            return 1;
        case 114:
            as_chan(d, RAW_F32, 1, 4);
            return 1;
        case 115:
            as_chan(d, RAW_F32, 2, 4);
            return 1;
        case 116:
            as_chan(d, RAW_F32, 4, 4);
            return 1;
        default:
            return 0;
        }
    }
    if (bits < 8 || bits > 32 || bits % 8)
        return 0;
    if (pf & DDPF_LUMINANCE)
    {
        as_mask(d, bits / 8, rd32(h + 92), 0, 0, rd32(h + 104));
        d->gray = 1;
        return 1;
    }
    if (pf & DDPF_RGB)
    {
        as_mask(d, bits / 8, rd32(h + 92), rd32(h + 96), rd32(h + 100),
                rd32(h + 104));
        return 1;
    }
    if (pf & DDPF_ALPHA)
    {
        as_mask(d, bits / 8, rd32(h + 104), 0, 0, rd32(h + 104));
        d->gray = 1;
        return 1;
    }
    return 0;
}

static unsigned char mask_chan(unsigned px, unsigned m)
{
    if (!m)
        return 0;
    int s = 0;
    unsigned t = m;
    while (!(t & 1))
    {
        t >>= 1;
        s++;
    }
    unsigned v = (px & m) >> s;
    return (unsigned char)((v * 255u + t / 2) / t);
}

static double half2d(unsigned short v)
{
    int s = (v >> 15) & 1, e = (v >> 10) & 31, m = v & 1023;
    double r;
    if (e == 0)
        r = ldexp((double)m, -24);
    else if (e == 31)
        r = m ? 0.0 : 1.0e30;
    else
        r = ldexp((double)(m + 1024), e - 25);
    return s ? -r : r;
}

static void raw_pixel(const unsigned char *p, const struct desc *d,
                      unsigned char *rgb)
{
    if (d->raw == RAW_MASK)
    {
        unsigned v = 0;
        for (int i = 0; i < d->bytes; i++)
            v |= (unsigned)p[i] << (8 * i);
        rgb[0] = mask_chan(v, d->mr);
        rgb[1] = d->gray ? rgb[0] : mask_chan(v, d->mg);
        rgb[2] = d->gray ? rgb[0] : mask_chan(v, d->mb);
        return;
    }
    double c[3] = {0.0, 0.0, 0.0};
    for (int i = 0; i < d->nch && i < 3; i++)
    {
        if (d->raw == RAW_U16)
            c[i] = ((unsigned)p[i * 2] | ((unsigned)p[i * 2 + 1] << 8)) / 65535.0;
        else if (d->raw == RAW_F16)
            c[i] = half2d((unsigned short)((unsigned)p[i * 2] |
                                           ((unsigned)p[i * 2 + 1] << 8)));
        else
        {
            float fv;
            memcpy(&fv, p + i * 4, 4);
            c[i] = fv;
        }
    }
    if (d->gray)
        c[1] = c[2] = c[0];
    for (int i = 0; i < 3; i++)
    {
        double v = c[i] != c[i] ? 0.0 : c[i] < 0.0 ? 0.0
                                    : c[i] > 1.0   ? 1.0
                                                   : c[i];
        rgb[i] = (unsigned char)(v * 255.0 + 0.5);
    }
}

static void fourcc_name(unsigned cc, char *out, size_t n)
{
    unsigned char c[4] = {(unsigned char)cc, (unsigned char)(cc >> 8),
                          (unsigned char)(cc >> 16), (unsigned char)(cc >> 24)};
    for (int i = 0; i < 4; i++)
        if (c[i] < 32 || c[i] > 126)
        {
            snprintf(out, n, "0x%08X", cc);
            return;
        }
    snprintf(out, n, "%c%c%c%c", c[0], c[1], c[2], c[3]);
}

static void report(const unsigned char *h, unsigned dxgi, int dx10)
{
    if (dx10)
    {
        const char *n = dxgi >= 94 && dxgi <= 96 ? " (BC6H)" : "";
        fprintf(stderr, "B-3D: DDS uses unsupported DXGI format %u%s\n", dxgi, n);
    }
    else if (rd32(h + 80) & DDPF_FOURCC)
    {
        char n[16];
        fourcc_name(rd32(h + 84), n, sizeof n);
        fprintf(stderr, "B-3D: DDS uses unsupported format '%s'\n", n);
    }
    else
    {
        fprintf(stderr, "B-3D: DDS uses an unsupported %u-bit pixel layout\n",
                rd32(h + 88));
    }
}

unsigned char *dds_load(FILE *f, int *ow, int *oh, unsigned char **alpha)
{
    if (alpha)
        *alpha = NULL;
    unsigned char h[128];
    if (fread(h, 1, sizeof h, f) != sizeof h)
        return NULL;
    if (rd32(h) != FCC('D', 'D', 'S', ' ') || rd32(h + 4) != 124)
        return NULL;

    unsigned flags = rd32(h + 8);
    int hgt = (int)rd32(h + 12), wid = (int)rd32(h + 16);
    unsigned pitch = rd32(h + 20);
    if (wid <= 0 || hgt <= 0 || wid > 16384 || hgt > 16384)
    {
        fprintf(stderr, "B-3D: DDS has an unusable size (%dx%d)\n", wid, hgt);
        return NULL;
    }

    unsigned dxgi = 0;
    int dx10 = (rd32(h + 80) & DDPF_FOURCC) && rd32(h + 84) == FCC('D', 'X', '1', '0');
    if (dx10)
    {
        unsigned char x[20];
        if (fread(x, 1, sizeof x, f) != sizeof x)
            return NULL;
        dxgi = rd32(x);
    }

    struct desc d;
    memset(&d, 0, sizeof d);
    if (!(dx10 ? desc_dxgi(dxgi, &d) : desc_legacy(h, &d)))
    {
        report(h, dxgi, dx10);
        return NULL;
    }

    unsigned char *rgb = malloc((size_t)wid * hgt * 3);
    unsigned char *av = malloc((size_t)wid * hgt);
    if (!rgb || !av)
    {
        free(rgb);
        free(av);
        return NULL;
    }
    memset(av, 255, (size_t)wid * hgt);

    if (d.fmt == FMT_RAW)
    {
        size_t row = (size_t)wid * d.bytes;
        if ((flags & DDSD_PITCH) && pitch >= row && pitch <= row + 16)
            row = pitch;
        unsigned char *line = malloc(row);
        if (!line)
        {
            free(rgb);
            free(av);
            return NULL;
        }
        for (int y = 0; y < hgt; y++)
        {
            if (fread(line, 1, row, f) != row)
            {
                fprintf(stderr, "B-3D: DDS pixel data is truncated\n");
                free(line);
                free(rgb);
                free(av);
                return NULL;
            }
            for (int x = 0; x < wid; x++)
            {
                const unsigned char *sp = line + (size_t)x * d.bytes;
                raw_pixel(sp, &d, rgb + ((size_t)y * wid + x) * 3);
                if (d.raw == RAW_MASK && d.ma)
                {
                    unsigned v = 0;
                    for (int i = 0; i < d.bytes; i++)
                        v |= (unsigned)sp[i] << (8 * i);
                    av[(size_t)y * wid + x] = mask_chan(v, d.ma);
                }
            }
        }
        free(line);
    }
    else
    {
        int bs = (d.fmt == FMT_BC1 || d.fmt == FMT_BC4) ? 8 : 16;
        int bx = (wid + 3) / 4, by = (hgt + 3) / 4;
        size_t rowbytes = (size_t)bx * bs;
        unsigned char *blocks = malloc(rowbytes);
        if (!blocks)
        {
            free(rgb);
            free(av);
            return NULL;
        }
        for (int by_i = 0; by_i < by; by_i++)
        {
            if (fread(blocks, 1, rowbytes, f) != rowbytes)
            {
                fprintf(stderr, "B-3D: DDS pixel data is truncated\n");
                free(blocks);
                free(rgb);
                free(av);
                return NULL;
            }
            for (int bx_i = 0; bx_i < bx; bx_i++)
            {
                const unsigned char *b = blocks + (size_t)bx_i * bs;
                unsigned char px[64];
                switch (d.fmt)
                {
                case FMT_BC1:
                    bc1_block(b, px, 0);
                    break;
                case FMT_BC2:
                    bc2_block(b, px);
                    break;
                case FMT_BC3:
                    bc3_block(b, px);
                    break;
                case FMT_BC4:
                    bc4_block(b, px, d.sign);
                    break;
                case FMT_BC5:
                    bc5_block(b, px, d.sign);
                    break;
                default:
                    bc7_block(b, px);
                    break;
                }
                for (int y = 0; y < 4; y++)
                {
                    int py = by_i * 4 + y;
                    if (py >= hgt)
                        break;
                    for (int x = 0; x < 4; x++)
                    {
                        int pxx = bx_i * 4 + x;
                        if (pxx >= wid)
                            break;
                        memcpy(rgb + ((size_t)py * wid + pxx) * 3,
                               px + (y * 4 + x) * 4, 3);
                        if (d.fmt != FMT_BC4 && d.fmt != FMT_BC5)
                            av[(size_t)py * wid + pxx] = px[(y * 4 + x) * 4 + 3];
                    }
                }
            }
        }
        free(blocks);
    }

    size_t npx = (size_t)wid * hgt, i = 0;
    while (i < npx && av[i] == 255)
        i++;
    if (i == npx)
    {
        free(av);
        av = NULL;
    }
    *ow = wid;
    *oh = hgt;
    if (alpha)
        *alpha = av;
    else
        free(av);
    return rgb;
}
