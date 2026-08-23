#include "b3d.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mesh_free(struct mesh *m)
{
    free(m->v);
    free(m->vn);
    free(m->fn);
    free(m->vt);
    free(m->vc);
    free(m->idx);
    free(m->fmat);
    for (int i = 0; i < m->nfn; i++)
        free(m->fmtnames[i]);
    free(m->fmtnames);
    for (int i = 0; i < m->nmtllib; i++)
        free(m->mtllib[i]);
    memset(m, 0, sizeof *m);
}

static void mesh_growv(struct mesh *m)
{
    if (m->nv < m->capv)
        return;
    m->capv = m->capv ? m->capv * 2 : 256;
    m->v = xrealloc(m->v, (size_t)m->capv * sizeof *m->v);
    m->vn = xrealloc(m->vn, (size_t)m->capv * sizeof *m->vn);
    m->vt = xrealloc(m->vt, (size_t)m->capv * 2 * sizeof *m->vt);
    m->vc = xrealloc(m->vc, (size_t)m->capv * 3 * sizeof *m->vc);
}

static void mesh_growf(struct mesh *m)
{
    if (m->nf < m->capf)
        return;
    m->capf = m->capf ? m->capf * 2 : 256;
    m->idx = xrealloc(m->idx, (size_t)m->capf * 3 * sizeof *m->idx);
    m->fn = xrealloc(m->fn, (size_t)m->capf * sizeof *m->fn);
    m->fmat = xrealloc(m->fmat, (size_t)m->capf * sizeof *m->fmat);
    m->fname = xrealloc(m->fname, (size_t)m->capf * sizeof *m->fname);
}

/* Appends a vertex with neutral UV and white vertex colour, and hands back its
   index: loaders that read attributes out of order fill them in afterwards
   through mesh_setuv / mesh_setcol. */
int mesh_addv(struct mesh *m, vec3 p)
{
    mesh_growv(m);
    int i = m->nv++;
    m->v[i] = p;
    m->vt[i * 2] = 0.0;
    m->vt[i * 2 + 1] = 0.0;
    m->vc[i * 3] = 1.0;
    m->vc[i * 3 + 1] = 1.0;
    m->vc[i * 3 + 2] = 1.0;
    return i;
}

void mesh_setuv(struct mesh *m, int i, double u, double v)
{
    if (i < 0 || i >= m->nv)
        return;
    m->vt[i * 2] = u;
    m->vt[i * 2 + 1] = v;
}

void mesh_setcol(struct mesh *m, int i, double r, double g, double b)
{
    if (i < 0 || i >= m->nv)
        return;
    m->vc[i * 3] = r;
    m->vc[i * 3 + 1] = g;
    m->vc[i * 3 + 2] = b;
    m->has_vcol = 1;
}

int mesh_addf(struct mesh *m, int a, int b, int c)
{
    if (a < 0 || b < 0 || c < 0 || a >= m->nv || b >= m->nv || c >= m->nv)
        return -1;
    if (a == b || b == c || a == c)
        return -1;
    mesh_growf(m);
    m->idx[m->nf * 3 + 0] = a;
    m->idx[m->nf * 3 + 1] = b;
    m->idx[m->nf * 3 + 2] = c;
    m->fmat[m->nf] = -1;
    m->fname[m->nf] = -1;
    return m->nf++;
}

/* Fans an n-gon into triangles, tagging each with the material name `name`
   (-1 for none). Every polygon-carrying format funnels through here. */
void mesh_addpoly(struct mesh *m, const int *v, int n, int name)
{
    for (int k = 1; k + 1 < n; k++)
    {
        int f = mesh_addf(m, v[0], v[k], v[k + 1]);
        if (f >= 0)
            m->fname[f] = name;
    }
}

/* Material names are interned per mesh: faces store an index, and mat_resolve
   later binds each index to a loaded material. */
int mesh_intern_name(struct mesh *m, const char *s)
{
    for (int i = 0; i < m->nfn; i++)
        if (!strcmp(m->fmtnames[i], s))
            return i;
    m->fmtnames = xrealloc(m->fmtnames, (size_t)(m->nfn + 1) * sizeof *m->fmtnames);
    m->fmtnames[m->nfn] = xstrdup(s);
    return m->nfn++;
}

static void addv(struct mesh *m, vec3 p) { mesh_addv(m, p); }

