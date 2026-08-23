#include "b3d.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static _Thread_local int by0, by1;
static _Thread_local int tl_mat = -1;

/* The ASCII Ramps for the renderer, i might add moer idk thou~ */
const struct b3d_ramp B3D_RAMPS[] = {
    {"simple", " .:-=+*#%@"},
    {"classic", " .,:;i1tfLCG08@"},
    {"dense", " .`^\",:;Il!i~+_-?][}{1)(|\\/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$"},
    {"blocks", " .:\xe2\x96\x91\xe2\x96\x92\xe2\x96\x93\xe2\x96\x88"},
    {"dots", " .oO@"},
    {"binary", " 01"},
};
const int B3D_NRAMPS = (int)(sizeof B3D_RAMPS / sizeof *B3D_RAMPS);

const struct b3d_ramp *b3d_ramp_find(const char *name)
{
    for (int i = 0; i < B3D_NRAMPS; i++)
        if (!strcmp(B3D_RAMPS[i].name, name))
            return &B3D_RAMPS[i];
    return NULL;
}

static char glyph_store[B3D_MAX_GLYPHS][5];

void render_set_ramp(const char *name)
{
    const char *s = g.opt.custom;
    if (strcmp(name, "custom") != 0)
    {
        const struct b3d_ramp *r = b3d_ramp_find(name);
        if (!r)
            die("unknown ramp '%s'", name); /* cli_parse should validate this first */
        s = r->glyphs;
    }
    const unsigned char *p = (const unsigned char *)s;
    int n = 0;
    while (*p && n < B3D_MAX_GLYPHS)
    {
        int len = *p < 0x80 ? 1 : *p < 0xE0 ? 2
                              : *p < 0xF0   ? 3
                                            : 4;
        memcpy(glyph_store[n], p, (size_t)len);
        glyph_store[n][len] = '\0';
        g.glyphs[n] = glyph_store[n];
        n++;
        while (len-- && *p)
            p++;
    }
    g.nglyphs = n > 0 ? n - 1 : 0;
}

void render_resolve_color(void)
{
    struct options *o = &g.opt;
    if (o->color >= 0)
    {
        g.usecolor = o->color;
        return;
    }
    g.usecolor = 0;
    if (getenv("NO_COLOR"))
        return;
    if (!isatty(1))
        return;
    const char *t = getenv("TERM");
    if (!t || !*t || !strcmp(t, "dumb"))
        return;
    if (strstr(t, "256color") || strstr(t, "xterm") || strstr(t, "kitty") ||
        strstr(t, "alacritty") || strstr(t, "wezterm") || strstr(t, "foot") ||
        !strcmp(t, "linux"))
        g.usecolor = 1;
    else
    {
        const char *c = getenv("COLORTERM");
        if (c && *c)
            g.usecolor = 1;
    }
}

void render_setup_light(void)
{
    g.light = v3norm(v3(-0.45, 0.70, -0.60));
    g.halfv = v3norm(v3(g.light.x, g.light.y, g.light.z - 1.0));
}

/* Splits shading into its diffuse and specular halves. Keeping them apart
   lets pix_color tint the highlight with Ks and a specular map instead of
   baking a white one into the vertex intensity. */
static double shade_parts(vec3 n, double *spec)
{
    double d = v3dot(n, g.light);
    if (d < 0.0)
        d = 0.0;
    double i = g.opt.ambient + (1.0 - g.opt.ambient) * d;
    double sp = v3dot(n, g.halfv);
    if (sp > 0.0)
    {
        sp *= sp;
        sp *= sp;
        sp *= sp;
        sp *= sp;
    }
    else
        sp = 0.0;
    *spec = sp;
    return i > 1.0 ? 1.0 : i;
}

double render_shade(vec3 n)
{
    double sp;
    double i = shade_parts(n, &sp) + g.opt.specular * sp;
    return i > 1.0 ? 1.0 : i;
}

void render_update_proj(void)
{
    struct screen *s = &g.scr;
    s->cx = s->w / 2.0;
    s->cy = s->h / 2.0;
    double psy = s->h * 0.85 * (g.opt.zoom / 100.0) * g.opt.dist / 2.0;
    if (psy < 0.0625)
        psy = 0.0625;
    s->py = psy;
    s->px = psy * g.opt.aspect_n / g.opt.aspect_d;
}

