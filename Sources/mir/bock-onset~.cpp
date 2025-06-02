#include <fftw3.h>
#include <m_pd.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FFT_SIZE 2048
#define HOP_SIZE 256
#define PI 3.14159265358979323846

static t_class *bock_tilde_class;

typedef struct _bock_tilde {
    t_object x_obj;
    t_sample x_f;

    // FFTW3 Resources
    fftw_plan fft_plan;
    float *fft_buffer;
    float *window;
    fftw_complex *fft_out;

    // Phase and Magnitude History
    float *prev_mag;
    float *prev_phase1;
    float *prev_phase2;

    // Buffer Management
    unsigned int buffer_pointer;

    // Threshold Control
    t_float threshold;
    t_outlet *detection_out;
    t_outlet *bang_out;

} t_bock_tilde;

// ─────────────────────────────────────
// Phase wrapping function
static float princarg(float phase) { return fmod(phase + PI, 2 * PI) - PI; }

// ─────────────────────────────────────
// DSP Perform Routine
static t_int *bock_tilde_perform(t_int *w) {
    t_bock_tilde *x = (t_bock_tilde *)(w[1]);
    t_sample *in = (t_sample *)(w[2]);
    int n = (int)(w[3]);

    float eta_norm = 0;

    // Buffer incoming samples
    for (int i = 0; i < n; i++) {
        x->fft_buffer[x->buffer_pointer++] = in[i] * x->window[x->buffer_pointer];

        if (x->buffer_pointer == FFT_SIZE) {
            fftw_execute(x->fft_plan);
            float eta = 0.0f;

            // Process each bin
            for (int k = 0; k < FFT_SIZE / 2; k++) {
                float R = x->prev_mag[k];
                float phi = atan2f(x->fft_out[k][1], x->fft_out[k][0]);
                float R_hat = x->prev_mag[k];
                float phi_hat = princarg(2 * x->prev_phase1[k] - x->prev_phase2[k]);
                float real_diff = R_hat * cosf(phi_hat) - x->fft_out[k][0];
                float imag_diff = R_hat * sinf(phi_hat) - x->fft_out[k][1];
                float gamma = sqrtf(real_diff * real_diff + imag_diff * imag_diff);

                eta += gamma;

                // Update history
                x->prev_phase2[k] = x->prev_phase1[k];
                x->prev_phase1[k] = phi;
                x->prev_mag[k] = sqrtf(x->fft_out[k][0] * x->fft_out[k][0] +
                                       x->fft_out[k][1] * x->fft_out[k][1]);
            }

            // Output detection function and check threshold (Equation 11)
            eta_norm = eta / ((float)FFT_SIZE / 2);

            // Reset buffer pointer with overlap
            x->buffer_pointer = FFT_SIZE - HOP_SIZE;
            memmove(x->fft_buffer, x->fft_buffer + HOP_SIZE, x->buffer_pointer * sizeof(float));
        }
    }
    outlet_float(x->detection_out, eta_norm);
    if (eta_norm > x->threshold) {
        outlet_bang(x->bang_out);
    }

    return (w + 4);
}

// ─────────────────────────────────────
// DSP Setup
static void bock_tilde_dsp(t_bock_tilde *x, t_signal **sp) {
    x->buffer_pointer = 0;
    dsp_add(bock_tilde_perform, 3, x, sp[0]->s_vec, sp[0]->s_n);
}

// ─────────────────────────────────────
// Constructor
static void *bock_tilde_new(t_floatarg threshold) {
    t_bock_tilde *x = (t_bock_tilde *)pd_new(bock_tilde_class);

    // Initialize FFT buffers
    x->fft_buffer = (float *)fftw_malloc(FFT_SIZE * sizeof(float));
    x->fft_out = (fftw_complex *)fftw_malloc(sizeof(fftw_complex) * (FFT_SIZE / 2 + 1));
    x->fft_plan = fftw_plan_dft_r2c_1d(FFT_SIZE, (double*)x->fft_buffer, x->fft_out, FFTW_MEASURE);

    // Create Hann window
    x->window = (float *)malloc(FFT_SIZE * sizeof(float));
    for (int i = 0; i < FFT_SIZE; i++) {
        x->window[i] = 0.5f * (1 - cosf(2 * PI * i / (FFT_SIZE - 1)));
    }

    // Allocate history buffers
    x->prev_mag = (float *)calloc(FFT_SIZE / 2, sizeof(float));
    x->prev_phase1 = (float *)calloc(FFT_SIZE / 2, sizeof(float));
    x->prev_phase2 = (float *)calloc(FFT_SIZE / 2, sizeof(float));

    // Create outlets
    x->detection_out = outlet_new(&x->x_obj, &s_float);
    x->bang_out = outlet_new(&x->x_obj, &s_bang);

    // Set default threshold
    x->threshold = (threshold > 0) ? threshold : 0.1f;

    return (void *)x;
}

// ─────────────────────────────────────
// Destructor
static void bock_tilde_free(t_bock_tilde *x) {
    fftw_destroy_plan(x->fft_plan);
    fftw_free(x->fft_buffer);
    fftw_free(x->fft_out);
    free(x->window);
    free(x->prev_mag);
    free(x->prev_phase1);
    free(x->prev_phase2);
}

// ─────────────────────────────────────
// Threshold Control
static void bock_tilde_threshold(t_bock_tilde *x, t_floatarg f) { x->threshold = f; }

// ─────────────────────────────────────
// Setup Function
void bock_tilde_setup(void) {
    bock_tilde_class =
        class_new(gensym("bock.onset~"), (t_newmethod)bock_tilde_new, (t_method)bock_tilde_free,
                  sizeof(t_bock_tilde), CLASS_DEFAULT, A_DEFFLOAT, 0);

    CLASS_MAINSIGNALIN(bock_tilde_class, t_bock_tilde, x_f);
    class_addmethod(bock_tilde_class, (t_method)bock_tilde_dsp, gensym("dsp"), A_CANT, 0);

    class_addmethod(bock_tilde_class, (t_method)bock_tilde_threshold, gensym("threshold"), A_FLOAT,
                    0);
}
