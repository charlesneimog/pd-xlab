#include <m_pd.h>

#include <cstring>
#include <signalsmith-stretch/signalsmith-stretch.h>

static t_class *pv_tilde_class;

// ─────────────────────────────────────
class pv_tilde {
  public:
    t_object xobj;
    t_sample s;
    t_float factor;
    signalsmith::stretch::SignalsmithStretch<t_sample> *pv;
    std::vector<std::vector<t_sample>> in;
    std::vector<std::vector<t_sample>> out;
    int FFTSize = 4096;

    size_t BlockIndex = 0;
    size_t OutIndex = 0;
    t_outlet *x_out;
};

// ─────────────────────────────────────
static void pv_tilde_float(pv_tilde *x, t_float cents) {
    x->factor = std::pow(2, cents / 1200);
    x->pv->setTransposeFactor(x->factor);
}

// ─────────────────────────────────────
static t_int *pv_tilde_perform(t_int *w) {
    pv_tilde *x = (pv_tilde *)(w[1]);
    t_sample *inSig = (t_sample *)(w[2]);
    t_sample *outSig = (t_sample *)(w[3]);
    int n = (int)(w[4]);

    std::memmove(x->in[0].data(), x->in[0].data() + n, (x->FFTSize - n) * sizeof(t_sample));
    std::memcpy(x->in[0].data() + (x->FFTSize - n), inSig, n * sizeof(t_sample));
    x->BlockIndex += n;

    if (x->BlockIndex >= x->FFTSize && x->OutIndex >= x->FFTSize) {
        x->BlockIndex = 0;
        x->OutIndex = 0;
        x->pv->process(x->in, (int)x->FFTSize, x->out, (int)x->FFTSize);
    }

    if (x->OutIndex < x->FFTSize) {
        std::memcpy(outSig, x->out[0].data() + x->OutIndex, n * sizeof(t_sample));
        x->OutIndex += n;
    } else {
        std::fill(outSig, outSig + n, 0.0f);
    }
    return (w + 5);
}

// ─────────────────────────────────────
static void pv_tilde_dsp(pv_tilde *x, t_signal **sp) {
    x->BlockIndex = 0;
    x->OutIndex = x->FFTSize;

    x->pv->setTransposeFactor(x->factor);
    x->in = std::vector<std::vector<t_sample>>(1, std::vector<t_sample>(x->FFTSize, 0.0f));
    x->out = std::vector<std::vector<t_sample>>(1, std::vector<t_sample>(x->FFTSize, 0.0f));

    dsp_add(pv_tilde_perform, 4, x, sp[0]->s_vec, sp[1]->s_vec, sp[0]->s_n);
}

// ─────────────────────────────────────
static void *pv_tilde_new(t_symbol *s, int argc, t_atom *argv) {
    pv_tilde *x = (pv_tilde *)pd_new(pv_tilde_class);

    x->pv = new signalsmith::stretch::SignalsmithStretch<t_sample>();
    x->FFTSize = 1024;
    x->factor = 1.0;

    for (int i = 0; i < argc; i++) {

        // pitch in cents (single float, no flag)
        if (argv[i].a_type == A_FLOAT) {
            float cents = atom_getfloat(&argv[i]);
            x->factor = std::pow(2.0, cents / 1200.0);
            break;
        }

        // flags
        if (argv[i].a_type == A_SYMBOL) {
            t_symbol *sym = atom_getsymbol(&argv[i]);
            if (sym == gensym("-fftsize")) {
                if (i + 1 < argc && argv[i + 1].a_type == A_FLOAT) {
                    x->FFTSize = (int)atom_getfloat(&argv[i + 1]);
                    i++;
                } else {
                    pd_error(x, "-fftsize requires an integer argument");
                }
            }
        }
    }

    x->x_out = outlet_new(&x->xobj, &s_signal);
    x->pv->configure(1, x->FFTSize, x->FFTSize / 4);
    logpost(x, PD_DEBUG, "[x.pv~] FFTSize set to %d", x->FFTSize);

    return x;
}

// ─────────────────────────────────────
static void pv_tilde_free(pv_tilde *x) {
    delete x->pv;
}

// ─────────────────────────────────────
extern "C" void setup_x0x2epv_tilde(void) {
    pv_tilde_class = class_new(gensym("x.pv~"), (t_newmethod)pv_tilde_new, (t_method)pv_tilde_free,
                               sizeof(pv_tilde), CLASS_DEFAULT, A_GIMME, A_NULL);
    CLASS_MAINSIGNALIN(pv_tilde_class, pv_tilde, s);
    class_addmethod(pv_tilde_class, (t_method)pv_tilde_dsp, gensym("dsp"), A_CANT, 0);
    class_addfloat(pv_tilde_class, (t_method)pv_tilde_float);
}
