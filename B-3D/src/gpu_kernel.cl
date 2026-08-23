
#define NEAR_Z 0.25f
#define LEV_EMPTY 255

typedef struct
{
    float r0, r1, r2, r3, r4, r5, r6, r7, r8;
    float offx, offy, offz;
    float dist;
    float cx, cy, px, py;
    float lx, ly, lz;
    float hx, hy, hz;
    float ambient, specular;
    float br, bg, bb;
    int w, h;
    int nf, nv;
    int smooth, cull, tex_on, usecolor;
    int filter, nglyphs, nmats, gtex;
    int ntri;
    int pad0, pad1, pad2;
} uni;

typedef struct
{
    int off, w, h, aoff;
    float avg_lum;
    int pad0, pad1, pad2;
} gtex;

typedef struct
{
    float kd0, kd1, kd2, ns;
    float ks0, ks1, ks2, pad0;
    float ke0, ke1, ke2, pad1;
    int map, map_ke, map_ks, map_d;
} gmat;

typedef struct
{
    float x, y, z;
    float i, s;
    float u, v;
    float r, g, b;
} tvtx;

typedef struct
{
    tvtx a, b, c;
    int mat;
    int valid;
} tri;

static float shade_parts(__constant uni *U, float3 n, float *spec)
{
    float d = dot(n, (float3)(U->lx, U->ly, U->lz));
    if (d < 0.0f)
        d = 0.0f;
    float i = U->ambient + (1.0f - U->ambient) * d;
    float sp = dot(n, (float3)(U->hx, U->hy, U->hz));
    if (sp > 0.0f)
    {
        sp *= sp;
        sp *= sp;
        sp *= sp;
        sp *= sp;
    }
    else
        sp = 0.0f;
    *spec = sp;
    return i > 1.0f ? 1.0f : i;
}

static float3 rot3(__constant uni *U, float3 p)
{
    return (float3)(U->r0 * p.x + U->r1 * p.y + U->r2 * p.z,
                    U->r3 * p.x + U->r4 * p.y + U->r5 * p.z,
                    U->r6 * p.x + U->r7 * p.y + U->r8 * p.z);
}

static float lum3(float3 c)
{
    return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
}

static float3 texel(__global const uchar *td, __global const gtex *t, int x, int y)
{
    __global const uchar *p = td + t->off + ((size_t)y * t->w + x) * 3;
    return (float3)(p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f);
}

static int wrapi(int v, int n)
{
    v %= n;
    return v < 0 ? v + n : v;
}

static float3 tex_sample(__constant uni *U, __global const uchar *td,
                         __global const gtex *tt, int ti, float u, float v)
{
    __global const gtex *t = tt + ti;
    u -= floor(u);
    v -= floor(v);
    if (U->filter == 1 && t->w > 1 && t->h > 1)
    {
        float fx = u * t->w - 0.5f, fy = v * t->h - 0.5f;
        int x0 = (int)floor(fx), y0 = (int)floor(fy);
        float tx = fx - x0, ty = fy - y0;
        int x1 = wrapi(x0 + 1, t->w), y1 = wrapi(y0 + 1, t->h);
        x0 = wrapi(x0, t->w);
        y0 = wrapi(y0, t->h);
        float3 a = texel(td, t, x0, y0);
        float3 b = texel(td, t, x1, y0);
        float3 d = texel(td, t, x0, y1);
        float3 e = texel(td, t, x1, y1);
        return (a * (1.0f - tx) + b * tx) * (1.0f - ty) + (d * (1.0f - tx) + e * tx) * ty;
    }
    int x = (int)(u * t->w);
    int y = (int)(v * t->h);
    x = clamp(x, 0, t->w - 1);
    y = clamp(y, 0, t->h - 1);
    return texel(td, t, x, y);
}

static float tex_alpha(__global const uchar *td, __global const gtex *tt, int ti,
                       float u, float v)
{
    __global const gtex *t = tt + ti;
    if (t->aoff < 0)
        return 1.0f;
    u -= floor(u);
    v -= floor(v);
    int x = (int)(u * t->w);
    int y = (int)(v * t->h);
    x = clamp(x, 0, t->w - 1);
    y = clamp(y, 0, t->h - 1);
    return td[t->aoff + (size_t)y * t->w + x] / 255.0f;
}

