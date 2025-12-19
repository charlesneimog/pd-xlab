#include <m_pd.h>
#include <math.h>

// ╭─────────────────────────────────────╮
// │ Kullback-Leibler Divergence (KLD),  │
// │       is a measure of how one       │
// │  probability distribution differs   │
// │      from a second, reference       │
// │   probability distribution. It is   │
// │ used extensively in fields such as  │
// │   information theory, statistics,   │
// │   and machine learning. For music   │
// │   reference, check the thesis of    │
// │   Arshia Cont, pages 141 and 142.   │
// ╰─────────────────────────────────────╯

static t_class *neimog_kl;

// ─────────────────────────────────────
class kl {
  public:
    kl() {};
    ~kl() {};

    t_object Obj;
    t_sample Sample;
    t_float Beta;

    bool RealTime;
    bool Normalize;
    bool Exp;

    t_clock *Clock;

    t_inlet *In2;
    t_outlet *Out;

    t_symbol *Q;
    t_symbol *P;

    t_float Diversity;
};

// ─────────────────────────────────────
static void kl_bang(kl *x) {
    t_float KLDiv = 0.0;
    t_garray *Arr1 = (t_garray *)pd_findbyclass(x->P, garray_class);
    t_garray *Arr2 = (t_garray *)pd_findbyclass(x->Q, garray_class);
    if (!Arr1 || !Arr2) {
        pd_error(x, "[divergence.kl] couldn't find some table");
        return;
    }
    t_word *Arr1Vec;
    int Arr1Size;
    garray_getfloatwords(Arr1, &Arr1Size, &Arr1Vec);

    t_word *Arr2Vec;
    int Arr2Size;
    garray_getfloatwords(Arr2, &Arr2Size, &Arr2Vec);
    if (Arr1Size != Arr2Size) {
        pd_error(x, "[divergence.kl] tables must have the same size");
        return;
    }
    if (x->Normalize) {
        float Sum1 = 0.0;
        float Sum2 = 0.0;
        for (int i = 0; i < Arr1Size; i++) {
            Sum1 += Arr1Vec[i].w_float;
            Sum2 += Arr2Vec[i].w_float;
        }
        for (int i = 0; i < Arr1Size; i++) {
            Arr1Vec[i].w_float /= Sum1;
            Arr2Vec[i].w_float /= Sum2;
        }
    }

    for (int i = 0; i < Arr1Size; i++) {
        float IVec1 = Arr1Vec[i].w_float;
        float IVec2 = Arr2Vec[i].w_float;
        if (IVec1 > 0 && IVec2 > 0) {
            // Equation 7.18 in Cont's thesis
            KLDiv += IVec1 * log(IVec1 / IVec2); //- IVec1 + IVec2;
        }
    }

    if (x->Exp) {
        KLDiv = exp(-x->Beta * KLDiv); // Equation 7.19 in Cont's thesis
    }

    // Set the diversity value and output it
    x->Diversity = KLDiv;

    outlet_float(x->Out, x->Diversity);
}

// ─────────────────────────────────────
static void kl_setbeta(kl *x, t_floatarg f) { x->Beta = f; }
// ─────────────────────────────────────
static void kl_expo(kl *x, t_floatarg f) { x->Exp = f; }
// ─────────────────────────────────────
static void kl_tick(kl *x) { outlet_float(x->Out, x->Diversity); }
// ─────────────────────────────────────
static void kl_norm(kl *x, t_floatarg f) {
    if (f == 1) {
        x->Normalize = true;
    } else {
        x->Normalize = false;
    }
    return;
}