static void adduv(struct mesh *m, double u, double v)
{
    mesh_setuv(m, m->nv - 1, u, v);
}

static void addf(struct mesh *m, int a, int b, int c) { mesh_addf(m, a, b, c); }

static void addq(struct mesh *m, int a, int b, int c, int d)
{
    addf(m, a, b, c);
    addf(m, a, c, d);
}

static void mesh_compute_normals(struct mesh *m)
{
    double vol = 0.0;
    for (int f = 0; f < m->nf; f++)
    {
        int a = m->idx[f * 3], b = m->idx[f * 3 + 1], c = m->idx[f * 3 + 2];
        vec3 e1 = v3sub(m->v[b], m->v[a]);
        vec3 e2 = v3sub(m->v[c], m->v[a]);
        vol += v3dot(m->v[a], v3cross(e1, e2));
    }
    int flip = vol < 0.0;

    for (int i = 0; i < m->nv; i++)
        m->vn[i] = v3(0.0, 0.0, 0.0);

    for (int f = 0; f < m->nf; f++)
    {
        int a = m->idx[f * 3], b = m->idx[f * 3 + 1], c = m->idx[f * 3 + 2];
        if (flip)
        {
            int t = b;
            b = c;
            c = t;
        }
        vec3 e1 = v3sub(m->v[b], m->v[a]);
        vec3 e2 = v3sub(m->v[c], m->v[a]);
        vec3 n = v3norm(v3cross(e1, e2));
        m->fn[f] = n;
        m->vn[a] = v3(m->vn[a].x + n.x, m->vn[a].y + n.y, m->vn[a].z + n.z);
        m->vn[b] = v3(m->vn[b].x + n.x, m->vn[b].y + n.y, m->vn[b].z + n.z);
        m->vn[c] = v3(m->vn[c].x + n.x, m->vn[c].y + n.y, m->vn[c].z + n.z);
    }

    for (int i = 0; i < m->nv; i++)
        m->vn[i] = v3norm(m->vn[i]);
}

void mesh_normalize(struct mesh *m)
{
    if (!m->nv)
        return;
    vec3 ctr = v3(0.0, 0.0, 0.0);
    for (int i = 0; i < m->nv; i++)
        ctr = v3(ctr.x + m->v[i].x, ctr.y + m->v[i].y, ctr.z + m->v[i].z);
    ctr = v3(ctr.x / m->nv, ctr.y / m->nv, ctr.z / m->nv);
    double maxr2 = 0.0;
    for (int i = 0; i < m->nv; i++)
    {
        m->v[i] = v3sub(m->v[i], ctr);
        double r2 = v3dot(m->v[i], m->v[i]);
        if (r2 > maxr2)
            maxr2 = r2;
    }
    if (maxr2 <= 0.0)
        return;
    double k = 1.0 / sqrt(maxr2);
    for (int i = 0; i < m->nv; i++)
        m->v[i] = v3(m->v[i].x * k, m->v[i].y * k, m->v[i].z * k);
}

void mesh_gen_uvs(struct mesh *m, int mode)
{
    if (!m->nv)
        return;
    m->vt = xrealloc(m->vt, (size_t)m->nv * 2 * sizeof *m->vt);
    m->has_uv = 1;
    double xmin = 1e30, xmax = -1e30, ymin = 1e30, ymax = -1e30, zmin = 1e30, zmax = -1e30;
    for (int i = 0; i < m->nv; i++)
    {
        vec3 p = m->v[i];
        if (p.x < xmin)
            xmin = p.x;
        if (p.x > xmax)
            xmax = p.x;
        if (p.y < ymin)
            ymin = p.y;
        if (p.y > ymax)
            ymax = p.y;
        if (p.z < zmin)
            zmin = p.z;
        if (p.z > zmax)
            zmax = p.z;
    }
    double sx = xmax - xmin, sy = ymax - ymin, sz = zmax - zmin;
    if (sx < 1e-12)
        sx = 1.0;
    if (sy < 1e-12)
        sy = 1.0;
    if (sz < 1e-12)
        sz = 1.0;
    for (int i = 0; i < m->nv; i++)
    {
        vec3 p = m->v[i];
        double u = 0.0, v = 0.0;
        if (mode == UV_SPHERE)
        {
            double len = sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
            if (len <= 1e-12)
                len = 1.0;
            u = atan2(p.z, p.x) / (2.0 * M_PI) + 0.5;
            double cy = p.y / len;
            if (cy > 1.0)
                cy = 1.0;
            if (cy < -1.0)
                cy = -1.0;
            v = acos(cy) / M_PI;
        }
        else if (mode == UV_CYL)
        {
            u = atan2(p.z, p.x) / (2.0 * M_PI) + 0.5;
            v = 1.0 - (p.y - ymin) / sy;
        }
        else if (mode == UV_PLANE)
        {
            u = (p.x - xmin) / sx;
            v = 1.0 - (p.z - zmin) / sz;
        }
        else
        {
            vec3 n = m->vn[i];
            double ax = fabs(n.x), ay = fabs(n.y), az = fabs(n.z);
            if (ax >= ay && ax >= az)
            {
                u = (p.z - zmin) / sz;
                v = 1.0 - (p.y - ymin) / sy;
            }
            else if (ay >= az)
            {
                u = (p.x - xmin) / sx;
                v = (p.z - zmin) / sz;
            }
            else
            {
                u = (p.x - xmin) / sx;
                v = 1.0 - (p.y - ymin) / sy;
            }
        }
        m->vt[i * 2] = u;
        m->vt[i * 2 + 1] = v;
    }
}

