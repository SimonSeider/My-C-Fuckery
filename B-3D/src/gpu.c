
#include "b3d.h"
#include "cl_api.h"
#include "gpu_kernel.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define X(ret, name, args) static ret(*p_##name) args;
B3D_CL_FUNCS
#undef X

struct uni
{
    float r0, r1, r2, r3, r4, r5, r6, r7, r8;
    float offx, offy, offz;
    float dist;
    float cx, cy, px, py;
    float lx, ly, lz;
    float hx, hy, hz;
    float ambient, specular;
    float br, bg, bb;
    int32_t w, h;
    int32_t nf, nv;
    int32_t smooth, cull, tex_on, usecolor;
    int32_t filter, nglyphs, nmats, gtex;
    int32_t ntri;
    int32_t pad0, pad1, pad2;
};

struct gtex
{
    int32_t off, w, h, aoff;
    float avg_lum;
    int32_t pad0, pad1, pad2;
};

struct gmat
{
    float kd0, kd1, kd2, ns;
    float ks0, ks1, ks2, pad0;
    float ke0, ke1, ke2, pad1;
    int32_t map, map_ke, map_ks, map_d;
};

#define TRI_BYTES 128

enum
{
    GPU_UNTRIED,
    GPU_READY,
    GPU_OFF
};

static struct
{
    void *lib;
    int state;
    char devname[128];
    char err[256];
    cl_platform_id plat;
    cl_device_id dev;
    cl_context ctx;
    cl_command_queue q;
    cl_program prog;
    cl_kernel k_xform, k_setup, k_raster;
    cl_ulong maxalloc;
    cl_mem b_uni, b_vtx, b_nrm, b_uv, b_vc, b_idx, b_fn;
    cl_mem b_tv, b_pv, b_vsh, b_tri, b_bbox;
    cl_mem b_texdata, b_textab, b_mats;
    cl_mem b_lev, b_clr;
    int up_nv, up_nf;
    int fb_w, fb_h;
    int mesh_dirty;
    unsigned char *lev;
    int32_t *clr;
    int used_last_frame;
} G;

static void gpu_fail(const char *what, cl_int e)
{
    snprintf(G.err, sizeof G.err, "%s failed (%d)", what, (int)e);
    G.state = GPU_OFF;
}

