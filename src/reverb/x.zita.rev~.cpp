#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <m_pd.h>

#include "reverb.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static t_class *zita_class;

// ─────────────────────────────────────
typedef struct _zita_tilde {
    t_object x_obj;
    t_sample f;

    double sr;
    Reverb *reverb;

    t_outlet *x_outL;
    t_outlet *x_outR;
} t_zita_tilde;

// ─────────────────────────────────────
static void zita_tilde_set(t_zita_tilde *x, t_symbol *s, int argc, t_atom *argv) {
    if (!x || !x->reverb || !s) {
        return;
    }

    const char *method = s->s_name;

    if (!strcmp(method, "delay")) {
        if (argc == 1) {
            x->reverb->set_delay(atom_getfloat(argv));
        }
    } else if (!strcmp(method, "xover")) {
        if (argc == 1) {
            x->reverb->set_xover(atom_getfloat(argv));
        }
    } else if (!strcmp(method, "rtlow")) {
        if (argc == 1) {
            x->reverb->set_rtlow(atom_getfloat(argv));
        }
    } else if (!strcmp(method, "rtmid") || !strcmp(method, "rt60")) {
        if (argc == 1) {
            x->reverb->set_rtmid(atom_getfloat(argv));
        }
    } else if (!strcmp(method, "fdamp")) {
        if (argc == 1) {
            x->reverb->set_fdamp(atom_getfloat(argv));
        }
    } else if (!strcmp(method, "opmix")) {
        if (argc == 1) {
            x->reverb->set_opmix(atom_getfloat(argv));
        }
    } else if (!strcmp(method, "rgxyz")) {
        if (argc == 1) {
            x->reverb->set_rgxyz(atom_getfloat(argv));
        }
    } else if (!strcmp(method, "eq1")) {
        if (argc == 2) {
            x->reverb->set_eq1(atom_getfloat(argv), atom_getfloat(argv + 1));
        }
    } else if (!strcmp(method, "eq2")) {
        if (argc == 2) {
            x->reverb->set_eq2(atom_getfloat(argv), atom_getfloat(argv + 1));
        }
    }
}

// ─────────────────────────────────────
static t_int *zita_tilde_perform(t_int *w) {
    t_zita_tilde *x = (t_zita_tilde *)w[1];

    t_sample *in = (t_sample *)w[2];
    t_sample *outL = (t_sample *)w[3];
    t_sample *outR = (t_sample *)w[4];

    int n = (int)w[5];

    float *inp[2] = {(float *)in, (float *)in};
    float *out[2] = {(float *)outL, (float *)outR};

    x->reverb->prepare(n);
    x->reverb->process(n, inp, out);

    return (w + 6);
}

// ─────────────────────────────────────
static void zita_tilde_dsp(t_zita_tilde *x, t_signal **sp) {
    double new_sr = sp[0]->s_sr > 0 ? sp[0]->s_sr : 44100.0;
    dsp_add(zita_tilde_perform, 5, x,
            sp[0]->s_vec, // input
            sp[1]->s_vec, // output L
            sp[2]->s_vec, // output R
            sp[0]->s_n);
}

// ─────────────────────────────────────
static void *zita_tilde_new(t_symbol *s, int argc, t_atom *argv) {
    t_zita_tilde *x = (t_zita_tilde *)pd_new(zita_class);
    x->sr = sys_getsr();
    x->x_outL = outlet_new(&x->x_obj, &s_signal);
    x->x_outR = outlet_new(&x->x_obj, &s_signal);

    x->reverb = new Reverb();
    x->reverb->init(x->sr, false);

    return x;
}

// ─────────────────────────────────────
static void zita_tilde_free(t_zita_tilde *x) {
    //
    outlet_free(x->x_outL);
    outlet_free(x->x_outR);
}

// ─────────────────────────────────────
extern "C" void setup_x0x2ezita0x2erev_tilde(void) {
    zita_class =
        class_new(gensym("x.zita.rev~"), (t_newmethod)zita_tilde_new, (t_method)zita_tilde_free,
                  sizeof(t_zita_tilde), CLASS_DEFAULT, A_GIMME, 0);

    CLASS_MAINSIGNALIN(zita_class, t_zita_tilde, f);
    class_addmethod(zita_class, (t_method)zita_tilde_dsp, gensym("dsp"), A_CANT, 0);
    class_addmethod(zita_class, (t_method)zita_tilde_set, gensym("delay"), A_GIMME, 0);
    class_addmethod(zita_class, (t_method)zita_tilde_set, gensym("xover"), A_GIMME, 0);
    class_addmethod(zita_class, (t_method)zita_tilde_set, gensym("rtlow"), A_GIMME, 0);
    class_addmethod(zita_class, (t_method)zita_tilde_set, gensym("rtmid"), A_GIMME, 0);
    class_addmethod(zita_class, (t_method)zita_tilde_set, gensym("rt60"), A_GIMME, 0);
    class_addmethod(zita_class, (t_method)zita_tilde_set, gensym("fdamp"), A_GIMME, 0);
    class_addmethod(zita_class, (t_method)zita_tilde_set, gensym("opmix"), A_GIMME, 0);
    class_addmethod(zita_class, (t_method)zita_tilde_set, gensym("rgxyz"), A_GIMME, 0);
    class_addmethod(zita_class, (t_method)zita_tilde_set, gensym("eq1"), A_GIMME, 0);
    class_addmethod(zita_class, (t_method)zita_tilde_set, gensym("eq2"), A_GIMME, 0);
}
