#include "onsetsds.h"
#include <m_pd.h>
#include <math.h>

static t_class *onsetsds_tilde_class;

// ─────────────────────────────────────
typedef struct _onsetsds_tilde {
    t_object x_obj;
    t_sample x_sample;
    t_clock *clock;

    t_sample *x_fft_in;
    t_sample *x_fft_imag;

    t_int onset_type;
    t_int medspan;

    t_sample *x_window;
    int accum;
    int fftsize;

    // OnsetsDS
    OnsetsDS *ODS;
    float *ods_data;
    int onset;

    // Outlets
    t_outlet *x_out_bang, *x_out_odf;
} t_onsetsds_tilde;

// ─────────────────────────────────────
static void onsetsds_tilde_restart(t_onsetsds_tilde *x) {
    freebytes(x->ods_data, onsetsds_memneeded(x->onset_type, x->fftsize, 10));
    freebytes(x->ODS, sizeof(OnsetsDS));

    x->ods_data = (float *)getbytes(onsetsds_memneeded(x->onset_type, x->fftsize, 10));
    x->ODS = (OnsetsDS *)getbytes(sizeof(OnsetsDS));
    onsetsds_init(x->ODS, x->ods_data, ODS_FFT_FFTW3_R2C, x->onset_type, x->fftsize, 10,
                  sys_getsr());
    x->x_fft_in = (t_sample *)getbytes(x->fftsize * sizeof(t_sample));
    x->x_fft_imag = (t_sample *)getbytes(x->fftsize * sizeof(t_sample));
    memset(x->x_fft_in, 0, x->fftsize * sizeof(t_sample));
    memset(x->x_fft_imag, 0, x->fftsize * sizeof(t_sample));

    x->x_window = (t_sample *)getbytes(x->fftsize * sizeof(t_sample));
    for (int i = 0; i < x->fftsize; i++) {
        x->x_window[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (x->fftsize - 1)));
    }
}

// ─────────────────────────────────────
static void onsetsds_tilde_set(t_onsetsds_tilde *x, t_symbol *s, int argc, t_atom *argv) {
    const char *method = atom_getsymbol(argv)->s_name;
    if (strcmp(method, "relaxtime") == 0) {
        onsetsds_setrelax(x->ODS, atom_getfloat(argv + 1), x->fftsize);
    } else if (strcmp(method, "floor") == 0) {
        x->ODS->floor = atom_getfloat(argv + 1);
        logpost(x, 3, "Floor: %f", x->ODS->floor);
    } else if (strcmp(method, "threshold") == 0) {
        x->ODS->thresh = atom_getfloat(argv + 1);
        logpost(x, 3, "Threshold: %f", x->ODS->thresh);
    } else if (strcmp(method, "method") == 0) {
        x->onset_type = atom_getint(argv + 1) - 1;
        if (x->onset_type < 0 || x->onset_type > 6) {
            pd_error(x, "Invalid method type, choose between:");
            pd_error(x, "  1: Power");
            pd_error(x, "  2: Sum of magnitues");
            pd_error(x, "  3: Complex-domain deviation");
            pd_error(x, "  4: Complex-domain deviation, rectified");
            pd_error(x, "  5: Phase deviation");
            pd_error(x, "  6: Weighted phase deviation");
            pd_error(x, "  7: Modified Kullback-Liebler deviation (default)");
            return;
        }
        onsetsds_tilde_restart(x);
    } else if (strcmp(method, "whtype") == 0) {
        int wh_type = atom_getint(argv + 1);
        if (wh_type == ODS_WH_NONE || wh_type == ODS_WH_ADAPT_MAX1) {
            x->ODS->whtype = wh_type;
            x->ODS->whtype ? post("Wh Type On") : post("Wh Type Off");
        } else {
            pd_error(x, "Invalid window type, choose between:");
            pd_error(x, "  0: None");
            pd_error(x, "  1: Adaptive max 1");
        }
    } else if (strcmp(method, "fftsize") == 0) {
        // x->fftsize=atom_getint(argv+1);
        // onsetsds_tilde_restart(x);
    } else if (strcmp(method, "whtype") == 0) {
    } else {
        pd_error(x, "Unknown method");
    }
}

// ─────────────────────────────────────
static void onsetsds_tilde_tick(t_onsetsds_tilde *x) {
    if (x->onset) {
        outlet_bang(x->x_out_bang);
        x->onset = 0;
    }
    outlet_float(x->x_out_odf, x->ODS->odfvalpost);
}

