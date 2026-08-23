#include "b3d.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct b3d g;

void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("B-3D: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n);
    if (!q)
        die("out of memory");
    return q;
}

char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = xrealloc(NULL, n);
    memcpy(d, s, n);
    return d;
}

void path_dir(const char *path, char *dir, size_t cap)
{
    snprintf(dir, cap, "%s", path);
    char *sl = strrchr(dir, '/');
    if (sl)
        *sl = '\0';
    else
        snprintf(dir, cap, ".");
}

void path_join(char *out, size_t cap, const char *dir, const char *rel)
{
    if (rel[0] == '/')
        snprintf(out, cap, "%s", rel);
    else
        snprintf(out, cap, "%s/%s", dir, rel);
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void ensure_caches(void)
{
    size_t n = g.mesh.capv > 0 ? (size_t)g.mesh.capv : 1;
    g.tv = xrealloc(g.tv, n * sizeof *g.tv);
    g.pv = xrealloc(g.pv, n * sizeof *g.pv);
    g.vi = xrealloc(g.vi, n * sizeof *g.vi);
    g.vspec = xrealloc(g.vspec, n * sizeof *g.vspec);
}

static void rebuild(void)
{
    mesh_build();
    ensure_caches();
    g.smooth = g.opt.shading == SH_SMOOTH ||
               (g.opt.shading == SH_AUTO && !g.mesh.flat_default);
    mat_resolve();
    gpu_mesh_dirty();
}

static void advance_spin(void)
{
    if (!g.opt.spin_on)
        return;
    g.opt.ax += g.opt.spx;
    g.opt.ay += g.opt.spy;
    g.opt.az += g.opt.spz;
}

static int model_index(const char *name)
{
    const struct b3d_model *m = b3d_model_find(name);
    return m ? (int)(m - B3D_MODELS) : -1;
}

static void cycle_model(int dir)
{
    g.opt.objfile[0] = '\0';
    int i = model_index(g.opt.model);
    if (i < 0)
        i = 0;
    g.opt.model = B3D_MODELS[(i + dir + B3D_NMODELS) % B3D_NMODELS].name;
    rebuild();
}

static void seg_step(int d)
{
    if (g.opt.seg == 0)
    {
        const struct b3d_model *m = b3d_model_find(g.opt.model);
        g.opt.seg = m ? m->seg : 12;
    }
    g.opt.seg += d * 4;
    if (g.opt.seg < 4)
        g.opt.seg = 4;
    if (g.opt.seg > 64)
        g.opt.seg = 64;
    rebuild();
}

static void dump_frame(void)
{
    FILE *f = fopen("B-3D.txt", "w");
    if (!f)
        return;
    for (int r = 0; r < g.scr.h; r++)
    {
        for (int c = 0; c < g.scr.w; c++)
            fputs(g.scr.chr[r * g.scr.w + c], f);
        fputc('\n', f);
    }
    fclose(f);
}

static const char *mode_name(void)
{
    switch (g.opt.mode)
    {
    case MODE_WIRE:
        return "wire";
    case MODE_POINTS:
        return "points";
    default:
        return "solid";
    }
}

static void hud_append(char *s, int *L, size_t cap, const char *fmt, ...)
{
    if (*L < 0 || (size_t)*L >= cap - 1)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s + *L, cap - (size_t)*L, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n >= cap - (size_t)*L)
        *L = (int)cap - 1;
    else
        *L += n;
}

static void draw_hud(void)
{
    char ds[32];
    double d = g.opt.dist;
    snprintf(ds, sizeof ds, "%d.%d", (int)d, (int)((d - (double)(int)d) * 10.0));
    char s[1024];
    int L = snprintf(s, sizeof s, " %s  %s/%s  %s", g.opt.model, mode_name(),
                     g.smooth ? "smooth" : "flat", g.opt.ramp);
    if (L < 0)
        return;
    if (L >= (int)sizeof s)
        L = (int)sizeof s - 1;
    int mi = g.globmat_id;
    if (mi >= 0 && mi < g.nmats)
        hud_append(s, &L, sizeof s, " %s", g.mats[mi].name);
    if (g.opt.tex_on && (g.tex || (mi >= 0 && mi < g.nmats && g.mats[mi].map)))
        hud_append(s, &L, sizeof s, " tex");
    if (g.opt.seg > 0)
        hud_append(s, &L, sizeof s, "(%d)", g.opt.seg);
    hud_append(s, &L, sizeof s, "  %d/%d  z%d%% d%s", g.mesh.nv, g.mesh.nf, g.opt.zoom, ds);
    if (g.fps_show)
        hud_append(s, &L, sizeof s, " %dfps", g.fps_show);
    if (gpu_active())
        hud_append(s, &L, sizeof s, " gpu");
    if (g.msg[0])
        hud_append(s, &L, sizeof s, "  <%s>", g.msg);
    hud_append(s, &L, sizeof s, "  [spc m f c t g n/p ,. 1-6 h x esc]");
    size_t sl = strlen(s);
    if (sl > (size_t)g.scr.w)
        s[g.scr.w] = '\0';
    printf("\033[%d;1H\033[7m%s\033[0m\033[K", g.scr.h + 1, s);
    fflush(stdout);
}

