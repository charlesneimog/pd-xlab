/*
** nw.gverb~.cpp
**
** Pd external
** msp implementation of Griesinger vst plugin, ported from Max/MSP
** 2002/05/20 started by Nathan Wolek
**
** Copyright (c) 2002,2014 by Nathan Wolek
** License: http://opensource.org/licenses/BSD-3-Clause
*/

#include <m_pd.h>

#include <cmath>

#include "reverb_bb.h"

#define OBJECT_NAME "x.nw.gverb~"

// constant settings for the reverb algorithm
#define ALLPASS_SHORT_DELAY_VALUES {142, 107, 379, 277}
#define ALLPASS_LONG_DELAY_VALUES {1800, 2656}
#define ALLPASS_MOD_DELAY_INIT_VALUES {672, 908}
#define DELAY_SMALL_VALUES {4453, 4217, 3720, 3163}

#define LOWPASS_NUM 3
#define ALLPASS_SHORT_NUM 4
#define ALLPASS_LONG_NUM 2
#define ALLPASS_MOD_NUM 2
#define DELAYBUFF_SMALL_NUM 4

// coefficients for reverb components
#define IN_DIFF_1 0.750
#define IN_DIFF_2 0.625
#define DEC_DIFF_1 0.700
#define DEC_DIFF_2 0.500
#define DAMPING 0.0005
#define BANDWIDTH 0.9995
#define AP_MODRATE_1 1.13671
#define AP_MODRATE_2 1.11718
#define AP_MODDEPTH_1 16.11
#define AP_MODDEPTH_2 15.87

// fix for denormal through square injection of dc offset
#define TINY_DC 0.0000000000000000000000001f

static t_class *gverb_class;

typedef struct _gverb {
    t_object x_obj;
    t_float x_f;
    t_inlet *decay_inlet;
    t_outlet *left_outlet;
    t_outlet *right_outlet;

    // arrays to hold exact buffer lengths
    long apShort_values[ALLPASS_SHORT_NUM];
    long apLong_values[ALLPASS_LONG_NUM];
    long smallDelay_values[DELAYBUFF_SMALL_NUM];
    long apMod_init_values[ALLPASS_MOD_NUM];

    // structs for reverb building blocks
    rbb_sintable oscTable;
    rbb_lowpass lpFilters[LOWPASS_NUM];
    rbb_allpass_short apFilters_short[ALLPASS_SHORT_NUM];
    rbb_allpass_long apFilters_long[ALLPASS_LONG_NUM];
    rbb_allpass_mod apFilters_mod[ALLPASS_MOD_NUM];
    rbb_delaybuff_short delayBuffs_small[DELAYBUFF_SMALL_NUM];

    double verb_decay;       // in milliseconds
    double verb_decay_1over; // in milliseconds
    double verb_decay_coeff;

    double lastout_L;
    double lastout_R;

    // sample rate info
    double output_sr;
    double output_msr;
    double output_1overmsr;

    // maintain dc_offset for square injection
    double sqinject_val;
} t_gverb;

static void gverb_init(t_gverb *x);
static void gverb_free(t_gverb *x);

static void gverb_update_decay(t_gverb *x, double decay_ms) {
    if (decay_ms <= 0.0) {
        pd_error(x, "[%s] decay time must be greater than zero", OBJECT_NAME);
        return;
    }

    x->verb_decay = decay_ms;
    x->verb_decay_1over = 1.0 / x->verb_decay;
    x->verb_decay_coeff = pow(10.0, (-16416.0 * x->verb_decay_1over * x->output_1overmsr));
}

