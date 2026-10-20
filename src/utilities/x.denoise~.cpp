#include "m_pd.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#define FFT_SIZE 4096
#define HOP_SIZE (FFT_SIZE / 4)
#define NUM_BINS (FFT_SIZE / 2 + 1)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct _xdenoise {
    t_object x_obj;

    // DSP state
    t_float x_sr;
    int x_input_pos;
    int x_hop_counter;
    int x_output_pos;

    // Circular input buffer
    t_float x_input[FFT_SIZE];
    t_float x_output[FFT_SIZE];

    // FFT buffers
    t_float x_real[FFT_SIZE];
    t_float x_imag[FFT_SIZE];

    // Hann window
    t_float x_window[FFT_SIZE];
    t_float x_noise[NUM_BINS];
    t_float x_gain[NUM_BINS];
    t_float x_reduction;
    t_float x_floor;
    t_float x_learn_smooth;
    t_float x_gain_smooth;
    int x_learning;
} t_xdenoise;

static t_class *xdenoise_class;

// ─────────────────────────────────────
static inline t_float xdenoise_clip(t_float value, t_float minimum, t_float maximum) {
    if (value < minimum)
        return minimum;

    if (value > maximum)
        return maximum;

    return value;
}

// ─────────────────────────────────────
static void xdenoise_make_window(t_xdenoise *x) {
    for (int i = 0; i < FFT_SIZE; ++i) {
        x->x_window[i] = 0.5f - 0.5f * cosf(2.0f * (t_float)M_PI * (t_float)i / (t_float)FFT_SIZE);
    }
}

// ─────────────────────────────────────
// Clear DSP buffers
static void xdenoise_clear(t_xdenoise *x) {
    memset(x->x_input, 0, sizeof(x->x_input));
    memset(x->x_output, 0, sizeof(x->x_output));

    memset(x->x_real, 0, sizeof(x->x_real));
    memset(x->x_imag, 0, sizeof(x->x_imag));

    x->x_input_pos = 0;
    x->x_output_pos = 0;
    x->x_hop_counter = 0;
}

// ─────────────────────────────────────
// Clear learned noise profile
static void xdenoise_clearprofile(t_xdenoise *x) {
    memset(x->x_noise, 0, sizeof(x->x_noise));
    for (int i = 0; i < NUM_BINS; ++i)
        x->x_gain[i] = 1.0f;
}

// ─────────────────────────────────────
// Process one FFT frame
static void xdenoise_process_frame(t_xdenoise *x) {
    for (int i = 0; i < FFT_SIZE; ++i) {
        int index = x->x_input_pos + i;
        if (index >= FFT_SIZE)
            index -= FFT_SIZE;
        x->x_real[i] = x->x_input[index] * x->x_window[i];
        x->x_imag[i] = 0.0f;
    }

    // Forward FFT
    mayer_fft(FFT_SIZE, x->x_real, x->x_imag);

    // Spectral processing
    for (int k = 0; k < NUM_BINS; ++k) {
        t_float real = x->x_real[k];
        t_float imag = x->x_imag[k];
        t_float magnitude = sqrtf(real * real + imag * imag);

        // Learn noise profile
        if (x->x_learning) {
            t_float old_noise = x->x_noise[k];
            if (old_noise <= 0.0f) {
                x->x_noise[k] = magnitude;
            } else {
                x->x_noise[k] =
                    x->x_learn_smooth * old_noise + (1.0f - x->x_learn_smooth) * magnitude;
            }
        }

        // Spectral subtraction
        t_float gain = 1.0f;
        if (!x->x_learning && x->x_noise[k] > 0.0f && magnitude > 1.0e-12f) {
            gain = 1.0f - x->x_reduction * x->x_noise[k] / (magnitude + 1.0e-12f);
            gain = xdenoise_clip(gain, x->x_floor, 1.0f);
        }

        // Temporal gain smoothing
        t_float smooth_gain = x->x_gain_smooth * x->x_gain[k] + (1.0f - x->x_gain_smooth) * gain;
        x->x_gain[k] = smooth_gain;
        x->x_real[k] *= smooth_gain;
        x->x_imag[k] *= smooth_gain;

        if (k > 0 && k < FFT_SIZE / 2) {
            int mirror = FFT_SIZE - k;
            x->x_real[mirror] = x->x_real[k];
            x->x_imag[mirror] = -x->x_imag[k];
        }
    }

    // DC and Nyquist must have zero imaginary component.
    x->x_imag[0] = 0.0f;
    x->x_imag[FFT_SIZE / 2] = 0.0f;

    // Inverse FFT
    mayer_ifft(FFT_SIZE, x->x_real, x->x_imag);
    const t_float scale = 1.0f / ((t_float)FFT_SIZE * 1.5f);
    for (int i = 0; i < FFT_SIZE; ++i) {
        int index = x->x_output_pos + i;
        if (index >= FFT_SIZE)
            index -= FFT_SIZE;
        x->x_output[index] += x->x_real[i] * x->x_window[i] * scale;
    }
}

