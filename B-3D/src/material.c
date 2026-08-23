#include "b3d.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int mat_find(const char *name)
{
    for (int i = 0; i < g.nmats; i++)
        if (!strcmp(g.mats[i].name, name))
            return i;
    return -1;
}

static int mat_upsert(const struct material *mt)
{
    int id = mat_find(mt->name);
    if (id >= 0)
    {
        g.mats[id] = *mt;
        return id;
    }
    if (g.nmats >= g.capmats)
    {
        g.capmats = g.capmats ? g.capmats * 2 : 8;
        g.mats = xrealloc(g.mats, (size_t)g.capmats * sizeof *g.mats);
    }
    g.mats[g.nmats] = *mt;
    return g.nmats++;
}

int mat_register(const struct material *mt)
{
    return mat_upsert(mt);
}

static struct texture *map_texture(char *args, const char *dir)
{
    char *q = args, *last = NULL;
    while (*q)
    {
        while (*q == ' ' || *q == '\t')
            q++;
        if (!*q || *q == '\n' || *q == '\r')
            break;
        last = q;
        while (*q && *q != ' ' && *q != '\t' && *q != '\n' && *q != '\r')
            q++;
        if (*q)
            *q++ = '\0';
    }
    if (!last)
        return NULL;
    char fp[8192];
    path_join(fp, sizeof fp, dir, last);
    return tex_cache_get(fp);
}

static int kw(const char *p, const char *k, size_t n)
{
    return !strncmp(p, k, n) && (p[n] == ' ' || p[n] == '\t');
}

int mat_load_lib(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
    {
        fprintf(stderr, "B-3D: cannot read materials '%s'\n", path);
        return -1;
    }
    char dir[4096];
    path_dir(path, dir, sizeof dir);
    char line[8192];
    struct material cur;
    int have = 0, first = -1;
    memset(&cur, 0, sizeof cur);
    while (fgets(line, sizeof line, f))
    {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (!strncmp(p, "newmtl", 6) && (p[6] == ' ' || p[6] == '\t'))
        {
            if (have)
            {
                int id = mat_upsert(&cur);
                if (first < 0)
                    first = id;
            }
            memset(&cur, 0, sizeof cur);
            cur.kd[0] = cur.kd[1] = cur.kd[2] = 1.0;
            char nm[64];
            if (sscanf(p + 6, " %63s", nm) == 1)
                snprintf(cur.name, sizeof cur.name, "%s", nm);
            else
                snprintf(cur.name, sizeof cur.name, "mat%d", g.nmats);
            have = 1;
        }
        else if (!strncmp(p, "Kd", 2) && (p[2] == ' ' || p[2] == '\t'))
        {
            double r, gg, b;
            if (sscanf(p + 2, " %lf %lf %lf", &r, &gg, &b) == 3)
            {
                cur.kd[0] = r;
                cur.kd[1] = gg;
                cur.kd[2] = b;
            }
        }
        else if (!strncmp(p, "Ka", 2) && (p[2] == ' ' || p[2] == '\t'))
        {
            double r, gg, b;
            if (sscanf(p + 2, " %lf %lf %lf", &r, &gg, &b) == 3)
            {
                cur.ka[0] = r;
                cur.ka[1] = gg;
                cur.ka[2] = b;
            }
        }
        else if (!strncmp(p, "Ks", 2) && (p[2] == ' ' || p[2] == '\t'))
        {
            double r, gg, b;
            if (sscanf(p + 2, " %lf %lf %lf", &r, &gg, &b) == 3)
            {
                cur.ks[0] = r;
                cur.ks[1] = gg;
                cur.ks[2] = b;
            }
        }
        else if (!strncmp(p, "Ke", 2) && (p[2] == ' ' || p[2] == '\t'))
        {
            double r, gg, b;
            if (sscanf(p + 2, " %lf %lf %lf", &r, &gg, &b) == 3)
            {
                cur.ke[0] = r;
                cur.ke[1] = gg;
                cur.ke[2] = b;
            }
        }
        else if (!strncmp(p, "Ns", 2) && (p[2] == ' ' || p[2] == '\t'))
        {
            double v;
            if (sscanf(p + 2, " %lf", &v) == 1)
                cur.ns = v;
        }
        else if ((p[0] == 'd' && (p[1] == ' ' || p[1] == '\t')) ||
                 (!strncmp(p, "Tr", 2) && (p[2] == ' ' || p[2] == '\t')))
        {
            char *vp = p[0] == 'T' ? p + 2 : p + 1;
            double v;
            if (sscanf(vp, " %lf", &v) == 1)
            {
                if (p[0] == 'T')
                    v = 1.0 - v;
                if (v < 0.0)
                    v = 0.0;
                if (v > 1.0)
                    v = 1.0;
                for (int k = 0; k < 3; k++)
                    cur.kd[k] *= v;
            }
        }
        else if (kw(p, "map_Kd", 6))
        {
            cur.map = map_texture(p + 6, dir);
        }
        else if (kw(p, "map_Ke", 6))
        {
            cur.map_ke = map_texture(p + 6, dir);
        }
        else if (kw(p, "map_Ks", 6))
        {
            cur.map_ks = map_texture(p + 6, dir);
        }
        else if (kw(p, "map_d", 5))
        {
            cur.map_d = map_texture(p + 5, dir);
        }
        else if (kw(p, "map_Ka", 6))
        {
            struct texture *t = map_texture(p + 6, dir);
            if (!cur.map)
                cur.map = t;
        }
    }
    if (have)
    {
        int id = mat_upsert(&cur);
        if (first < 0)
            first = id;
    }
    fclose(f);
    return first;
}

void mat_resolve(void)
{
    struct mesh *m = &g.mesh;
    struct options *o = &g.opt;
    if (o->objfile[0] && m->nmtllib)
    {
        char dir[4096];
        path_dir(o->objfile, dir, sizeof dir);
        for (int i = 0; i < m->nmtllib; i++)
        {
            char fp[8192];
            path_join(fp, sizeof fp, dir, m->mtllib[i]);
            mat_load_lib(fp);
        }
    }
    if (m->fname)
    {
        for (int f = 0; f < m->nf; f++)
        {
            m->fmat[f] = -1;
            int ni = m->fname[f];
            if (ni >= 0 && m->fmtnames)
                m->fmat[f] = mat_find(m->fmtnames[ni]);
        }
    }
    g.globmat_id = -1;
    if (o->matfile[0])
    {
        int id = mat_load_lib(o->matfile);
        if (id < 0 && g.nmats > 0)
            id = 0;
        if (id >= 0)
        {
            g.globmat_id = id;
            if (m->fmat)
                for (int f = 0; f < m->nf; f++)
                    m->fmat[f] = id;
        }
    }
    g.tex = NULL;
    if (o->texfile[0])
        g.tex = tex_cache_get(o->texfile);
}
