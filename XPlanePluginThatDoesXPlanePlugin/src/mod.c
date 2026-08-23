#include <math.h>
#include <string.h>

#include "XPLMCamera.h"
#include "XPLMDataAccess.h"
#include "XPLMDefs.h"
#include "XPLMPlugin.h"
#include "XPLMProcessing.h"

#include "tel.h"

static const float dtcadence = 0.25f;
static const float warmup     = 3.0f;
static const float bleed       = 0.80f;
static const float brim        = 1.0f;
static const float wlo          = 8.0f;
static const float whi          = 30.0f;
static const float galt         = 12.0f;
static const float gias         = 25.0f;

struct book
{
    long  n;
    float t;
    float iasx;
    float iass;
    float altx;
    float vsx;
    float vsn;
    float gsx;
};

static struct book bk;

static void book_zero(void)
{
    memset(&bk, 0, sizeof(bk));
}

static void book_add(float dt)
{
    float a = tel_f(T_IAS);
    float b = tel_f(T_ELEV);
    float c = tel_f(T_VSI);
    float d = tel_f(T_GS);

    bk.n += 1;
    bk.t += dt;
    bk.iass += a;

    if (a > bk.iasx) bk.iasx = a;
    if (b > bk.altx) bk.altx = b;
    if (d > bk.gsx)  bk.gsx  = d;
    if (c > bk.vsx)  bk.vsx  = c;
    if (c < bk.vsn)  bk.vsn  = c;
}

static unsigned st = 0x9E3779B9u;

static void seed(unsigned s)
{
    st = s ? s : 0x9E3779B9u;
}

static float roll(void)
{
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return (float)(st >> 8) / 16777216.0f;
}

static float sway(void)
{
    return roll() < 0.5f ? -1.0f : 1.0f;
}

static float clamp1(float v)
{
    if (v >  1.0f) return  1.0f;
    if (v < -1.0f) return -1.0f;
    return v;
}

/* Never hand the control path a NaN or an out-of-range deflection. */
static float safe1(float v)
{
    if (v != v)
        return 0.0f;
    return clamp1(v);
}

/* Fold an angle into [-180,180]. NaN and absurd magnitudes collapse to 0 --
   the magnitude test also keeps inf away from fmodf. */
static float wrap180(float v)
{
    if (v != v || v > 1.0e6f || v < -1.0e6f)
        return 0.0f;

    v = fmodf(v + 180.0f, 360.0f);
    if (v < 0.0f)
        v += 360.0f;

    return v - 180.0f;
}

/* Head pitch is not an angle that wraps -- past vertical it gimbals. */
static float clamp89(float v)
{
    if (v != v)     return   0.0f;
    if (v >  89.0f) return  89.0f;
    if (v < -89.0f) return -89.0f;
    return v;
}

enum
{
    M_OFF = 0,
    M_PIN,
    M_FLIP,
    M_CAMINV,
    M_YOKEINV,
    M_ALARM
};

#define NWARN 10

static float acc;
static float rate;
static float held;
static int   flew;
static int   mode;
static float strobe;
static int   phase;
static float lasth;
static float lastg;
static float lastyp;
static float lastyr;
static int   lit;

static int cam(XPLMCameraPosition_t *p, int lost, void *r)
{
    (void)r;

    if (lost || mode != M_FLIP || !p)
        return 0;

    p->x       = tel_f(T_LX);
    p->y       = tel_f(T_LY);
    p->z       = tel_f(T_LZ);
    p->pitch   = tel_f(T_THE);
    p->heading = tel_f(T_PSI);
    p->roll    = tel_f(T_PHI) + 180.0f;
    p->zoom    = 1.0f;

    return 1;
}

static void warn(int on)
{
    int k;

    for (k = 0; k < NWARN; ++k)
        if (lit & (1 << k))
            tel_seti(T_W0 + k, on);

    tel_seti(T_WMC, on);
    tel_seti(T_WMW, on);
}

static void herring(void)
{
    int n = 1 + (int)(roll() * 3.0f) % 3;
    int k;

    lit = 0;
    for (k = 0; k < n; ++k)
        lit |= 1 << ((int)(roll() * (float)NWARN) % NWARN);

    tel_seti(T_OVA, 1);
    warn(1);
}

static void unmode(void)
{
    if (mode == M_ALARM)
    {
        warn(0);
        tel_seti(T_OVA, 0);
        lit = 0;
    }

    mode = M_OFF;
}