enum
{
    KEY_ESC = 27,
    KEY_UP = 256,
    KEY_DOWN,
    KEY_RIGHT,
    KEY_LEFT,
    KEY_NONE = -1
};

static int key_decode(const char *k)
{
    if (k[0] == KEY_ESC && (k[1] == '[' || k[1] == 'O') && k[2] && !k[3])
    {
        switch (k[2])
        {
        case 'A':
            return KEY_UP;
        case 'B':
            return KEY_DOWN;
        case 'C':
            return KEY_RIGHT;
        case 'D':
            return KEY_LEFT;
        default:
            return KEY_NONE;
        }
    }
    if (k[1] != '\0')
        return KEY_NONE;
    return (unsigned char)k[0];
}

static int handle_key(const char *k)
{
    struct options *o = &g.opt;
    g.msg[0] = '\0';
    int c = key_decode(k);
    switch (c)
    {
    case KEY_ESC:
        return 1;

    case ' ':
        o->spin_on ^= 1;
        break;
    case 'r':
    case 'R':
        o->ax = 20;
        o->ay = 30;
        o->az = 0;
        o->offx = o->offy = o->offz = 0;
        o->zoom = 100;
        o->dist = 3.0;
        o->spin_on = 1;
        render_update_proj();
        break;
    case 'm':
    case 'M':
        o->mode = (o->mode + 1) % 3;
        break;
    case 'f':
    case 'F':
        g.smooth ^= 1;
        break;
    case 'c':
    case 'C':
        g.usecolor ^= 1;
        if (g.usecolor)
            render_clear_colors();
        break;
    case 't':
    case 'T':
        o->tex_on ^= 1;
        snprintf(g.msg, sizeof g.msg, "texture %s", o->tex_on ? "on" : "off");
        break;
    case 'g':
    case 'G':
        o->gpu ^= 1;
        if (o->gpu && !gpu_device_name())
        {
            render_frame();
            if (!gpu_active())
            {
                o->gpu = 0;
                snprintf(g.msg, sizeof g.msg, "no gpu: %s",
                         gpu_error() ? gpu_error() : "unavailable");
                break;
            }
        }
        snprintf(g.msg, sizeof g.msg, "gpu %s%s%s", o->gpu ? "on" : "off",
                 o->gpu && gpu_device_name() ? ": " : "",
                 o->gpu && gpu_device_name() ? gpu_device_name() : "");
        break;
    case 'h':
    case 'H':
        o->hud ^= 1;
        term_apply_size();
        break;
    case 'x':
    case 'X':
        dump_frame();
        snprintf(g.msg, sizeof g.msg, "saved B-3D.txt");
        break;

    case 'n':
    case 'N':
        cycle_model(1);
        break;
    case 'p':
    case 'P':
        cycle_model(-1);
        break;
    case ',':
        seg_step(-1);
        break;
    case '.':
        seg_step(1);
        break;

    case '+':
    case '=':
        if (o->zoom <= 390)
            o->zoom += 10;
        render_update_proj();
        break;
    case '-':
    case '_':
        if (o->zoom >= 20)
            o->zoom -= 10;
        render_update_proj();
        break;
    case '[':
        if (o->dist > 1.0)
            o->dist -= 0.25;
        render_update_proj();
        break;
    case ']':
        if (o->dist < 12.0)
            o->dist += 0.25;
        render_update_proj();
        break;

    case 'i':
    case 'I':
        if (o->offy < 3.0)
            o->offy += 0.25;
        break;
    case 'k':
    case 'K':
        if (o->offy > -3.0)
            o->offy -= 0.25;
        break;
    case 'j':
    case 'J':
        if (o->offx > -3.0)
            o->offx -= 0.25;
        break;
    case 'l':
    case 'L':
        if (o->offx < 3.0)
            o->offx += 0.25;
        break;
    case 'u':
    case 'U':
        if (o->offz < 2.0)
            o->offz += 0.25;
        break;
    case 'o':
    case 'O':
        if (o->offz > -2.0)
            o->offz -= 0.25;
        break;

    case 'w':
    case 'W':
    case KEY_UP:
        o->ax -= 6;
        break;
    case 's':
    case 'S':
    case KEY_DOWN:
        o->ax += 6;
        break;
    case 'a':
    case 'A':
    case KEY_LEFT:
        o->ay -= 6;
        break;
    case 'd':
    case 'D':
    case KEY_RIGHT:
        o->ay += 6;
        break;
    case 'q':
    case 'Q':
        o->az += 6;
        break;
    case 'e':
    case 'E':
        o->az -= 6;
        break;

    default:
        if (c >= '1' && c <= '9' && c - '1' < B3D_NRAMPS)
        {
            snprintf(o->ramp, sizeof o->ramp, "%s", B3D_RAMPS[c - '1'].name);
            render_set_ramp(o->ramp);
        }
        break;
    }
    return 0;
}