void render_resize_fb(void)
{
    struct screen *s = &g.scr;
    size_t n = (size_t)s->w * s->h;
    s->chr = xrealloc(s->chr, n * sizeof *s->chr);
    s->zbuf = xrealloc(s->zbuf, n * sizeof *s->zbuf);
    s->clr = xrealloc(s->clr, n * sizeof *s->clr);
}

void render_clear_colors(void)
{
    struct screen *s = &g.scr;
    size_t n = (size_t)s->w * s->h;
    for (size_t i = 0; i < n; i++)
        s->clr[i] = -1;
}

static void fb_clear(void)
{
    struct screen *s = &g.scr;
    size_t n = (size_t)s->w * s->h;
    for (size_t i = 0; i < n; i++)
    {
        s->chr[i] = " ";
        s->zbuf[i] = 0.0;
        s->clr[i] = -1;
    }
}

static void mkrot(void)
{
    int a = ((g.opt.ax % 360) + 360) % 360;
    int b = ((g.opt.ay % 360) + 360) % 360;
    int c = ((g.opt.az % 360) + 360) % 360;
    double sa = sin(a * M_PI / 180.0), ca = cos(a * M_PI / 180.0);
    double sb = sin(b * M_PI / 180.0), cb = cos(b * M_PI / 180.0);
    double sc = sin(c * M_PI / 180.0), cc = cos(c * M_PI / 180.0);
    double *r = g.rot;
    r[0] = cc * cb;
    r[1] = (cc * sb) * sa - sc * ca;
    r[2] = (cc * sb) * ca + sc * sa;
    r[3] = sc * cb;
    r[4] = (sc * sb) * sa + cc * ca;
    r[5] = (sc * sb) * ca - cc * sa;
    r[6] = -sb;
    r[7] = cb * sa;
    r[8] = cb * ca;
}

static void transform_vertices(void)
{
    struct mesh *m = &g.mesh;
    struct screen *s = &g.scr;
    double *r = g.rot;
    for (int i = 0; i < m->nv; i++)
    {
        vec3 p = m->v[i];
        vec3 t = v3(r[0] * p.x + r[1] * p.y + r[2] * p.z + g.opt.offx,
                    r[3] * p.x + r[4] * p.y + r[5] * p.z + g.opt.offy,
                    r[6] * p.x + r[7] * p.y + r[8] * p.z + g.opt.dist + g.opt.offz);
        g.tv[i] = t;
        if (t.z >= NEAR_Z)
            g.pv[i] = v3(s->cx + t.x * s->px / t.z, s->cy - t.y * s->py / t.z, 1.0 / t.z);
        else
            g.pv[i] = v3(0.0, 0.0, 0.0);
        if (g.smooth)
        {
            vec3 n = m->vn[i];
            vec3 rn = v3(r[0] * n.x + r[1] * n.y + r[2] * n.z,
                         r[3] * n.x + r[4] * n.y + r[5] * n.z,
                         r[6] * n.x + r[7] * n.y + r[8] * n.z);
            g.vi[i] = shade_parts(rn, &g.vspec[i]);
        }
    }
}

/* A vertex ready for rasterising: screen position, the two shading terms, the
   texture coordinate and the interpolated vertex colour. */
typedef struct
{
    double x, y, z;
    double i, s;
    double u, v;
    double r, g, b;
} rvtx;

