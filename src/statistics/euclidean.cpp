#include <m_pd.h>

#include <math.h>
#include <string>
#include <vector>

static t_class *Euclidean;

// ─────────────────────────────────────
class EntropyObj {
  public:
    EntropyObj() {};

    t_object obj;
    t_sample Sample;

    float Alpha;

    bool RealTime;
    bool Normalize;
    bool Exp;

    std::vector<float> array1;

    t_clock *Clock;

    t_inlet *In2;
    t_outlet *out;

    t_symbol *Q;
    t_symbol *P;

    t_float distance;
};

// ─────────────────────────────────────
static void euclidean_bang(EntropyObj *x) {
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
    x->distance = RenyiDiv;

    outlet_float(x->out, x->distance);
}

// ─────────────────────────────────────
static void euclidean_list(EntropyObj *x, t_symbol *s, int argc, t_atom *argv) {
    t_float distance = 0.0;
    int size = x->array1.size();
    if (size != argc) {
        pd_error(x, "[euclidean] list must have the same size");
        return;
    }

    // Calcula a soma dos quadrados das diferenças
    for (int i = 0; i < argc; i++) {
        float number = atom_getfloat(argv + i);
        distance += pow(number - x->array1[i], 2);
    }

    // Distância euclidiana
    t_float euclidean_distance = sqrt(distance);
    // Normaliza dividindo pela distância máxima possível sqrt(n)
    t_float normalized_distance = euclidean_distance / sqrt(argc);

    x->distance = normalized_distance;
    outlet_float(x->out, normalized_distance);
}

// ─────────────────────────────────────
static void euclidean_storelist(EntropyObj *x, t_symbol *s, int argc, t_atom *argv) {
    x->array1.clear();
    for (int i = 0; i < argc; i++) {
        x->array1.push_back(atom_getfloat(argv + i));
    }
    return;
}

// ─────────────────────────────────────
static void euclidean_tick(EntropyObj *x) {
    //
    // outlet_float(x->Out, x->distance);
}
// ─────────────────────────────────────
static void euclidean_norm(EntropyObj *x, t_floatarg f) {
    if (f == 1) {
        x->Normalize = true;
    } else {
        x->Normalize = false;
    }
    return;
}

// ─────────────────────────────────────
static t_int *euclidean_perform(t_int *w) {
    EntropyObj *x = (EntropyObj *)(w[1]);
    t_float *Arr1Vec = (t_float *)(w[2]);
    t_float *Arr2Vec = (t_float *)(w[3]);
    int n = (int)(w[4]);

    // Initialize the Euclidean distance variable
    t_float distance = 0.0;

    // Calculate the sum of squared differences
    for (int i = 0; i < n; i++) {
        distance += pow(Arr1Vec[i] - Arr2Vec[i], 2);
    }

    x->distance = sqrt(distance);
    clock_delay(x->Clock, 0);
    return (w + 5);
}

// ─────────────────────────────────────
static void euclidean_dsp(EntropyObj *x, t_signal **sp) {
    if (x->RealTime) {
        dsp_add(euclidean_perform, 4, x, sp[0]->s_vec, sp[1]->s_vec, sp[0]->s_n);
    }
}

// ─────────────────────────────────────
static void *euclidean_new(t_symbol *s, int argc, t_atom *argv) {
    EntropyObj *x = (EntropyObj *)pd_new(Euclidean);
    bool isSinal = false;
    std::string method = "euclidean~";
    if (s->s_name == method) {
        x->RealTime = true;
    } else {
        x->RealTime = false;
        x->In2 = inlet_new(&x->obj, &x->obj.ob_pd, &s_list, gensym("_storelist"));
    }
    x->out = outlet_new(&x->obj, &s_anything);
    return (x);
}

// ─────────────────────────────────────
void euclidean_setup(void) {
    Euclidean = class_new(gensym("euclidean"), (t_newmethod)euclidean_new, 0, sizeof(EntropyObj),
                          0, A_GIMME, 0);
    class_addcreator((t_newmethod)euclidean_new, gensym("euclidean~"), A_GIMME, 0);
    CLASS_MAINSIGNALIN(Euclidean, EntropyObj, Sample);

    class_addmethod(Euclidean, (t_method)euclidean_dsp, gensym("dsp"), A_CANT, 0);
    class_addmethod(Euclidean, (t_method)euclidean_norm, gensym("norm"), A_FLOAT, 0);
    class_addmethod(Euclidean, (t_method)euclidean_storelist, gensym("_storelist"), A_GIMME, 0);
    class_addlist(Euclidean, euclidean_list);
    class_addbang(Euclidean, euclidean_bang);
}