static t_int *gverb_perform(t_int *w) {
    t_gverb *x = (t_gverb *)(w[1]);
    t_sample *in_dry = (t_sample *)(w[2]);
    t_sample *in_decay = (t_sample *)(w[3]);
    t_sample *out_wet1 = (t_sample *)(w[4]);
    t_sample *out_wet2 = (t_sample *)(w[5]);
    int n = (int)(w[6]);

    double lastout_L = x->lastout_L;
    double lastout_R = x->lastout_R;
    double sqinject_val = x->sqinject_val * -1.0;

    while (n--) {
        double val_dry = *in_dry + sqinject_val;
        double val_decay = *in_decay;
        double fDecay = x->verb_decay_coeff;

        if (val_decay > 0.0) {
            fDecay = pow(10.0, (-16416.0 * x->output_1overmsr / val_decay));
        }

        float val_dry_float = (float)val_dry;
        float x2 = 0.0f, x3 = 0.0f, x4 = 0.0f, x5 = 0.0f, x6 = 0.0f;
        float x7L = 0.0f, x8L = 0.0f, x9L = 0.0f, x10L = 0.0f;
        float x11L = 0.0f, x12L = 0.0f, x13L = 0.0f;
        float x7R = 0.0f, x8R = 0.0f, x9R = 0.0f, x10R = 0.0f;
        float x11R = 0.0f, x12R = 0.0f, x13R = 0.0f;

        rbb_compute_lowPass1(&val_dry_float, x->lpFilters, &x2);
        rbb_compute_allpassShort(&x2, x->apFilters_short, &x3);
        rbb_compute_allpassShort(&x3, x->apFilters_short + 1, &x4);
        rbb_compute_allpassShort(&x4, x->apFilters_short + 2, &x5);
        rbb_compute_allpassShort(&x5, x->apFilters_short + 3, &x6);

        x7L = x6 + (float)lastout_R;
        x7R = x6 + (float)lastout_L;

        rbb_compute_allpassMod(&x7L, x->apFilters_mod, &x8L);
        rbb_compute_allpassMod(&x7R, x->apFilters_mod + 1, &x8R);
        rbb_compute_shortDelay(&x8L, x->delayBuffs_small, &x9L);
        rbb_compute_shortDelay(&x8R, x->delayBuffs_small + 1, &x9R);
        rbb_compute_lowPass2(&x9L, x->lpFilters + 1, &x10L);
        rbb_compute_lowPass2(&x9R, x->lpFilters + 2, &x10R);

        x11L = (float)(fDecay * x10L);
        x11R = (float)(fDecay * x10R);

        rbb_compute_allpassLong(&x11L, x->apFilters_long, &x12L);
        rbb_compute_allpassLong(&x11R, x->apFilters_long + 1, &x12R);
        rbb_compute_shortDelay(&x12L, x->delayBuffs_small + 2, &x13L);
        rbb_compute_shortDelay(&x12R, x->delayBuffs_small + 3, &x13R);

        double val_wet1 = fDecay * x13L;
        double val_wet2 = fDecay * x13R;
        lastout_L = val_wet1;
        lastout_R = val_wet2;

        *out_wet1 = (t_sample)(1.2 * x9R - 0.6 * x12R + 0.6 * x13R - 0.6 * x9L -
                               0.6 * x12L - 0.6 * x13L);
        *out_wet2 = (t_sample)(1.2 * x9L - 0.6 * x12L + 0.6 * x13L - 0.6 * x9R -
                               0.6 * x12R - 0.6 * x13R);

        ++in_dry;
        ++in_decay;
        ++out_wet1;
        ++out_wet2;
    }

    x->lastout_L = lastout_L;
    x->lastout_R = lastout_R;
    x->sqinject_val = sqinject_val;

    return (w + 7);
}