static void hold(void)
{
    float a, b, p, q;

    if (mode == M_PIN)
    {
        tel_setf(T_HEAD, -85.0f);
    }
    else if (mode == M_CAMINV)
    {
        /* The sim reports our own last write plus whatever the user just
           added, so the difference is exactly the user's movement. Mirror
           that delta instead of doubling the absolute value -- the old form
           ran away without bound the first time heading wrapped. */
        a = wrap180(tel_f(T_HDG) - lastg);
        b = clamp89(tel_f(T_HEAD) - lasth);

        lastg = wrap180(lastg - a);
        lasth = clamp89(lasth - b);

        tel_setf(T_HDG,  lastg);
        tel_setf(T_HEAD, lasth);
    }
    else if (mode == M_YOKEINV)
    {
        p = safe1(tel_f(T_YP));
        q = safe1(tel_f(T_YR));

        if (p != lastyp)
        {
            lastyp = -p;
            tel_setf(T_YP, lastyp);
        }

        if (q != lastyr)
        {
            lastyr = -q;
            tel_setf(T_YR, lastyr);
        }
    }
    else if (mode == M_ALARM)
    {
        warn(1);
    }
}

static void lights(int on)
{
    int k;
    for (k = T_S0; k <= T_S4; ++k)
        tel_seti(k, on);
}

static void collapse(void)
{
    int g[T_NGEAR];
    int idx[T_NGEAR];
    int n = tel_iv(T_GTY, g, T_NGEAR);
    int i, c = 0;

    if (n > T_NGEAR)
        n = T_NGEAR;

    for (i = 0; i < n; ++i)
        if (g[i] > 0)
            idx[c++] = i;

    if (c < 1)
    {
        idx[0] = 0;
        c = 1;
    }

    tel_seti(T_G0 + idx[(int)(roll() * (float)c) % c], 6);
}

static void engfail(void)
{
    int n = tel_i(T_NEN);

    if (n < 1) n = 1;
    if (n > T_NENG) n = T_NENG;

    tel_seti(T_F0 + (int)(roll() * (float)n) % n, 6);
}

static void boost(void)
{
    float vx = tel_f(T_VX);
    float vy = tel_f(T_C3);
    float vz = tel_f(T_VZ);
    float sp = sqrtf(vx * vx + vy * vy + vz * vz);
    float ps = tel_f(T_PSI) * 0.0174532925f;
    float th = tel_f(T_THE) * 0.0174532925f;
    float cx = cosf(th);
    float k  = 9.0f * sp;

    tel_setf(T_VX, vx + k * sinf(ps) * cx);
    tel_setf(T_C3, vy + k * sinf(th));
    tel_setf(T_VZ, vz - k * cosf(ps) * cx);
}

static void touchdown(void)
{
    int   w = tel_i(T_WOW);
    float g = tel_f(T_AGL);

    if (!w && g > 10.0f)
    {
        flew = 1;
        return;
    }

    if (w && flew)
    {
        flew = 0;
        if (roll() < 0.30f)
            collapse();
    }
}

static void arm(void)
{
    float w = wlo + roll() * (whi - wlo);
    rate = 1.0f / w;
    acc  = 0.0f;
}

static int steady(float dt)
{
    int   w = tel_i(T_WOW);
    float g = tel_f(T_AGL);
    float v = tel_f(T_IAS);

    if (w || g < galt || v < gias)
    {
        held = 0.0f;
        return 0;
    }

    held += dt;
    return held >= warmup;
}

