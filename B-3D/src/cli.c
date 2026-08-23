#include "b3d.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void defaults(struct options *o)
{
    memset(o, 0, sizeof *o);
    o->model = "torus";
    o->mode = MODE_SOLID;
    o->shading = SH_AUTO;
    o->color = -1;
    snprintf(o->ramp, sizeof o->ramp, "classic");
    o->dist = 3.0;
    o->zoom = 100;
    o->ambient = 0.22;
    o->specular = 0.70;
    o->aspect_n = 2;
    o->aspect_d = 1;
    o->ax = 20;
    o->ay = 30;
    o->spx = 1;
    o->spy = 2;
    o->spin_on = 1;
    o->cull = 1;
    o->fps = 30;
    o->hud = 1;
    o->cr = 90;
    o->cg = 190;
    o->cb = 255;
    o->tex_on = 1;
    o->gpu_dev = -1;
}

#define NEEDVAL()                                      \
    do                                                 \
    {                                                  \
        if (i + 1 >= argc)                             \
            die("option '%s' needs a value", argv[i]); \
        val = argv[++i];                               \
    } while (0)

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi
                                                                       : v; }

static long parse_long(const char *val, const char *opt)
{
    char *end;
    errno = 0;
    long v = strtol(val, &end, 10);
    if (end == val || *end != '\0')
        die("option '%s' wants a whole number, got '%s'", opt, val);
    if (errno == ERANGE)
        die("option '%s' value out of range: '%s'", opt, val);
    return v;
}

static int parse_int(const char *val, const char *opt)
{
    long v = parse_long(val, opt);
    if (v < INT_MIN || v > INT_MAX)
        die("option '%s' value out of range: '%s'", opt, val);
    return (int)v;
}

static double parse_double(const char *val, const char *opt)
{
    char *end;
    errno = 0;
    double v = strtod(val, &end);
    if (end == val || *end != '\0')
        die("option '%s' wants a number, got '%s'", opt, val);
    return v;
}

static void parse_triple(const char *val, const char *opt, const char *form,
                         int *x, int *y, int *z)
{
    char tail;
    if (sscanf(val, "%d,%d,%d%c", x, y, z, &tail) != 3)
        die("option '%s' wants %s, got '%s'", opt, form, val);
}

static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi
                                                                                   : v; }