static int base_tex(__constant uni *U, __global const gmat *mats, int mi)
{
    if (U->gtex >= 0)
        return U->gtex;
    if (mi >= 0)
        return mats[mi].map;
    return -1;
}

static int frag_opaque(__constant uni *U, __global const uchar *td,
                       __global const gtex *tt, __global const gmat *mats,
                       int mi, float u, float v)
{
    if (!U->tex_on)
        return 1;
    float a = 1.0f;
    if (mi >= 0 && mats[mi].map_d >= 0)
    {
        float3 t = tex_sample(U, td, tt, mats[mi].map_d, u, v);
        a = (t.x + t.y + t.z) / 3.0f;
    }
    else
    {
        int bt = base_tex(U, mats, mi);
        if (bt >= 0)
            a = tex_alpha(td, tt, bt, u, v);
    }
    return a >= 0.5f;
}

__kernel void k_xform(__constant uni *U, __global const float4 *vtx,
                      __global const float4 *nrm, __global float4 *tv,
                      __global float4 *pv, __global float2 *vsh)
{
    int i = get_global_id(0);
    if (i >= U->nv)
        return;
    float3 p = vtx[i].xyz;
    float3 t = rot3(U, p);
    t.x += U->offx;
    t.y += U->offy;
    t.z += U->dist + U->offz;
    tv[i] = (float4)(t, 0.0f);
    if (t.z >= NEAR_Z)
        pv[i] = (float4)(U->cx + t.x * U->px / t.z, U->cy - t.y * U->py / t.z, 1.0f / t.z, 0.0f);
    else
        pv[i] = (float4)(0.0f, 0.0f, 0.0f, 0.0f);
    if (U->smooth)
    {
        float sp;
        float d = shade_parts(U, rot3(U, nrm[i].xyz), &sp);
        vsh[i] = (float2)(d, sp);
    }
}

static tvtx corner(__global const float4 *tv, __global const float2 *uv,
                   __global const float4 *vc, int i, float sh, float sp)
{
    tvtx r;
    float3 p = tv[i].xyz;
    r.x = p.x;
    r.y = p.y;
    r.z = p.z;
    r.i = sh;
    r.s = sp;
    r.u = uv[i].x;
    r.v = uv[i].y;
    float4 c = vc[i];
    r.r = c.x;
    r.g = c.y;
    r.b = c.z;
    return r;
}

static tvtx tlerp(tvtx a, tvtx b, float t)
{
    tvtx m;
    m.x = a.x + (b.x - a.x) * t;
    m.y = a.y + (b.y - a.y) * t;
    m.z = a.z + (b.z - a.z) * t;
    m.i = a.i + (b.i - a.i) * t;
    m.s = a.s + (b.s - a.s) * t;
    m.u = a.u + (b.u - a.u) * t;
    m.v = a.v + (b.v - a.v) * t;
    m.r = a.r + (b.r - a.r) * t;
    m.g = a.g + (b.g - a.g) * t;
    m.b = a.b + (b.b - a.b) * t;
    return m;
}

