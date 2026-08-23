#include "b3d.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum
{
    JS_OBJ,
    JS_ARR,
    JS_STR,
    JS_NUM,
    JS_BOOL,
    JS_NULL
};

struct jnode
{
    int type;
    double num;
    char *str;
    char **keys;
    struct jnode **kids;
    int n;
};

struct jp
{
    const char *s;
    size_t len, pos;
    int depth;
    int bad;
};

static struct jnode *jparse_value(struct jp *p);

static void jfree(struct jnode *n)
{
    if (!n)
        return;
    for (int i = 0; i < n->n; i++)
    {
        if (n->keys)
            free(n->keys[i]);
        jfree(n->kids[i]);
    }
    free(n->keys);
    free(n->kids);
    free(n->str);
    free(n);
}

static void jskip(struct jp *p)
{
    while (p->pos < p->len && isspace((unsigned char)p->s[p->pos]))
        p->pos++;
}

static char *jstring(struct jp *p)
{
    if (p->pos >= p->len || p->s[p->pos] != '"')
    {
        p->bad = 1;
        return NULL;
    }
    p->pos++;
    size_t cap = 32, n = 0;
    char *out = xrealloc(NULL, cap);
    while (p->pos < p->len && p->s[p->pos] != '"')
    {
        char c = p->s[p->pos++];
        char buf[4];
        int nb = 1;
        if (c == '\\' && p->pos < p->len)
        {
            char e = p->s[p->pos++];
            switch (e)
            {
            case 'n':
                c = '\n';
                break;
            case 't':
                c = '\t';
                break;
            case 'r':
                c = '\r';
                break;
            case 'b':
                c = '\b';
                break;
            case 'f':
                c = '\f';
                break;
            case 'u':
            {
                unsigned cp = 0;
                for (int k = 0; k < 4 && p->pos < p->len; k++)
                {
                    char h = p->s[p->pos++];
                    cp = cp * 16 + (unsigned)(h >= 'a'   ? h - 'a' + 10
                                              : h >= 'A' ? h - 'A' + 10
                                                         : h - '0');
                }
                if (cp < 0x80)
                {
                    buf[0] = (char)cp;
                    nb = 1;
                }
                else if (cp < 0x800)
                {
                    buf[0] = (char)(0xC0 | (cp >> 6));
                    buf[1] = (char)(0x80 | (cp & 0x3F));
                    nb = 2;
                }
                else
                {
                    buf[0] = (char)(0xE0 | (cp >> 12));
                    buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    buf[2] = (char)(0x80 | (cp & 0x3F));
                    nb = 3;
                }
                break;
            }
            default:
                c = e;
                break;
            }
            if (e != 'u')
            {
                buf[0] = c;
                nb = 1;
            }
        }
        else
        {
            buf[0] = c;
        }
        while (n + (size_t)nb + 1 > cap)
        {
            cap *= 2;
            out = xrealloc(out, cap);
        }
        memcpy(out + n, buf, (size_t)nb);
        n += (size_t)nb;
    }
    if (p->pos >= p->len)
    {
        p->bad = 1;
        free(out);
        return NULL;
    }
    p->pos++;
    out[n] = '\0';
    return out;
}

static void jpush(struct jnode *o, char *key, struct jnode *val)
{
    o->kids = xrealloc(o->kids, (size_t)(o->n + 1) * sizeof *o->kids);
    if (key || o->keys)
        o->keys = xrealloc(o->keys, (size_t)(o->n + 1) * sizeof *o->keys);
    if (o->keys)
        o->keys[o->n] = key;
    o->kids[o->n] = val;
    o->n++;
}