void cli_parse(int argc, char **argv)
{
    struct options *o = &g.opt;
    defaults(o);
    for (int i = 1; i < argc; i++)
    {
        const char *a = argv[i];
        const char *val = NULL;
        if (!strcmp(a, "--model"))
        {
            NEEDVAL();
            o->model = val;
        }
        else if (!strcmp(a, "--obj") || !strcmp(a, "--file"))
        {
            NEEDVAL();
            snprintf(o->objfile, sizeof o->objfile, "%s", val);
        }
        else if (!strcmp(a, "--seg"))
        {
            NEEDVAL();
            int v = parse_int(val, a);
            if (v < 0)
                die("--seg wants 0 (per-model default) or 4..64, got '%s'", val);
            o->seg = v ? clampi(v, 4, 64) : 0;
        }
        else if (!strcmp(a, "--list"))
        {
            o->list = 1;
        }
        else if (!strcmp(a, "--mode"))
        {
            NEEDVAL();
            if (!strcmp(val, "solid"))
                o->mode = MODE_SOLID;
            else if (!strcmp(val, "wire"))
                o->mode = MODE_WIRE;
            else if (!strcmp(val, "points"))
                o->mode = MODE_POINTS;
            else
                die("bad --mode '%s'", val);
        }
        else if (!strcmp(a, "--shading"))
        {
            NEEDVAL();
            if (!strcmp(val, "flat"))
                o->shading = SH_FLAT;
            else if (!strcmp(val, "smooth"))
                o->shading = SH_SMOOTH;
            else if (!strcmp(val, "auto"))
                o->shading = SH_AUTO;
            else
                die("bad --shading '%s'", val);
        }
        else if (!strcmp(a, "--ramp"))
        {
            NEEDVAL();
            snprintf(o->ramp, sizeof o->ramp, "%s", val);
        }
        else if (!strcmp(a, "--ramp-chars"))
        {
            NEEDVAL();
            snprintf(o->custom, sizeof o->custom, "%s", val);
            snprintf(o->ramp, sizeof o->ramp, "custom");
        }
        else if (!strcmp(a, "--color"))
        {
            o->color = 1;
        }
        else if (!strcmp(a, "--no-color"))
        {
            o->color = 0;
        }
        else if (!strcmp(a, "--rgb"))
        {
            NEEDVAL();
            int r, gg, b;
            parse_triple(val, a, "R,G,B", &r, &gg, &b);
            o->cr = clampi(r, 0, 255);
            o->cg = clampi(gg, 0, 255);
            o->cb = clampi(b, 0, 255);
        }
        else if (!strcmp(a, "--no-cull"))
        {
            o->cull = 0;
        }
        else if (!strcmp(a, "--cull"))
        {
            o->cull = 1;
        }
        else if (!strcmp(a, "--texture"))
        {
            NEEDVAL();
            snprintf(o->texfile, sizeof o->texfile, "%s", val);
        }
        else if (!strcmp(a, "--material"))
        {
            NEEDVAL();
            snprintf(o->matfile, sizeof o->matfile, "%s", val);
        }
        else if (!strcmp(a, "--uv"))
        {
            NEEDVAL();
            if (!strcmp(val, "auto"))
                o->uv_mode = UV_AUTO;
            else if (!strcmp(val, "sphere"))
                o->uv_mode = UV_SPHERE;
            else if (!strcmp(val, "cyl") || !strcmp(val, "cylinder"))
                o->uv_mode = UV_CYL;
            else if (!strcmp(val, "plane"))
                o->uv_mode = UV_PLANE;
            else if (!strcmp(val, "box"))
                o->uv_mode = UV_BOX;
            else
                die("bad --uv '%s' (auto sphere cyl plane box)", val);
        }
        else if (!strcmp(a, "--filter"))
        {
            NEEDVAL();
            if (!strcmp(val, "nearest"))
                o->filter = TEX_NEAREST;
            else if (!strcmp(val, "linear") || !strcmp(val, "bilinear"))
                o->filter = TEX_LINEAR;
            else
                die("bad --filter '%s' (nearest linear)", val);
        }
        else if (!strcmp(a, "--no-texture"))
        {
            o->tex_on = 0;
        }
        else if (!strcmp(a, "--ambient"))
        {
            NEEDVAL();
            o->ambient = clampd(parse_double(val, a) / 100.0, 0.0, 1.0);
        }
        else if (!strcmp(a, "--specular"))
        {
            NEEDVAL();
            o->specular = clampd(parse_double(val, a) / 100.0, 0.0, 4.0);
        }
        else if (!strcmp(a, "--size"))
        {
            NEEDVAL();
            int w, h;
            char tail;
            if (sscanf(val, "%dx%d%c", &w, &h, &tail) != 2 || w <= 0 || h <= 0)
                die("--size wants WxH, got '%s'", val);
            g.scr.req_w = w;
            g.scr.req_h = h;
        }
        else if (!strcmp(a, "--dist"))
        {
            NEEDVAL();
            o->dist = clampd(parse_double(val, a), 1.0, 20.0);
        }
        else if (!strcmp(a, "--zoom"))
        {
            NEEDVAL();
            o->zoom = clampi(parse_int(val, a), 5, 800);
        }
        else if (!strcmp(a, "--angles"))
        {
            NEEDVAL();
            parse_triple(val, a, "X,Y,Z", &o->ax, &o->ay, &o->az);
        }
        else if (!strcmp(a, "--spin"))
        {
            NEEDVAL();
            parse_triple(val, a, "X,Y,Z", &o->spx, &o->spy, &o->spz);
        }
        else if (!strcmp(a, "--no-spin"))
        {
            o->spx = o->spy = o->spz = 0;
        }
        else if (!strcmp(a, "--aspect"))
        {
            NEEDVAL();
            int n, d;
            char tail;
            if (sscanf(val, "%d:%d%c", &n, &d, &tail) != 2 || n < 1 || d < 1)
                die("--aspect wants N:D, got '%s'", val);
            o->aspect_n = n;
            o->aspect_d = d;
        }
        else if (!strcmp(a, "--fps"))
        {
            NEEDVAL();
            o->fps = clampi(parse_int(val, a), 1, 240);
        }
        else if (!strcmp(a, "--frames"))
        {
            NEEDVAL();
            long v = parse_long(val, a);
            o->frames = v < 0 ? 0 : v;
        }
        else if (!strcmp(a, "--jobs"))
        {
            NEEDVAL();
            o->jobs = clampi(parse_int(val, a), 0, 16);
        }
        else if (!strcmp(a, "--gpu"))
        {
            o->gpu = 1;
        }
        else if (!strcmp(a, "--no-gpu"))
        {
            o->gpu = 0;
        }
        else if (!strcmp(a, "--gpu-device"))
        {
            NEEDVAL();
            o->gpu = 1;
            o->gpu_dev = clampi(parse_int(val, a), 0, 63);
        }
        else if (!strcmp(a, "--gpu-info"))
        {
            o->gpu_info = 1;
        }
        else if (!strcmp(a, "--no-hud"))
        {
            o->hud = 0;
        }
        else if (!strcmp(a, "--help") || !strcmp(a, "-h"))
        {
            cli_usage();
            exit(0);
        }
        else
        {
            die("unknown option '%s' (see --help)", a);
        }
    }
    if (!strcmp(o->ramp, "custom"))
    {
        if (!o->custom[0])
            die("--ramp-chars needs a non-empty string");
    }
    else if (!b3d_ramp_find(o->ramp))
    {
        die("bad --ramp '%s' (try --list)", o->ramp);
    }
}