__kernel void k_setup(__constant uni *U, __global const int4 *idx,
                      __global const float4 *fnrm, __global const float4 *tv,
                      __global const float2 *uv, __global const float4 *vc,
                      __global const float2 *vsh, __global tri *tris,
                      __global float4 *bbox)
{
    int f = get_global_id(0);
    if (f >= U->nf)
        return;
    const float4 nobox = (float4)(1e30f, 1e30f, -1e30f, -1e30f);
    tris[f * 2].valid = 0;
    tris[f * 2 + 1].valid = 0;
    bbox[f * 2] = nobox;
    bbox[f * 2 + 1] = nobox;

    int4 ix = idx[f];
    int a = ix.x, b = ix.y, c = ix.z, mat = ix.w;
    float3 rn = rot3(U, fnrm[f].xyz);
    float dp = dot(rn, tv[a].xyz);
    if (U->cull)
    {
        if (dp >= 0.0f)
            return;
    }
    else if (dp > 0.0f)
        rn = -rn;

    float sa, sb, sc, pa, pb, pc;
    if (U->smooth)
    {
        sa = vsh[a].x;
        pa = vsh[a].y;
        sb = vsh[b].x;
        pb = vsh[b].y;
        sc = vsh[c].x;
        pc = vsh[c].y;
    }
    else
    {
        float sp;
        float d = shade_parts(U, rn, &sp);
        sa = sb = sc = d;
        pa = pb = pc = sp;
    }

    tvtx q[3];
    q[0] = corner(tv, uv, vc, a, sa, pa);
    q[1] = corner(tv, uv, vc, b, sb, pb);
    q[2] = corner(tv, uv, vc, c, sc, pc);
    tvtx o[4];
    int n = 0;
    for (int i = 0; i < 3; i++)
    {
        int j = (i + 1) % 3;
        float zi = q[i].z, zj = q[j].z;
        int ina = zi >= NEAR_Z, inb = zj >= NEAR_Z;
        if (ina && n < 4)
            o[n++] = q[i];
        if (ina != inb && n < 4)
        {
            tvtx m = tlerp(q[i], q[j], (NEAR_Z - zi) / (zj - zi));
            m.z = NEAR_Z;
            o[n++] = m;
        }
    }
    if (n < 3)
        return;
    for (int i = 0; i < n; i++)
    {
        float oz = o[i].z;
        o[i].x = U->cx + o[i].x * U->px / oz;
        o[i].y = U->cy - o[i].y * U->py / oz;
        o[i].z = 1.0f / oz;
    }
    for (int i = 1; i + 1 < n; i++)
    {
        tvtx A = o[0], B = o[i], C = o[i + 1];
        float area = (B.x - A.x) * (C.y - A.y) - (B.y - A.y) * (C.x - A.x);
        if (area == 0.0f)
            continue;
        if (area < 0.0f)
        {
            tvtx t = B;
            B = C;
            C = t;
        }
        int slot = f * 2 + (i - 1);
        tri o2;
        o2.a = A;
        o2.b = B;
        o2.c = C;
        o2.mat = mat;
        o2.valid = 1;
        tris[slot] = o2;
        bbox[slot] = (float4)(fmin(A.x, fmin(B.x, C.x)), fmin(A.y, fmin(B.y, C.y)),
                              fmax(A.x, fmax(B.x, C.x)), fmax(A.y, fmax(B.y, C.y)));
    }
}

static void shade_frag(__constant uni *U, __global const uchar *td,
                       __global const gtex *tt, __global const gmat *mats, int mi,
                       tvtx f, __global uchar *lev, __global int *clr, int id)
{
    float3 base;
    if (mi >= 0)
        base = (float3)(mats[mi].kd0, mats[mi].kd1, mats[mi].kd2);
    else
        base = (float3)(U->br, U->bg, U->bb);

    float gi = f.i;
    int bt = base_tex(U, mats, mi);
    if (bt >= 0 && U->tex_on)
    {
        float3 t = tex_sample(U, td, tt, bt, f.u, f.v);
        base = t;
        if (!U->usecolor)
        {
            float av = tt[bt].avg_lum > 0.04f ? tt[bt].avg_lum : 1.0f;
            gi = f.i * (lum3(t) / av);
        }
    }
    float3 vcol = (float3)(f.r, f.g, f.b);
    base *= vcol;

    float3 sp = (float3)(1.0f, 1.0f, 1.0f);
    if (mi >= 0 && mats[mi].ks0 + mats[mi].ks1 + mats[mi].ks2 > 0.0f)
        sp = (float3)(mats[mi].ks0, mats[mi].ks1, mats[mi].ks2);
    if (mi >= 0 && mats[mi].map_ks >= 0 && U->tex_on)
        sp *= tex_sample(U, td, tt, mats[mi].map_ks, f.u, f.v);
    float sw = U->specular * f.s;

    float3 em = (float3)(0.0f, 0.0f, 0.0f);
    if (mi >= 0)
    {
        em = (float3)(mats[mi].ke0, mats[mi].ke1, mats[mi].ke2);
        if (mats[mi].map_ke >= 0 && U->tex_on)
            em *= tex_sample(U, td, tt, mats[mi].map_ke, f.u, f.v);
    }

    if (U->usecolor)
    {
        float3 o = base * f.i + sp * sw + em;
        int r = (int)(o.x * 255.0f + 0.5f);
        int g = (int)(o.y * 255.0f + 0.5f);
        int b = (int)(o.z * 255.0f + 0.5f);
        clr[id] = (clamp(r, 0, 255) << 16) | (clamp(g, 0, 255) << 8) | clamp(b, 0, 255);
    }
    else
        clr[id] = -1;

    gi += lum3(sp) * sw + lum3(em);
    if (!U->usecolor && (f.r != 1.0f || f.g != 1.0f || f.b != 1.0f))
        gi *= lum3(vcol);
    gi = clamp(gi, 0.0f, 1.0f);
    int l = (int)(gi * U->nglyphs);
    lev[id] = (uchar)min(l, U->nglyphs);
}

