#include <m_pd.h>
#include <math.h>

static t_class *nonset_tilde_class;

class nonset {
  public:
    t_object x_obj;
    t_sample x_f;
    t_clock *x_clock;
    t_int rms_window;
    double previous_rms;
    t_int index;
    t_int iterations;
    double init_value;

    double noise_covariance;
    double kalman;
    double diff;

    double *history;

    unsigned int buffer_pointer;
    t_float threshold;

    t_outlet *diff_out;
    t_outlet *detection_out;
    t_outlet *bang_out;
};

// ─────────────────────────────────────
static void nonset_tick(nonset *x) {
    if (isnan(x->kalman)) {
        x->kalman = 0;
    }

    outlet_float(x->detection_out, x->kalman);
    outlet_float(x->diff_out, x->diff);

    if (x->kalman > x->threshold) {
        outlet_bang(x->bang_out);
    }
}

// ─────────────────────────────────────
static t_int *nonset_perform(t_int *w) {
    nonset *x = (nonset *)(w[1]);
    t_sample *in = (t_sample *)(w[2]);
    int n = (int)(w[3]);

    double sum = 0.0;
    for (int i = 0; i < n; i++) {
        sum += in[i] * in[i];
    }
    double rms = sqrt(sum / n);
    double diff = (rms - x->previous_rms) / x->previous_rms;
    x->diff = 1 - exp(-0.5 * diff);

    // Kalman filter
    x->history[x->index] = diff;
    x->index = (x->index + 1) % x->iterations;
    double Pk = 1;
    double xk = (double)x->init_value;
    int i;
    for (i = 0; i < x->iterations; i++) {
        double Zk = (double)x->history[(i + x->index) % x->iterations];
        double Kk = Pk / (Pk + (double)x->noise_covariance);
        xk = xk + Kk * (Zk - xk);
        Pk = (1 - Kk) * Pk;
    }

    x->kalman = xk;
    x->previous_rms = rms;
    clock_delay(x->x_clock, 0);
    return (w + 4);
}

// ─────────────────────────────────────
static void nonset_dsp(nonset *x, t_signal **sp) {
    if (x->rms_window == 0) {
        x->rms_window = sp[0]->s_n;
    }
    dsp_add(nonset_perform, 3, x, sp[0]->s_vec, sp[0]->s_n);
}

// ─────────────────────────────────────
// Constructor
static void *nonset_new(t_floatarg threshold) {
    nonset *x = (nonset *)pd_new(nonset_tilde_class);
    x->rms_window = 0;
    x->previous_rms = 0.e-21;
    x->history = new double[100];
    for (int i = 0; i < 100; i++) {
        x->history[i] = x->init_value;
    }

    x->iterations = 20;
    x->noise_covariance = 2;
    x->init_value = 0;
    x->index = 0;
    x->threshold = threshold;

    x->x_clock = clock_new(x, (t_method)nonset_tick);

    x->bang_out = outlet_new(&x->x_obj, &s_bang);
    x->detection_out = outlet_new(&x->x_obj, &s_float);
    x->diff_out = outlet_new(&x->x_obj, &s_float);
    return (void *)x;
}

// ─────────────────────────────────────
// Destructor
static void nonset_free(nonset *x) {}

// ─────────────────────────────────────
// Setup Function
void nonset_tilde_setup(void) {
    nonset_tilde_class =
        class_new(gensym("n.onset~"), (t_newmethod)nonset_new, (t_method)nonset_free,
                  sizeof(nonset), CLASS_DEFAULT, A_DEFFLOAT, 0);

    CLASS_MAINSIGNALIN(nonset_tilde_class, nonset, x_f);
    class_addmethod(nonset_tilde_class, (t_method)nonset_dsp, gensym("dsp"), A_CANT, 0);
}
