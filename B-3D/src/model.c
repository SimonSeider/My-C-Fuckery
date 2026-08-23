#include "b3d.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Reads a whole file into memory. The bin formats all need random access
   to a header that sits after the payload, or a size to regonize different variants,
   so slurping this shit is simpler and faster than seeking around. */
static unsigned char *slurp(const char *path, size_t *outn)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        die("cannot read '%s'", path);
    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        die("cannot size '%s'", path);
    }
    long n = ftell(f);
    if (n < 0)
    {
        fclose(f);
        die("cannot size '%s'", path);
    }
    rewind(f);
    unsigned char *buf = xrealloc(NULL, (size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    *outn = got;
    return buf;
}

struct weld
{
    vec3 p;
    int id;
};

struct weldtab
{
    struct weld *e;
    int mask, count;
};

static void weld_init(struct weldtab *t)
{
    t->mask = 4095;
    t->count = 0;
    t->e = xrealloc(NULL, (size_t)(t->mask + 1) * sizeof *t->e);
    for (int i = 0; i <= t->mask; i++)
        t->e[i].id = -1;
}

static unsigned weld_hash(vec3 p)
{
    unsigned h = 2166136261u;
    const unsigned char *b = (const unsigned char *)&p;
    for (size_t i = 0; i < sizeof p; i++)
        h = (h ^ b[i]) * 16777619u;
    return h;
}

static void weld_put(struct weld *e, int mask, vec3 p, int id)
{
    unsigned h = weld_hash(p) & (unsigned)mask;
    for (;; h = (h + 1) & (unsigned)mask)
        if (e[h].id == -1)
        {
            e[h].p = p;
            e[h].id = id;
            return;
        }
}

static int weld_vertex(struct weldtab *t, struct mesh *m, vec3 p)
{
    unsigned h = weld_hash(p) & (unsigned)t->mask;
    for (;; h = (h + 1) & (unsigned)t->mask)
    {
        if (t->e[h].id == -1)
            break;
        vec3 q = t->e[h].p;
        if (q.x == p.x && q.y == p.y && q.z == p.z)
            return t->e[h].id;
    }
    int id = mesh_addv(m, p);
    if (t->count * 10 >= (t->mask + 1) * 7)
    {
        int nmask = t->mask * 2 + 1;
        struct weld *ne = xrealloc(NULL, (size_t)(nmask + 1) * sizeof *ne);
        for (int i = 0; i <= nmask; i++)
            ne[i].id = -1;
        for (int i = 0; i <= t->mask; i++)
            if (t->e[i].id >= 0)
                weld_put(ne, nmask, t->e[i].p, t->e[i].id);
        free(t->e);
        t->e = ne;
        t->mask = nmask;
    }
    weld_put(t->e, t->mask, p, id);
    t->count++;
    return id;
}

static void weld_free(struct weldtab *t) { free(t->e); }

static float rdf32(const unsigned char *p)
{
    unsigned u = (unsigned)p[0] | ((unsigned)p[1] << 8) |
                 ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static unsigned rdu32(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

/* Wavefront parsing */

struct hent
{
    int v, t, id;
};

static unsigned hv_hash(int v, int t)
{
    return ((unsigned)v * 2654435761u) ^ ((unsigned)t * 97531u);
}

static int hv_find(struct hent *tab, int mask, int v, int t)
{
    unsigned h = hv_hash(v, t) & (unsigned)mask;
    for (;; h = (h + 1) & (unsigned)mask)
    {
        if (tab[h].id == -1)
            return -1;
        if (tab[h].v == v && tab[h].t == t)
            return tab[h].id;
    }
}

static void hv_put(struct hent *tab, int mask, int v, int t, int id)
{
    unsigned h = hv_hash(v, t) & (unsigned)mask;
    for (;; h = (h + 1) & (unsigned)mask)
    {
        if (tab[h].id == -1)
        {
            tab[h].v = v;
            tab[h].t = t;
            tab[h].id = id;
            return;
        }
        if (tab[h].v == v && tab[h].t == t)
            return;
    }
}

static int obj_corner(struct mesh *m, struct hent **tab, int *mask, int *count,
                      const double *tuv, int ntuv, int vid, int tid)
{
    if (tid < 0 || tid >= ntuv || !tuv)
        return vid;
    int found = hv_find(*tab, *mask, vid, tid);
    if (found >= 0)
        return found;
    int nid = mesh_addv(m, m->v[vid]);
    mesh_setuv(m, nid, tuv[tid * 2], tuv[tid * 2 + 1]);
    if (m->has_vcol)
        mesh_setcol(m, nid, m->vc[vid * 3], m->vc[vid * 3 + 1], m->vc[vid * 3 + 2]);
    if (*count * 10 >= (*mask + 1) * 7)
    {
        int nmask = *mask * 2 + 1;
        struct hent *ntab = xrealloc(NULL, (size_t)(nmask + 1) * sizeof *ntab);
        for (int i = 0; i <= nmask; i++)
            ntab[i].id = -1;
        for (int i = 0; i <= *mask; i++)
            if ((*tab)[i].id >= 0)
                hv_put(ntab, nmask, (*tab)[i].v, (*tab)[i].t, (*tab)[i].id);
        free(*tab);
        *tab = ntab;
        *mask = nmask;
    }
    hv_put(*tab, *mask, vid, tid, nid);
    (*count)++;
    return nid;
}
/* TODO: Dont use sscanf so much since it could cause a stack overflow*/
void obj_load(struct mesh *m, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        die("cannot read '%s'", path);
    char line[8192];
    double *tuv = NULL;
    int ntuv = 0, captuv = 0;
    struct hent *tab = xrealloc(NULL, 1024 * sizeof *tab);
    int mask = 1023, hcount = 0;
    for (int i = 0; i < 1024; i++)
        tab[i].id = -1;
    int *vmap = NULL;
    int nvfile = 0, capvmap = 0;
    int cur_name = -1;
    while (fgets(line, sizeof line, f))
    {
        const char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (!strncmp(p, "vt", 2) && (p[2] == ' ' || p[2] == '\t'))
        {
            char *e;
            double u = strtod(p + 2, &e);
            double v = (e > p + 2) ? strtod(e, &e) : 0.0;
            if (ntuv >= captuv)
            {
                captuv = captuv ? captuv * 2 : 256;
                tuv = xrealloc(tuv, (size_t)captuv * 2 * sizeof *tuv);
            }
            tuv[ntuv * 2] = u;
            tuv[ntuv * 2 + 1] = 1.0 - v;
            ntuv++;
            continue;
        }
        if (!strncmp(p, "usemtl", 6) && (p[6] == ' ' || p[6] == '\t'))
        {
            char nm[128];
            if (sscanf(p + 6, " %127s", nm) == 1)
                cur_name = mesh_intern_name(m, nm);
            continue;
        }
        if (!strncmp(p, "mtllib", 6) && (p[6] == ' ' || p[6] == '\t'))
        {
            char nm[1024];
            if (sscanf(p + 6, " %1023s", nm) == 1 && m->nmtllib < 8)
                m->mtllib[m->nmtllib++] = xstrdup(nm);
            continue;
        }
        if (p[0] != 'v' && p[0] != 'f')
            continue;
        if (p[1] != ' ' && p[1] != '\t')
            continue;
        if (p[0] == 'v')
        {
            char *e;
            double x = strtod(p + 2, &e);
            const char *after_x = e;
            double y = (e > p + 2) ? strtod(e, &e) : 0.0;
            double z = (e > after_x) ? strtod(e, &e) : 0.0;
            if (nvfile >= capvmap)
            {
                capvmap = capvmap ? capvmap * 2 : 256;
                vmap = xrealloc(vmap, (size_t)capvmap * sizeof *vmap);
            }
            int id = mesh_addv(m, v3(x, y, z));
            vmap[nvfile++] = id;
            /* 'v x y z r g b' is a widespread extension for vertex colours. */
            const char *before = e;
            double r = strtod(e, &e);
            if (e > before)
            {
                before = e;
                double gg = strtod(e, &e);
                if (e > before)
                {
                    before = e;
                    double b = strtod(e, &e);
                    if (e > before)
                        mesh_setcol(m, id, r, gg, b);
                }
            }
        }
        else
        {
            int vi[64], ti[64], np = 0;
            char *q = (char *)p + 2;
            while (*q && np < 64)
            {
                while (*q == ' ' || *q == '\t')
                    q++;
                if (!*q || *q == '\n' || *q == '\r')
                    break;
                char *tok = q;
                while (*q && *q != ' ' && *q != '\t' && *q != '\n' && *q != '\r')
                    q++;
                if (*q)
                    *q++ = '\0';
                char *endp;
                long vv = strtol(tok, &endp, 10);
                if (endp == tok)
                    continue;
                long tt = 0;
                if (*endp == '/')
                {
                    endp++;
                    if (*endp != '/')
                        tt = strtol(endp, &endp, 10);
                    else
                        endp++;
                }
                int id;
                if (vv < 0)
                {
                    if (vv < -nvfile)
                        continue;
                    id = vmap[nvfile + (int)vv];
                }
                else
                {
                    /* OBJ indices are 1-based, so 0 names no vertex. */
                    if (vv < 1 || vv > nvfile)
                        continue;
                    id = vmap[(int)vv - 1];
                }
                int td = -1;
                if (tt != 0)
                    td = tt < 0 ? ntuv + (int)tt : (int)tt - 1;
                vi[np] = id;
                ti[np] = td;
                np++;
            }
            for (int k = 1; k + 1 < np; k++)
            {
                int a = obj_corner(m, &tab, &mask, &hcount, tuv, ntuv, vi[0], ti[0]);
                int b = obj_corner(m, &tab, &mask, &hcount, tuv, ntuv, vi[k], ti[k]);
                int c = obj_corner(m, &tab, &mask, &hcount, tuv, ntuv, vi[k + 1], ti[k + 1]);
                int fi = mesh_addf(m, a, b, c);
                if (fi >= 0)
                    m->fname[fi] = cur_name;
            }
        }
    }
    free(tab);
    free(tuv);
    free(vmap);
    fclose(f);
    if (ntuv)
        m->has_uv = 1;
}

/* STL Parsing */

/* A binary STL is exactly 84 + 50*n bytes. ASCII files *should* never match that, which
   is the only reliable discriminator. */
static int stl_is_binary(const unsigned char *b, size_t n)
{
    if (n < 84)
        return 0;
    unsigned tris = rdu32(b + 80);
    if (tris > (0xFFFFFFFFu - 84) / 50)
        return 0;
    return (size_t)tris * 50 + 84 == n;
}

static void stl_binary(struct mesh *m, const unsigned char *b, size_t n)
{
    unsigned tris = rdu32(b + 80);
    struct weldtab wt;
    weld_init(&wt);
    for (unsigned i = 0; i < tris; i++)
    {
        const unsigned char *t = b + 84 + (size_t)i * 50;
        if ((size_t)(t - b) + 50 > n)
            break;
        int id[3];
        for (int k = 0; k < 3; k++)
        {
            const unsigned char *p = t + 12 + k * 12;
            id[k] = weld_vertex(&wt, m, v3(rdf32(p), rdf32(p + 4), rdf32(p + 8)));
        }
        unsigned attr = (unsigned)t[48] | ((unsigned)t[49] << 8);
        if (attr & 0x8000u)
        {
            double r = ((attr >> 10) & 31) / 31.0;
            double gg = ((attr >> 5) & 31) / 31.0;
            double bb = (attr & 31) / 31.0;
            for (int k = 0; k < 3; k++)
                mesh_setcol(m, id[k], r, gg, bb);
        }
        mesh_addf(m, id[0], id[1], id[2]);
    }
    weld_free(&wt);
}

static void stl_ascii(struct mesh *m, const char *s)
{
    struct weldtab wt;
    weld_init(&wt);
    int tri[3], np = 0;
    for (const char *p = s; *p;)
    {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (!*p)
            break;
        if (!strncmp(p, "vertex", 6) && (p[6] == ' ' || p[6] == '\t'))
        {
            char *e;
            double x = strtod(p + 6, &e);
            double y = strtod(e, &e);
            double z = strtod(e, &e);
            if (np < 3)
                tri[np] = weld_vertex(&wt, m, v3(x, y, z));
            np++;
            p = e;
            continue;
        }
        if (!strncmp(p, "outer", 5))
            np = 0;
        else if (!strncmp(p, "endloop", 7) && np == 3)
            mesh_addf(m, tri[0], tri[1], tri[2]);
        while (*p && *p != '\n')
            p++;
    }
    weld_free(&wt);
}

void stl_load(struct mesh *m, const char *path)
{
    size_t n = 0;
    unsigned char *b = slurp(path, &n);
    m->flat_default = 1;
    if (stl_is_binary(b, n))
        stl_binary(m, b, n);
    else
        stl_ascii(m, (const char *)b);
    free(b);
}

/* PLY Parsing */

enum
{
    PLY_ASCII,
    PLY_LE,
    PLY_BE
};

/* Scalar types a PLY property can take. The width table doubles as the
   "skip this many bytes" rule for properties we don't give a shit about. */
enum
{
    PT_NONE,
    PT_I8,
    PT_U8,
    PT_I16,
    PT_U16,
    PT_I32,
    PT_U32,
    PT_F32,
    PT_F64
};

/* Where a property lands in our mesh. */
enum
{
    PP_SKIP,
    PP_X,
    PP_Y,
    PP_Z,
    PP_U,
    PP_V,
    PP_R,
    PP_G,
    PP_B
};

struct ply_prop
{
    int type;
    int list_type;
    int slot;
    int is_index;
};

struct ply_elem
{
    char name[64];
    long count;
    struct ply_prop *props;
    int nprops;
};

static int ply_type(const char *s)
{
    if (!strcmp(s, "char") || !strcmp(s, "int8"))
        return PT_I8;
    if (!strcmp(s, "uchar") || !strcmp(s, "uint8"))
        return PT_U8;
    if (!strcmp(s, "short") || !strcmp(s, "int16"))
        return PT_I16;
    if (!strcmp(s, "ushort") || !strcmp(s, "uint16"))
        return PT_U16;
    if (!strcmp(s, "int") || !strcmp(s, "int32"))
        return PT_I32;
    if (!strcmp(s, "uint") || !strcmp(s, "uint32"))
        return PT_U32;
    if (!strcmp(s, "float") || !strcmp(s, "float32"))
        return PT_F32;
    if (!strcmp(s, "double") || !strcmp(s, "float64"))
        return PT_F64;
    return PT_NONE;
}

static int ply_width(int t)
{
    switch (t)
    {
    case PT_I8:
    case PT_U8:
        return 1;
    case PT_I16:
    case PT_U16:
        return 2;
    case PT_I32:
    case PT_U32:
    case PT_F32:
        return 4;
    case PT_F64:
        return 8;
    default:
        return 0;
    }
}

static int ply_slot(const char *n)
{
    if (!strcmp(n, "x"))
        return PP_X;
    if (!strcmp(n, "y"))
        return PP_Y;
    if (!strcmp(n, "z"))
        return PP_Z;
    if (!strcmp(n, "s") || !strcmp(n, "u") || !strcmp(n, "texture_u"))
        return PP_U;
    if (!strcmp(n, "t") || !strcmp(n, "v") || !strcmp(n, "texture_v"))
        return PP_V;
    if (!strcmp(n, "red") || !strcmp(n, "r") || !strcmp(n, "diffuse_red"))
        return PP_R;
    if (!strcmp(n, "green") || !strcmp(n, "g") || !strcmp(n, "diffuse_green"))
        return PP_G;
    if (!strcmp(n, "blue") || !strcmp(n, "b") || !strcmp(n, "diffuse_blue"))
        return PP_B;
    return PP_SKIP;
}

struct ply_rd
{
    const unsigned char *p;
    const unsigned char *end;
    int order;
};

static double ply_bin(struct ply_rd *r, int t)
{
    int w = ply_width(t);
    if (!w || r->p + w > r->end)
    {
        r->p = r->end;
        return 0.0;
    }
    unsigned char b[8];
    memcpy(b, r->p, (size_t)w);
    r->p += w;
    if (r->order == PLY_BE)
        for (int i = 0; i < w / 2; i++)
        {
            unsigned char t2 = b[i];
            b[i] = b[w - 1 - i];
            b[w - 1 - i] = t2;
        }
    switch (t)
    {
    case PT_I8:
        return (double)(signed char)b[0];
    case PT_U8:
        return (double)b[0];
    case PT_I16:
        return (double)(short)((unsigned)b[0] | ((unsigned)b[1] << 8));
    case PT_U16:
        return (double)(unsigned short)((unsigned)b[0] | ((unsigned)b[1] << 8));
    case PT_I32:
        return (double)(int)rdu32(b);
    case PT_U32:
        return (double)rdu32(b);
    case PT_F32:
        return (double)rdf32(b);
    default:
    {
        double d;
        memcpy(&d, b, 8);
        return d;
    }
    }
}

static double ply_num(struct ply_rd *r, int t)
{
    if (r->order != PLY_ASCII)
        return ply_bin(r, t);
    while (r->p < r->end && isspace(*r->p))
        r->p++;
    char *e;
    double v = strtod((const char *)r->p, &e);
    if ((const unsigned char *)e > r->p)
        r->p = (const unsigned char *)e;
    else
        r->p = r->end;
    return v;
}

/* Colour channels are integers 0..255 except when stored as float, where the
   convention is already 0..1. */
static double ply_colour(double v, int t)
{
    return (t == PT_F32 || t == PT_F64) ? v : v / 255.0;
}

void ply_load(struct mesh *m, const char *path)
{
    size_t n = 0;
    unsigned char *buf = slurp(path, &n);
    const char *s = (const char *)buf;
    if (n < 4 || strncmp(s, "ply", 3))
        die("'%s' is not a PLY file", path);

    struct ply_elem *els = NULL;
    int nels = 0;
    int order = -1;
    const char *p = s;
    const char *body = NULL;
    char line[1024];
    while (p < s + n)
    {
        const char *nl = memchr(p, '\n', (size_t)(s + n - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(s + n - p);
        if (len >= sizeof line)
            len = sizeof line - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        while (len && (line[len - 1] == '\r' || line[len - 1] == ' '))
            line[--len] = '\0';
        p = nl ? nl + 1 : s + n;
        if (!strncmp(line, "format", 6))
        {
            if (strstr(line, "ascii"))
                order = PLY_ASCII;
            else if (strstr(line, "binary_little_endian"))
                order = PLY_LE;
            else if (strstr(line, "binary_big_endian"))
                order = PLY_BE;
        }
        else if (!strncmp(line, "element", 7))
        {
            char nm[64];
            long cnt = 0;
            if (sscanf(line + 7, " %63s %ld", nm, &cnt) == 2)
            {
                els = xrealloc(els, (size_t)(nels + 1) * sizeof *els);
                memset(&els[nels], 0, sizeof els[nels]);
                snprintf(els[nels].name, sizeof els[nels].name, "%s", nm);
                els[nels].count = cnt < 0 ? 0 : cnt;
                nels++;
            }
        }
        else if (!strncmp(line, "property", 8) && nels)
        {
            struct ply_elem *e = &els[nels - 1];
            char a[64], b[64], c[64];
            struct ply_prop pr;
            memset(&pr, 0, sizeof pr);
            if (sscanf(line + 8, " list %63s %63s %63s", a, b, c) == 3)
            {
                pr.list_type = ply_type(a);
                pr.type = ply_type(b);
                pr.is_index = !strcmp(c, "vertex_indices") || !strcmp(c, "vertex_index");
                pr.slot = PP_SKIP;
            }
            else if (sscanf(line + 8, " %63s %63s", a, b) == 2)
            {
                pr.list_type = PT_NONE;
                pr.type = ply_type(a);
                pr.slot = ply_slot(b);
            }
            else
            {
                continue;
            }
            e->props = xrealloc(e->props, (size_t)(e->nprops + 1) * sizeof *e->props);
            e->props[e->nprops++] = pr;
        }
        else if (!strcmp(line, "end_header"))
        {
            body = p;
            break;
        }
    }
    if (!body || order < 0)
    {
        free(buf);
        die("'%s' has a malformed PLY header", path);
    }

    struct ply_rd rd = {(const unsigned char *)body, buf + n, order};
    int base = m->nv;
    for (int ei = 0; ei < nels; ei++)
    {
        struct ply_elem *e = &els[ei];
        int isv = !strcmp(e->name, "vertex");
        int isf = !strcmp(e->name, "face");
        for (long i = 0; i < e->count && rd.p < rd.end; i++)
        {
            double val[9] = {0, 0, 0, 0, 0, 0, 1, 1, 1};
            int have_uv = 0, have_col = 0;
            int idx[256];
            int nidx = 0;
            for (int pi = 0; pi < e->nprops; pi++)
            {
                struct ply_prop *pr = &e->props[pi];
                if (pr->list_type != PT_NONE)
                {
                    long cnt = (long)ply_num(&rd, pr->list_type);
                    if (cnt < 0)
                        cnt = 0;
                    for (long k = 0; k < cnt; k++)
                    {
                        double v = ply_num(&rd, pr->type);
                        if (pr->is_index && nidx < 256)
                            idx[nidx++] = (int)v + base;
                    }
                    continue;
                }
                double v = ply_num(&rd, pr->type);
                if (pr->slot == PP_SKIP)
                    continue;
                if (pr->slot == PP_U || pr->slot == PP_V)
                    have_uv = 1;
                if (pr->slot >= PP_R)
                {
                    have_col = 1;
                    v = ply_colour(v, pr->type);
                }
                val[pr->slot - 1] = v;
            }
            if (isv)
            {
                int id = mesh_addv(m, v3(val[0], val[1], val[2]));
                if (have_uv)
                {
                    mesh_setuv(m, id, val[3], 1.0 - val[4]);
                    m->has_uv = 1;
                }
                if (have_col)
                    mesh_setcol(m, id, val[5], val[6], val[7]);
            }
            else if (isf)
            {
                mesh_addpoly(m, idx, nidx, -1);
            }
        }
    }
    for (int i = 0; i < nels; i++)
        free(els[i].props);
    free(els);
    free(buf);
}

/* OFF Parser */

/* Reads the next whitespace-separated token, skipping '#' comments. */
static const char *off_tok(const char **pp, char *out, size_t cap)
{
    const char *p = *pp;
    for (;;)
    {
        while (*p && isspace((unsigned char)*p))
            p++;
        if (*p == '#')
        {
            while (*p && *p != '\n')
                p++;
            continue;
        }
        break;
    }
    if (!*p)
        return NULL;
    size_t k = 0;
    while (*p && !isspace((unsigned char)*p))
    {
        if (k + 1 < cap)
            out[k++] = *p;
        p++;
    }
    out[k] = '\0';
    *pp = p;
    return out;
}

static int off_num(const char **pp, double *out)
{
    char t[128];
    if (!off_tok(pp, t, sizeof t))
        return 0;
    char *e;
    double v = strtod(t, &e);
    if (e == t)
        return 0;
    *out = v;
    return 1;
}

void off_load(struct mesh *m, const char *path)
{
    size_t n = 0;
    unsigned char *buf = slurp(path, &n);
    const char *p = (const char *)buf;
    char tok[128];
    if (!off_tok(&p, tok, sizeof tok))
    {
        free(buf);
        die("'%s' is empty", path);
    }
    /* The magic may be OFF, COFF (colours), NOFF (normals), STOFF, 4OFF so we never know and why am i handling this stupid ah model format?? */
    const char *off = strstr(tok, "OFF");
    if (!off)
    {
        free(buf);
        die("'%s' is not an OFF file", path);
    }
    int has_col = strchr(tok, 'C') != NULL;
    int has_nrm = strchr(tok, 'N') != NULL;
    int has_st = strstr(tok, "ST") != NULL;
    int dim4 = tok[0] == '4';

    double nv = 0, nf = 0, ne = 0;
    if (!off_num(&p, &nv) || !off_num(&p, &nf) || !off_num(&p, &ne))
    {
        free(buf);
        die("'%s' has a malformed OFF header", path);
    }
    (void)ne;
    int base = m->nv;
    for (long i = 0; i < (long)nv; i++)
    {
        double x = 0, y = 0, z = 0, w = 0;
        if (!off_num(&p, &x) || !off_num(&p, &y) || !off_num(&p, &z))
            break;
        if (dim4)
            off_num(&p, &w);
        int id = mesh_addv(m, v3(x, y, z));
        if (has_nrm)
        {
            double t;
            off_num(&p, &t);
            off_num(&p, &t);
            off_num(&p, &t);
        }
        if (has_col)
        {
            double c[4] = {1, 1, 1, 1};
            for (int k = 0; k < 3; k++)
                off_num(&p, &c[k]);
            const char *save = p;
            double a;
            if (!off_num(&p, &a) || a < 0.0 || a > 255.0)
                p = save;
            double sc = (c[0] > 1.0 || c[1] > 1.0 || c[2] > 1.0) ? 1.0 / 255.0 : 1.0;
            mesh_setcol(m, id, c[0] * sc, c[1] * sc, c[2] * sc);
        }
        if (has_st)
        {
            double u = 0, v = 0;
            off_num(&p, &u);
            off_num(&p, &v);
            mesh_setuv(m, id, u, 1.0 - v);
            m->has_uv = 1;
        }
    }
    for (long i = 0; i < (long)nf; i++)
    {
        double cnt = 0;
        if (!off_num(&p, &cnt))
            break;
        int k = (int)cnt;
        if (k < 3 || k > 256)
            break;
        int idx[256];
        int ok = 1;
        for (int j = 0; j < k; j++)
        {
            double v;
            if (!off_num(&p, &v))
            {
                ok = 0;
                break;
            }
            idx[j] = (int)v + base;
        }
        if (!ok)
            break;
        /* A face may carry a trailing colour on its own line. We have no
           per-face colour, but it still has to be consumed or the next face
           count would be read from it. So there has to be so much handling
           im considering to touch grass. */
        const char *eol = p;
        int leftover = 0;
        while (*eol && *eol != '\n')
        {
            while (*eol == ' ' || *eol == '\t' || *eol == '\r')
                eol++;
            if (!*eol || *eol == '\n')
                break;
            leftover++;
            while (*eol && !isspace((unsigned char)*eol))
                eol++;
        }
        if (leftover == 3 || leftover == 4)
            p = eol;
        mesh_addpoly(m, idx, k, -1);
    }
    free(buf);
}

/* The formats we support so far */
const struct b3d_format B3D_FORMATS[] = {
    {"obj", "Wavefront OBJ", obj_load},
    {"stl", "STL, binary + ASCII", stl_load},
    {"ply", "Stanford PLY, ASCII + binary", ply_load},
    {"off", "Geomview OFF / COFF / NOFF", off_load},
    {"gltf", "glTF 2.0, JSON", gltf_load},
    {"glb", "glTF 2.0, binary", gltf_load},
};
const int B3D_NFORMATS = (int)(sizeof B3D_FORMATS / sizeof *B3D_FORMATS);

const struct b3d_format *b3d_format_find(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot)
        return NULL;
    char ext[16];
    size_t k = 0;
    for (const char *q = dot + 1; *q && k + 1 < sizeof ext; q++)
        ext[k++] = (char)tolower((unsigned char)*q);
    ext[k] = '\0';
    for (int i = 0; i < B3D_NFORMATS; i++)
        if (!strcmp(B3D_FORMATS[i].ext, ext))
            return &B3D_FORMATS[i];
    return NULL;
}

/* Extension-less or misnamed files still load here, the first bytes identify every
   format we support well enough to pick a parser. */
static const struct b3d_format *format_sniff(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        die("cannot read '%s'", path);
    char h[64];
    size_t got = fread(h, 1, sizeof h - 1, f);
    fclose(f);
    h[got] = '\0';
    const char *ext = NULL;
    if (got >= 4 && !memcmp(h, "glTF", 4))
        ext = "glb";
    else if (got >= 3 && !memcmp(h, "ply", 3))
        ext = "ply";
    else if (got >= 3 && (!memcmp(h, "OFF", 3) || strstr(h, "OFF") == h + 1))
        ext = "off";
    else if (got >= 5 && !memcmp(h, "solid", 5))
        ext = "stl";
    else
    {
        for (size_t i = 0; i < got; i++)
            if (h[i] == '{')
            {
                ext = "gltf";
                break;
            }
            else if (!isspace((unsigned char)h[i]))
                break;
    }
    if (!ext)
        return NULL;
    for (int i = 0; i < B3D_NFORMATS; i++)
        if (!strcmp(B3D_FORMATS[i].ext, ext))
            return &B3D_FORMATS[i];
    return NULL;
}

void model_load(struct mesh *m, const char *path)
{
    const struct b3d_format *fmt = b3d_format_find(path);
    if (!fmt)
        fmt = format_sniff(path);
    if (!fmt)
        fmt = &B3D_FORMATS[0]; /* an unknown extension is most often an .obj but could be somth also so idk */
    fmt->load(m, path);
    if (!m->nv || !m->nf)
        die("'%s' contains no usable geometry", path);
    mesh_normalize(m);
}