static struct jnode *jparse_value(struct jp *p)
{
    if (p->bad || ++p->depth > 128)
    {
        p->bad = 1;
        return NULL;
    }
    jskip(p);
    if (p->pos >= p->len)
    {
        p->bad = 1;
        p->depth--;
        return NULL;
    }
    struct jnode *n = xrealloc(NULL, sizeof *n);
    memset(n, 0, sizeof *n);
    char c = p->s[p->pos];
    if (c == '{' || c == '[')
    {
        int obj = c == '{';
        n->type = obj ? JS_OBJ : JS_ARR;
        p->pos++;
        jskip(p);
        if (p->pos < p->len && p->s[p->pos] == (obj ? '}' : ']'))
            p->pos++;
        else
            for (;;)
            {
                char *key = NULL;
                if (obj)
                {
                    jskip(p);
                    key = jstring(p);
                    jskip(p);
                    if (p->pos < p->len && p->s[p->pos] == ':')
                        p->pos++;
                    else
                        p->bad = 1;
                }
                struct jnode *v = jparse_value(p);
                if (p->bad)
                {
                    free(key);
                    jfree(v);
                    break;
                }
                jpush(n, key, v);
                jskip(p);
                if (p->pos < p->len && p->s[p->pos] == ',')
                {
                    p->pos++;
                    continue;
                }
                if (p->pos < p->len && p->s[p->pos] == (obj ? '}' : ']'))
                    p->pos++;
                else
                    p->bad = 1;
                break;
            }
    }
    else if (c == '"')
    {
        n->type = JS_STR;
        n->str = jstring(p);
    }
    else if (!strncmp(p->s + p->pos, "true", 4))
    {
        n->type = JS_BOOL;
        n->num = 1;
        p->pos += 4;
    }
    else if (!strncmp(p->s + p->pos, "false", 5))
    {
        n->type = JS_BOOL;
        p->pos += 5;
    }
    else if (!strncmp(p->s + p->pos, "null", 4))
    {
        n->type = JS_NULL;
        p->pos += 4;
    }
    else
    {
        char *e;
        n->type = JS_NUM;
        n->num = strtod(p->s + p->pos, &e);
        if (e == p->s + p->pos)
            p->bad = 1;
        else
            p->pos = (size_t)(e - p->s);
    }
    p->depth--;
    if (p->bad)
    {
        jfree(n);
        return NULL;
    }
    return n;
}

static struct jnode *jget(const struct jnode *o, const char *key)
{
    if (!o || o->type != JS_OBJ || !o->keys)
        return NULL;
    for (int i = 0; i < o->n; i++)
        if (o->keys[i] && !strcmp(o->keys[i], key))
            return o->kids[i];
    return NULL;
}

static struct jnode *jat(const struct jnode *a, int i)
{
    if (!a || a->type != JS_ARR || i < 0 || i >= a->n)
        return NULL;
    return a->kids[i];
}

static double jnum(const struct jnode *n, double def)
{
    return (n && (n->type == JS_NUM || n->type == JS_BOOL)) ? n->num : def;
}

static int jint(const struct jnode *n, int def)
{
    return (n && n->type == JS_NUM) ? (int)n->num : def;
}

static const char *jstr(const struct jnode *n, const char *def)
{
    return (n && n->type == JS_STR && n->str) ? n->str : def;
}

/* ---------------------------------------------------------------- base64 */

static int b64val(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+' || c == '-')
        return 62;
    if (c == '/' || c == '_')
        return 63;
    return -1;
}

static unsigned char *b64decode(const char *s, size_t *outn)
{
    size_t cap = strlen(s) / 4 * 3 + 4, n = 0;
    unsigned char *out = xrealloc(NULL, cap);
    unsigned acc = 0;
    int bits = 0;
    for (; *s; s++)
    {
        int v = b64val((unsigned char)*s);
        if (v < 0)
            continue;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            if (n < cap)
                out[n++] = (unsigned char)(acc >> bits);
        }
    }
    *outn = n;
    return out;
}

/* ------------------------------------------------------------- 4x4 matrix */

/* Column-major, matching glTF's own layout: m[col * 4 + row]. */
static void m_identity(double *m)
{
    memset(m, 0, 16 * sizeof *m);
    m[0] = m[5] = m[10] = m[15] = 1.0;
}

