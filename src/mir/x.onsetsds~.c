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
    t_sample *x_fft_data;

    t_int onset_type;
    t_int medspan;
    t_float silence_threshold;

    t_sample *x_window;
    int accum;
    int fftsize;

    // OnsetsDS
    OnsetsDS *ODS;
    float *ods_data;
    size_t ods_data_size;
    int onset;

    // Outlets
    t_outlet *x_out_bang, *x_out_odf;
} t_onsetsds_tilde;

// ─────────────────────────────────────
static void onsetsds_tilde_restart(t_onsetsds_tilde *x) {
    if (x->ods_data)
        freebytes(x->ods_data, x->ods_data_size);
    x->ods_data_size = onsetsds_memneeded(x->onset_type, x->fftsize, x->medspan);
    x->ods_data = (float *)getbytes(x->ods_data_size);
    onsetsds_init(x->ODS, x->ods_data, ODS_FFT_FFTW3_R2C, x->onset_type, x->fftsize,
                  x->medspan, sys_getsr());
}

// ─────────────────────────────────────
static void onsetsds_tilde_set(t_onsetsds_tilde *x, t_symbol *s, int argc, t_atom *argv) {
    (void)s;
    if (argc < 2 || argv[0].a_type != A_SYMBOL) {
        pd_error(x, "[x.onsetsds~] set expects a property and a value");
        return;
    }
    const char *method = atom_getsymbol(argv)->s_name;
    if (strcmp(method, "relaxtime") == 0) {
        onsetsds_setrelax(x->ODS, atom_getfloat(argv + 1), x->fftsize);
    } else if (strcmp(method, "floor") == 0) {
        x->ODS->floor = atom_getfloat(argv + 1);
        logpost(x, 3, "Floor: %f", x->ODS->floor);
    } else if (strcmp(method, "threshold") == 0) {
        x->ODS->thresh = atom_getfloat(argv + 1);
        logpost(x, 3, "Threshold: %f", x->ODS->thresh);
    } else if (strcmp(method, "silencethreshold") == 0) {
        const t_float threshold = atom_getfloat(argv + 1);
        if (threshold < 0) {
            pd_error(x, "[x.onsetsds~] silence threshold must be non-negative");
            return;
        }
        x->silence_threshold = threshold;
        logpost(x, 3, "Silence threshold (RMS): %f", x->silence_threshold);
    } else if (strcmp(method, "method") == 0) {
        const int onset_type = atom_getint(argv + 1) - 1;
        if (onset_type < 0 || onset_type > 6) {
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
        x->onset_type = onset_type;
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
    } else {
        pd_error(x, "Unknown method");
    }
}

// Analyze a Pd array offline and output onset positions, in milliseconds, as a list.
static void onsetsds_tilde_array(t_onsetsds_tilde *x, t_symbol *array_name) {
    t_garray *array = (t_garray *)pd_findbyclass(array_name, garray_class);
    int array_size;
    t_word *array_data;

    if (!array) {
        pd_error(x, "[x.onsetsds~] array '%s' not found", array_name->s_name);
        return;
    }
    if (!garray_getfloatwords(array, &array_size, &array_data)) {
        pd_error(x, "[x.onsetsds~] bad template for array '%s'", array_name->s_name);
        return;
    }
    if (x->fftsize < 2) {
        pd_error(x, "[x.onsetsds~] FFT size must be at least 2");
        return;
    }

    const t_float sample_rate = sys_getsr();
    if (sample_rate <= 0) {
        pd_error(x, "[x.onsetsds~] cannot convert samples to milliseconds: invalid sample rate");
        return;
    }

    const int frame_count = array_size > 0 ? ((array_size - 1) / x->fftsize) + 1 : 0;
    t_atom *onset_times =
        frame_count > 0 ? (t_atom *)getbytes((size_t)frame_count * sizeof(t_atom)) : NULL;
    t_sample *fft_real = (t_sample *)getbytes((size_t)x->fftsize * sizeof(t_sample));
    t_sample *fft_imag = (t_sample *)getbytes((size_t)x->fftsize * sizeof(t_sample));
    const int num_bins = x->fftsize / 2 + 1;
    t_sample *fft_data = (t_sample *)getbytes((size_t)(2 * num_bins) * sizeof(t_sample));
    const size_t ods_size = onsetsds_memneeded(x->onset_type, x->fftsize, x->medspan);
    float *ods_data = (float *)getbytes(ods_size);
    OnsetsDS ods;
    int onset_count = 0;

    onsetsds_init(&ods, ods_data, ODS_FFT_FFTW3_R2C, x->onset_type, x->fftsize, x->medspan,
                  sample_rate);

    // Match the current real-time detector settings without disturbing its state.
    onsetsds_setrelax(&ods, x->ODS->relaxtime, x->fftsize);
    ods.floor = x->ODS->floor;
    ods.odfparam = x->ODS->odfparam;
    ods.thresh = x->ODS->thresh;
    ods.whtype = x->ODS->whtype;
    ods.whiten = x->ODS->whiten;
    ods.logmags = x->ODS->logmags;
    ods.mingap = x->ODS->mingap;

    for (int frame = 0; frame < frame_count; frame++) {
        const int frame_start = frame * x->fftsize;
        double energy = 0;

        for (int i = 0; i < x->fftsize; i++) {
            const int sample_index = frame_start + i;
            const t_sample sample =
                sample_index < array_size ? array_data[sample_index].w_float : 0;
            energy += (double)sample * (double)sample;
            fft_real[i] = sample * x->x_window[i];
            fft_imag[i] = 0;
        }

        mayer_fft(x->fftsize, fft_real, fft_imag);
        for (int i = 0; i < num_bins; i++) {
            fft_data[2 * i] = fft_real[i];
            fft_data[2 * i + 1] = fft_imag[i];
        }

        const t_float frame_rms = (t_float)sqrt(energy / (double)x->fftsize);
        const bool onset_detected = onsetsds_process(&ods, (float *)fft_data);
        if (onset_detected && frame_rms > x->silence_threshold) {
            const t_float milliseconds =
                (t_float)((double)frame_start * 1000.0 / (double)sample_rate);
            SETFLOAT(onset_times + onset_count, milliseconds);
            onset_count++;
        }
    }

    outlet_list(x->x_out_bang, &s_list, onset_count, onset_times);

    freebytes(ods_data, ods_size);
    freebytes(fft_data, (size_t)(2 * num_bins) * sizeof(t_sample));
    freebytes(fft_imag, (size_t)x->fftsize * sizeof(t_sample));
    freebytes(fft_real, (size_t)x->fftsize * sizeof(t_sample));
    if (onset_times) {
        freebytes(onset_times, (size_t)frame_count * sizeof(t_atom));
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
        x->x_fft_in[x->accum++] = in[i];

        if (x->accum >= x->fftsize) {
            double energy = 0;
            for (int j = 0; j < x->fftsize; j++) {
                energy += (double)x->x_fft_in[j] * (double)x->x_fft_in[j];
                x->x_fft_in[j] *= x->x_window[j];
                x->x_fft_imag[j] = 0;
            }
            mayer_fft(x->fftsize, x->x_fft_in, x->x_fft_imag);
            int num_bins = x->fftsize / 2 + 1;
            for (int j = 0; j < num_bins; j++) {
                x->x_fft_data[2 * j] = x->x_fft_in[j];
                x->x_fft_data[2 * j + 1] = x->x_fft_imag[j];
            }
            bool onset_detected = onsetsds_process(x->ODS, (float *)x->x_fft_data);
            const t_float frame_rms = (t_float)sqrt(energy / (double)x->fftsize);
            if (onset_detected && frame_rms > x->silence_threshold) {
                x->onset = 1;
            }
            clock_delay(x->clock, 0);
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
    (void)s;
    t_onsetsds_tilde *x = (t_onsetsds_tilde *)pd_new(onsetsds_tilde_class);
    x->fftsize = (argc > 0) ? atom_getint(argv) : 512;
    x->medspan = (argc > 1) ? atom_getint(argv + 1) : 20;
    if (x->fftsize < 2 || (x->fftsize & (x->fftsize - 1)) != 0) {
        pd_error(x, "[x.onsetsds~] FFT size must be a power of two; using 512");
        x->fftsize = 512;
    }
    if (x->medspan < 1) {
        pd_error(x, "[x.onsetsds~] median span must be positive; using 20");
        x->medspan = 20;
    }
    x->accum = 0;
    x->onset = 0;
    x->silence_threshold = 0.0001f;

    /* Inicializa o OnsetsDS */
    x->onset_type = ODS_ODF_MKL;
    x->ods_data_size = onsetsds_memneeded(x->onset_type, x->fftsize, x->medspan);
    x->ods_data = (float *)getbytes(x->ods_data_size);
    x->ODS = (OnsetsDS *)getbytes(sizeof(OnsetsDS));
    onsetsds_init(x->ODS, x->ods_data, ODS_FFT_FFTW3_R2C, x->onset_type, x->fftsize, x->medspan,
                  sys_getsr());

    x->clock = clock_new(&x->x_obj, (t_method)onsetsds_tilde_tick);

    logpost(x, 3, "Relax threshold: %f", x->ODS->relaxtime);
    logpost(x, 3, "Floor threshold: %f", x->ODS->floor);
    logpost(x, 3, "Median span: %i", x->ODS->medspan);
    logpost(x, 3, "threshold : %f", (float)x->ODS->thresh);
    logpost(x, 3, "FFT size: %f", (float)x->fftsize);
    logpost(x, 3, "Silence threshold (RMS): %f", x->silence_threshold);

    x->x_fft_in = (t_sample *)getbytes(x->fftsize * sizeof(t_sample));
    x->x_fft_imag = (t_sample *)getbytes(x->fftsize * sizeof(t_sample));
    x->x_fft_data =
        (t_sample *)getbytes((size_t)(2 * (x->fftsize / 2 + 1)) * sizeof(t_sample));
    memset(x->x_fft_in, 0, x->fftsize * sizeof(t_sample));
    memset(x->x_fft_imag, 0, x->fftsize * sizeof(t_sample));

    x->x_window = (t_sample *)getbytes(x->fftsize * sizeof(t_sample));
    for (int i = 0; i < x->fftsize; i++) {
        x->x_window[i] = 0.5f * (1.0f - cosf(2.0f * PI * i / (x->fftsize - 1)));
    }
    x->x_out_bang = outlet_new(&x->x_obj, &s_bang);
    x->x_out_odf = outlet_new(&x->x_obj, &s_float);

    return (void *)x;
}

// ─────────────────────────────────────
static void onsetsds_tilde_free(t_onsetsds_tilde *x) {
    freebytes(x->ods_data, x->ods_data_size);
    freebytes(x->ODS, sizeof(OnsetsDS));
    freebytes(x->x_fft_in, x->fftsize * sizeof(t_sample));
    freebytes(x->x_fft_imag, x->fftsize * sizeof(t_sample));
    freebytes(x->x_fft_data, (size_t)(2 * (x->fftsize / 2 + 1)) * sizeof(t_sample));
    if (x->x_window) {
        freebytes(x->x_window, x->fftsize * sizeof(t_sample));
    }
    clock_free(x->clock);
}

// ─────────────────────────────────────
void setup_x0x2eonsetsds_tilde(void) {
    onsetsds_tilde_class = class_new(gensym("x.onsetsds~"), (t_newmethod)onsetsds_tilde_new,
                                     (t_method)onsetsds_tilde_free, sizeof(t_onsetsds_tilde),
                                     CLASS_DEFAULT, A_GIMME, 0);

    CLASS_MAINSIGNALIN(onsetsds_tilde_class, t_onsetsds_tilde, x_sample);
    class_addmethod(onsetsds_tilde_class, (t_method)onsetsds_tilde_dsp, gensym("dsp"), A_CANT, 0);
    class_addmethod(onsetsds_tilde_class, (t_method)onsetsds_tilde_set, gensym("set"), A_GIMME, 0);
    class_addmethod(onsetsds_tilde_class, (t_method)onsetsds_tilde_array, gensym("array"), A_SYMBOL,
                    0);
}