static void mesh_cube(struct mesh *m, int seg)
{
    (void)seg;
    m->flat_default = 1;
    double s = 1.0;
    addv(m, v3(-s, -s, -s));
    addv(m, v3(s, -s, -s));
    addv(m, v3(s, s, -s));
    addv(m, v3(-s, s, -s));
    addv(m, v3(-s, -s, s));
    addv(m, v3(s, -s, s));
    addv(m, v3(s, s, s));
    addv(m, v3(-s, s, s));
    addq(m, 0, 1, 2, 3);
    addq(m, 5, 4, 7, 6);
    addq(m, 4, 0, 3, 7);
    addq(m, 1, 5, 6, 2);
    addq(m, 3, 2, 6, 7);
    addq(m, 4, 5, 1, 0);
}

static void mesh_plane(struct mesh *m, int n)
{
    m->flat_default = 1;
    for (int i = 0; i <= n; i++)
        for (int j = 0; j <= n; j++)
        {
            addv(m, v3(-1.0 + 2.0 * j / n, -0.25, -1.0 + 2.0 * i / n));
            adduv(m, (double)j / n, 1.0 - (double)i / n);
        }
    m->has_uv = 1;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
        {
            int a = i * (n + 1) + j;
            addq(m, a, a + 1, a + n + 2, a + n + 1);
        }
}

static void mesh_pyramid(struct mesh *m, int seg)
{
    (void)seg;
    m->flat_default = 1;
    double s = 1.0;
    addv(m, v3(-s, -s, -s));
    addv(m, v3(s, -s, -s));
    addv(m, v3(s, -s, s));
    addv(m, v3(-s, -s, s));
    addv(m, v3(0.0, 1.5, 0.0));
    addq(m, 3, 2, 1, 0);
    addf(m, 0, 1, 4);
    addf(m, 1, 2, 4);
    addf(m, 2, 3, 4);
    addf(m, 3, 0, 4);
}

static void mesh_octahedron(struct mesh *m, int seg)
{
    (void)seg;
    m->flat_default = 1;
    double s = 1.3;
    addv(m, v3(s, 0, 0));
    addv(m, v3(-s, 0, 0));
    addv(m, v3(0, s, 0));
    addv(m, v3(0, -s, 0));
    addv(m, v3(0, 0, s));
    addv(m, v3(0, 0, -s));
    addf(m, 0, 2, 4);
    addf(m, 2, 1, 4);
    addf(m, 1, 3, 4);
    addf(m, 3, 0, 4);
    addf(m, 2, 0, 5);
    addf(m, 1, 2, 5);
    addf(m, 3, 1, 5);
    addf(m, 0, 3, 5);
}