static void m_mul(const double *a, const double *b, double *out)
{
    double r[16];
    for (int c = 0; c < 4; c++)
        for (int i = 0; i < 4; i++)
        {
            double s = 0.0;
            for (int k = 0; k < 4; k++)
                s += a[k * 4 + i] * b[c * 4 + k];
            r[c * 4 + i] = s;
        }
    memcpy(out, r, sizeof r);
}

static vec3 m_point(const double *m, vec3 p)
{
    return v3(m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
              m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
              m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]);
}

static void node_matrix(const struct jnode *node, double *out)
{
    const struct jnode *mm = jget(node, "matrix");
    if (mm && mm->type == JS_ARR && mm->n == 16)
    {
        for (int i = 0; i < 16; i++)
            out[i] = jnum(jat(mm, i), 0.0);
        return;
    }
    double t[3] = {0, 0, 0}, r[4] = {0, 0, 0, 1}, s[3] = {1, 1, 1};
    const struct jnode *a;
    if ((a = jget(node, "translation")))
        for (int i = 0; i < 3; i++)
            t[i] = jnum(jat(a, i), 0.0);
    if ((a = jget(node, "rotation")))
        for (int i = 0; i < 4; i++)
            r[i] = jnum(jat(a, i), i == 3 ? 1.0 : 0.0);
    if ((a = jget(node, "scale")))
        for (int i = 0; i < 3; i++)
            s[i] = jnum(jat(a, i), 1.0);
    double x = r[0], y = r[1], z = r[2], w = r[3];
    m_identity(out);
    /* Quaternion to rotation, with the columns already scaled. */
    out[0] = (1 - 2 * (y * y + z * z)) * s[0];
    out[1] = (2 * (x * y + z * w)) * s[0];
    out[2] = (2 * (x * z - y * w)) * s[0];
    out[4] = (2 * (x * y - z * w)) * s[1];
    out[5] = (1 - 2 * (x * x + z * z)) * s[1];
    out[6] = (2 * (y * z + x * w)) * s[1];
    out[8] = (2 * (x * z + y * w)) * s[2];
    out[9] = (2 * (y * z - x * w)) * s[2];
    out[10] = (1 - 2 * (x * x + y * y)) * s[2];
    out[12] = t[0];
    out[13] = t[1];
    out[14] = t[2];
}

/* ------------------------------------------------------------- glTF state */

struct gbuf
{
    unsigned char *data;
    size_t n;
    int owned;
};

struct gctx
{
    struct jnode *root;
    struct gbuf *bufs;
    int nbufs;
    unsigned char *bin;
    size_t binn;
    char dir[4096];
    struct mesh *mesh;
    int *matname; /* interned mesh name per glTF material */
    int nmat;
};

static const unsigned char *view_data(struct gctx *c, int vi, size_t *len, int *stride)
{
    struct jnode *v = jat(jget(c->root, "bufferViews"), vi);
    if (!v)
        return NULL;
    int bi = jint(jget(v, "buffer"), -1);
    if (bi < 0 || bi >= c->nbufs || !c->bufs[bi].data)
        return NULL;
    size_t off = (size_t)jnum(jget(v, "byteOffset"), 0);
    size_t n = (size_t)jnum(jget(v, "byteLength"), 0);
    if (off + n > c->bufs[bi].n)
        return NULL;
    if (len)
        *len = n;
    if (stride)
        *stride = jint(jget(v, "byteStride"), 0);
    return c->bufs[bi].data + off;
}

static int comp_size(int ct)
{
    switch (ct)
    {
    case 5120:
    case 5121:
        return 1;
    case 5122:
    case 5123:
        return 2;
    default:
        return 4;
    }
}

static int type_count(const char *t)
{
    if (!t)
        return 0;
    if (!strcmp(t, "SCALAR"))
        return 1;
    if (!strcmp(t, "VEC2"))
        return 2;
    if (!strcmp(t, "VEC3"))
        return 3;
    if (!strcmp(t, "VEC4"))
        return 4;
    if (!strcmp(t, "MAT2"))
        return 4;
    if (!strcmp(t, "MAT3"))
        return 9;
    if (!strcmp(t, "MAT4"))
        return 16;
    return 0;
}