static rvtx rlerp(rvtx a, rvtx b, double t)
{
    rvtx m;
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

static double lum3(const double *c)
{
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
}

/* Shades one fragment. Returns 0 when an opacity map cuts the fragment away,
   in which case the caller must leave the framebuffer untouched. */
static int pix_color(rvtx f, int32_t *clr, double *gio)
{
    const struct material *mt = NULL;
    int mi = tl_mat >= 0 ? tl_mat : g.globmat_id;
    if (mi >= 0 && mi < g.nmats)
        mt = &g.mats[mi];
    const struct texture *tx = g.tex ? g.tex : (mt ? mt->map : NULL);
    double r, gg, b;
    if (mt)
    {
        r = mt->kd[0];
        gg = mt->kd[1];
        b = mt->kd[2];
    }
    else
    {
        r = g.opt.cr / 255.0;
        gg = g.opt.cg / 255.0;
        b = g.opt.cb / 255.0;
    }
    if (g.opt.tex_on)
    {
        /* An explicit map_d wins; otherwise the base texture's own alpha
           channel, which only exists when the image was not fully opaque. */
        double a = 1.0;
        if (mt && mt->map_d && mt->map_d->rgb)
        {
            double t[3];
            tex_sample(mt->map_d, f.u, f.v, t);
            a = (t[0] + t[1] + t[2]) / 3.0;
        }
        else if (tx && tx->alpha)
            a = tex_alpha(tx, f.u, f.v);
        if (a < 0.5)
            return 0;
    }
    double gi = f.i;
    if (tx && tx->rgb && g.opt.tex_on)
    {
        double t[3];
        tex_sample(tx, f.u, f.v, t);
        r = t[0];
        gg = t[1];
        b = t[2];
        if (!g.usecolor)
        {
            double lum = lum3(t);
            double av = tx->avg_lum > 0.04 ? tx->avg_lum : 1.0;
            gi = f.i * (lum / av);
        }
    }
    r *= f.r;
    gg *= f.g;
    b *= f.b;

    /* Highlight colour: a specular map wins, then a non-black Ks, otherwise
       the plain white highlight the renderer has always drawn. */
    double sp[3] = {1.0, 1.0, 1.0};
    if (mt && mt->ks[0] + mt->ks[1] + mt->ks[2] > 0.0)
        for (int k = 0; k < 3; k++)
            sp[k] = mt->ks[k];
    if (mt && mt->map_ks && mt->map_ks->rgb && g.opt.tex_on)
    {
        double t[3];
        tex_sample(mt->map_ks, f.u, f.v, t);
        for (int k = 0; k < 3; k++)
            sp[k] *= t[k];
    }
    double sw = g.opt.specular * f.s;

    double em[3] = {0.0, 0.0, 0.0};
    if (mt)
    {
        for (int k = 0; k < 3; k++)
            em[k] = mt->ke[k];
        if (mt->map_ke && mt->map_ke->rgb && g.opt.tex_on)
        {
            double t[3];
            tex_sample(mt->map_ke, f.u, f.v, t);
            for (int k = 0; k < 3; k++)
                em[k] *= t[k];
        }
    }

    if (g.usecolor)
    {
        double o[3];
        o[0] = r * f.i + sp[0] * sw + em[0];
        o[1] = gg * f.i + sp[1] * sw + em[1];
        o[2] = b * f.i + sp[2] * sw + em[2];
        int32_t v = 0;
        for (int k = 0; k < 3; k++)
        {
            int c = (int)(o[k] * 255.0 + 0.5);
            if (c < 0)
                c = 0;
            if (c > 255)
                c = 255;
            v = (v << 8) | c;
        }
        *clr = v;
    }
    else
    {
        *clr = -1;
    }
    /* Mono output keeps the same budget: brightness picks the glyph. */
    gi += lum3(sp) * sw + lum3(em);
    if (!g.usecolor && (f.r != 1.0 || f.g != 1.0 || f.b != 1.0))
        gi *= lum3((double[]){f.r, f.g, f.b});
    *gio = gi;
    return 1;
}

static void put_cell(int id, double iz, rvtx f)
{
    struct screen *s = &g.scr;
    if (f.i < 0.0)
        f.i = 0.0;
    if (f.i > 1.0)
        f.i = 1.0;
    int32_t clr;
    double gi;
    if (!pix_color(f, &clr, &gi))
        return;
    s->zbuf[id] = iz;
    if (gi < 0.0)
        gi = 0.0;
    if (gi > 1.0)
        gi = 1.0;
    int lev = (int)(gi * g.nglyphs);
    if (lev > g.nglyphs)
        lev = g.nglyphs;
    s->chr[id] = g.glyphs[lev];
    if (g.usecolor)
        s->clr[id] = clr;
}

static void raster_tri(rvtx A, rvtx B, rvtx C)
{
    struct screen *s = &g.scr;
    int w = s->w;
    double area = (B.x - A.x) * (C.y - A.y) - (B.y - A.y) * (C.x - A.x);
    if (area == 0.0)
        return;
    if (area < 0.0)
    {
        rvtx t = B;
        B = C;
        C = t;
        area = -area;
    }
    double xmin = A.x < B.x ? A.x : B.x;
    if (C.x < xmin)
        xmin = C.x;
    double xmax = A.x > B.x ? A.x : B.x;
    if (C.x > xmax)
        xmax = C.x;
    double ymin = A.y < B.y ? A.y : B.y;
    if (C.y < ymin)
        ymin = C.y;
    double ymax = A.y > B.y ? A.y : B.y;
    if (C.y > ymax)
        ymax = C.y;
    int xi = (int)floor(xmin), xa = (int)ceil(xmax);
    int yi = (int)floor(ymin), ya = (int)ceil(ymax);
    if (xi < 0)
        xi = 0;
    if (yi < by0)
        yi = by0;
    if (xa > w - 1)
        xa = w - 1;
    if (ya > by1)
        ya = by1;
    if (xi > xa || yi > ya)
        return;
    double e0x = C.x - B.x, e0y = C.y - B.y;
    double e1x = A.x - C.x, e1y = A.y - C.y;
    double e2x = B.x - A.x, e2y = B.y - A.y;
    double ia = 1.0 / area;
    double az = A.z, bz = B.z, cz = C.z;
    /* u, v and the vertex colour are interpolated over 1/z so perspective
       does not skew them across large triangles. */
    double au = A.u * az, av = A.v * az;
    double bu = B.u * bz, bv = B.v * bz;
    double cu = C.u * cz, cv = C.v * cz;
    double ar = A.r * az, ag = A.g * az, ab = A.b * az;
    double br = B.r * bz, bg = B.g * bz, bb = B.b * bz;
    double cr = C.r * cz, cg = C.g * cz, cb = C.b * cz;
    for (int y = yi; y <= ya; y++)
    {
        double py = y + 0.5;
        int base = y * w;
        for (int x = xi; x <= xa; x++)
        {
            double px = x + 0.5;
            double w0 = e0x * (py - B.y) - e0y * (px - B.x);
            double w1 = e1x * (py - C.y) - e1y * (px - C.x);
            double w2 = e2x * (py - A.y) - e2y * (px - A.x);
            if (w0 >= 0.0 && w1 >= 0.0 && w2 >= 0.0)
            {
                int id = base + x;
                double iz = (w0 * az + w1 * bz + w2 * cz) * ia;
                if (iz > s->zbuf[id])
                {
                    double inv = 1.0 / iz;
                    rvtx f;
                    f.x = px;
                    f.y = py;
                    f.z = iz;
                    f.i = (w0 * A.i + w1 * B.i + w2 * C.i) * ia;
                    f.s = (w0 * A.s + w1 * B.s + w2 * C.s) * ia;
                    f.u = (w0 * au + w1 * bu + w2 * cu) * ia * inv;
                    f.v = (w0 * av + w1 * bv + w2 * cv) * ia * inv;
                    f.r = (w0 * ar + w1 * br + w2 * cr) * ia * inv;
                    f.g = (w0 * ag + w1 * bg + w2 * cg) * ia * inv;
                    f.b = (w0 * ab + w1 * bb + w2 * cb) * ia * inv;
                    put_cell(id, iz, f);
                }
            }
        }
    }
}

static void draw_line(rvtx a, rvtx b)
{
    struct screen *s = &g.scr;
    int w = s->w, h = s->h;
    int x0 = lround(a.x), y0 = lround(a.y), x1 = lround(b.x), y1 = lround(b.y);
    int dx = abs(x1 - x0), dy = abs(y1 - y0);
    int steps = dx > dy ? dx : dy;
    if (steps > (w + h) * 8)
        return;
    if (steps == 0)
        steps = 1;
    int sx = x0 > x1 ? -1 : 1, sy = y0 > y1 ? -1 : 1;
    int err = dx - dy, i = 0;
    for (;;)
    {
        if (x0 >= 0 && x0 < w && y0 >= by0 && y0 <= by1)
        {
            int id = y0 * w + x0;
            rvtx f = rlerp(a, b, (double)i / steps);
            if (f.i < 0.0)
                f.i = 0.0;
            if (f.i > 1.0)
                f.i = 1.0;
            if (f.z >= s->zbuf[id])
            {
                int32_t clr;
                double gi;
                if (pix_color(f, &clr, &gi))
                {
                    if (gi < 0.0)
                        gi = 0.0;
                    if (gi > 1.0)
                        gi = 1.0;
                    int lev = (int)(gi * g.nglyphs);
                    if (lev < 1)
                        lev = 1;
                    if (lev > g.nglyphs)
                        lev = g.nglyphs;
                    s->zbuf[id] = f.z;
                    s->chr[id] = g.glyphs[lev];
                    if (g.usecolor)
                        s->clr[id] = clr;
                }
            }
        }
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = err * 2;
        if (e2 > -dy)
        {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx)
        {
            err += dx;
            y0 += sy;
        }
        if (++i > steps + 2)
            break;
    }
}

static void plot_point(rvtx f)
{
    struct screen *s = &g.scr;
    int px = lround(f.x), py = lround(f.y);
    if (px < 0 || px >= s->w || py < by0 || py > by1)
        return;
    int id = py * s->w + px;
    if (f.z < s->zbuf[id])
        return;
    if (f.i < 0.0)
        f.i = 0.0;
    if (f.i > 1.0)
        f.i = 1.0;
    int32_t clr;
    double gi;
    if (!pix_color(f, &clr, &gi))
        return;
    if (gi < 0.0)
        gi = 0.0;
    if (gi > 1.0)
        gi = 1.0;
    int lev = (int)(gi * g.nglyphs);
    if (lev < 2)
        lev = 2;
    if (lev > g.nglyphs)
        lev = g.nglyphs;
    s->zbuf[id] = f.z;
    s->chr[id] = g.glyphs[lev];
    if (g.usecolor)
        s->clr[id] = clr;
}

static void clip_and_emit(rvtx a, rvtx b, rvtx c)
{
    struct screen *s = &g.scr;
    rvtx q[3] = {a, b, c};
    rvtx o[5];
    int n = 0;
    for (int i = 0; i < 3; i++)
    {
        int j = (i + 1) % 3;
        double zi = q[i].z, zj = q[j].z;
        int ina = zi >= NEAR_Z, inb = zj >= NEAR_Z;
        if (ina)
            o[n++] = q[i];
        if (ina != inb)
        {
            rvtx m = rlerp(q[i], q[j], (NEAR_Z - zi) / (zj - zi));
            m.z = NEAR_Z;
            o[n++] = m;
        }
    }
    if (n < 3)
        return;
    for (int i = 0; i < n; i++)
    {
        double oz = o[i].z;
        o[i].x = s->cx + o[i].x * s->px / oz;
        o[i].y = s->cy - o[i].y * s->py / oz;
        o[i].z = 1.0 / oz;
    }
    switch (g.opt.mode)
    {
    case MODE_SOLID:
        for (int i = 1; i + 1 < n; i++)
            raster_tri(o[0], o[i], o[i + 1]);
        break;
    case MODE_WIRE:
        for (int i = 0; i < n; i++)
            draw_line(o[i], o[(i + 1) % n]);
        break;
    case MODE_POINTS:
        for (int i = 0; i < n; i++)
            plot_point(o[i]);
        break;
    }
}

/* Fills in everything about a corner except its shading terms. */
static rvtx vtx_of(const struct mesh *m, int i, vec3 p)
{
    rvtx r;
    r.x = p.x;
    r.y = p.y;
    r.z = p.z;
    r.i = r.s = 0.0;
    r.u = m->vt ? m->vt[i * 2] : 0.0;
    r.v = m->vt ? m->vt[i * 2 + 1] : 0.0;
    if (m->has_vcol && m->vc)
    {
        r.r = m->vc[i * 3];
        r.g = m->vc[i * 3 + 1];
        r.b = m->vc[i * 3 + 2];
    }
    else
        r.r = r.g = r.b = 1.0;
    return r;
}

static void draw_faces(void)
{
    struct mesh *m = &g.mesh;
    double *r = g.rot;
    for (int f = 0; f < m->nf; f++)
    {
        int a = m->idx[f * 3], b = m->idx[f * 3 + 1], c = m->idx[f * 3 + 2];
        tl_mat = (m->fmat && m->fmat[f] >= 0) ? m->fmat[f] : -1;
        vec3 fn = m->fn[f];
        vec3 rn = v3(r[0] * fn.x + r[1] * fn.y + r[2] * fn.z,
                     r[3] * fn.x + r[4] * fn.y + r[5] * fn.z,
                     r[6] * fn.x + r[7] * fn.y + r[8] * fn.z);
        double dp = v3dot(rn, g.tv[a]);
        if (g.opt.cull)
        {
            if (dp >= 0.0)
                continue;
        }
        else if (dp > 0.0)
        {
            rn = v3neg(rn);
        }
        rvtx A = vtx_of(m, a, g.pv[a]);
        rvtx B = vtx_of(m, b, g.pv[b]);
        rvtx C = vtx_of(m, c, g.pv[c]);
        if (g.smooth)
        {
            A.i = g.vi[a];
            A.s = g.vspec[a];
            B.i = g.vi[b];
            B.s = g.vspec[b];
            C.i = g.vi[c];
            C.s = g.vspec[c];
        }
        else
        {
            double sp;
            double d = shade_parts(rn, &sp);
            A.i = B.i = C.i = d;
            A.s = B.s = C.s = sp;
        }
        /* g.pv holds 1/z, so the near test has to look at view space. */
        if (g.tv[a].z >= NEAR_Z && g.tv[b].z >= NEAR_Z && g.tv[c].z >= NEAR_Z)
        {
            double symin = A.y < B.y ? A.y : B.y;
            if (C.y < symin)
                symin = C.y;
            double symax = A.y > B.y ? A.y : B.y;
            if (C.y > symax)
                symax = C.y;
            if (symax < by0 - 0.5 || symin > by1 + 0.5)
                continue;
            /* g.pv already holds screen x, y and 1/z for on-screen vertices. */
            switch (g.opt.mode)
            {
            case MODE_SOLID:
                raster_tri(A, B, C);
                break;
            case MODE_WIRE:
                draw_line(A, B);
                draw_line(B, C);
                draw_line(C, A);
                break;
            case MODE_POINTS:
                plot_point(A);
                plot_point(B);
                plot_point(C);
                break;
            }
        }
        else
        {
            /* Off-screen corners need the unprojected view-space positions. */
            A.x = g.tv[a].x;
            A.y = g.tv[a].y;
            A.z = g.tv[a].z;
            B.x = g.tv[b].x;
            B.y = g.tv[b].y;
            B.z = g.tv[b].z;
            C.x = g.tv[c].x;
            C.y = g.tv[c].y;
            C.z = g.tv[c].z;
            clip_and_emit(A, B, C);
        }
    }
}

typedef struct
{
    int y0, y1;
} band;

#define B3D_MAX_THREADS 16

static pthread_t pool_tid[B3D_MAX_THREADS];
static band pool_band[B3D_MAX_THREADS];
static int pool_n;
static int band_count;
static unsigned long frame_gen;
static int pool_stop;

static pthread_mutex_t start_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t start_cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t done_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t done_cv = PTHREAD_COND_INITIALIZER;
static int done_count;

/* The Main Worker for the thread pool so your pc doesnt wanna kill itself */
static void *pool_worker(void *arg)
{
    long id = (long)arg;
    unsigned long seen = 0;
    for (;;)
    {
        pthread_mutex_lock(&start_mu);
        while (frame_gen == seen && !pool_stop)
            pthread_cond_wait(&start_cv, &start_mu);
        if (pool_stop)
        {
            pthread_mutex_unlock(&start_mu);
            return NULL;
        }
        seen = frame_gen;
        band b = {1, 0};
        if (id < band_count)
            b = pool_band[id];
        pthread_mutex_unlock(&start_mu);

        if (b.y0 <= b.y1)
        {
            by0 = b.y0;
            by1 = b.y1;
            draw_faces();
        }

        pthread_mutex_lock(&done_mu);
        done_count++;
        pthread_cond_signal(&done_cv);
        pthread_mutex_unlock(&done_mu);
    }
}

void render_pool_start(void)
{
    if (g.jobs_eff < 2)
        return;
    for (long i = 0; i < g.jobs_eff && i < B3D_MAX_THREADS; i++)
    {
        if (pthread_create(&pool_tid[i], NULL, pool_worker, (void *)i) != 0)
            break;
        pool_n++;
    }
}

void render_pool_stop(void)
{
    if (!pool_n)
        return;
    pthread_mutex_lock(&start_mu);
    pool_stop = 1;
    pthread_cond_broadcast(&start_cv);
    pthread_mutex_unlock(&start_mu);
    for (int i = 0; i < pool_n; i++)
        pthread_join(pool_tid[i], NULL);
    pool_n = 0;
    pool_stop = 0;
}

void render_frame(void)
{
    mkrot();
    if (gpu_render_frame())
        return;
    transform_vertices();
    int h = g.scr.h;
    int par = 0;
    if (pool_n > 1 && g.mesh.nf >= 32 && (long)g.scr.w * h >= 1500)
    {
        par = pool_n;
        if (par > h / 2)
            par = h / 2;
    }
    if (par <= 1)
    {
        by0 = 0;
        by1 = h - 1;
        fb_clear();
        draw_faces();
        return;
    }
    fb_clear();
    int step = (h + par - 1) / par;
    pthread_mutex_lock(&start_mu);
    for (int k = 0; k < par; k++)
    {
        int y0 = k * step, y1 = y0 + step - 1;
        if (y1 >= h)
            y1 = h - 1;
        pool_band[k].y0 = y0;
        pool_band[k].y1 = y1;
    }
    band_count = par;
    done_count = 0;
    frame_gen++;
    pthread_cond_broadcast(&start_cv);
    pthread_mutex_unlock(&start_mu);

    pthread_mutex_lock(&done_mu);
    while (done_count < pool_n)
        pthread_cond_wait(&done_cv, &done_mu);
    pthread_mutex_unlock(&done_mu);
}

static char *out_buf;
static size_t out_cap, out_len;

static void format_rows(int y0, int y1)
{
    struct screen *s = &g.scr;
    size_t need = (size_t)s->w * (size_t)(y1 - y0 + 1) * 32 + 64;
    if (need > out_cap)
    {
        out_cap = need * 2;
        out_buf = xrealloc(out_buf, out_cap);
    }
    char *p = out_buf;
    if (g.usecolor)
    {
        for (int r = y0; r <= y1; r++)
        {
            int prev = -2;
            int base = r * s->w;
            for (int c = 0; c < s->w; c++)
            {
                int32_t col = s->clr[base + c];
                if (col != prev)
                {
                    if (col < 0)
                    {
                        memcpy(p, "\033[0m", 4);
                        p += 4;
                    }
                    else
                        p += sprintf(p, "\033[38;2;%d;%d;%dm", (col >> 16) & 255, (col >> 8) & 255, col & 255);
                    prev = col;
                }
                const char *gl = s->chr[base + c];
                size_t glen = strlen(gl);
                memcpy(p, gl, glen);
                p += glen;
            }
            memcpy(p, "\033[0m\n", 5);
            p += 5;
        }
    }
    else
    {
        for (int r = y0; r <= y1; r++)
        {
            int base = r * s->w;
            for (int c = 0; c < s->w; c++)
            {
                const char *gl = s->chr[base + c];
                size_t glen = strlen(gl);
                memcpy(p, gl, glen);
                p += glen;
            }
            *p++ = '\n';
        }
    }
    out_len = (size_t)(p - out_buf);
}

void render_present(void)
{
    format_rows(0, g.scr.h - 1);
    if (g.tui)
        fputs("\033[H", stdout);
    fwrite(out_buf, 1, out_len, stdout);
    fflush(stdout);
    if (ferror(stdout))
    {
        clearerr(stdout);
        g.running = 0;
    }
}