void cli_usage(void)
{
    fputs(
        "B-3D — a 3D ASCII renderer in C\n"
        "\n"
        "    B-3D [options]\n"
        "\n"
        "MODELS\n"
        "    --model NAME      cube sphere torus knot pyramid cone cylinder\n"
        "                      octahedron icosahedron plane   (default: torus)\n"
        "    --obj, --file F   load a model file instead (--list for formats)\n"
        "    --seg N           tessellation detail 4..64, 0 = per-model default\n"
        "    --list            list built-in models and ASCII ramps, then exit\n"
        "\n"
        "RENDERING\n"
        "    --mode M          solid | wire | points            (default: solid)\n"
        "    --shading S       flat | smooth | auto             (default: auto)\n"
        "    --ramp NAME       simple classic dense blocks dots binary\n"
        "    --ramp-chars STR  use STR literally, dark -> bright\n"
        "    --color / --no-color        24-bit colour (default: auto-detect)\n"
        "    --rgb R,G,B       base material colour (default: 90,190,255)\n"
        "    --texture FILE    map an image onto the surface (--list for formats)\n"
        "    --material FILE   load a Wavefront .mtl material library\n"
        "    --uv MODE         force UV mapping: auto sphere cyl plane box\n"
        "    --filter M        texture filter: nearest | linear (default: nearest)\n"
        "    --no-texture      disable texture sampling\n"
        "    --no-cull         disable backface culling (show interiors)\n"
        "    --ambient PCT     ambient light 0..100            (default: 22)\n"
        "    --specular PCT    specular highlight 0..100       (default: 70)\n"
        "    --jobs N          rasteriser threads, 1 = disable (default: auto)\n"
        "\n"
        "GPU\n"
        "    --gpu             rasterise on an OpenCL device (solid mode)\n"
        "    --gpu-device N    use device N from --gpu-info\n"
        "    --no-gpu          stay on the CPU                     (default)\n"
        "    --gpu-info        list the OpenCL devices found, then exit\n"
        "\n"
        "CAMERA / MOTION\n"
        "    --size WxH        framebuffer size (default: terminal size)\n"
        "    --dist F          camera distance, e.g. 3.5       (default: 3.0)\n"
        "    --zoom PCT        zoom percentage                 (default: 100)\n"
        "    --angles X,Y,Z    initial euler angles in degrees\n"
        "    --spin X,Y,Z      auto-rotation speed, deg/frame  (default: 1,2,0)\n"
        "    --no-spin         start paused\n"
        "    --aspect N:D      character aspect ratio          (default: 2:1)\n"
        "\n"
        "OUTPUT\n"
        "    --fps N           target frames per second        (default: 30)\n"
        "    --frames N        render N frames and exit (non-interactive)\n"
        "    --no-hud          hide the status line\n"
        "    --help            this text\n"
        "\n"
        "INTERACTIVE KEYS\n"
        "    arrows / WASD  rotate        q e      roll\n"
        "    + -            zoom          [ ]      move camera closer / further\n"
        "    IJKL           pan object    U O      push object along Z\n"
        "    space          pause spin    r        reset view\n"
        "    m              cycle solid/wire/points\n"
        "    f              toggle flat/smooth shading\n"
        "    c              toggle colour\n"
        "    t              toggle texture sampling\n"
        "    n              next model    p        previous model\n"
        "    , .            decrease / increase tessellation\n"
        "    1..6           select ASCII ramp\n"
        "    g              toggle GPU rasteriser\n"
        "    h              toggle HUD    x        dump current frame to B-3D.txt\n"
        "    ESC / Ctrl-C   quit\n",
        stdout);
}

void cli_list(void)
{
    printf("Built-in models:\n");
    for (int i = 0; i < B3D_NMODELS; i++)
        printf("  %-12s seg %d\n", B3D_MODELS[i].name, B3D_MODELS[i].seg);
    printf("\nASCII ramps (dark -> bright):\n");
    for (int k = 0; k < B3D_NRAMPS; k++)
        printf("  %d %-8s |%s|\n", k + 1, B3D_RAMPS[k].name, B3D_RAMPS[k].glyphs);
    printf("\nModel file formats (--obj / --file):\n");
    for (int i = 0; i < B3D_NFORMATS; i++)
        printf("  .%-5s %s\n", B3D_FORMATS[i].ext, B3D_FORMATS[i].desc);
    printf("\nTexture file formats (--texture, and maps named by a .mtl):\n");
    for (int i = 0; i < B3D_NIMGFMTS; i++)
        printf("  .%-5s %s\n", B3D_IMGFMTS[i].ext, B3D_IMGFMTS[i].name);
    printf("\nMaterials: Wavefront .mtl (Ka Kd Ks Ke Ns d Tr\n");
    printf("           map_Ka map_Kd map_Ke map_Ks map_d)\n");
}