static double comp_read(const unsigned char *p, int ct, int normalized)
{
    switch (ct)
    {
    case 5120:
    {
        int v = (signed char)p[0];
        return normalized ? (v < -127 ? -1.0 : v / 127.0) : v;
    }
    case 5121:
        return normalized ? p[0] / 255.0 : p[0];
    case 5122:
    {
        int v = (short)((unsigned)p[0] | ((unsigned)p[1] << 8));
        return normalized ? (v < -32767 ? -1.0 : v / 32767.0) : v;
    }
    case 5123:
    {
        unsigned v = (unsigned)p[0] | ((unsigned)p[1] << 8);
        return normalized ? v / 65535.0 : v;
    }
    case 5125:
        return (double)((unsigned)p[0] | ((unsigned)p[1] << 8) |
                        ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24));
    default:
    {
        unsigned u = (unsigned)p[0] | ((unsigned)p[1] << 8) |
                     ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
        float f;
        memcpy(&f, &u, 4);
        return f;
    }
    }
}

/* Flattens one accessor into count*ncomp doubles. Sparse accessors and
   accessors with no bufferView read as zeroes, which is what the spec says. */
static double *accessor(struct gctx *c, int ai, int *count, int *ncomp)
{
    struct jnode *a = jat(jget(c->root, "accessors"), ai);
    if (!a)
        return NULL;
    int ct = jint(jget(a, "componentType"), 0);
    int n = jint(jget(a, "count"), 0);
    int nc = type_count(jstr(jget(a, "type"), NULL));
    int norm = (int)jnum(jget(a, "normalized"), 0);
    if (n <= 0 || nc <= 0 || !ct || n > 1 << 26)
        return NULL;
    double *out = xrealloc(NULL, (size_t)n * nc * sizeof *out);
    memset(out, 0, (size_t)n * nc * sizeof *out);
    struct jnode *bv = jget(a, "bufferView");
    if (bv)
    {
        size_t vlen = 0;
        int stride = 0;
        const unsigned char *base = view_data(c, jint(bv, -1), &vlen, &stride);
        size_t off = (size_t)jnum(jget(a, "byteOffset"), 0);
        int csz = comp_size(ct);
        if (!stride)
            stride = csz * nc;
        if (base && off + (size_t)(n - 1) * stride + (size_t)csz * nc <= vlen)
            for (int i = 0; i < n; i++)
                for (int k = 0; k < nc; k++)
                    out[i * nc + k] =
                        comp_read(base + off + (size_t)i * stride + (size_t)k * csz,
                                  ct, norm);
    }
    *count = n;
    *ncomp = nc;
    return out;
}

/* ----------------------------------------------------------- images/mats */

static struct texture *gltf_texture(struct gctx *c, int ti)
{
    struct jnode *t = jat(jget(c->root, "textures"), ti);
    if (!t)
        return NULL;
    int si = jint(jget(t, "source"), -1);
    struct jnode *img = jat(jget(c->root, "images"), si);
    if (!img)
        return NULL;
    const char *uri = jstr(jget(img, "uri"), NULL);
    if (uri)
    {
        if (!strncmp(uri, "data:", 5))
        {
            const char *comma = strchr(uri, ',');
            if (!comma)
                return NULL;
            size_t n = 0;
            unsigned char *raw = b64decode(comma + 1, &n);
            char key[64];
            snprintf(key, sizeof key, "<gltf image %d>", si);
            struct texture *tex = tex_from_memory(key, raw, n);
            free(raw);
            return tex;
        }
        char fp[8192];
        path_join(fp, sizeof fp, c->dir, uri);
        return tex_cache_get(fp);
    }
    struct jnode *bv = jget(img, "bufferView");
    if (!bv)
        return NULL;
    size_t len = 0;
    const unsigned char *d = view_data(c, jint(bv, -1), &len, NULL);
    if (!d)
        return NULL;
    char key[64];
    snprintf(key, sizeof key, "<gltf image %d>", si);
    return tex_from_memory(key, d, len);
}