// ─────────────────────────────────────
static void xdenoise_learn(t_xdenoise *x, t_floatarg value) {
    x->x_learning = value != 0;
    post("x.denoise~: noise learning %s", x->x_learning ? "ON" : "OFF");
}

// ─────────────────────────────────────
static void xdenoise_reduction(t_xdenoise *x, t_floatarg value) {
    if (value < 0.0f)
        value = 0.0f;
    x->x_reduction = value;
    post("x.denoise~: reduction = %.3f", value);
}

// ─────────────────────────────────────
static void xdenoise_floor(t_xdenoise *x, t_floatarg value) {
    x->x_floor = xdenoise_clip(value, 0.0f, 1.0f);
    post("x.denoise~: floor = %.3f", x->x_floor);
}

// ─────────────────────────────────────
static void xdenoise_smooth(t_xdenoise *x, t_floatarg value) {
    x->x_gain_smooth = xdenoise_clip(value, 0.0f, 0.9999f);
    post("x.denoise~: gain smoothing = %.4f", x->x_gain_smooth);
}

// ─────────────────────────────────────
static void xdenoise_learnsmooth(t_xdenoise *x, t_floatarg value) {
    x->x_learn_smooth = xdenoise_clip(value, 0.0f, 0.99999f);
    post("x.denoise~: learning smoothing = %.5f", x->x_learn_smooth);
}

// ─────────────────────────────────────
static void xdenoise_reset(t_xdenoise *x) {
    xdenoise_clearprofile(x);
    post("x.denoise~: noise profile cleared");
}

// DSP
static t_int *xdenoise_perform(t_int *w) {
    t_xdenoise *x = (t_xdenoise *)(w[1]);
    t_sample *in = (t_sample *)(w[2]);
    t_sample *out = (t_sample *)(w[3]);
    int n = (int)(w[4]);

    while (n--) {
        x->x_input[x->x_input_pos] = *in++;
        ++x->x_input_pos;
        if (x->x_input_pos >= FFT_SIZE)
            x->x_input_pos = 0;

        *out++ = x->x_output[x->x_output_pos];
        x->x_output[x->x_output_pos] = 0.0f;
        ++x->x_output_pos;
        if (x->x_output_pos >= FFT_SIZE)
            x->x_output_pos = 0;

        ++x->x_hop_counter;
        if (x->x_hop_counter >= HOP_SIZE) {
            x->x_hop_counter = 0;
            xdenoise_process_frame(x);
        }
    }
    return w + 5;
}

// ─────────────────────────────────────
static void xdenoise_dsp(t_xdenoise *x, t_signal **sp) {
    x->x_sr = sp[0]->s_sr;
    xdenoise_clear(x);
    dsp_add(xdenoise_perform, 4, x, sp[0]->s_vec, sp[1]->s_vec, sp[0]->s_n);
}

// Constructor
static void *xdenoise_new(t_symbol *s, int ac, t_atom *av) {
    t_xdenoise *x = (t_xdenoise *)pd_new(xdenoise_class);
    x->x_sr = sys_getsr();
    x->x_reduction = 1.5f;
    x->x_floor = 0.05f;
    x->x_learn_smooth = 0.98f;
    x->x_gain_smooth = 0.80f;
    x->x_learning = 0;

    xdenoise_make_window(x);
    xdenoise_clear(x);
    xdenoise_clearprofile(x);

    if (ac > 0 && av[0].a_type == A_FLOAT) {
        t_float value = atom_getfloat(av);
        if (value >= 0.0f)
            x->x_reduction = value;
    }

    // Signal outlet
    outlet_new(&x->x_obj, &s_signal);
    return x;
}

// Setup
extern "C" void setup_x0x2edenoise_tilde(void) {
    xdenoise_class = class_new(gensym("x.denoise~"), (t_newmethod)xdenoise_new, 0,
                               sizeof(t_xdenoise), CLASS_DEFAULT, A_GIMME, 0);

    // Signal inlet
    CLASS_MAINSIGNALIN(xdenoise_class, t_xdenoise, x_sr);
    class_addmethod(xdenoise_class, (t_method)xdenoise_dsp, gensym("dsp"), A_CANT, 0);
    class_addmethod(xdenoise_class, (t_method)xdenoise_learn, gensym("learn"), A_FLOAT, 0);
    class_addmethod(xdenoise_class, (t_method)xdenoise_reduction, gensym("reduction"), A_FLOAT, 0);
    class_addmethod(xdenoise_class, (t_method)xdenoise_floor, gensym("floor"), A_FLOAT, 0);
    class_addmethod(xdenoise_class, (t_method)xdenoise_smooth, gensym("smooth"), A_FLOAT, 0);
    class_addmethod(xdenoise_class, (t_method)xdenoise_learnsmooth, gensym("learnsmooth"), A_FLOAT,
                    0);
    class_addmethod(xdenoise_class, (t_method)xdenoise_reset, gensym("reset"), A_NULL, 0);
}