static void gverb_dsp(t_gverb *x, t_signal **sp) {
    x->output_sr = sp[0]->s_sr > 0 ? sp[0]->s_sr : sys_getsr();
    if (x->output_sr <= 0.0) {
        x->output_sr = 44100.0;
    }

    x->output_msr = x->output_sr * 0.001;
    x->output_1overmsr = 1.0 / x->output_msr;
    x->verb_decay_coeff = pow(10.0, (-16416.0 * x->verb_decay_1over * x->output_1overmsr));

    rbb_set_allpassMod_freq(x->apFilters_mod, AP_MODRATE_1, (float)x->output_sr);
    rbb_set_allpassMod_freq(x->apFilters_mod + 1, AP_MODRATE_2, (float)x->output_sr);

    dsp_add(gverb_perform, 6, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, sp[3]->s_vec,
            sp[0]->s_n);
}

static void gverb_float(t_gverb *x, t_floatarg f) {
    gverb_update_decay(x, f);
}

static void gverb_getinfo(t_gverb *x) {
    post("%s object by Nathan Wolek", OBJECT_NAME);
    post("Last updated on %s - www.nathanwolek.com", __DATE__);
}

static void gverb_init(t_gverb *x) {
    int curr_num;
    long temp_apsv[] = ALLPASS_SHORT_DELAY_VALUES;
    long temp_aplv[] = ALLPASS_LONG_DELAY_VALUES;
    long temp_sdv[] = DELAY_SMALL_VALUES;
    long temp_apmv[] = ALLPASS_MOD_DELAY_INIT_VALUES;

    rbb_sintable *ot_ptr = &(x->oscTable);
    rbb_lowpass *lpf_ptr = x->lpFilters;
    rbb_allpass_short *aps_ptr = x->apFilters_short;
    rbb_allpass_long *apl_ptr = x->apFilters_long;
    rbb_allpass_mod *apm_ptr = x->apFilters_mod;
    rbb_delaybuff_short *sd_ptr = x->delayBuffs_small;

    curr_num = ALLPASS_SHORT_NUM;
    while (--curr_num >= 0) {
        x->apShort_values[curr_num] = temp_apsv[curr_num];
    }

    curr_num = ALLPASS_LONG_NUM;
    while (--curr_num >= 0) {
        x->apLong_values[curr_num] = temp_aplv[curr_num];
    }

    curr_num = DELAYBUFF_SMALL_NUM;
    while (--curr_num >= 0) {
        x->smallDelay_values[curr_num] = temp_sdv[curr_num];
    }

    curr_num = ALLPASS_MOD_NUM;
    while (--curr_num >= 0) {
        x->apMod_init_values[curr_num] = temp_apmv[curr_num];
    }

    rbb_init_sinTable(ot_ptr);

    curr_num = LOWPASS_NUM;
    while (--curr_num >= 0) {
        rbb_init_lowPass(lpf_ptr + curr_num);
    }

    curr_num = ALLPASS_SHORT_NUM;
    while (--curr_num >= 0) {
        rbb_init_allpassShort(aps_ptr + curr_num);
        rbb_set_allpassShort_delay(aps_ptr + curr_num, x->apShort_values[curr_num]);
    }

    curr_num = ALLPASS_LONG_NUM;
    while (--curr_num >= 0) {
        rbb_init_allpassLong(apl_ptr + curr_num);
        rbb_set_allpassLong_delay(apl_ptr + curr_num, x->apLong_values[curr_num]);
    }

    curr_num = ALLPASS_MOD_NUM;
    while (--curr_num >= 0) {
        rbb_init_allpassMod(apm_ptr + curr_num, ot_ptr);
        rbb_set_allpassMod_delay(apm_ptr + curr_num, x->apMod_init_values[curr_num]);
    }

    curr_num = DELAYBUFF_SMALL_NUM;
    while (--curr_num >= 0) {
        rbb_init_shortDelay(sd_ptr + curr_num);
        rbb_set_shortDelay_delay(sd_ptr + curr_num, x->smallDelay_values[curr_num]);
    }

    rbb_set_lowPass_coeff(lpf_ptr, BANDWIDTH);
    rbb_set_lowPass_coeff(lpf_ptr + 1, DAMPING);
    rbb_set_lowPass_coeff(lpf_ptr + 2, DAMPING);

    rbb_set_allpassShort_coeff(aps_ptr, IN_DIFF_1);
    rbb_set_allpassShort_coeff(aps_ptr + 1, IN_DIFF_1);
    rbb_set_allpassShort_coeff(aps_ptr + 2, IN_DIFF_2);
    rbb_set_allpassShort_coeff(aps_ptr + 3, IN_DIFF_2);

    rbb_set_allpassMod_coeff(apm_ptr, DEC_DIFF_1);
    rbb_set_allpassMod_coeff(apm_ptr + 1, DEC_DIFF_1);

    apm_ptr->oscDepth = AP_MODDEPTH_1;
    (apm_ptr + 1)->oscDepth = AP_MODDEPTH_2;

    rbb_set_allpassLong_coeff(apl_ptr, DEC_DIFF_2);
    rbb_set_allpassLong_coeff(apl_ptr + 1, DEC_DIFF_2);

    x->lastout_L = 0.0;
    x->lastout_R = 0.0;
}

