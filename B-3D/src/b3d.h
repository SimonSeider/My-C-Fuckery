#define _DEFAULT_SOURCE

#ifndef B3D_H
#define B3D_H

#include <math.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <signal.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define NEAR_Z 0.25
#define B3D_MAX_GLYPHS 120

enum
{
    MODE_SOLID,
    MODE_WIRE,
    MODE_POINTS
};
enum
{
    SH_FLAT,
    SH_SMOOTH,
    SH_AUTO
};
enum
{
    TEX_NEAREST,
    TEX_LINEAR
};
enum
{
    UV_AUTO,
    UV_SPHERE,
    UV_CYL,
    UV_PLANE,
    UV_BOX
};

typedef struct
{
    double x, y, z;
} vec3;

struct texture
{
    char path[4096];
    int w, h;
    unsigned char *rgb;
    unsigned char *alpha;
    double avg_lum;
};

struct material
{
    char name[64];
    double ka[3], kd[3], ks[3], ke[3];
    double ns;
    struct texture *map;
    struct texture *map_ke;
    struct texture *map_ks;
    struct texture *map_d;
};

static inline vec3 v3(double x, double y, double z)
{
    vec3 v = {x, y, z};
    return v;
}
static inline vec3 v3sub(vec3 a, vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline vec3 v3neg(vec3 a) { return v3(-a.x, -a.y, -a.z); }
static inline vec3 v3cross(vec3 a, vec3 b)
{
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline double v3dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline vec3 v3norm(vec3 a)
{
    double s = a.x * a.x + a.y * a.y + a.z * a.z;
    if (s <= 0.0)
        return v3(0.0, 0.0, 0.0);
    double k = 1.0 / sqrt(s);
    return v3(a.x * k, a.y * k, a.z * k);
}

struct mesh
{
    vec3 *v;
    vec3 *vn;
    vec3 *fn;
    double *vt;
    double *vc;
    int *idx;
    int *fmat;
    int *fname;
    char **fmtnames;
    int nfn;
    char *mtllib[8];
    int nmtllib;
    int has_uv;
    int has_vcol;
    int nv, nf;
    int capv, capf;
    int flat_default;
};

struct options
{
    const char *model;
    char objfile[4096];
    int seg;
    int mode;
    int shading;
    int color;
    char ramp[32];
    char custom[256];
    double dist;
    int zoom;
    double ambient, specular;
    int aspect_n, aspect_d;
    int ax, ay, az;
    int spx, spy, spz;
    double offx, offy, offz;
    int spin_on, cull;
    int fps;
    long frames;
    int jobs;
    int hud;
    int cr, cg, cb;
    char texfile[4096];
    char matfile[4096];
    int uv_mode;
    int tex_on;
    int filter;
    int list;
    int gpu;
    int gpu_dev;
    int gpu_info;
};

struct screen
{
    int w, h;
    int cols, rows;
    int req_w, req_h;
    const char **chr;
    double *zbuf;
    int32_t *clr;
    double cx, cy, px, py;
};

struct b3d
{
    struct options opt;
    struct mesh mesh;
    struct screen scr;
    int usecolor, smooth, interactive, tui;
    const char *glyphs[B3D_MAX_GLYPHS];
    int nglyphs;
    vec3 light, halfv;
    double rot[9];
    vec3 *tv, *pv;
    double *vi, *vspec;
    int jobs_eff;
    char msg[128];
    int fps_show;
    struct texture **texs;
    int ntexs, captexs;
    struct material *mats;
    int nmats, capmats;
    int globmat_id;
    struct texture *tex;
    volatile sig_atomic_t winch, running;
};

extern struct b3d g;

struct b3d_model
{
    const char *name;
    int seg;
    void (*build)(struct mesh *m, int seg);
};

struct b3d_format
{
    const char *ext;
    const char *desc;
    void (*load)(struct mesh *m, const char *path);
};

struct b3d_imgfmt
{
    const char *name;
    const char *ext;
    unsigned char magic[8];
    int nmagic;
    unsigned char *(*load)(FILE *f, int *w, int *h, unsigned char **alpha);
};

struct b3d_ramp
{
    const char *name;
    const char *glyphs;
};

extern const struct b3d_model B3D_MODELS[];
extern const int B3D_NMODELS;
extern const struct b3d_format B3D_FORMATS[];
extern const int B3D_NFORMATS;
extern const struct b3d_imgfmt B3D_IMGFMTS[];
extern const int B3D_NIMGFMTS;
extern const struct b3d_ramp B3D_RAMPS[];
extern const int B3D_NRAMPS;

const struct b3d_model *b3d_model_find(const char *name);
const struct b3d_ramp *b3d_ramp_find(const char *name);

void die(const char *fmt, ...);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
void path_dir(const char *path, char *dir, size_t cap);
void path_join(char *out, size_t cap, const char *dir, const char *rel);

int mesh_build(void);
void mesh_free(struct mesh *m);
void mesh_gen_uvs(struct mesh *m, int mode);
void mesh_normalize(struct mesh *m);

int mesh_addv(struct mesh *m, vec3 p);
void mesh_setuv(struct mesh *m, int i, double u, double v);
void mesh_setcol(struct mesh *m, int i, double r, double g, double b);
int mesh_addf(struct mesh *m, int a, int b, int c);
void mesh_addpoly(struct mesh *m, const int *v, int n, int name);
int mesh_intern_name(struct mesh *m, const char *s);

void model_load(struct mesh *m, const char *path);
const struct b3d_format *b3d_format_find(const char *path);
void obj_load(struct mesh *m, const char *path);
void stl_load(struct mesh *m, const char *path);
void ply_load(struct mesh *m, const char *path);
void off_load(struct mesh *m, const char *path);
void gltf_load(struct mesh *m, const char *path);

struct texture *tex_cache_get(const char *path);
struct texture *tex_from_memory(const char *name, const unsigned char *buf, size_t n);
unsigned char *stb_load(FILE *f, int *ow, int *oh, unsigned char **alpha);
unsigned char *stb_load_hdr(FILE *f, int *ow, int *oh, unsigned char **alpha);
unsigned char *dds_load(FILE *f, int *ow, int *oh, unsigned char **alpha);
unsigned char *qoi_load(FILE *f, int *ow, int *oh, unsigned char **alpha);
void tex_sample(const struct texture *t, double u, double v, double c[3]);
double tex_alpha(const struct texture *t, double u, double v);
int mat_load_lib(const char *path);
int mat_register(const struct material *mt);
void mat_resolve(void);

void render_set_ramp(const char *name);
void render_resolve_color(void);
void render_setup_light(void);
void render_update_proj(void);
void render_resize_fb(void);
void render_clear_colors(void);
void render_frame(void);
void render_pool_start(void);
void render_pool_stop(void);
void render_present(void);
double render_shade(vec3 n);

void term_detect_size(void);
void term_apply_size(void);
void term_enter_tui(void);
void term_cleanup(void);
int term_read_key(char *out, int ms);

int gpu_render_frame(void);
void gpu_mesh_dirty(void);
int gpu_active(void);
const char *gpu_device_name(void);
const char *gpu_error(void);
int gpu_warmup(void);
void gpu_list(void);
void gpu_shutdown(void);

void cli_parse(int argc, char **argv);
void cli_usage(void);
void cli_list(void);

#endif