static void interactive_loop(void)
{
    double budget = 1.0 / g.opt.fps;
    double next = now_sec() + budget;
    double stat_last = 0.0;
    int stat_n = 0;
    while (g.running)
    {
        if (g.winch)
        {
            g.winch = 0;
            term_detect_size();
            term_apply_size();
            fputs("\033[2J", stdout);
            fflush(stdout);
        }
        render_frame();
        render_present();
        if (!g.running)
            break;
        if (g.opt.hud)
            draw_hud();
        advance_spin();
        double t = now_sec();
        if (stat_last == 0.0)
        {
            stat_last = t;
            stat_n = 0;
        }
        else
        {
            stat_n++;
            double rem = t - stat_last;
            if (rem >= 0.5)
            {
                g.fps_show = (int)((double)stat_n / rem + 0.5);
                stat_last = t;
                stat_n = 0;
            }
        }
        double leftms = (next - t) * 1000.0;
        if (leftms < 1.0)
            leftms = 1.0;
        next += budget;
        char key[8];
        int n = term_read_key(key, (int)leftms);
        if (n == -1)
            break;
        if (n > 0 && handle_key(key))
            break;
    }
}

static void batch_loop(void)
{
    for (long i = 0; i < g.opt.frames && g.running; i++)
    {
        render_frame();
        render_present();
        advance_spin();
    }
}

static void on_winch(int sig)
{
    (void)sig;
    g.winch = 1;
}
static void on_stop(int sig)
{
    (void)sig;
    g.running = 0;
}

int main(int argc, char **argv)
{
    memset(&g, 0, sizeof g);
    cli_parse(argc, argv);
    if (g.opt.list)
    {
        cli_list();
        return 0;
    }
    if (g.opt.gpu_info)
    {
        gpu_list();
        return 0;
    }
    int cores = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (cores < 1)
        cores = 1;
    g.jobs_eff = g.opt.jobs > 0 ? g.opt.jobs : (cores > 8 ? 8 : cores);
    if (g.jobs_eff < 1)
        g.jobs_eff = 1;
    if (g.jobs_eff > 16)
        g.jobs_eff = 16;
    render_set_ramp(g.opt.ramp);
    render_resolve_color();
    rebuild();
    render_setup_light();
    if (g.opt.gpu && !gpu_warmup())
    {
        fprintf(stderr, "B-3D: --gpu unavailable (%s); rendering on the CPU\n",
                gpu_error() ? gpu_error() : "unknown reason");
        g.opt.gpu = 0;
    }
    term_detect_size();
    if (g.opt.frames == 0)
    {
        if (!isatty(0) || !isatty(1))
            die("stdout is not a terminal; use --frames N for headless rendering");
        g.interactive = 1;
    }
    term_apply_size();
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_winch;
    sigaction(SIGWINCH, &sa, NULL);
    sigaction(SIGCONT, &sa, NULL);
    sa.sa_handler = on_stop;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    atexit(term_cleanup);
    g.running = 1;
    render_pool_start();
    if (g.interactive)
    {
        term_enter_tui();
        interactive_loop();
    }
    else
    {
        batch_loop();
    }
    render_pool_stop();
    gpu_shutdown();
    return 0;
}