static void mesh_icosahedron(struct mesh *m, int seg)
{
    (void)seg;
    m->flat_default = 1;
    double t = 1.618, o = 1.0;
    addv(m, v3(-o, t, 0));
    addv(m, v3(o, t, 0));
    addv(m, v3(-o, -t, 0));
    addv(m, v3(o, -t, 0));
    addv(m, v3(0, -o, t));
    addv(m, v3(0, o, t));
    addv(m, v3(0, -o, -t));
    addv(m, v3(0, o, -t));
    addv(m, v3(t, 0, -o));
    addv(m, v3(t, 0, o));
    addv(m, v3(-t, 0, -o));
    addv(m, v3(-t, 0, o));
    static const int F[20][3] = {
        {0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8}, {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
    for (int i = 0; i < 20; i++)
        addf(m, F[i][0], F[i][1], F[i][2]);
}

static void mesh_sphere(struct mesh *m, int n)
{
    int rings = n, segs = n * 2;
    for (int i = 0; i <= rings; i++)
    {
        double a = M_PI * (double)i / rings;
        double st = sin(a), ct = cos(a);
        for (int j = 0; j <= segs; j++)
        {
            double b = 2.0 * M_PI * (double)j / segs;
            addv(m, v3(st * cos(b), ct, st * sin(b)));
            adduv(m, (double)j / segs, (double)i / rings);
        }
    }
    m->has_uv = 1;
    for (int i = 0; i < rings; i++)
        for (int j = 0; j < segs; j++)
        {
            int a = i * (segs + 1) + j, b = a + segs + 1;
            addq(m, a, a + 1, b + 1, b);
        }
}

static void mesh_torus(struct mesh *m, int n)
{
    int maj = n * 2, min = n;
    double R = 0.7, r = 0.3;
    for (int i = 0; i <= maj; i++)
    {
        double u = 2.0 * M_PI * (double)i / maj;
        double su = sin(u), cu = cos(u);
        for (int j = 0; j <= min; j++)
        {
            double v = 2.0 * M_PI * (double)j / min;
            double rx = R + r * cos(v);
            addv(m, v3(rx * cu, r * sin(v), rx * su));
            adduv(m, (double)i / maj, (double)j / min);
        }
    }
    m->has_uv = 1;
    for (int i = 0; i < maj; i++)
        for (int j = 0; j < min; j++)
        {
            int a = i * (min + 1) + j, b = a + min + 1;
            addq(m, a, a + 1, b + 1, b);
        }
}

static void mesh_knot(struct mesh *m, int n)
{
    int steps = n * 8, ring = n;
    double rad = 0.22;
    double *cx = xrealloc(NULL, (size_t)steps * sizeof *cx);
    double *cy = xrealloc(NULL, (size_t)steps * sizeof *cy);
    double *cz = xrealloc(NULL, (size_t)steps * sizeof *cz);
    for (int i = 0; i < steps; i++)
    {
        double t = 2.0 * M_PI * (double)i / steps;
        double rr = 2.0 + cos(3.0 * t);
        cx[i] = rr * cos(2.0 * t);
        cy[i] = rr * sin(2.0 * t);
        cz[i] = 2.0 * sin(3.0 * t);
    }
    vec3 u = v3(1.0, 0.0, 0.0);
    vec3 *ring0 = xrealloc(NULL, (size_t)(ring + 1) * sizeof *ring0);
    for (int s = 0; s <= steps; s++)
    {
        int i = s % steps;
        int j = (i + 1) % steps;
        vec3 c0 = v3(cx[i], cy[i], cz[i]), c1 = v3(cx[j], cy[j], cz[j]);
        vec3 tang = v3norm(v3sub(c1, c0));
        double d = v3dot(u, tang);
        u = v3norm(v3(u.x - d * tang.x, u.y - d * tang.y, u.z - d * tang.z));
        if (u.x == 0.0 && u.y == 0.0 && u.z == 0.0)
            u = v3(0.0, 1.0, 0.0);
        vec3 w = v3cross(tang, u);
        for (int a = 0; a <= ring; a++)
        {
            double b = 2.0 * M_PI * (double)a / ring;
            double sv = sin(b), cv = cos(b);
            if (s == 0)
            {
                ring0[a] = v3(c0.x + rad * (cv * u.x + sv * w.x),
                              c0.y + rad * (cv * u.y + sv * w.y),
                              c0.z + rad * (cv * u.z + sv * w.z));
                addv(m, ring0[a]);
            }
            else if (s == steps)
            {
                addv(m, ring0[a]);
            }
            else
            {
                addv(m, v3(c0.x + rad * (cv * u.x + sv * w.x),
                           c0.y + rad * (cv * u.y + sv * w.y),
                           c0.z + rad * (cv * u.z + sv * w.z)));
            }
            adduv(m, (double)s / steps, (double)a / ring);
        }
    }
    free(ring0);
    free(cx);
    free(cy);
    free(cz);
    m->has_uv = 1;
    int stride = ring + 1;
    for (int i = 0; i < steps; i++)
    {
        for (int a = 0; a < ring; a++)
        {
            int p0 = i * stride + a, p1 = p0 + 1;
            int p2 = (i + 1) * stride + a + 1, p3 = (i + 1) * stride + a;
            addq(m, p0, p1, p2, p3);
        }
    }
}

static void mesh_cylinder(struct mesh *m, int n)
{
    m->flat_default = 1;
    int segs = n * 2;
    for (int i = 0; i <= segs; i++)
    {
        double a = 2.0 * M_PI * (double)i / segs;
        addv(m, v3(cos(a), -1.0, sin(a)));
        adduv(m, (double)i / segs, 1.0);
    }
    for (int i = 0; i <= segs; i++)
    {
        double a = 2.0 * M_PI * (double)i / segs;
        addv(m, v3(cos(a), 1.0, sin(a)));
        adduv(m, (double)i / segs, 0.0);
    }
    addv(m, v3(0.0, -1.0, 0.0));
    adduv(m, 0.5, 0.5);
    int bot = m->nv - 1;
    addv(m, v3(0.0, 1.0, 0.0));
    adduv(m, 0.5, 0.5);
    int top = m->nv - 1;
    m->has_uv = 1;
    int stride = segs + 1;
    for (int i = 0; i < segs; i++)
    {
        addq(m, i, i + 1, stride + i + 1, stride + i);
        addf(m, bot, i + 1, i);
        addf(m, top, stride + i, stride + i + 1);
    }
}

static void mesh_cone(struct mesh *m, int n)
{
    m->flat_default = 1;
    int segs = n * 2;
    for (int i = 0; i <= segs; i++)
    {
        double a = 2.0 * M_PI * (double)i / segs;
        addv(m, v3(cos(a), -1.0, sin(a)));
        adduv(m, (double)i / segs, 1.0);
    }
    addv(m, v3(0.0, 1.5, 0.0));
    adduv(m, 0.5, 0.0);
    int apex = m->nv - 1;
    addv(m, v3(0.0, -1.0, 0.0));
    adduv(m, 0.5, 0.5);
    int base = m->nv - 1;
    m->has_uv = 1;
    for (int i = 0; i < segs; i++)
    {
        addf(m, i, i + 1, apex);
        addf(m, base, i + 1, i);
    }
}

/* Here we store the models again, this is mostly used now for testing and might get removed. */
const struct b3d_model B3D_MODELS[] = {
    {"cube", 12, mesh_cube},
    {"sphere", 16, mesh_sphere},
    {"torus", 14, mesh_torus},
    {"knot", 10, mesh_knot},
    {"pyramid", 12, mesh_pyramid},
    {"cone", 16, mesh_cone},
    {"cylinder", 14, mesh_cylinder},
    {"octahedron", 12, mesh_octahedron},
    {"icosahedron", 12, mesh_icosahedron},
    {"plane", 10, mesh_plane},
};
const int B3D_NMODELS = (int)(sizeof B3D_MODELS / sizeof *B3D_MODELS);

const struct b3d_model *b3d_model_find(const char *name)
{
    for (int i = 0; i < (int)(sizeof B3D_MODELS / sizeof *B3D_MODELS); i++)
        if (!strcmp(B3D_MODELS[i].name, name))
            return &B3D_MODELS[i];
    return NULL;
}

int mesh_build(void)
{
    struct mesh *m = &g.mesh;
    struct options *o = &g.opt;
    mesh_free(m);
    if (o->objfile[0])
    {
        model_load(m, o->objfile);
    }
    else
    {
        const struct b3d_model *md = b3d_model_find(o->model);
        if (!md)
            die("unknown model '%s' (try --list)", o->model);
        md->build(m, o->seg ? o->seg : md->seg);
        mesh_normalize(m);
    }
    mesh_compute_normals(m);
    if (!m->has_uv || o->uv_mode != UV_AUTO)
        mesh_gen_uvs(m, o->uv_mode != UV_AUTO ? o->uv_mode : UV_BOX);
    return 0;
}