static void *gverb_new(t_floatarg decay_ms) {
    t_gverb *x = (t_gverb *)pd_new(gverb_class);

    x->verb_decay = decay_ms > 0.0 ? decay_ms : 1000.0;
    x->verb_decay_1over = 1.0 / x->verb_decay;

    x->output_sr = sys_getsr();
    if (x->output_sr <= 0.0) {
        x->output_sr = 44100.0;
    }
    x->output_msr = x->output_sr * 0.001;
    x->output_1overmsr = 1.0 / x->output_msr;
    x->verb_decay_coeff = pow(10.0, (-16416.0 * x->verb_decay_1over * x->output_1overmsr));
    x->sqinject_val = TINY_DC;

    x->decay_inlet = signalinlet_new(&x->x_obj, (t_float)x->verb_decay);
    x->left_outlet = outlet_new(&x->x_obj, &s_signal);
    x->right_outlet = outlet_new(&x->x_obj, &s_signal);

    gverb_init(x);

    return x;
}

static void gverb_free(t_gverb *x) {
    int curr_num;
    rbb_sintable *ot_ptr = &(x->oscTable);
    rbb_allpass_short *aps_ptr = x->apFilters_short;
    rbb_allpass_long *apl_ptr = x->apFilters_long;
    rbb_allpass_mod *apm_ptr = x->apFilters_mod;
    rbb_delaybuff_short *sd_ptr = x->delayBuffs_small;

    rbb_free_sinTable(ot_ptr);

    curr_num = ALLPASS_SHORT_NUM;
    while (--curr_num >= 0) {
        rbb_free_allpassShort(aps_ptr + curr_num);
    }

    curr_num = ALLPASS_LONG_NUM;
    while (--curr_num >= 0) {
        rbb_free_allpassLong(apl_ptr + curr_num);
    }

    curr_num = ALLPASS_MOD_NUM;
    while (--curr_num >= 0) {
        rbb_free_allpassMod(apm_ptr + curr_num);
    }

    curr_num = DELAYBUFF_SMALL_NUM;
    while (--curr_num >= 0) {
        rbb_free_shortDelay(sd_ptr + curr_num);
    }
}

extern "C" void setup_x0x2enw0x2egverb_tilde(void) {
    gverb_class = class_new(gensym(OBJECT_NAME), (t_newmethod)gverb_new, (t_method)gverb_free,
                            sizeof(t_gverb), CLASS_DEFAULT, A_DEFFLOAT, A_NULL);

    CLASS_MAINSIGNALIN(gverb_class, t_gverb, x_f);

    class_addmethod(gverb_class, (t_method)gverb_dsp, gensym("dsp"), A_CANT, 0);
    class_addfloat(gverb_class, gverb_float);
    class_addmethod(gverb_class, (t_method)gverb_getinfo, gensym("getinfo"), A_NULL);
}