__kernel void k_raster(__constant uni *U, __global const tri *tris,
                       __global const float4 *bbox, __global const uchar *td,
                       __global const gtex *tt, __global const gmat *mats,
                       __global uchar *lev, __global int *clr)
{
    int x = get_global_id(0);
    int y = get_global_id(1);
    if (x >= U->w || y >= U->h)
        return;
    int id = y * U->w + x;
    float px = x + 0.5f, py = y + 0.5f;

    float best = 0.0f;
    int hit = -1;
    tvtx frag;
    int fmat = -1;
    for (int t = 0; t < U->ntri; t++)
    {
        float4 bb = bbox[t];
        if (px < bb.x || px > bb.z || py < bb.y || py > bb.w)
            continue;
        tvtx A = tris[t].a, B = tris[t].b, C = tris[t].c;
        float e0x = C.x - B.x, e0y = C.y - B.y;
        float e1x = A.x - C.x, e1y = A.y - C.y;
        float e2x = B.x - A.x, e2y = B.y - A.y;
        float w0 = e0x * (py - B.y) - e0y * (px - B.x);
        float w1 = e1x * (py - C.y) - e1y * (px - C.x);
        float w2 = e2x * (py - A.y) - e2y * (px - A.x);
        if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
            continue;
        float area = (B.x - A.x) * (C.y - A.y) - (B.y - A.y) * (C.x - A.x);
        if (area <= 0.0f)
            continue;
        float ia = 1.0f / area;
        float iz = (w0 * A.z + w1 * B.z + w2 * C.z) * ia;
        if (iz <= best)
            continue;
        float inv = 1.0f / iz;
        float u = (w0 * A.u * A.z + w1 * B.u * B.z + w2 * C.u * C.z) * ia * inv;
        float v = (w0 * A.v * A.z + w1 * B.v * B.z + w2 * C.v * C.z) * ia * inv;
        int mi = tris[t].mat;
        if (!frag_opaque(U, td, tt, mats, mi, u, v))
            continue;
        frag.x = px;
        frag.y = py;
        frag.z = iz;
        frag.i = clamp((w0 * A.i + w1 * B.i + w2 * C.i) * ia, 0.0f, 1.0f);
        frag.s = (w0 * A.s + w1 * B.s + w2 * C.s) * ia;
        frag.u = u;
        frag.v = v;
        frag.r = (w0 * A.r * A.z + w1 * B.r * B.z + w2 * C.r * C.z) * ia * inv;
        frag.g = (w0 * A.g * A.z + w1 * B.g * B.z + w2 * C.g * C.z) * ia * inv;
        frag.b = (w0 * A.b * A.z + w1 * B.b * B.z + w2 * C.b * C.z) * ia * inv;
        fmat = mi;
        best = iz;
        hit = t;
    }
    if (hit < 0)
    {
        lev[id] = LEV_EMPTY;
        clr[id] = -1;
        return;
    }
    shade_frag(U, td, tt, mats, fmat, frag, lev, clr, id);
}
