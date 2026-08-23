/*
 * dfpwm - convert WAV audio to ComputerCraft's DFPWM1a format (and back).
 * Copyright (c) 2026 Exil_S (Simon)  <exil@exil.dev>
 * Build: gcc -O3 -std=c11 -Wall -Wextra -pthread -o build/dfpwm dfpwm.c -lm
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CC_SAMPLE_RATE 48000
#define DF_PI 3.14159265358979323846

#define PREC 10
#define PREC_POW (1 << PREC)
#define PREC_POW_HALF (1 << (PREC - 1))
#define STRENGTH_MIN (1 << (PREC - 8 + 1))

_Static_assert((-1 >> 1) == -1, "arithmetic right shift required");
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "this build assumes a little-endian host"
#endif

#define WAVE_PCM 0x0001
#define WAVE_FLOAT 0x0003
#define WAVE_EXTENSIBLE 0xFFFE

static _Thread_local char g_err[512];

static bool fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof g_err, fmt, ap);
    va_end(ap);
    return false;
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
    {
        fprintf(stderr, "dfpwm: out of memory (wanted %zu bytes)\n", n);
        exit(1);
    }
    return p;
}
// Buffered per-file logging, so parallel batch runs don't interleave.
typedef struct
{
    char *buf;
    size_t len, cap;
    bool direct; // write to stderr immediately
    bool quiet;
} Log;

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

static void log_msg(Log *L, const char *fmt, ...)
{
    if (!L || L->quiet)
        return;
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (L->direct)
    {
        fputs(line, stderr);
        return;
    }
    size_t need = L->len + (size_t)n + 1;
    if (need > L->cap)
    {
        L->cap = need * 2;
        L->buf = realloc(L->buf, L->cap);
        if (!L->buf)
            exit(1);
    }
    memcpy(L->buf + L->len, line, (size_t)n + 1);
    L->len += (size_t)n;
}

static void log_flush(Log *L)
{
    if (!L || L->direct || !L->len)
        return;
    pthread_mutex_lock(&g_log_lock);
    fwrite(L->buf, 1, L->len, stderr);
    fflush(stderr);
    pthread_mutex_unlock(&g_log_lock);
    L->len = 0;
}

static void fmt_time(double seconds, char *out, size_t n)
{
    int m = (int)(seconds / 60.0);
    snprintf(out, n, "%d:%05.2f", m, seconds - m * 60.0);
}

typedef void (*range_fn)(void *ctx, size_t begin, size_t end, int tid);

typedef struct
{
    range_fn fn;
    void *ctx;
    size_t begin, end;
    int tid;
} PfTask;

static void *pf_thread(void *p)
{
    PfTask *t = p;
    t->fn(t->ctx, t->begin, t->end, t->tid);
    return NULL;
}

static void parallel_for(size_t n, int jobs, range_fn fn, void *ctx)
{
    if (n == 0)
        return;
    if (jobs < 1)
        jobs = 1;

    const size_t min_chunk = 1u << 14;
    size_t max_jobs = (n + min_chunk - 1) / min_chunk;
    if ((size_t)jobs > max_jobs)
        jobs = (int)max_jobs;
    if (jobs <= 1)
    {
        fn(ctx, 0, n, 0);
        return;
    }

    PfTask *tasks = xmalloc(sizeof *tasks * (size_t)jobs);
    pthread_t *th = xmalloc(sizeof *th * (size_t)jobs);
    bool *started = xmalloc(sizeof *started * (size_t)jobs);

    size_t chunk = n / (size_t)jobs, rem = n % (size_t)jobs, pos = 0;
    for (int i = 0; i < jobs; i++)
    {
        size_t c = chunk + (i < (int)rem ? 1 : 0);
        tasks[i] = (PfTask){fn, ctx, pos, pos + c, i};
        pos += c;
    }
    for (int i = 1; i < jobs; i++)
    {
        started[i] = pthread_create(&th[i], NULL, pf_thread, &tasks[i]) == 0;
        if (!started[i])
            pf_thread(&tasks[i]);
    }
    pf_thread(&tasks[0]);
    for (int i = 1; i < jobs; i++)
        if (started[i])
            pthread_join(th[i], NULL);

    free(tasks);
    free(th);
    free(started);
}

static int cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

typedef void (*progress_fn)(void *ud, size_t done, size_t total);

static void dfpwm_encode(const int8_t *samples, size_t n, uint8_t *out,
                         progress_fn prog, void *ud)
{
    int charge = 0, strength = 0, previous_bit = 0;
    size_t nbytes = (n + 7) / 8;

    for (size_t i = 0; i < nbytes; i++)
    {
        int this_byte = 0;
        for (int j = 0; j < 8; j++)
        {
            size_t k = i * 8 + (size_t)j;
            int v = k < n ? samples[k] : 0;

            int current_bit = (v > charge) || (v == charge && v == 127);
            this_byte = (this_byte >> 1) | (current_bit ? 128 : 0);

            int target = current_bit ? 127 : -128;
            int next_charge =
                charge + ((strength * (target - charge) + PREC_POW_HALF) >> PREC);
            if (next_charge == charge && next_charge != target)
                next_charge += current_bit ? 1 : -1;

            int same = current_bit == previous_bit;
            int z = same ? PREC_POW - 1 : 0;
            if (strength != z)
                strength += same ? 1 : -1;
            if (strength < STRENGTH_MIN)
                strength = STRENGTH_MIN;

            charge = next_charge;
            previous_bit = current_bit;
        }
        out[i] = (uint8_t)this_byte;
        if (prog && (i & 0x1FFFF) == 0)
            prog(ud, i, nbytes);
    }
    if (prog)
        prog(ud, nbytes, nbytes);
}

static void dfpwm_decode(const uint8_t *data, size_t n, int8_t *out)
{
    int charge = 0, strength = 0, previous_bit = 0;
    int low_pass_charge = 0, previous_charge = 0;
    size_t o = 0;

    for (size_t i = 0; i < n; i++)
    {
        int input_byte = data[i];
        for (int j = 0; j < 8; j++)
        {
            int current_bit = input_byte & 1;
            input_byte >>= 1;

            int target = current_bit ? 127 : -128;
            int next_charge =
                charge + ((strength * (target - charge) + PREC_POW_HALF) >> PREC);
            if (next_charge == charge && next_charge != target)
                next_charge += current_bit ? 1 : -1;

            int same = current_bit == previous_bit;
            int z = same ? PREC_POW - 1 : 0;
            if (strength != z)
                strength += same ? 1 : -1;
            if (strength < STRENGTH_MIN)
                strength = STRENGTH_MIN;

            charge = next_charge;

            int antijerk = charge;
            if (!same)
                antijerk = (charge + previous_charge + 1) >> 1;

            previous_charge = charge;
            previous_bit = current_bit;
            low_pass_charge += ((antijerk - low_pass_charge) * 140 + 0x80) >> 8;
            out[o++] = (int8_t)low_pass_charge;
        }
    }
}

static uint8_t *read_fd_all(int fd, size_t *out_len)
{
    size_t cap = 1 << 20, len = 0;
    uint8_t *buf = xmalloc(cap);
    for (;;)
    {
        if (len == cap)
        {
            cap *= 2;
            uint8_t *nb = realloc(buf, cap);
            if (!nb)
            {
                free(buf);
                fail("out of memory");
                return NULL;
            }
            buf = nb;
        }
        ssize_t r = read(fd, buf + len, cap - len);
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            free(buf);
            fail("read failed: %s", strerror(errno));
            return NULL;
        }
        if (r == 0)
            break;
        len += (size_t)r;
    }
    *out_len = len;
    return buf;
}

static uint8_t *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        fail("%s: %s", path, strerror(errno));
        return NULL;
    }

    struct stat st;
    if (fstat(fileno(f), &st) == 0 && S_ISREG(st.st_mode))
    {
        size_t len = (size_t)st.st_size;
        uint8_t *buf = xmalloc(len ? len : 1);
        if (len && fread(buf, 1, len, f) != len)
        {
            free(buf);
            fclose(f);
            fail("%s: short read", path);
            return NULL;
        }
        fclose(f);
        *out_len = len;
        return buf;
    }
    uint8_t *buf = read_fd_all(fileno(f), out_len);
    fclose(f);
    return buf;
}

static bool write_file(const char *path, const void *data, size_t len)
{
    if (strcmp(path, "-") == 0)
    {
        if (fwrite(data, 1, len, stdout) != len || fflush(stdout) != 0)
            return fail("<stdout>: %s", strerror(errno));
        return true;
    }
    FILE *f = fopen(path, "wb");
    if (!f)
        return fail("%s: %s", path, strerror(errno));
    bool ok = fwrite(data, 1, len, f) == len;
    if (fclose(f) != 0)
        ok = false;
    if (!ok)
        return fail("%s: %s", path, strerror(errno));
    return true;
}

typedef struct
{
    int format, channels, rate, bits;
    const uint8_t *data;
    size_t data_bytes;
} Wav;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool wav_parse(const uint8_t *buf, size_t len, Wav *w)
{
    if (len < 12 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0)
    {
        if (len >= 4 && memcmp(buf, "RF64", 4) == 0)
            return fail("RF64 (>4GB wav) files are not supported");
        if (len >= 4 && memcmp(buf, "RIFX", 4) == 0)
            return fail("big-endian RIFX wav files are not supported");
        return fail("not a RIFF/WAVE file");
    }

    const uint8_t *fmt = NULL;
    size_t fmt_len = 0;
    memset(w, 0, sizeof *w);

    size_t pos = 12;
    while (pos + 8 <= len)
    {
        const uint8_t *id = buf + pos;
        uint32_t size = rd32(buf + pos + 4);
        pos += 8;
        size_t avail = len - pos;
        size_t csize = (size == 0 || size > avail) ? avail : size;

        if (memcmp(id, "fmt ", 4) == 0)
        {
            fmt = buf + pos;
            fmt_len = csize;
        }
        else if (memcmp(id, "data", 4) == 0)
        {
            w->data = buf + pos;
            w->data_bytes = csize;
            if (csize == avail)
                break;
        }
        pos += csize + (csize & 1);
    }

    if (!fmt)
        return fail("wav file has no fmt chunk");
    if (fmt_len < 16)
        return fail("fmt chunk is truncated");
    if (!w->data)
        return fail("wav file has no data chunk");

    w->format = rd16(fmt);
    w->channels = rd16(fmt + 2);
    w->rate = (int)rd32(fmt + 4);
    w->bits = rd16(fmt + 14);

    if (w->format == WAVE_EXTENSIBLE)
    {
        if (fmt_len < 40)
            return fail("WAVE_FORMAT_EXTENSIBLE fmt chunk is truncated");
        w->format = rd16(fmt + 24);
    }
    if (w->channels < 1)
        return fail("wav file reports %d channels", w->channels);
    if (w->rate <= 0)
        return fail("wav file reports a sample rate of %d Hz", w->rate);
    if (w->format != WAVE_PCM && w->format != WAVE_FLOAT)
        return fail("unsupported wav encoding (format tag %d); only uncompressed "
                    "PCM and IEEE float are supported",
                    w->format);
    if (w->bits % 8 != 0 || w->bits < 8 || w->bits > 64)
        return fail("unsupported bit depth: %d", w->bits);
    if (w->format == WAVE_FLOAT && w->bits != 32 && w->bits != 64)
        return fail("unsupported float bit depth: %d", w->bits);
    return true;
}

typedef struct
{
    const uint8_t *data;
    int channels, bits, format;
    size_t stride;
    size_t bps;
    float *out;
} ConvCtx;

static void conv_range(void *vc, size_t begin, size_t end, int tid)
{
    (void)tid;
    const ConvCtx *c = vc;
    const double inv_ch = 1.0 / c->channels;

    for (size_t f = begin; f < end; f++)
    {
        const uint8_t *p = c->data + f * c->stride;
        double acc = 0.0;
        for (int ch = 0; ch < c->channels; ch++, p += c->bps)
        {
            switch (c->format == WAVE_FLOAT ? 100 + c->bits : c->bits)
            {
            case 8:
                acc += ((double)p[0] - 128.0) / 128.0;
                break;
            case 16:
            {
                int16_t v = (int16_t)rd16(p);
                acc += (double)v / 32768.0;
                break;
            }
            case 24:
            {
                int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                                      ((uint32_t)p[2] << 16));
                if (v & 0x800000)
                    v -= 0x1000000;
                acc += (double)v / 8388608.0;
                break;
            }
            case 32:
            {
                int32_t v = (int32_t)rd32(p);
                acc += (double)v / 2147483648.0;
                break;
            }
            case 132:
            {
                float v;
                uint32_t bits = rd32(p);
                memcpy(&v, &bits, 4);
                acc += (double)v;
                break;
            }
            case 164:
            {
                double v;
                uint64_t bits = (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
                memcpy(&v, &bits, 8);
                acc += v;
                break;
            }
            default:
                acc = 0.0;
                break;
            }
        }
        c->out[f] = (float)(acc * inv_ch);
    }
}

static float *wav_to_mono(const Wav *w, size_t *out_frames, int jobs)
{
    size_t bps = (size_t)w->bits / 8;
    size_t stride = bps * (size_t)w->channels;
    size_t frames = w->data_bytes / stride;
    if (frames == 0)
    {
        fail("wav file contains no audio frames");
        return NULL;
    }

    float *out = xmalloc(sizeof(float) * frames);
    ConvCtx c = {w->data, w->channels, w->bits, w->format, stride, bps, out};
    parallel_for(frames, jobs, conv_range, &c);
    *out_frames = frames;
    return out;
}

static bool write_wav16(const char *path, const int16_t *pcm, size_t n, int rate)
{
    size_t body = n * 2;
    size_t total = 44 + body;
    uint8_t *buf = xmalloc(total);
    uint8_t *p = buf;

    memcpy(p, "RIFF", 4);
    p += 4;
    uint32_t riff = (uint32_t)(36 + body);
    memcpy(p, &riff, 4);
    p += 4;
    memcpy(p, "WAVEfmt ", 8);
    p += 8;
    struct
    {
        uint32_t sz;
        uint16_t fmt, ch;
        uint32_t rate, brate;
        uint16_t align, bits;
    } f = {16, WAVE_PCM, 1, (uint32_t)rate, (uint32_t)rate * 2, 2, 16};
    memcpy(p, &f.sz, 4);
    p += 4;
    memcpy(p, &f.fmt, 2);
    p += 2;
    memcpy(p, &f.ch, 2);
    p += 2;
    memcpy(p, &f.rate, 4);
    p += 4;
    memcpy(p, &f.brate, 4);
    p += 4;
    memcpy(p, &f.align, 2);
    p += 2;
    memcpy(p, &f.bits, 2);
    p += 2;
    memcpy(p, "data", 4);
    p += 4;
    uint32_t dsz = (uint32_t)body;
    memcpy(p, &dsz, 4);
    p += 4;
    memcpy(p, pcm, body);

    bool ok = write_file(path, buf, total);
    free(buf);
    return ok;
}

#define RES_PHASES 1024

typedef struct
{
    const float *xp;
    float *out;
    const float *table;
    int taps, half;
    double step;
} ResCtx;

static void res_range(void *vc, size_t begin, size_t end, int tid)
{
    (void)tid;
    const ResCtx *c = vc;
    const int taps = c->taps;
    const size_t lead = 3;

    for (size_t i = begin; i < end; i++)
    {
        double centre = (double)i * c->step;
        double base = floor(centre);
        int phase = (int)lround((centre - base) * RES_PHASES);
        const float *h = c->table + (size_t)phase * (size_t)taps;
        const float *x = c->xp + (size_t)base + lead;

        double acc = 0.0;
        for (int j = 0; j < taps; j++)
            acc += (double)h[j] * (double)x[j];
        c->out[i] = (float)acc;
    }
}

static float *resample(const float *x, size_t n, int sr_in, int sr_out,
                       size_t *out_n, int jobs)
{
    double ratio = (double)sr_out / (double)sr_in;
    double fc = (ratio < 1.0 ? ratio : 1.0) * 0.94;
    int half = (int)ceil(24.0 / fc);
    if (half < 4)
        half = 4;
    int taps = 2 * half;
    size_t n_out = (size_t)((double)n * ratio);
    if (n_out < 1)
        n_out = 1;

    float *table = xmalloc(sizeof(float) * (size_t)(RES_PHASES + 1) * (size_t)taps);
    for (int p = 0; p <= RES_PHASES; p++)
    {
        double frac = (double)p / RES_PHASES;
        double sum = 0.0;
        float *row = table + (size_t)p * (size_t)taps;
        for (int j = 0; j < taps; j++)
        {
            double d = (double)(-half + 1 + j) - frac;
            double w = 0.0;
            if (fabs(d) <= half)
                w = 0.42 + 0.5 * cos(DF_PI * d / half) + 0.08 * cos(2.0 * DF_PI * d / half);
            double s = d == 0.0 ? 1.0 : sin(DF_PI * fc * d) / (DF_PI * fc * d);
            double v = s * w;
            row[j] = (float)v;
            sum += v;
        }
        for (int j = 0; j < taps; j++)
            row[j] = (float)(row[j] / sum);
    }

    size_t pad = (size_t)half + 2;
    float *xp = xmalloc(sizeof(float) * (n + 2 * pad));
    memset(xp, 0, sizeof(float) * pad);
    memcpy(xp + pad, x, sizeof(float) * n);
    memset(xp + pad + n, 0, sizeof(float) * pad);

    float *out = xmalloc(sizeof(float) * n_out);
    ResCtx c = {xp, out, table, taps, half, (double)sr_in / (double)sr_out};
    parallel_for(n_out, jobs, res_range, &c);

    free(xp);
    free(table);
    *out_n = n_out;
    return out;
}

typedef struct
{
    const float *x;
    float *partial;
} PeakCtx;

static void peak_range(void *vc, size_t begin, size_t end, int tid)
{
    const PeakCtx *c = vc;
    float m = 0.0f;
    for (size_t i = begin; i < end; i++)
    {
        float v = fabsf(c->x[i]);
        if (v > m)
            m = v;
    }
    if (c->partial[tid] < m)
        c->partial[tid] = m;
}

static float peak_of(const float *x, size_t n, int jobs)
{
    int slots = jobs < 1 ? 1 : jobs;
    float *partial = xmalloc(sizeof(float) * (size_t)slots);
    for (int i = 0; i < slots; i++)
        partial[i] = 0.0f;
    PeakCtx c = {x, partial};
    parallel_for(n, jobs, peak_range, &c);
    float m = 0.0f;
    for (int i = 0; i < slots; i++)
        if (partial[i] > m)
            m = partial[i];
    free(partial);
    return m;
}

typedef struct
{
    const float *x;
    int8_t *out;
    double gain;
    bool dither;
    uint64_t *clipped;
} QuantCtx;

static inline uint64_t splitmix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

static void quant_range(void *vc, size_t begin, size_t end, int tid)
{
    const QuantCtx *c = vc;
    uint64_t clipped = 0;

    for (size_t i = begin; i < end; i++)
    {
        double s = (double)c->x[i] * c->gain * 127.0;
        if (c->dither)
        {
            uint64_t r = splitmix64(i);
            double u1 = (double)(uint32_t)r / 4294967296.0;
            double u2 = (double)(uint32_t)(r >> 32) / 4294967296.0;
            s += u1 - u2;
        }
        if (s > 127.4999 || s < -128.4999)
            clipped++;
        double q = floor(s + 0.5);
        if (q > 127.0)
            q = 127.0;
        if (q < -128.0)
            q = -128.0;
        c->out[i] = (int8_t)q;
    }
    c->clipped[tid] = clipped;
}

static int8_t *quantise(const float *x, size_t n, double gain, bool dither,
                        int jobs, uint64_t *clipped_total)
{
    int slots = jobs < 1 ? 1 : jobs;
    uint64_t *clipped = xmalloc(sizeof(uint64_t) * (size_t)slots);
    memset(clipped, 0, sizeof(uint64_t) * (size_t)slots);

    int8_t *out = xmalloc(n ? n : 1);
    QuantCtx c = {x, out, gain, dither, clipped};
    parallel_for(n, jobs, quant_range, &c);

    uint64_t total = 0;
    for (int i = 0; i < slots; i++)
        total += clipped[i];
    free(clipped);
    *clipped_total = total;
    return out;
}

static uint8_t *ffmpeg_decode(const char *path, int rate, size_t *out_len)
{
    int fds[2];
    if (pipe(fds) != 0)
    {
        fail("pipe: %s", strerror(errno));
        return NULL;
    }

    char rate_s[32];
    snprintf(rate_s, sizeof rate_s, "%d", rate);

    pid_t pid = fork();
    if (pid < 0)
    {
        close(fds[0]);
        close(fds[1]);
        fail("fork: %s", strerror(errno));
        return NULL;
    }
    if (pid == 0)
    {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        char *const argv[] = {"ffmpeg", "-v", "error", "-i", (char *)path,
                              "-ac", "1", "-ar", rate_s, "-c:a", "pcm_s16le",
                              "-f", "wav", "-", NULL};
        execvp("ffmpeg", argv);
        _exit(127);
    }
    close(fds[1]);
    size_t len = 0;
    uint8_t *buf = read_fd_all(fds[0], &len);
    close(fds[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
    {
    }
    if (!buf)
        return NULL;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        free(buf);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
            fail("ffmpeg is not installed");
        else
            fail("ffmpeg failed to decode this file");
        return NULL;
    }
    *out_len = len;
    return buf;
}

static bool have_ffmpeg(void)
{
    static int cached = -1;
    if (cached >= 0)
        return cached != 0;
    const char *path = getenv("PATH");
    cached = 0;
    if (path)
    {
        char *copy = strdup(path);
        for (char *tok = strtok(copy, ":"); tok; tok = strtok(NULL, ":"))
        {
            char probe[4096];
            snprintf(probe, sizeof probe, "%s/ffmpeg", tok);
            if (access(probe, X_OK) == 0)
            {
                cached = 1;
                break;
            }
        }
        free(copy);
    }
    return cached != 0;
}

typedef struct
{
    int rate;
    int jobs;
    bool quiet, dither, raw, no_ffmpeg;
    bool normalize;
    double norm_db, gain_db;
    const char *output;
    const char *outdir;
} Opts;

typedef struct
{
    Log *log;
    bool active;
} Progress;

static void progress_cb(void *ud, size_t done, size_t total)
{
    Progress *p = ud;
    if (!p->active)
        return;
    double pct = total ? 100.0 * (double)done / (double)total : 100.0;
    int width = 28, filled = (int)(width * pct / 100.0);
    char bar[64];
    for (int i = 0; i < width; i++)
        bar[i] = i < filled ? '#' : '-';
    bar[width] = 0;
    fprintf(stderr, "\r  encoding [%s] %5.1f%%", bar, pct);
    if (done >= total)
        fputc('\n', stderr);
    fflush(stderr);
}

static bool encode_file(const char *src, const char *dst, const Opts *o,
                        int jobs, Log *L)
{
    size_t raw_len = 0;
    uint8_t *raw = read_file(src, &raw_len);
    if (!raw)
        return false;

    log_msg(L, "%s\n", src);

    float *mono = NULL;
    size_t n = 0;
    int in_rate = o->rate;

    if (o->raw)
    {
        n = raw_len;
        mono = xmalloc(sizeof(float) * (n ? n : 1));
        for (size_t i = 0; i < n; i++)
            mono[i] = (float)((int8_t)raw[i]) / 127.0f;
        log_msg(L, "  in : raw signed 8-bit mono, %zu samples\n", n);
    }
    else
    {
        Wav w;
        uint8_t *decoded = NULL;
        if (!wav_parse(raw, raw_len, &w))
        {
            char why[512];
            snprintf(why, sizeof why, "%s", g_err);
            size_t sl = strlen(src);
            if (sl >= 6 && strcasecmp(src + sl - 6, ".dfpwm") == 0)
            {
                free(raw);
                return fail("%s is already a dfpwm file; use `dfpwm decode %s` "
                            "to turn it back into a wav",
                            src, src);
            }
            if (o->no_ffmpeg || !have_ffmpeg())
            {
                free(raw);
                return fail("%s: %s", src, why);
            }
            log_msg(L, "  not a plain wav -> decoding with ffmpeg\n");
            size_t dlen = 0;
            decoded = ffmpeg_decode(src, o->rate, &dlen);
            if (!decoded)
            {
                free(raw);
                return fail("%s: %s", src, g_err);
            }
            free(raw);
            raw = decoded;
            raw_len = dlen;
            if (!wav_parse(raw, raw_len, &w))
            {
                free(raw);
                return fail("%s: %s", src, g_err);
            }
        }

        mono = wav_to_mono(&w, &n, jobs);
        if (!mono)
        {
            free(raw);
            return fail("%s: %s", src, g_err);
        }
        in_rate = w.rate;

        char dur[32];
        fmt_time((double)n / in_rate, dur, sizeof dur);
        log_msg(L, "  in : %s %d-bit %dch %d Hz, %s\n",
                w.format == WAVE_FLOAT ? "float" : "pcm", w.bits, w.channels,
                w.rate, dur);
    }
    free(raw);

    if (in_rate != o->rate)
    {
        log_msg(L, "  resampling %d -> %d Hz\n", in_rate, o->rate);
        size_t rn = 0;
        float *res = resample(mono, n, in_rate, o->rate, &rn, jobs);
        free(mono);
        mono = res;
        n = rn;
    }

    double gain = 1.0;
    if (o->normalize)
    {
        float peak = peak_of(mono, n, jobs);
        if (peak > 0.0f)
        {
            gain = pow(10.0, o->norm_db / 20.0) / peak;
            log_msg(L, "  normalise: %+.1f dBFS -> %+.1f dBFS\n",
                    20.0 * log10((double)peak), o->norm_db);
        }
    }
    else if (o->gain_db != 0.0)
    {
        gain = pow(10.0, o->gain_db / 20.0);
        log_msg(L, "  gain: %+.1f dB\n", o->gain_db);
    }

    uint64_t clipped = 0;
    int8_t *samples = quantise(mono, n, gain, o->dither, jobs, &clipped);
    free(mono);
    if (clipped)
        log_msg(L, "  warning: %llu sample(s) clipped (%.2f%%)\n",
                (unsigned long long)clipped,
                100.0 * (double)clipped / (double)(n ? n : 1));

    size_t nbytes = (n + 7) / 8;
    uint8_t *out = xmalloc(nbytes ? nbytes : 1);
    Progress p = {L, L && L->direct && !L->quiet && isatty(STDERR_FILENO) && n > (size_t)o->rate * 5};
    dfpwm_encode(samples, n, out, p.active ? progress_cb : NULL, &p);
    free(samples);

    bool ok = write_file(dst, out, nbytes);
    free(out);
    if (!ok)
        return false;

    char dur[32];
    fmt_time((double)n / o->rate, dur, sizeof dur);
    log_msg(L, "  out: %s  %zu bytes  %s  (%d KiB/s at %d Hz)\n",
            strcmp(dst, "-") == 0 ? "<stdout>" : dst, nbytes, dur,
            o->rate / 8000, o->rate);
    return true;
}

static bool decode_file(const char *src, const char *dst, const Opts *o, Log *L)
{
    size_t len = 0;
    uint8_t *data = read_file(src, &len);
    if (!data)
        return false;

    log_msg(L, "%s\n", src);
    size_t n = len * 8;
    int8_t *pcm8 = xmalloc(n ? n : 1);
    dfpwm_decode(data, len, pcm8);
    free(data);

    bool ok;
    if (o->raw)
    {
        ok = write_file(dst, pcm8, n);
    }
    else
    {
        int16_t *pcm16 = xmalloc(sizeof(int16_t) * (n ? n : 1));
        for (size_t i = 0; i < n; i++)
            pcm16[i] = (int16_t)(pcm8[i] * 256);
        ok = write_wav16(dst, pcm16, n, o->rate);
        free(pcm16);
    }
    free(pcm8);
    if (!ok)
        return false;

    char dur[32];
    fmt_time((double)n / o->rate, dur, sizeof dur);
    log_msg(L, "  out: %s  %zu %s %s\n", strcmp(dst, "-") == 0 ? "<stdout>" : dst,
            n, o->raw ? "raw signed 8-bit samples" : "frames mono 16-bit", dur);
    return true;
}

static bool info_file(const char *src, const Opts *o)
{
    size_t len = 0;
    uint8_t *raw = read_file(src, &len);
    if (!raw)
        return false;

    printf("%s\n", src);
    Wav w;
    if (wav_parse(raw, len, &w))
    {
        size_t n = 0;
        float *mono = wav_to_mono(&w, &n, o->jobs);
        if (!mono)
        {
            free(raw);
            return fail("%s: %s", src, g_err);
        }
        float peak = peak_of(mono, n, o->jobs);
        char dur[32];
        fmt_time((double)n / w.rate, dur, sizeof dur);
        printf("  wav: %s %d-bit %dch %d Hz\n",
               w.format == WAVE_FLOAT ? "float" : "pcm", w.bits, w.channels, w.rate);
        printf("  length: %s (%zu frames)\n", dur, n);
        if (peak > 0.0f)
            printf("  peak: %+.1f dBFS\n", 20.0 * log10((double)peak));
        else
            printf("  peak: -inf dBFS\n");
        printf("  -> %zu bytes wav becomes ~%zu bytes of dfpwm at %d Hz\n", len,
               (size_t)((double)n * o->rate / w.rate / 8.0), o->rate);
        free(mono);
    }
    else
    {
        char dur[32];
        fmt_time((double)len * 8.0 / o->rate, dur, sizeof dur);
        printf("  dfpwm (raw, headerless): %zu bytes, %zu samples\n", len, len * 8);
        printf("  length at %d Hz: %s\n", o->rate, dur);
    }
    free(raw);
    return true;
}

static char *replace_ext(const char *path, const char *ext, const char *outdir)
{
    const char *base = path;
    const char *slash = strrchr(path, '/');
    if (slash)
        base = slash + 1;

    const char *dot = strrchr(base, '.');
    size_t stem = dot ? (size_t)(dot - path) : strlen(path);
    if (outdir)
        stem = dot ? (size_t)(dot - base) : strlen(base);

    size_t dlen = outdir ? strlen(outdir) + 1 : 0;
    char *out = xmalloc(dlen + stem + strlen(ext) + 1);
    char *p = out;
    if (outdir)
    {
        memcpy(p, outdir, strlen(outdir));
        p += strlen(outdir);
        *p++ = '/';
        memcpy(p, base, stem);
    }
    else
    {
        memcpy(p, path, stem);
    }
    p += stem;
    strcpy(p, ext);
    return out;
}

static bool same_file(const char *a, const char *b)
{
    if (strcmp(b, "-") == 0)
        return false;
    if (strcmp(a, b) == 0)
        return true;
    struct stat sa, sb;
    if (stat(a, &sa) != 0 || stat(b, &sb) != 0)
        return false;
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

static bool make_outdir(const char *dir)
{
    if (!dir)
        return true;
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s", dir);
    for (char *p = tmp + 1; *p; p++)
    {
        if (*p != '/')
            continue;
        *p = 0;
        if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
            return fail("%s: %s", tmp, strerror(errno));
        *p = '/';
    }
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
        return fail("%s: %s", tmp, strerror(errno));
    return true;
}

typedef struct
{
    char **inputs;
    char **outputs;
    size_t n;
    const Opts *o;
    bool decode;
    size_t next;
    size_t failures;
    pthread_mutex_t lock;
} Batch;

static void run_one(Batch *b, size_t i, bool direct, int jobs)
{
    Log L = {NULL, 0, 0, direct, b->o->quiet};
    bool ok = b->decode ? decode_file(b->inputs[i], b->outputs[i], b->o, &L)
                        : encode_file(b->inputs[i], b->outputs[i], b->o, jobs, &L);
    if (!ok)
    {
        pthread_mutex_lock(&b->lock);
        b->failures++;
        pthread_mutex_unlock(&b->lock);
        log_msg(&L, "dfpwm: error: %s\n", g_err);
    }
    log_flush(&L);
    free(L.buf);
}

static void *batch_worker(void *vb)
{
    Batch *b = vb;
    for (;;)
    {
        pthread_mutex_lock(&b->lock);
        size_t i = b->next < b->n ? b->next++ : (size_t)-1;
        pthread_mutex_unlock(&b->lock);
        if (i == (size_t)-1)
            return NULL;
        run_one(b, i, false, 1);
    }
}

static size_t run_batch(Batch *b)
{
    int jobs = b->o->jobs;
    if (b->n == 1 || jobs <= 1)
    {
        for (size_t i = 0; i < b->n; i++)
            run_one(b, i, b->n == 1, jobs);
        return b->failures;
    }

    int workers = (size_t)jobs < b->n ? jobs : (int)b->n;
    pthread_t *th = xmalloc(sizeof *th * (size_t)workers);
    bool *started = xmalloc(sizeof *started * (size_t)workers);
    for (int i = 1; i < workers; i++)
        started[i] = pthread_create(&th[i], NULL, batch_worker, b) == 0;
    batch_worker(b);
    for (int i = 1; i < workers; i++)
        if (started[i])
            pthread_join(th[i], NULL);
    free(th);
    free(started);
    return b->failures;
}

static const char *USAGE =
    "usage: dfpwm [encode|decode|info] [options] FILE...\n"
    "\n"
    "Convert WAV audio to ComputerCraft's DFPWM1a format (and back).\n"
    "encode is the default command, so `dfpwm song.wav` just works.\n"
    "\n"
    "options:\n"
    "  -o, --output PATH     output file ('-' for stdout); single input only\n"
    "  -d, --outdir DIR      write outputs into DIR\n"
    "  -r, --rate HZ         sample rate (default 48000, what CC speakers use)\n"
    "  -j, --threads N       worker threads (default: number of cores)\n"
    "  -n, --normalize [dB]  peak-normalise, to 0 dBFS unless a level is given\n"
    "  -g, --gain dB         fixed gain before encoding\n"
    "      --dither          TPDF dither at the 8-bit quantisation step\n"
    "      --raw             encode: input is raw signed 8-bit mono samples\n"
    "                        decode: write raw signed 8-bit instead of a wav\n"
    "      --no-ffmpeg       don't fall back to ffmpeg for non-wav input\n"
    "  -q, --quiet           only report errors\n"
    "  -h, --help            this help\n"
    "\n"
    "examples:\n"
    "  dfpwm song.wav                     -> song.dfpwm (48 kHz mono, ready for CC)\n"
    "  dfpwm song.wav -o /srv/cc/song     write to an explicit path\n"
    "  dfpwm *.wav --outdir out/ -n       batch convert, peak-normalised to 0 dBFS\n"
    "  dfpwm decode song.dfpwm            -> song.wav, so you can hear the result\n"
    "  dfpwm info song.wav                show format / level / predicted size\n"
    "\n";

static bool parse_num(const char *s, double *out)
{
    if (!s || !*s)
        return false;
    char *end = NULL;
    errno = 0;
    double v = strtod(s, &end);
    if (end == s || *end != 0 || errno == ERANGE)
        return false;
    *out = v;
    return true;
}

static int die_usage(const char *msg)
{
    fprintf(stderr, "dfpwm: error: %s\nTry `dfpwm --help`.\n", msg);
    return 2;
}

int main(int argc, char **argv)
{
    Opts o = {CC_SAMPLE_RATE, cpu_count(), false, false, false, false, false,
              0.0, 0.0, NULL, NULL};
    bool decode = false, info = false;

    int argi = 1;
    if (argi < argc && argv[argi][0] != '-')
    {
        if (strcmp(argv[argi], "encode") == 0)
            argi++;
        else if (strcmp(argv[argi], "decode") == 0)
        {
            decode = true;
            argi++;
        }
        else if (strcmp(argv[argi], "info") == 0)
        {
            info = true;
            argi++;
        }
    }

    char **inputs = xmalloc(sizeof(char *) * (size_t)argc);
    size_t n_inputs = 0;
    bool no_more_opts = false;

    for (; argi < argc; argi++)
    {
        char *a = argv[argi];
        if (no_more_opts || a[0] != '-' || a[1] == 0)
        {
            inputs[n_inputs++] = a;
            continue;
        }
        char *val = NULL;
        char sbuf[3] = {'-', 0, 0};
        if (a[1] == '-')
        {
            char *eq = strchr(a, '=');
            if (eq)
            {
                *eq = 0;
                val = eq + 1;
            }
        }
        else if (a[2] && strchr("odrjgn", a[1]))
        {
            sbuf[1] = a[1];
            val = a + 2;
            a = sbuf;
        }
#define NEXTVAL(name) \
    (val ? val : (argi + 1 < argc ? argv[++argi] : (fprintf(stderr, "dfpwm: error: %s needs a value\n", name), exit(2), (char *)NULL)))

        if (!strcmp(a, "-h") || !strcmp(a, "--help"))
        {
            fputs(USAGE, stdout);
            return 0;
        }
        else if (!strcmp(a, "-V") || !strcmp(a, "--version"))
        {
            puts("dfpwm 1.0");
            return 0;
        }
        else if (!strcmp(a, "--"))
            no_more_opts = true;
        else if (!strcmp(a, "-q") || !strcmp(a, "--quiet"))
            o.quiet = true;
        else if (!strcmp(a, "--dither"))
            o.dither = true;
        else if (!strcmp(a, "--raw"))
            o.raw = true;
        else if (!strcmp(a, "--no-ffmpeg"))
            o.no_ffmpeg = true;
        else if (!strcmp(a, "-o") || !strcmp(a, "--output"))
            o.output = NEXTVAL("--output");
        else if (!strcmp(a, "-d") || !strcmp(a, "--outdir"))
            o.outdir = NEXTVAL("--outdir");
        else if (!strcmp(a, "-r") || !strcmp(a, "--rate"))
        {
            double v;
            if (!parse_num(NEXTVAL("--rate"), &v) || v < 1000 || v > 768000)
                return die_usage("--rate must be between 1000 and 768000");
            o.rate = (int)v;
        }
        else if (!strcmp(a, "-j") || !strcmp(a, "--threads"))
        {
            double v;
            if (!parse_num(NEXTVAL("--threads"), &v) || v < 1 || v > 1024)
                return die_usage("--threads must be between 1 and 1024");
            o.jobs = (int)v;
        }
        else if (!strcmp(a, "-g") || !strcmp(a, "--gain"))
        {
            if (!parse_num(NEXTVAL("--gain"), &o.gain_db))
                return die_usage("--gain needs a number in dB");
        }
        else if (!strcmp(a, "-n") || !strcmp(a, "--normalize") ||
                 !strcmp(a, "--normalise"))
        {
            o.normalize = true;
            o.norm_db = 0.0;
            if (val)
            {
                if (!parse_num(val, &o.norm_db))
                    return die_usage("--normalize needs a level in dBFS");
            }
            else if (argi + 1 < argc)
            {
                double v;
                if (parse_num(argv[argi + 1], &v))
                {
                    o.norm_db = v;
                    argi++;
                }
            }
        }
        else
        {
            fprintf(stderr, "dfpwm: error: unknown option %s\nTry `dfpwm --help`.\n", a);
            return 2;
        }
#undef NEXTVAL
    }

    if (n_inputs == 0)
    {
        fputs(USAGE, stderr);
        return 1;
    }
    if (o.rate != CC_SAMPLE_RATE && !o.quiet)
        fprintf(stderr, "note: rate is %d Hz; ComputerCraft speakers play DFPWM "
                        "at %d Hz\n",
                o.rate, CC_SAMPLE_RATE);

    if (info)
    {
        int rc = 0;
        for (size_t i = 0; i < n_inputs; i++)
            if (!info_file(inputs[i], &o))
            {
                fprintf(stderr, "dfpwm: error: %s\n", g_err);
                rc = 1;
            }
        free(inputs);
        return rc;
    }

    if (o.output && n_inputs != 1)
    {
        free(inputs);
        return die_usage("-o/--output takes a single input file; use --outdir "
                         "for batches");
    }
    char *outdir_buf = NULL;
    if (o.outdir)
    {
        outdir_buf = strdup(o.outdir);
        size_t dl = strlen(outdir_buf);
        while (dl > 1 && outdir_buf[dl - 1] == '/')
            outdir_buf[--dl] = 0;
        o.outdir = outdir_buf;
    }
    if (o.outdir && !make_outdir(o.outdir))
    {
        fprintf(stderr, "dfpwm: error: %s\n", g_err);
        free(inputs);
        return 1;
    }

    const char *ext = decode ? (o.raw ? ".raw" : ".wav") : ".dfpwm";
    char **outputs = xmalloc(sizeof(char *) * n_inputs);
    for (size_t i = 0; i < n_inputs; i++)
    {
        outputs[i] = o.output ? strdup(o.output) : replace_ext(inputs[i], ext, o.outdir);
        if (same_file(inputs[i], outputs[i]))
        {
            fprintf(stderr, "dfpwm: error: refusing to overwrite the input file %s\n",
                    inputs[i]);
            return 1;
        }
    }

    Batch b = {inputs, outputs, n_inputs, &o, decode, 0, 0, PTHREAD_MUTEX_INITIALIZER};
    size_t failures = run_batch(&b);

    for (size_t i = 0; i < n_inputs; i++)
        free(outputs[i]);
    free(outputs);
    free(outdir_buf);
    free(inputs);
    return failures ? 1 : 0;
}