/* Maps glTF's metallic-roughness parameters onto the renderer's Phong-ish
   material: base colour becomes Kd, metals get a tinted Ks, roughness Ns. */
static void gltf_materials(struct gctx *c)
{
    struct jnode *mats = jget(c->root, "materials");
    if (!mats || mats->type != JS_ARR)
        return;
    c->nmat = mats->n;
    c->matname = xrealloc(NULL, (size_t)(mats->n ? mats->n : 1) * sizeof *c->matname);
    for (int i = 0; i < mats->n; i++)
    {
        struct jnode *jm = jat(mats, i);
        struct material mt;
        memset(&mt, 0, sizeof mt);
        char nm[64];
        const char *jn = jstr(jget(jm, "name"), NULL);
        if (jn && *jn)
            snprintf(nm, sizeof nm, "%s", jn);
        else
            snprintf(nm, sizeof nm, "material_%d", i);
        snprintf(mt.name, sizeof mt.name, "%s", nm);
        mt.kd[0] = mt.kd[1] = mt.kd[2] = 1.0;

        struct jnode *pbr = jget(jm, "pbrMetallicRoughness");
        double metal = 1.0, rough = 1.0;
        if (pbr)
        {
            struct jnode *bc = jget(pbr, "baseColorFactor");
            if (bc)
                for (int k = 0; k < 3; k++)
                    mt.kd[k] = jnum(jat(bc, k), 1.0);
            metal = jnum(jget(pbr, "metallicFactor"), 1.0);
            rough = jnum(jget(pbr, "roughnessFactor"), 1.0);
            struct jnode *bt = jget(pbr, "baseColorTexture");
            if (bt)
                mt.map = gltf_texture(c, jint(jget(bt, "index"), -1));
            struct jnode *mr = jget(pbr, "metallicRoughnessTexture");
            if (mr)
                mt.map_ks = gltf_texture(c, jint(jget(mr, "index"), -1));
        }
        struct jnode *ef = jget(jm, "emissiveFactor");
        if (ef)
            for (int k = 0; k < 3; k++)
                mt.ke[k] = jnum(jat(ef, k), 0.0);
        struct jnode *et = jget(jm, "emissiveTexture");
        if (et)
            mt.map_ke = gltf_texture(c, jint(jget(et, "index"), -1));
        for (int k = 0; k < 3; k++)
        {
            mt.ks[k] = 0.04 * (1.0 - metal) + mt.kd[k] * metal;
            mt.ka[k] = mt.kd[k] * 0.1;
        }
        double gloss = 1.0 - (rough < 0.0 ? 0.0 : rough > 1.0 ? 1.0
                                                              : rough);
        mt.ns = gloss * gloss * 256.0;
        mat_register(&mt);
        c->matname[i] = mesh_intern_name(c->mesh, nm);
    }
}

/* ------------------------------------------------------------- geometry */

