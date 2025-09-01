#include <m_pd.h>
#include <math.h>

static t_class *switch_control_class;

typedef struct _switch_control {
    t_object x_obj;
    t_sample x_f;
    t_clock *x_defer;
    t_clock *x_tick;

    float delay;
    float silence_threshold; // threshold in mean-square domain
    int x_period;            // analysis window in samples
    int x_realperiod;
    int x_phase;
    t_sample *x_buf;
    t_sample *x_sumbuf;
    int x_npoints;
    int x_allocforvs;

    double x_result;
    int is_silence;
    int run;

    t_outlet *out;
} switch_control;

// ─────────────────────────────────────
static void switch_control_defer(switch_control *x) { x->run = 1; }

// ─────────────────────────────────────
static void switch_control_bang(switch_control *x) {
    x->run = 0;
    clock_delay(x->x_defer, x->delay);
}

// ─────────────────────────────────────
static void switch_control_tick(switch_control *x) {
    // Output 1 if silence, 0 otherwise
    outlet_float(x->out, x->is_silence ? 1 : 0);
}

// ─────────────────────────────────────
static t_int *switch_control_perform(t_int *w) {
    switch_control *x = (switch_control *)(w[1]);
    t_sample *in = (t_sample *)(w[2]);
    int n = (int)(w[3]);

    if (!x->run || n <= 0)
        return (w + 4);

    // --- env~ style accumulation ---
    t_sample *sump;
    int count;
    in += n;

    for (count = x->x_phase, sump = x->x_sumbuf; count < x->x_npoints;
         count += x->x_realperiod, sump++) {
        t_sample *hp = x->x_buf + count;
        t_sample *fp = in;
        t_sample sum = *sump;
        int i;

        for (i = 0; i < n; i++) {
            fp--;
            sum += *hp++ * (*fp * *fp);
        }
        *sump = sum;
    }

    sump[0] = 0;
    x->x_phase -= n;
    if (x->x_phase < 0) {
        x->x_result = x->x_sumbuf[0];

        // shift buffer
        for (count = x->x_realperiod, sump = x->x_sumbuf; count < x->x_npoints;
             count += x->x_realperiod, sump++) {
            sump[0] = sump[1];
        }
        sump[0] = 0;
        x->x_phase = x->x_realperiod - n;

        // classify silence vs sound
        x->is_silence = (x->x_result <= x->silence_threshold);
        clock_delay(x->x_tick, 0);
    }

    return (w + 4);
}

// ─────────────────────────────────────
static void switch_control_dsp(switch_control *x, t_signal **sp) {
    if (x->x_period % sp[0]->s_n)
        x->x_realperiod = x->x_period + sp[0]->s_n - (x->x_period % sp[0]->s_n);
    else
        x->x_realperiod = x->x_period;

    if (sp[0]->s_n > x->x_allocforvs) {
        void *xx = resizebytes(x->x_buf, (x->x_npoints + x->x_allocforvs) * sizeof(t_sample),
                               (x->x_npoints + sp[0]->s_n) * sizeof(t_sample));
        if (!xx) {
            pd_error(0, "switch.control~: out of memory");
            return;
        }
        x->x_buf = (t_sample *)xx;
        x->x_allocforvs = sp[0]->s_n;
    }
    dsp_add(switch_control_perform, 3, x, sp[0]->s_vec, sp[0]->s_n);
}

// ─────────────────────────────────────
static void *switch_control_new(t_symbol *s, int argc, t_atom *argv) {
    (void)s;
    switch_control *x = (switch_control *)pd_new(switch_control_class);

    if (argc < 3) {
        pd_error(x, "[switch.control~] usage: silence_threshold delay_ms period_samples");
        return NULL;
    }

    x->silence_threshold = atom_getfloat(argv);
    x->delay = atom_getfloat(argv + 1);
    x->x_period = (int)atom_getfloat(argv + 2);

    x->x_phase = x->x_period;
    x->x_npoints = x->x_period * 2;
    x->x_allocforvs = 64; // initial blocksize
    x->x_buf = (t_sample *)getbytes((x->x_npoints + x->x_allocforvs) * sizeof(t_sample));
    x->x_sumbuf = (t_sample *)getbytes((x->x_npoints / x->x_period + 2) * sizeof(t_sample));

    x->run = 1;
    x->is_silence = 0;
    x->x_result = 0;

    x->x_defer = clock_new(x, (t_method)switch_control_defer);
    x->x_tick = clock_new(x, (t_method)switch_control_tick);
    x->out = outlet_new(&x->x_obj, &s_float);

    return (x);
}

// ─────────────────────────────────────
static void switch_control_free(switch_control *x) {
    if (x->x_buf)
        freebytes(x->x_buf, (x->x_npoints + x->x_allocforvs) * sizeof(t_sample));
    if (x->x_sumbuf)
        freebytes(x->x_sumbuf, (x->x_npoints / x->x_period + 2) * sizeof(t_sample));
    clock_free(x->x_defer);
    clock_free(x->x_tick);
}

// ─────────────────────────────────────
void switch0x2econtrol_tilde_setup(void) {
    switch_control_class =
        class_new(gensym("switch.control~"), (t_newmethod)switch_control_new,
                  (t_method)switch_control_free, sizeof(switch_control), CLASS_DEFAULT, A_GIMME, 0);

    CLASS_MAINSIGNALIN(switch_control_class, switch_control, x_f);
    class_addmethod(switch_control_class, (t_method)switch_control_dsp, gensym("dsp"), A_CANT, 0);
    class_addbang(switch_control_class, switch_control_bang);
}
