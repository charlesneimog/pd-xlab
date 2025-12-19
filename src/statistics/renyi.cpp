#include <m_pd.h>
#include <math.h>

static t_class *renyi_class;

// ─────────────────────────────────────
class renyi {
  public:

    t_object Obj;
    t_sample Sample;

    float Alpha;

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
static void renyi_bang(renyi *x) {
    t_float RenyiDiv = 0.0;
    t_garray *Arr1 = (t_garray *)pd_findbyclass(x->P, garray_class);
    t_garray *Arr2 = (t_garray *)pd_findbyclass(x->Q, garray_class);
    if (!Arr1 || !Arr2) {
        pd_error(x, "[renyi] couldn't find some table");
        return;
    }
    t_word *Arr1Vec;
    int Arr1Size;
    garray_getfloatwords(Arr1, &Arr1Size, &Arr1Vec);

    t_word *Arr2Vec;
    int Arr2Size;
    garray_getfloatwords(Arr2, &Arr2Size, &Arr2Vec);
    if (Arr1Size != Arr2Size) {
        pd_error(x, "[renyi] tables must have the same size");
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
            RenyiDiv += pow(IVec1, x->Alpha) * pow(IVec2, 1 - x->Alpha);
        }
    }

    RenyiDiv = log(RenyiDiv) / (x->Alpha - 1.0);

    // Set the diversity value and output it
    x->Diversity = RenyiDiv;

    outlet_float(x->Out, x->Diversity);
}

// ─────────────────────────────────────
static void renyi_alpha(renyi *x, t_floatarg f) { x->Alpha = f; }
// ─────────────────────────────────────
static void renyi_expo(renyi *x, t_floatarg f) { x->Exp = f; }
// ─────────────────────────────────────
static void renyi_tick(renyi *x) { outlet_float(x->Out, x->Diversity); }
// ─────────────────────────────────────
static void renyi_norm(renyi *x, t_floatarg f) {
    if (f == 1) {
        x->Normalize = true;
    } else {
        x->Normalize = false;
    }
    return;
}

// ─────────────────────────────────────
static t_int *renyi_perform(t_int *w) {
    renyi *x = (renyi *)(w[1]);
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

    t_float RenyiDiv = 0.0;
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
            RenyiDiv += pow(IVec1, x->Alpha) * pow(IVec2, 1 - x->Alpha);
        }
    }

    RenyiDiv = log(RenyiDiv) / (x->Alpha - 1.0);

    x->Diversity = RenyiDiv;
    clock_delay(x->Clock, 0);

    return (w + 5);
}

// ─────────────────────────────────────
static void renyi_dsp(renyi *x, t_signal **sp) {
    if (x->RealTime) {
        dsp_add(renyi_perform, 4, x, sp[0]->s_vec, sp[1]->s_vec, sp[0]->s_n);
    }
}

// ─────────────────────────────────────
static void *renyi_new(t_symbol *s, int argc, t_atom *argv) {
    renyi *x = (renyi *)pd_new(renyi_class);
    bool isSinal = false;
    if (s == gensym("renyi~")) {
        isSinal = true;
    }
    if (argc == 2) {
        x->RealTime = false;
        if (argv[0].a_type != A_SYMBOL || argv[1].a_type != A_SYMBOL) {
            pd_error(x, "[renyi~] arg1 and arg2 must be symbols");
            return NULL;
        }
        x->P = atom_getsymbol(argv);
        x->Q = atom_getsymbol(argv + 1);
    } else {
        x->RealTime = true;
        x->In2 = inlet_new(&x->Obj, &x->Obj.ob_pd, &s_signal, &s_signal);
        x->Clock = clock_new(x, (t_method)renyi_tick);
    }
    x->Out = outlet_new(&x->Obj, &s_anything);
    x->Alpha = 1.0;
    return (x);
}

// ─────────────────────────────────────
void renyi_setup(void) {
    renyi_class =
        class_new(gensym("renyi"), (t_newmethod)renyi_new, 0, sizeof(renyi), 0, A_GIMME, 0);
    class_addcreator((t_newmethod)renyi_new, gensym("renyi~"), A_GIMME, 0);
    CLASS_MAINSIGNALIN(renyi_class, renyi, Sample);
    class_addmethod(renyi_class, (t_method)renyi_dsp, gensym("dsp"), A_CANT, 0);
    class_addmethod(renyi_class, (t_method)renyi_norm, gensym("norm"), A_FLOAT, 0);
    class_addmethod(renyi_class, (t_method)renyi_alpha, gensym("alpha"), A_FLOAT, 0);
    class_addmethod(renyi_class, (t_method)renyi_expo, gensym("exp"), A_FLOAT, 0);
    class_addbang(renyi_class, renyi_bang);
}