static void emit_primitive(struct gctx *c, struct jnode *prim, const double *xf)
{
    struct jnode *attr = jget(prim, "attributes");
    if (!attr)
        return;
    int pi = jint(jget(attr, "POSITION"), -1);
    if (pi < 0)
        return;
    int mode = jint(jget(prim, "mode"), 4);
    if (mode < 4)
        return; /* points and lines have no faces to rasterise */

    int np = 0, pc = 0;
    double *pos = accessor(c, pi, &np, &pc);
    if (!pos || pc < 3)
    {
        free(pos);
        return;
    }
    int nu = 0, uc = 0, ncol = 0, cc = 0;
    double *uv = NULL, *col = NULL;
    struct jnode *tj = jget(attr, "TEXCOORD_0");
    if (tj)
        uv = accessor(c, jint(tj, -1), &nu, &uc);
    struct jnode *cj = jget(attr, "COLOR_0");
    if (cj)
        col = accessor(c, jint(cj, -1), &ncol, &cc);

    int base = c->mesh->nv;
    for (int i = 0; i < np; i++)
    {
        int id = mesh_addv(c->mesh, m_point(xf, v3(pos[i * pc], pos[i * pc + 1],
                                                   pos[i * pc + 2])));
        if (uv && i < nu && uc >= 2)
        {
            mesh_setuv(c->mesh, id, uv[i * uc], uv[i * uc + 1]);
            c->mesh->has_uv = 1;
        }
        if (col && i < ncol && cc >= 3)
            mesh_setcol(c->mesh, id, col[i * cc], col[i * cc + 1], col[i * cc + 2]);
    }
    free(pos);
    free(uv);
    free(col);

    int name = -1;
    struct jnode *mj = jget(prim, "material");
    if (mj)
    {
        int mi = jint(mj, -1);
        if (mi >= 0 && mi < c->nmat && c->matname)
            name = c->matname[mi];
    }

    int ni = 0, ic = 0;
    double *ind = NULL;
    struct jnode *ij = jget(prim, "indices");
    if (ij)
        ind = accessor(c, jint(ij, -1), &ni, &ic);
    int total = ind ? ni : np;
    for (int k = 0; k + 2 < total; k++)
    {
        int a, b, d;
        if (mode == 5)
        {
            /* Strips alternate winding so the facing stays consistent. */
            a = k;
            b = (k & 1) ? k + 2 : k + 1;
            d = (k & 1) ? k + 1 : k + 2;
        }
        else if (mode == 6)
        {
            a = 0;
            b = k + 1;
            d = k + 2;
        }
        else
        {
            if (k % 3)
                continue;
            a = k;
            b = k + 1;
            d = k + 2;
        }
        int va = base + (ind ? (int)ind[a * ic] : a);
        int vb = base + (ind ? (int)ind[b * ic] : b);
        int vd = base + (ind ? (int)ind[d * ic] : d);
        int fi = mesh_addf(c->mesh, va, vb, vd);
        if (fi >= 0)
            c->mesh->fname[fi] = name;
    }
    free(ind);
}

static void walk_node(struct gctx *c, int ni, const double *parent, int depth)
{
    if (depth > 64)
        return;
    struct jnode *node = jat(jget(c->root, "nodes"), ni);
    if (!node)
        return;
    double local[16], xf[16];
    node_matrix(node, local);
    m_mul(parent, local, xf);
    struct jnode *mj = jget(node, "mesh");
    if (mj)
    {
        struct jnode *mesh = jat(jget(c->root, "meshes"), jint(mj, -1));
        struct jnode *prims = jget(mesh, "primitives");
        for (int i = 0; prims && i < prims->n; i++)
            emit_primitive(c, jat(prims, i), xf);
    }
    struct jnode *kids = jget(node, "children");
    for (int i = 0; kids && i < kids->n; i++)
        walk_node(c, jint(jat(kids, i), -1), xf, depth + 1);
}

/* -------------------------------------------------------------- buffers */

static void load_buffers(struct gctx *c)
{
    struct jnode *bufs = jget(c->root, "buffers");
    c->nbufs = bufs ? bufs->n : 0;
    if (!c->nbufs)
        return;
    c->bufs = xrealloc(NULL, (size_t)c->nbufs * sizeof *c->bufs);
    memset(c->bufs, 0, (size_t)c->nbufs * sizeof *c->bufs);
    for (int i = 0; i < c->nbufs; i++)
    {
        struct jnode *b = jat(bufs, i);
        const char *uri = jstr(jget(b, "uri"), NULL);
        if (!uri)
        {
            /* No URI means the GLB binary chunk, which only buffer 0 may use. */
            c->bufs[i].data = c->bin;
            c->bufs[i].n = c->binn;
            continue;
        }
        if (!strncmp(uri, "data:", 5))
        {
            const char *comma = strchr(uri, ',');
            if (!comma)
                continue;
            c->bufs[i].data = b64decode(comma + 1, &c->bufs[i].n);
            c->bufs[i].owned = 1;
            continue;
        }
        char fp[8192];
        path_join(fp, sizeof fp, c->dir, uri);
        FILE *f = fopen(fp, "rb");
        if (!f)
        {
            fprintf(stderr, "B-3D: cannot read glTF buffer '%s'\n", fp);
            continue;
        }
        if (fseek(f, 0, SEEK_END) == 0)
        {
            long n = ftell(f);
            rewind(f);
            if (n > 0)
            {
                c->bufs[i].data = xrealloc(NULL, (size_t)n);
                c->bufs[i].n = fread(c->bufs[i].data, 1, (size_t)n, f);
                c->bufs[i].owned = 1;
            }
        }
        fclose(f);
    }
}