// ─────────────────────────────────────
static t_int *onsetsds_tilde_perform(t_int *w) {
    t_onsetsds_tilde *x = (t_onsetsds_tilde *)(w[1]);
    t_sample *in = (t_sample *)(w[2]);
    int n = (int)(w[3]);

    for (int i = 0; i < n; i++) {
        memmove(x->x_fft_in, x->x_fft_in + 1, (x->fftsize - 1) * sizeof(t_sample));
        memmove(x->x_fft_imag, x->x_fft_imag + 1, (x->fftsize - 1) * sizeof(t_sample));
        x->x_fft_in[x->fftsize - 1] = in[i];
        x->x_fft_imag[x->fftsize - 1] = 0;
        x->accum++;

        if (x->accum >= x->fftsize) {
            for (int j = 0; j < x->fftsize; j++) {
                x->x_fft_in[j] *= x->x_window[j];
            }
            mayer_fft(x->fftsize, x->x_fft_in, x->x_fft_imag);
            int num_bins = x->fftsize / 2 + 1;
            t_sample *fft_data = (t_sample *)getbytes(2 * num_bins * sizeof(t_sample));
            for (int j = 0; j < num_bins; j++) {
                fft_data[2 * j] = x->x_fft_in[j];
                fft_data[2 * j + 1] = x->x_fft_imag[j];
            }
            bool onset_detected = onsetsds_process(x->ODS, (float *)fft_data);
            if (onset_detected) {
                x->onset = 1;
            }
            clock_delay(x->clock, 0);
            freebytes(fft_data, 2 * num_bins * sizeof(t_sample));
            x->accum = 0;
        }
    }
    return (w + 4);
}

// ─────────────────────────────────────
static void onsetsds_tilde_dsp(t_onsetsds_tilde *x, t_signal **sp) {
    x->accum = 0;
    dsp_add(onsetsds_tilde_perform, 3, x, sp[0]->s_vec, sp[0]->s_n);
}

// ─────────────────────────────────────
static void *onsetsds_tilde_new(t_symbol *s, int argc, t_atom *argv) {
    t_onsetsds_tilde *x = (t_onsetsds_tilde *)pd_new(onsetsds_tilde_class);
    argc > 0 ? x->fftsize = atom_getfloat(argv) : 512;
    argc > 1 ? x->medspan = atom_getfloat(argv + 1) : 10;
    x->accum = 0;

    /* Inicializa o OnsetsDS */
    x->onset_type = ODS_ODF_MKL;
    x->ods_data = (float *)getbytes(onsetsds_memneeded(x->onset_type, x->fftsize, x->medspan));
    x->ODS = (OnsetsDS *)getbytes(sizeof(OnsetsDS));
    onsetsds_init(x->ODS, x->ods_data, ODS_FFT_FFTW3_R2C, x->onset_type, x->fftsize, x->medspan,
                  sys_getsr());

    x->clock = clock_new(&x->x_obj, (t_method)onsetsds_tilde_tick);

    logpost(x, 3, "Relax threshold: %f", x->ODS->relaxtime);
    logpost(x, 3, "Floor threshold: %f", x->ODS->floor);
    logpost(x, 3, "Median span: %i", x->ODS->medspan);
    logpost(x, 3, "threshold : %f", (float)x->ODS->thresh);
    logpost(x, 3, "FFT size: %f", (float)x->fftsize);

    x->x_fft_in = (t_sample *)getbytes(x->fftsize * sizeof(t_sample));
    x->x_fft_imag = (t_sample *)getbytes(x->fftsize * sizeof(t_sample));
    memset(x->x_fft_in, 0, x->fftsize * sizeof(t_sample));
    memset(x->x_fft_imag, 0, x->fftsize * sizeof(t_sample));

    x->x_window = (t_sample *)getbytes(x->fftsize * sizeof(t_sample));
    for (int i = 0; i < x->fftsize; i++) {
        x->x_window[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (x->fftsize - 1)));
    }
    x->x_out_bang = outlet_new(&x->x_obj, &s_bang);
    x->x_out_odf = outlet_new(&x->x_obj, &s_float);

    return (void *)x;
}

// ─────────────────────────────────────
static void onsetsds_tilde_free(t_onsetsds_tilde *x) {
    freebytes(x->ods_data, onsetsds_memneeded(x->onset_type, x->fftsize, 10));
    freebytes(x->ODS, sizeof(OnsetsDS));
    freebytes(x->x_fft_in, x->fftsize * sizeof(t_sample));
    freebytes(x->x_fft_imag, x->fftsize * sizeof(t_sample));
    if (x->x_window) {
        freebytes(x->x_window, x->fftsize * sizeof(t_sample));
    }
    clock_free(x->clock);
}

// ─────────────────────────────────────
void onsetsds_tilde_setup(void) {
    onsetsds_tilde_class = class_new(gensym("onsetsds~"), (t_newmethod)onsetsds_tilde_new,
                                     (t_method)onsetsds_tilde_free, sizeof(t_onsetsds_tilde),
                                     CLASS_DEFAULT, A_GIMME, 0);

    CLASS_MAINSIGNALIN(onsetsds_tilde_class, t_onsetsds_tilde, x_sample);
    class_addmethod(onsetsds_tilde_class, (t_method)onsetsds_tilde_dsp, gensym("dsp"), A_CANT, 0);
    class_addmethod(onsetsds_tilde_class, (t_method)onsetsds_tilde_set, gensym("set"), A_GIMME, 0);
}