// ─────────────────────────────────────
static t_int *kl_perform(t_int *w) {
    kl *x = (kl *)(w[1]);
    t_float *Arr1Vec = (t_float *)(w[2]);
    t_float *Arr2Vec = (t_float *)(w[3]);
    int n = (int)(w[4]);

    // check if input 1 or 2 is silence
    float Vec1Sum = 0.0;
    float Vec2Sum = 0.0;
    for (int i = 0; i < n; i++) {
        Vec1Sum += Arr1Vec[i];
        Vec2Sum += Arr2Vec[i];
    }
    if (Vec1Sum == 0 || Vec2Sum == 0) {
        x->Diversity = 0.0;
        clock_delay(x->Clock, 0);
        return (w + 5);
    }

    t_float KLDiv = 0.0;
    if (x->Normalize) {
        float Sum1 = 0.0;
        float Sum2 = 0.0;
        for (int i = 0; i < n; i++) {
            Sum1 += Arr1Vec[i];
            Sum2 += Arr2Vec[i];
        }
        for (int i = 0; i < n; i++) {
            Arr1Vec[i] /= Sum1;
            Arr2Vec[i] /= Sum2;
        }
    }

    for (int i = 0; i < n; i++) {
        float IVec1 = Arr1Vec[i];
        float IVec2 = Arr2Vec[i];
        if (IVec1 > 0 && IVec2 > 0) {
            KLDiv += IVec1 * log(IVec1 / IVec2); //- IVec1 + IVec2;
        }
    }

    if (x->Exp) {
        KLDiv = exp(-x->Beta * KLDiv);
    }

    x->Diversity = KLDiv;
    clock_delay(x->Clock, 0);

    return (w + 5);
}

// ─────────────────────────────────────
static void kl_dsp(kl *x, t_signal **sp) {
    if (x->RealTime) {
        dsp_add(kl_perform, 4, x, sp[0]->s_vec, sp[1]->s_vec, sp[0]->s_n);
    }
}

// ─────────────────────────────────────
static void *kl_new(t_symbol *s, int argc, t_atom *argv) {
    kl *x = (kl *)pd_new(neimog_kl);
    bool isSinal = false;
    if (s == gensym("kl~")) {
        isSinal = true;
    }
    if (argc == 2 && !isSinal) {
        x->RealTime = false;
        if (argv[0].a_type != A_SYMBOL || argv[1].a_type != A_SYMBOL) {
            pd_error(x, "[divergence.kl] arg1 and arg2 must be symbols");
            return NULL;
        }
        x->P = atom_getsymbol(argv);
        x->Q = atom_getsymbol(argv + 1);
    } else if (argc == 0 && isSinal) {
        x->RealTime = true;
        x->In2 = inlet_new(&x->Obj, &x->Obj.ob_pd, &s_signal, &s_signal);
        x->Clock = clock_new(x, (t_method)kl_tick);
    } else {
        pd_error(x, "[divergence.kl] wrong number of args");
        return nullptr;
    }
    x->Out = outlet_new(&x->Obj, &s_anything);
    x->Beta = 1.0;
    return (x);
}

static void *kl_free(kl *x) {
    if (x->RealTime) {
        inlet_free(x->In2);
        clock_free(x->Clock);
    }
    return (void *)x;
}

// ─────────────────────────────────────
void kldivergence_setup(void) {
    neimog_kl =
        class_new(gensym("kl"), (t_newmethod)kl_new, (t_method)kl_free, sizeof(kl), 0, A_GIMME, 0);
    class_addcreator((t_newmethod)kl_new, gensym("kl~"), A_GIMME, 0);
    CLASS_MAINSIGNALIN(neimog_kl, kl, Sample);
    class_addmethod(neimog_kl, (t_method)kl_dsp, gensym("dsp"), A_CANT, 0);
    class_addmethod(neimog_kl, (t_method)kl_norm, gensym("norm"), A_FLOAT, 0);
    class_addmethod(neimog_kl, (t_method)kl_setbeta, gensym("beta"), A_FLOAT, 0);
    class_addmethod(neimog_kl, (t_method)kl_expo, gensym("exp"), A_FLOAT, 0);
    class_addbang(neimog_kl, kl_bang);
}