static void blend(void)
{
    float u = roll();
    float s = sway();

    unmode();

    if (u < 0.10f)
    {
        strobe = 2.5f + roll() * 3.0f;
        phase  = 0;
    }
    else if (u < 0.18f)
    {
        int k;
        for (k = T_S0; k <= T_S4; ++k)
            tel_seti(k, tel_i(k) ? 0 : 1);
    }
    else if (u < 0.26f)
    {
        tel_setf(T_C4, 18.0f + roll() * 112.0f);
    }
    else if (u < 0.34f)
    {
        mode = M_FLIP;
        XPLMControlCamera(xplm_ControlCameraUntilViewChanges, cam, 0);
    }
    else if (u < 0.41f)
    {
        mode  = M_CAMINV;
        lastg = wrap180(tel_f(T_HDG));
        lasth = clamp89(tel_f(T_HEAD));
    }
    else if (u < 0.47f)
    {
        mode = M_PIN;
    }
    else if (u < 0.52f)
    {
        tel_setf(T_TOD, roll() * 86400.0f);
    }
    else if (u < 0.55f)
    {
        tel_setf(T_VIS, 0.05f + roll() * 0.4f);
    }
    else if (u < 0.58f)
    {
        mode = M_ALARM;
        herring();
    }
    else if (u < 0.62f)
    {
        tel_setf(T_FLP, (roll() < 0.5f) ? 1.0f : 0.0f);
    }
    else if (u < 0.65f)
    {
        tel_setf(T_SB, 1.0f);
    }
    else if (u < 0.68f)
    {
        tel_setf(T_THR, (roll() < 0.5f) ? 0.0f : 1.0f);
    }
    else if (u < 0.73f)
    {
        mode   = M_YOKEINV;
        lastyp = 0.0f;
        lastyr = 0.0f;
    }
    else if (u < 0.76f)
    {
        tel_setf(T_C0, tel_f(T_C0) + s * (5.0f + roll() * 6.0f));
        tel_setf(T_C2, tel_f(T_C2) - s * (3.0f + roll() * 5.0f));
    }
    else if (u < 0.79f)
    {
        tel_setf(T_C1, tel_f(T_C1) + s * (5.0f + roll() * 6.0f));
    }
    else if (u < 0.82f)
    {
        tel_setf(T_C3, tel_f(T_C3) + s * (45.0f + roll() * 55.0f));
    }
    else if (u < 0.85f)
    {
        tel_setf(T_C5, clamp1(s * (0.6f + roll() * 0.4f)));
    }
    else if (u < 0.88f)
    {
        tel_setf(T_C6, clamp1(s * (0.5f + roll() * 0.5f)));
        tel_setf(T_C7, clamp1(-s * (0.5f + roll() * 0.5f)));
    }
    else if (u < 0.91f)
    {
        tel_setf(T_CG, s * (2.0f + roll() * 3.5f));
    }
    else if (u < 0.94f)
    {
        boost();
    }
    else if (u < 0.97f)
    {
        engfail();
    }
    else
    {
        collapse();
    }
}

static int fast(void)
{
    return strobe > 0.0f || mode == M_CAMINV || mode == M_YOKEINV;
}

static void step(float dt)
{
    acc += dt * rate;

    if (acc < brim)
        return;

    blend();
    arm();
}

static float loop(float e, float sl, int c, void *r)
{
    (void)sl;
    (void)c;
    (void)r;

    float dt = e;

    if (dt <= 0.0f || dt > 2.0f)
        dt = dtcadence;

    touchdown();
    hold();

    if (strobe > 0.0f)
    {
        strobe -= dt;
        phase ^= 1;
        lights(phase);

        if (strobe <= 0.0f)
        {
            strobe = 0.0f;
            lights(1);
        }
    }

    if (!steady(dt))
    {
        acc *= bleed;
        return fast() ? -1.0f : dtcadence;
    }

    book_add(dt);
    step(dt);

    return fast() ? -1.0f : dtcadence;
}

PLUGIN_API int XPluginStart(char *outName, char *outSig, char *outDesc)
{
    strcpy(outName, "XPlanePluginThatDoesXPlanePlugin");
    strcpy(outSig,  "com.xppl.session.recorder");
    strcpy(outDesc, "This Plugin does Something, try to figure it out by yourself.");

    tel_init();
    book_zero();

    seed((unsigned)(XPLMGetElapsedTime() * 1000.0f) ^ (unsigned)(long)outName);
    arm();

    XPLMRegisterFlightLoopCallback(loop, dtcadence, 0);
    return 1;
}

PLUGIN_API void XPluginStop(void)
{
    XPLMUnregisterFlightLoopCallback(loop, 0);
    unmode();
    XPLMDontControlCamera();
    tel_release();
}

PLUGIN_API int XPluginEnable(void)
{
    tel_init();
    return 1;
}

PLUGIN_API void XPluginDisable(void)
{
    unmode();
    XPLMDontControlCamera();
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID from, int msg, void *param)
{
    (void)from;
    (void)param;

    if (msg == XPLM_MSG_PLANE_LOADED || msg == XPLM_MSG_AIRPORT_LOADED)
    {
        tel_release();
        tel_init();
        book_zero();
        held = 0.0f;
        flew = 0;
        unmode();
        strobe = 0.0f;
        phase = 0;
        XPLMDontControlCamera();
        arm();
    }
}
