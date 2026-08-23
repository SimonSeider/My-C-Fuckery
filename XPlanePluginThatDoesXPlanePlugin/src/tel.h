#ifndef AEROLOG_TEL_H
#define AEROLOG_TEL_H

enum
{
    T_IAS = 0,
    T_VSI,
    T_ELEV,
    T_AGL,
    T_WOW,
    T_GS,
    T_C0,
    T_C1,
    T_C2,
    T_C3,
    T_C4,
    T_C5,
    T_C6,
    T_C7,
    T_S0,
    T_S1,
    T_S2,
    T_S3,
    T_S4,
    T_G0,
    T_G1,
    T_G2,
    T_G3,
    T_G4,
    T_G5,
    T_G6,
    T_G7,
    T_G8,
    T_G9,
    T_GTY,
    T_F0,
    T_F1,
    T_F2,
    T_F3,
    T_F4,
    T_F5,
    T_F6,
    T_F7,
    T_NEN,
    T_CG,
    T_FLP,
    T_SB,
    T_THR,
    T_TOD,
    T_VIS,
    T_HEAD,
    T_HDG,
    T_LX,
    T_LY,
    T_LZ,
    T_PSI,
    T_THE,
    T_PHI,
    T_YP,
    T_YR,
    T_VX,
    T_VZ,
    T_OVA,
    T_WMC,
    T_WMW,
    T_W0,
    T_W1,
    T_W2,
    T_W3,
    T_W4,
    T_W5,
    T_W6,
    T_W7,
    T_W8,
    T_W9,
    T_N
};

#define T_NGEAR 10
#define T_NENG  8

void  tel_init(void);
void  tel_release(void);
float tel_f(int id);
int   tel_i(int id);
int   tel_iv(int id, int *v, int n);
void  tel_setf(int id, float v);
void  tel_seti(int id, int v);

#endif
