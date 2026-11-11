#include <m_pd.h>
#include <math.h>

#define YIN_DEFAULT_SIZE 4096
#define YIN_MIN_FREQUENCY 50.0
#define YIN_MAX_FREQUENCY 2000.0
#define YIN_THRESHOLD 0.15

static t_class *yin_tilde_class;

typedef struct _yin_tilde {
    t_object x_obj;
    t_sample x_sample;
    t_clock *clock;

    t_sample *buffer;
    double *difference;
    double *cmndf;
    int frame_size;
    int filled;
    double sample_rate;

    t_float pitch;
    t_float confidence;

    t_outlet *out_pitch;
    t_outlet *out_confidence;
} t_yin_tilde;

// ─────────────────────────────────────
static void yin_tilde_clock(t_yin_tilde *x) {
    outlet_float(x->out_confidence, x->confidence);
    outlet_float(x->out_pitch, x->pitch);
}

// ─────────────────────────────────────
static void yin_tilde_analyze(t_yin_tilde *x) {
    int half = x->frame_size / 2;
    int min_tau = (int)ceil(x->sample_rate / YIN_MAX_FREQUENCY);
    int max_tau = (int)ceil(x->sample_rate / YIN_MIN_FREQUENCY);

    x->pitch = 0;
    x->confidence = 0;

    if (x->sample_rate <= 0 || min_tau >= half)
        return;

    if (min_tau < 2)
        min_tau = 2;
    if (max_tau > half)
        max_tau = half;
    if (max_tau <= min_tau)
        return;

    // YIN difference function and cumulative mean normalization.
    // Fixed comparison length (half) for every tau.
    double cumulative = 0.0;
    x->cmndf[0] = 1.0;

    for (int tau = 1; tau <= max_tau; tau++) {
        double sum = 0.0;

        for (int i = 0; i < half; i++) {
            double delta = (double)x->buffer[i] - (double)x->buffer[i + tau];
            sum += delta * delta;
        }

        x->difference[tau] = sum;
        cumulative += sum;
        x->cmndf[tau] = cumulative > 0.0 ? sum * tau / cumulative : 1.0;
    }

    // First local minimum below the threshold; otherwise global minimum.
    int tau_estimate = 0;
    double best = INFINITY;

    for (int tau = min_tau; tau <= max_tau; tau++) {
        double value = x->cmndf[tau];
        if (value < YIN_THRESHOLD) {
            tau_estimate = tau;
            best = value;
            while (tau + 1 <= max_tau && x->cmndf[tau + 1] < best) {
                tau++;
                tau_estimate = tau;
                best = x->cmndf[tau];
            }
            break;
        }
    }

    if (!tau_estimate) {
        for (int tau = min_tau; tau <= max_tau; tau++) {
            if (x->cmndf[tau] < best) {
                best = x->cmndf[tau];
                tau_estimate = tau;
            }
        }
    }

    if (!tau_estimate || !isfinite(best))
        return;

    // Parabolic interpolation.
    double refined_tau = (double)tau_estimate;
    if (tau_estimate > 1 && tau_estimate < max_tau) {
        double left = x->cmndf[tau_estimate - 1];
        double center = x->cmndf[tau_estimate];
        double right = x->cmndf[tau_estimate + 1];
        double denominator = left - 2.0 * center + right;

        if (fabs(denominator) > 1e-12) {
            double offset = 0.5 * (left - right) / denominator;
            if (offset < -1.0)
                offset = -1.0;
            if (offset > 1.0)
                offset = 1.0;
            refined_tau += offset;
        }
    }

    double confidence = 1.0 - best;
    if (confidence < 0.0)
        confidence = 0.0;
    if (confidence > 1.0)
        confidence = 1.0;
    if (refined_tau <= 0.0 || confidence <= 0.0)
        return;

    double pitch = x->sample_rate / refined_tau;
    if (pitch < YIN_MIN_FREQUENCY || pitch > YIN_MAX_FREQUENCY)
        return;

    x->pitch = (t_float)pitch;
    x->confidence = (t_float)confidence;
}

// ─────────────────────────────────────
static t_int *yin_tilde_perform(t_int *w) {
    t_yin_tilde *x = (t_yin_tilde *)(w[1]);
    t_sample *in = (t_sample *)(w[2]);
    int n = (int)(w[3]);

    for (int i = 0; i < n; i++) {
        x->buffer[x->filled++] = in[i];

        if (x->filled == x->frame_size) {
            yin_tilde_analyze(x);
            clock_delay(x->clock, 0);
            x->filled = 0;
        }
    }

    return (w + 4);
}

// ─────────────────────────────────────
static void yin_tilde_dsp(t_yin_tilde *x, t_signal **sp) {
    x->sample_rate = sp[0]->s_sr;
    x->filled = 0;
    dsp_add(yin_tilde_perform, 3, x, sp[0]->s_vec, sp[0]->s_n);
}

// ─────────────────────────────────────
static void *yin_tilde_new(t_symbol *s, int argc, t_atom *argv) {
    (void)s;
    t_yin_tilde *x = (t_yin_tilde *)pd_new(yin_tilde_class);

    int frame_size = YIN_DEFAULT_SIZE;
    if (argc > 0 && argv[0].a_type == A_FLOAT) {
        t_float requested = atom_getfloat(argv);
        if (requested >= 64 && requested <= 65536)
            frame_size = (int)requested;
    }

    x->frame_size = frame_size;
    x->filled = 0;
    x->sample_rate = 0.0;
    x->pitch = 0;
    x->confidence = 0;
    x->clock = NULL;
    x->buffer = NULL;
    x->difference = NULL;
    x->cmndf = NULL;

    x->buffer = (t_sample *)getbytes((size_t)frame_size * sizeof(t_sample));
    x->difference = (double *)getbytes((size_t)(frame_size / 2 + 1) * sizeof(double));
    x->cmndf = (double *)getbytes((size_t)(frame_size / 2 + 1) * sizeof(double));
    x->clock = clock_new(x, (t_method)yin_tilde_clock);

    if (!x->buffer || !x->difference || !x->cmndf || !x->clock) {
        pd_error(x, "x.yin~: memory allocation failed");
        pd_free((t_pd *)x);
        return NULL;
    }

    x->out_pitch = outlet_new(&x->x_obj, &s_float);
    x->out_confidence = outlet_new(&x->x_obj, &s_float);
    return (void *)x;
}

// ─────────────────────────────────────
static void yin_tilde_free(t_yin_tilde *x) {
    if (x->clock)
        clock_free(x->clock);
    if (x->buffer)
        freebytes(x->buffer, (size_t)x->frame_size * sizeof(t_sample));
    if (x->difference)
        freebytes(x->difference, (size_t)(x->frame_size / 2 + 1) * sizeof(double));
    if (x->cmndf)
        freebytes(x->cmndf, (size_t)(x->frame_size / 2 + 1) * sizeof(double));
}

// ─────────────────────────────────────
void setup_x0x2eyin_tilde(void) {
    yin_tilde_class =
        class_new(gensym("x.yin~"), (t_newmethod)yin_tilde_new, (t_method)yin_tilde_free,
                  sizeof(t_yin_tilde), CLASS_DEFAULT, A_GIMME, 0);

    CLASS_MAINSIGNALIN(yin_tilde_class, t_yin_tilde, x_sample);
    class_addmethod(yin_tilde_class, (t_method)yin_tilde_dsp, gensym("dsp"), A_CANT, 0);
}