static unsigned rdu32le(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) |
           ((unsigned)p[3] << 24);
}

void gltf_load(struct mesh *m, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        die("cannot read '%s'", path);
    if (fseek(f, 0, SEEK_END) != 0)
        die("cannot size '%s'", path);
    long fsz = ftell(f);
    rewind(f);
    if (fsz < 4)
        die("'%s' is too small to be glTF", path);
    unsigned char *raw = xrealloc(NULL, (size_t)fsz + 1);
    size_t got = fread(raw, 1, (size_t)fsz, f);
    fclose(f);
    raw[got] = '\0';

    struct gctx c;
    memset(&c, 0, sizeof c);
    c.mesh = m;
    path_dir(path, c.dir, sizeof c.dir);

    const char *json = (const char *)raw;
    size_t jsonn = got;
    if (got >= 12 && !memcmp(raw, "glTF", 4))
    {
        /* GLB: a 12-byte header then length-prefixed JSON and BIN chunks. */
        json = NULL;
        size_t p = 12;
        while (p + 8 <= got)
        {
            unsigned clen = rdu32le(raw + p);
            unsigned ctype = rdu32le(raw + p + 4);
            p += 8;
            if ((size_t)clen > got - p)
                break;
            if (ctype == 0x4E4F534A && !json)
            {
                json = (const char *)raw + p;
                jsonn = clen;
            }
            else if (ctype == 0x004E4942 && !c.bin)
            {
                c.bin = raw + p;
                c.binn = clen;
            }
            p += clen + (clen % 4 ? 4 - clen % 4 : 0);
        }
        if (!json)
        {
            free(raw);
            die("'%s' has no JSON chunk", path);
        }
    }

    struct jp jp = {json, jsonn, 0, 0, 0};
    c.root = jparse_value(&jp);
    if (!c.root || c.root->type != JS_OBJ)
    {
        jfree(c.root);
        free(raw);
        die("'%s' is not valid glTF JSON", path);
    }

    load_buffers(&c);
    gltf_materials(&c);

    double identity[16];
    m_identity(identity);
    struct jnode *scenes = jget(c.root, "scenes");
    struct jnode *scene = jat(scenes, jint(jget(c.root, "scene"), 0));
    if (!scene)
        scene = jat(scenes, 0);
    struct jnode *roots = jget(scene, "nodes");
    if (roots)
    {
        for (int i = 0; i < roots->n; i++)
            walk_node(&c, jint(jat(roots, i), -1), identity, 0);
    }
    else
    {
        /* No scene graph: draw every mesh where it sits. */
        struct jnode *meshes = jget(c.root, "meshes");
        for (int i = 0; meshes && i < meshes->n; i++)
        {
            struct jnode *prims = jget(jat(meshes, i), "primitives");
            for (int k = 0; prims && k < prims->n; k++)
                emit_primitive(&c, jat(prims, k), identity);
        }
    }

    for (int i = 0; i < c.nbufs; i++)
        if (c.bufs[i].owned)
            free(c.bufs[i].data);
    free(c.bufs);
    free(c.matname);
    jfree(c.root);
    free(raw);
}