static int load_lib(void)
{
    if (G.lib)
        return 1;
    const char *names[] = {"libOpenCL.so.1", "libOpenCL.so", "libOpenCL.so.1.0.0"};
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
    {
        G.lib = dlopen(names[i], RTLD_LAZY | RTLD_LOCAL);
        if (G.lib)
            break;
    }
    if (!G.lib)
    {
        snprintf(G.err, sizeof G.err, "no libOpenCL.so on this system");
        return 0;
    }
#define X(ret, name, args)                                               \
    *(void **)(&p_##name) = dlsym(G.lib, #name);                         \
    if (!p_##name)                                                       \
    {                                                                    \
        snprintf(G.err, sizeof G.err, "libOpenCL is missing %s", #name); \
        return 0;                                                        \
    }
    B3D_CL_FUNCS
#undef X
    return 1;
}

static int enum_devices(cl_platform_id *plats, cl_device_id *devs, int cap)
{
    cl_platform_id pl[16];
    cl_uint np = 0;
    if (p_clGetPlatformIDs(16, pl, &np) != CL_SUCCESS)
        return 0;
    int n = 0;
    for (cl_uint i = 0; i < np && n < cap; i++)
    {
        cl_device_id dv[16];
        cl_uint nd = 0;
        if (p_clGetDeviceIDs(pl[i], CL_DEVICE_TYPE_ALL, 16, dv, &nd) != CL_SUCCESS)
            continue;
        for (cl_uint k = 0; k < nd && n < cap; k++)
        {
            plats[n] = pl[i];
            devs[n] = dv[k];
            n++;
        }
    }
    return n;
}

static void dev_string(cl_device_id d, cl_device_info info, char *out, size_t cap)
{
    out[0] = '\0';
    p_clGetDeviceInfo(d, info, cap, out, NULL);
    out[cap - 1] = '\0';
}

static int pick_device(void)
{
    cl_platform_id plats[64];
    cl_device_id devs[64];
    int n = enum_devices(plats, devs, 64);
    if (n < 1)
    {
        snprintf(G.err, sizeof G.err, "no OpenCL devices (is an ICD installed?)");
        return 0;
    }
    int want = g.opt.gpu_dev;
    int pick = -1;
    if (want >= 0)
    {
        if (want >= n)
        {
            snprintf(G.err, sizeof G.err, "--gpu-device %d: only %d device(s)", want, n);
            return 0;
        }
        pick = want;
    }
    else
    {
        for (int i = 0; i < n && pick < 0; i++)
        {
            cl_device_type t = 0;
            p_clGetDeviceInfo(devs[i], CL_DEVICE_TYPE, sizeof t, &t, NULL);
            if (t & CL_DEVICE_TYPE_GPU)
                pick = i;
        }
        if (pick < 0)
            pick = 0;
    }
    cl_bool ok = CL_FALSE;
    p_clGetDeviceInfo(devs[pick], CL_DEVICE_COMPILER_AVAILABLE, sizeof ok, &ok, NULL);
    if (!ok)
    {
        snprintf(G.err, sizeof G.err, "device has no OpenCL compiler");
        return 0;
    }
    G.plat = plats[pick];
    G.dev = devs[pick];
    dev_string(G.dev, CL_DEVICE_NAME, G.devname, sizeof G.devname);
    p_clGetDeviceInfo(G.dev, CL_DEVICE_MAX_MEM_ALLOC_SIZE, sizeof G.maxalloc, &G.maxalloc, NULL);
    return 1;
}

static int build_program(void)
{
    cl_int e = 0;
    const char *src = B3D_CL_SRC;
    size_t len = strlen(src);
    G.prog = p_clCreateProgramWithSource(G.ctx, 1, &src, &len, &e);
    if (e != CL_SUCCESS)
    {
        gpu_fail("clCreateProgramWithSource", e);
        return 0;
    }
    e = p_clBuildProgram(G.prog, 1, &G.dev, "-cl-std=CL1.2", NULL, NULL);
    if (e != CL_SUCCESS)
    {
        size_t n = 0;
        p_clGetProgramBuildInfo(G.prog, G.dev, CL_PROGRAM_BUILD_LOG, 0, NULL, &n);
        char *log = malloc(n + 1);
        if (log)
        {
            p_clGetProgramBuildInfo(G.prog, G.dev, CL_PROGRAM_BUILD_LOG, n + 1, log, NULL);
            log[n] = '\0';
            fprintf(stderr, "B-3D: OpenCL build log:\n%s\n", log);
            free(log);
        }
        gpu_fail("clBuildProgram", e);
        return 0;
    }
    G.k_xform = p_clCreateKernel(G.prog, "k_xform", &e);
    if (e == CL_SUCCESS)
        G.k_setup = p_clCreateKernel(G.prog, "k_setup", &e);
    if (e == CL_SUCCESS)
        G.k_raster = p_clCreateKernel(G.prog, "k_raster", &e);
    if (e != CL_SUCCESS)
    {
        gpu_fail("clCreateKernel", e);
        return 0;
    }
    return 1;
}

static int gpu_init(void)
{
    if (G.state != GPU_UNTRIED)
        return G.state == GPU_READY;
    G.state = GPU_OFF;
    if (!load_lib())
        return 0;
    if (!pick_device())
        return 0;
    cl_int e = 0;
    G.ctx = p_clCreateContext(NULL, 1, &G.dev, NULL, NULL, &e);
    if (e != CL_SUCCESS)
    {
        gpu_fail("clCreateContext", e);
        return 0;
    }
    G.q = p_clCreateCommandQueue(G.ctx, G.dev, 0, &e);
    if (e != CL_SUCCESS)
    {
        gpu_fail("clCreateCommandQueue", e);
        return 0;
    }
    G.state = GPU_READY;
    if (!build_program())
        return 0;
    G.mesh_dirty = 1;
    return 1;
}

static void relmem(cl_mem *m)
{
    if (*m)
        p_clReleaseMemObject(*m);
    *m = NULL;
}

static cl_mem mkbuf(cl_mem_flags fl, size_t bytes, const void *host)
{
    if (bytes == 0)
        bytes = 16;
    cl_int e = 0;
    cl_mem m = p_clCreateBuffer(G.ctx, fl | (host ? CL_MEM_COPY_HOST_PTR : 0), bytes,
                                (void *)host, &e);
    if (e != CL_SUCCESS)
    {
        gpu_fail("clCreateBuffer", e);
        return NULL;
    }
    return m;
}

static int tex_index(const struct texture *t)
{
    if (!t || !t->rgb)
        return -1;
    for (int i = 0; i < g.ntexs; i++)
        if (g.texs[i] == t)
            return i;
    return -1;
}

static int upload_textures(void)
{
    size_t total = 0;
    for (int i = 0; i < g.ntexs; i++)
    {
        struct texture *t = g.texs[i];
        if (!t->rgb)
            continue;
        total += (size_t)t->w * t->h * 3;
        if (t->alpha)
            total += (size_t)t->w * t->h;
    }
    unsigned char *blob = malloc(total ? total : 1);
    struct gtex *tab = calloc((size_t)(g.ntexs > 0 ? g.ntexs : 1), sizeof *tab);
    if (!blob || !tab)
    {
        free(blob);
        free(tab);
        snprintf(G.err, sizeof G.err, "out of memory staging textures");
        G.state = GPU_OFF;
        return 0;
    }
    size_t off = 0;
    for (int i = 0; i < g.ntexs; i++)
    {
        struct texture *t = g.texs[i];
        tab[i].w = t->w;
        tab[i].h = t->h;
        tab[i].avg_lum = (float)t->avg_lum;
        tab[i].aoff = -1;
        if (!t->rgb)
        {
            tab[i].off = 0;
            tab[i].w = tab[i].h = 1;
            continue;
        }
        size_t n = (size_t)t->w * t->h * 3;
        tab[i].off = (int32_t)off;
        memcpy(blob + off, t->rgb, n);
        off += n;
        if (t->alpha)
        {
            size_t an = (size_t)t->w * t->h;
            tab[i].aoff = (int32_t)off;
            memcpy(blob + off, t->alpha, an);
            off += an;
        }
    }
    relmem(&G.b_texdata);
    relmem(&G.b_textab);
    G.b_texdata = mkbuf(CL_MEM_READ_ONLY, total, total ? blob : NULL);
    G.b_textab = mkbuf(CL_MEM_READ_ONLY, (size_t)(g.ntexs > 0 ? g.ntexs : 1) * sizeof *tab, tab);
    free(blob);
    free(tab);
    return G.b_texdata && G.b_textab;
}

static int upload_materials(void)
{
    int n = g.nmats > 0 ? g.nmats : 1;
    struct gmat *mm = calloc((size_t)n, sizeof *mm);
    if (!mm)
    {
        snprintf(G.err, sizeof G.err, "out of memory staging materials");
        G.state = GPU_OFF;
        return 0;
    }
    for (int i = 0; i < g.nmats; i++)
    {
        const struct material *s = &g.mats[i];
        mm[i].kd0 = (float)s->kd[0];
        mm[i].kd1 = (float)s->kd[1];
        mm[i].kd2 = (float)s->kd[2];
        mm[i].ns = (float)s->ns;
        mm[i].ks0 = (float)s->ks[0];
        mm[i].ks1 = (float)s->ks[1];
        mm[i].ks2 = (float)s->ks[2];
        mm[i].ke0 = (float)s->ke[0];
        mm[i].ke1 = (float)s->ke[1];
        mm[i].ke2 = (float)s->ke[2];
        mm[i].map = tex_index(s->map);
        mm[i].map_ke = tex_index(s->map_ke);
        mm[i].map_ks = tex_index(s->map_ks);
        mm[i].map_d = tex_index(s->map_d);
    }
    relmem(&G.b_mats);
    G.b_mats = mkbuf(CL_MEM_READ_ONLY, (size_t)n * sizeof *mm, mm);
    free(mm);
    return G.b_mats != NULL;
}

static int upload_mesh(void)
{
    struct mesh *m = &g.mesh;
    int nv = m->nv, nf = m->nf;
    size_t bytes = (size_t)nf * 2 * TRI_BYTES;
    if (G.maxalloc && bytes > G.maxalloc)
    {
        snprintf(G.err, sizeof G.err, "mesh needs %.0f MB, device allows %.0f MB",
                 bytes / 1048576.0, (double)G.maxalloc / 1048576.0);
        G.state = GPU_OFF;
        return 0;
    }
    float *v4 = malloc((size_t)nv * 4 * sizeof *v4);
    float *n4 = malloc((size_t)nv * 4 * sizeof *n4);
    float *uv = malloc((size_t)nv * 2 * sizeof *uv);
    float *vc = malloc((size_t)nv * 4 * sizeof *vc);
    int32_t *ix = malloc((size_t)nf * 4 * sizeof *ix);
    float *fn = malloc((size_t)nf * 4 * sizeof *fn);
    if (!v4 || !n4 || !uv || !vc || !ix || !fn)
    {
        free(v4);
        free(n4);
        free(uv);
        free(vc);
        free(ix);
        free(fn);
        snprintf(G.err, sizeof G.err, "out of memory staging the mesh");
        G.state = GPU_OFF;
        return 0;
    }
    for (int i = 0; i < nv; i++)
    {
        v4[i * 4] = (float)m->v[i].x;
        v4[i * 4 + 1] = (float)m->v[i].y;
        v4[i * 4 + 2] = (float)m->v[i].z;
        v4[i * 4 + 3] = 0.0f;
        vec3 nn = m->vn ? m->vn[i] : v3(0.0, 0.0, 0.0);
        n4[i * 4] = (float)nn.x;
        n4[i * 4 + 1] = (float)nn.y;
        n4[i * 4 + 2] = (float)nn.z;
        n4[i * 4 + 3] = 0.0f;
        uv[i * 2] = m->vt ? (float)m->vt[i * 2] : 0.0f;
        uv[i * 2 + 1] = m->vt ? (float)m->vt[i * 2 + 1] : 0.0f;
        int has = m->has_vcol && m->vc;
        vc[i * 4] = has ? (float)m->vc[i * 3] : 1.0f;
        vc[i * 4 + 1] = has ? (float)m->vc[i * 3 + 1] : 1.0f;
        vc[i * 4 + 2] = has ? (float)m->vc[i * 3 + 2] : 1.0f;
        vc[i * 4 + 3] = 1.0f;
    }
    for (int f = 0; f < nf; f++)
    {
        ix[f * 4] = m->idx[f * 3];
        ix[f * 4 + 1] = m->idx[f * 3 + 1];
        ix[f * 4 + 2] = m->idx[f * 3 + 2];
        int mi = (m->fmat && m->fmat[f] >= 0) ? m->fmat[f] : g.globmat_id;
        ix[f * 4 + 3] = (mi >= 0 && mi < g.nmats) ? mi : -1;
        fn[f * 4] = (float)m->fn[f].x;
        fn[f * 4 + 1] = (float)m->fn[f].y;
        fn[f * 4 + 2] = (float)m->fn[f].z;
        fn[f * 4 + 3] = 0.0f;
    }
    relmem(&G.b_vtx);
    relmem(&G.b_nrm);
    relmem(&G.b_uv);
    relmem(&G.b_vc);
    relmem(&G.b_idx);
    relmem(&G.b_fn);
    relmem(&G.b_tv);
    relmem(&G.b_pv);
    relmem(&G.b_vsh);
    relmem(&G.b_tri);
    relmem(&G.b_bbox);
    G.b_vtx = mkbuf(CL_MEM_READ_ONLY, (size_t)nv * 16, v4);
    G.b_nrm = mkbuf(CL_MEM_READ_ONLY, (size_t)nv * 16, n4);
    G.b_uv = mkbuf(CL_MEM_READ_ONLY, (size_t)nv * 8, uv);
    G.b_vc = mkbuf(CL_MEM_READ_ONLY, (size_t)nv * 16, vc);
    G.b_idx = mkbuf(CL_MEM_READ_ONLY, (size_t)nf * 16, ix);
    G.b_fn = mkbuf(CL_MEM_READ_ONLY, (size_t)nf * 16, fn);
    G.b_tv = mkbuf(CL_MEM_READ_WRITE, (size_t)nv * 16, NULL);
    G.b_pv = mkbuf(CL_MEM_READ_WRITE, (size_t)nv * 16, NULL);
    G.b_vsh = mkbuf(CL_MEM_READ_WRITE, (size_t)nv * 8, NULL);
    G.b_tri = mkbuf(CL_MEM_READ_WRITE, bytes, NULL);
    G.b_bbox = mkbuf(CL_MEM_READ_WRITE, (size_t)nf * 2 * 16, NULL);
    free(v4);
    free(n4);
    free(uv);
    free(vc);
    free(ix);
    free(fn);
    if (!G.b_vtx || !G.b_nrm || !G.b_uv || !G.b_vc || !G.b_idx || !G.b_fn || !G.b_tv ||
        !G.b_pv || !G.b_vsh || !G.b_tri || !G.b_bbox)
        return 0;
    if (!upload_textures() || !upload_materials())
        return 0;
    G.up_nv = nv;
    G.up_nf = nf;
    G.mesh_dirty = 0;
    return 1;
}

static int ensure_fb(int w, int h)
{
    if (G.fb_w == w && G.fb_h == h && G.b_lev)
        return 1;
    size_t n = (size_t)w * h;
    relmem(&G.b_lev);
    relmem(&G.b_clr);
    G.b_lev = mkbuf(CL_MEM_WRITE_ONLY, n, NULL);
    G.b_clr = mkbuf(CL_MEM_WRITE_ONLY, n * 4, NULL);
    G.lev = xrealloc(G.lev, n);
    G.clr = xrealloc(G.clr, n * 4);
    G.fb_w = w;
    G.fb_h = h;
    return G.b_lev && G.b_clr;
}

static void fill_uni(struct uni *u, int w, int h, int ntri)
{
    const double *r = g.rot;
    u->r0 = (float)r[0];
    u->r1 = (float)r[1];
    u->r2 = (float)r[2];
    u->r3 = (float)r[3];
    u->r4 = (float)r[4];
    u->r5 = (float)r[5];
    u->r6 = (float)r[6];
    u->r7 = (float)r[7];
    u->r8 = (float)r[8];
    u->offx = (float)g.opt.offx;
    u->offy = (float)g.opt.offy;
    u->offz = (float)g.opt.offz;
    u->dist = (float)g.opt.dist;
    u->cx = (float)g.scr.cx;
    u->cy = (float)g.scr.cy;
    u->px = (float)g.scr.px;
    u->py = (float)g.scr.py;
    u->lx = (float)g.light.x;
    u->ly = (float)g.light.y;
    u->lz = (float)g.light.z;
    u->hx = (float)g.halfv.x;
    u->hy = (float)g.halfv.y;
    u->hz = (float)g.halfv.z;
    u->ambient = (float)g.opt.ambient;
    u->specular = (float)g.opt.specular;
    u->br = g.opt.cr / 255.0f;
    u->bg = g.opt.cg / 255.0f;
    u->bb = g.opt.cb / 255.0f;
    u->w = w;
    u->h = h;
    u->nf = g.mesh.nf;
    u->nv = g.mesh.nv;
    u->smooth = g.smooth;
    u->cull = g.opt.cull;
    u->tex_on = g.opt.tex_on;
    u->usecolor = g.usecolor;
    u->filter = g.opt.filter;
    u->nglyphs = g.nglyphs;
    u->nmats = g.nmats;
    u->gtex = tex_index(g.tex);
    u->ntri = ntri;
    u->pad0 = u->pad1 = u->pad2 = 0;
}

#define ARG(k, i, v)                                            \
    do                                                          \
    {                                                           \
        cl_mem _m = (v);                                        \
        cl_int _e = p_clSetKernelArg((k), (i), sizeof _m, &_m); \
        if (_e != CL_SUCCESS)                                   \
        {                                                       \
            gpu_fail("clSetKernelArg", _e);                     \
            return 0;                                           \
        }                                                       \
    } while (0)

int gpu_render_frame(void)
{
    if (g.opt.gpu == 0)
        return 0;
    if (g.opt.mode != MODE_SOLID)
        return 0;
    if (G.state == GPU_OFF)
        return 0;
    if (!gpu_init())
        return 0;
    struct mesh *m = &g.mesh;
    if (m->nv < 1 || m->nf < 1)
        return 0;
    int w = g.scr.w, h = g.scr.h;
    if (w < 1 || h < 1)
        return 0;
    if (G.mesh_dirty || G.up_nv != m->nv || G.up_nf != m->nf)
        if (!upload_mesh())
            return 0;
    if (!ensure_fb(w, h))
        return 0;

    int ntri = m->nf * 2;
    struct uni u;
    fill_uni(&u, w, h, ntri);
    if (!G.b_uni)
        G.b_uni = mkbuf(CL_MEM_READ_ONLY, sizeof u, NULL);
    if (!G.b_uni)
        return 0;
    cl_int e = p_clEnqueueWriteBuffer(G.q, G.b_uni, CL_FALSE, 0, sizeof u, &u, 0, NULL, NULL);
    if (e != CL_SUCCESS)
    {
        gpu_fail("clEnqueueWriteBuffer", e);
        return 0;
    }

    ARG(G.k_xform, 0, G.b_uni);
    ARG(G.k_xform, 1, G.b_vtx);
    ARG(G.k_xform, 2, G.b_nrm);
    ARG(G.k_xform, 3, G.b_tv);
    ARG(G.k_xform, 4, G.b_pv);
    ARG(G.k_xform, 5, G.b_vsh);
    size_t gv = (size_t)m->nv;
    e = p_clEnqueueNDRangeKernel(G.q, G.k_xform, 1, NULL, &gv, NULL, 0, NULL, NULL);
    if (e != CL_SUCCESS)
    {
        gpu_fail("k_xform", e);
        return 0;
    }

    ARG(G.k_setup, 0, G.b_uni);
    ARG(G.k_setup, 1, G.b_idx);
    ARG(G.k_setup, 2, G.b_fn);
    ARG(G.k_setup, 3, G.b_tv);
    ARG(G.k_setup, 4, G.b_uv);
    ARG(G.k_setup, 5, G.b_vc);
    ARG(G.k_setup, 6, G.b_vsh);
    ARG(G.k_setup, 7, G.b_tri);
    ARG(G.k_setup, 8, G.b_bbox);
    size_t gf = (size_t)m->nf;
    e = p_clEnqueueNDRangeKernel(G.q, G.k_setup, 1, NULL, &gf, NULL, 0, NULL, NULL);
    if (e != CL_SUCCESS)
    {
        gpu_fail("k_setup", e);
        return 0;
    }

    ARG(G.k_raster, 0, G.b_uni);
    ARG(G.k_raster, 1, G.b_tri);
    ARG(G.k_raster, 2, G.b_bbox);
    ARG(G.k_raster, 3, G.b_texdata);
    ARG(G.k_raster, 4, G.b_textab);
    ARG(G.k_raster, 5, G.b_mats);
    ARG(G.k_raster, 6, G.b_lev);
    ARG(G.k_raster, 7, G.b_clr);
    size_t gs[2] = {(size_t)w, (size_t)h};
    e = p_clEnqueueNDRangeKernel(G.q, G.k_raster, 2, NULL, gs, NULL, 0, NULL, NULL);
    if (e != CL_SUCCESS)
    {
        gpu_fail("k_raster", e);
        return 0;
    }

    size_t n = (size_t)w * h;
    e = p_clEnqueueReadBuffer(G.q, G.b_lev, CL_FALSE, 0, n, G.lev, 0, NULL, NULL);
    if (e == CL_SUCCESS)
        e = p_clEnqueueReadBuffer(G.q, G.b_clr, CL_TRUE, 0, n * 4, G.clr, 0, NULL, NULL);
    if (e != CL_SUCCESS)
    {
        gpu_fail("clEnqueueReadBuffer", e);
        return 0;
    }

    struct screen *s = &g.scr;
    for (size_t i = 0; i < n; i++)
    {
        int lv = G.lev[i];
        s->chr[i] = lv == 255 ? " " : g.glyphs[lv <= g.nglyphs ? lv : g.nglyphs];
        s->clr[i] = G.clr[i];
        s->zbuf[i] = 0.0;
    }
    G.used_last_frame = 1;
    return 1;
}

void gpu_mesh_dirty(void)
{
    G.mesh_dirty = 1;
}

int gpu_active(void)
{
    return G.state == GPU_READY && G.used_last_frame && g.opt.gpu != 0 &&
           g.opt.mode == MODE_SOLID;
}

const char *gpu_device_name(void)
{
    return G.devname[0] ? G.devname : NULL;
}

const char *gpu_error(void)
{
    return G.err[0] ? G.err : NULL;
}

int gpu_warmup(void)
{
    return gpu_init();
}

void gpu_list(void)
{
    if (!load_lib())
    {
        printf("OpenCL: unavailable (%s)\n", G.err);
        return;
    }
    cl_platform_id plats[64];
    cl_device_id devs[64];
    int n = enum_devices(plats, devs, 64);
    if (n < 1)
    {
        printf("OpenCL: loader present, but no devices (no ICD driver installed?)\n");
        return;
    }
    printf("OpenCL devices (--gpu-device N):\n");
    for (int i = 0; i < n; i++)
    {
        char pn[128], dn[128], dv[128];
        pn[0] = '\0';
        p_clGetPlatformInfo(plats[i], CL_PLATFORM_NAME, sizeof pn, pn, NULL);
        pn[sizeof pn - 1] = '\0';
        dev_string(devs[i], CL_DEVICE_NAME, dn, sizeof dn);
        dev_string(devs[i], CL_DEVICE_VERSION, dv, sizeof dv);
        cl_device_type t = 0;
        cl_uint cu = 0;
        cl_ulong mem = 0;
        p_clGetDeviceInfo(devs[i], CL_DEVICE_TYPE, sizeof t, &t, NULL);
        p_clGetDeviceInfo(devs[i], CL_DEVICE_MAX_COMPUTE_UNITS, sizeof cu, &cu, NULL);
        p_clGetDeviceInfo(devs[i], CL_DEVICE_GLOBAL_MEM_SIZE, sizeof mem, &mem, NULL);
        const char *kind = (t & CL_DEVICE_TYPE_GPU)   ? "gpu"
                           : (t & CL_DEVICE_TYPE_CPU) ? "cpu"
                                                      : "other";
        printf("  %d  %-3s %s\n", i, kind, dn);
        printf("        %s | %u CU | %.0f MB | %s\n", pn, cu, mem / 1048576.0, dv);
    }
}

void gpu_shutdown(void)
{
    if (!G.lib)
        return;
    relmem(&G.b_uni);
    relmem(&G.b_vtx);
    relmem(&G.b_nrm);
    relmem(&G.b_uv);
    relmem(&G.b_vc);
    relmem(&G.b_idx);
    relmem(&G.b_fn);
    relmem(&G.b_tv);
    relmem(&G.b_pv);
    relmem(&G.b_vsh);
    relmem(&G.b_tri);
    relmem(&G.b_bbox);
    relmem(&G.b_texdata);
    relmem(&G.b_textab);
    relmem(&G.b_mats);
    relmem(&G.b_lev);
    relmem(&G.b_clr);
    if (G.k_xform)
        p_clReleaseKernel(G.k_xform);
    if (G.k_setup)
        p_clReleaseKernel(G.k_setup);
    if (G.k_raster)
        p_clReleaseKernel(G.k_raster);
    if (G.prog)
        p_clReleaseProgram(G.prog);
    if (G.q)
        p_clReleaseCommandQueue(G.q);
    if (G.ctx)
        p_clReleaseContext(G.ctx);
    free(G.lev);
    free(G.clr);
    G.lev = NULL;
    G.clr = NULL;
    G.k_xform = G.k_setup = G.k_raster = NULL;
    G.prog = NULL;
    G.q = NULL;
    G.ctx = NULL;
    G.state = GPU_OFF;
}
