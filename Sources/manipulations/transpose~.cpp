#include "m_pd.h"
#include <math.h>
#include <fftw3.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>

typedef struct _transposer {
    t_object    x_obj;
    t_outlet   *x_out;
    float *FFTIn;    // real-valued input buffer
    fftwf_complex *FFTOut; // complex FFT output buffer (only 0..N/2 unique bins)
    fftwf_plan   FFTPlan;   // forward FFT plan (real -> complex)
    fftwf_plan   IFFTPlan;  // inverse FFT plan (complex -> real)
    int         fftSize;   // FFT size (e.g., 1024)
    t_sample pitchScalar; // pitch scaling factor (e.g., 2^(50/1200) for 50 cents)
    float       freqShift;   // additional frequency shift in Hz
    int         clip;        // clipping flag (nonzero: clip out-of-range bins)
} t_transposer;

static t_class *transposer_class;

static t_int *transposer_perform(t_int *w) {
    t_transposer *x = (t_transposer *)(w[1]);
    t_float *in = (t_float *)(w[2]);    // input signal (time domain)
    t_float *out = (t_float *)(w[3]);     // output signal (time domain)
    int n = (int)(w[4]);                  // block size (must equal fftSize)
    float sr = sys_getsr();

    // If the block size doesn't match our FFT size, pass audio through.
    if(n != x->fftSize) {
        for (int i = 0; i < n; i++)
            out[i] = in[i];
        return (w + 5);
    }

    // Copy input into FFT input buffer.
    std::copy(in, in + n, x->FFTIn);

    // Execute forward FFT: real -> complex.
    fftwf_execute(x->FFTPlan);

    int halfSize = x->fftSize / 2;
    // Allocate temporary buffer for the new (shifted) FFT bins.
    fftwf_complex *newFFT = (fftwf_complex *)fftwf_alloc_complex(halfSize + 1);
    for (int i = 0; i <= halfSize; i++) {
        newFFT[i][0] = 0.0;
        newFFT[i][1] = 0.0;
    }

    /* For each unique FFT bin (0 .. fftSize/2):
       - Compute its center frequency: origFreq = i * sr / fftSize.
       - Apply pitch scaling and frequency offset:
             newFreq = origFreq * pitchScalar + freqShift.
       - Convert newFreq back to a (possibly fractional) bin index:
             newBin = newFreq * fftSize / sr.
       - If clipping is enabled, force newBin into [0, halfSize].
         Otherwise, if newBin falls outside [0, halfSize], ignore this bin.
       - Distribute the original bin's complex amplitude to the two
         nearest bins (linear interpolation).
    */
    for (int i = 0; i <= halfSize; i++) {
        double origFreq = (i * sr) / x->fftSize;
        double newFreq = origFreq * x->pitchScalar + x->freqShift;
        double newBin = newFreq * x->fftSize / sr;

        if (x->clip) {
            if (newBin < 0)
                newBin = 0;
            if (newBin > halfSize)
                newBin = halfSize;
        } else {
            if (newBin < 0 || newBin > halfSize)
                continue;  // skip bins that fall outside the valid range
        }

        int lower = (int)floor(newBin);
        int upper = lower + 1;
        double frac = newBin - lower;

        newFFT[lower][0] += (1.0 - frac) * x->FFTOut[i][0];
        newFFT[lower][1] += (1.0 - frac) * x->FFTOut[i][1];
        if (upper <= halfSize) {
            newFFT[upper][0] += frac * x->FFTOut[i][0];
            newFFT[upper][1] += frac * x->FFTOut[i][1];
        }
    }

    // Copy the shifted FFT bins back into our FFT output buffer.
    for (int i = 0; i <= halfSize; i++) {
        x->FFTOut[i][0] = newFFT[i][0];
        x->FFTOut[i][1] = newFFT[i][1];
    }
    fftwf_free(newFFT);

    // Execute inverse FFT: complex -> real.
    fftwf_execute(x->IFFTPlan);

    // Normalize the output (fftwf does not normalize automatically).
    for (int i = 0; i < n; i++) {
        out[i] = (t_float)(x->FFTIn[i] / x->fftSize);
    }
    return (w + 5);
}

static void transposer_dsp(t_transposer *x, t_signal **sp) {
    // We expect one inlet (time-domain input) and one outlet (time-domain output).
    // The block size (sp[0]->s_n) must equal our FFT size.
    dsp_add(transposer_perform, 4, x, sp[0]->s_vec, sp[1]->s_vec, sp[0]->s_n);
}

// Message to set the pitch scaling factor.
static void transposer_pitch(t_transposer *x, t_floatarg f) {
    x->pitchScalar = f;
}

// Message to set the frequency offset (Hz).
static void transposer_freqshift(t_transposer *x, t_floatarg f) {
    x->freqShift = f;
}

// Message to set the clip flag (nonzero to enable clipping).
static void transposer_clip(t_transposer *x, t_floatarg f) {
    x->clip = (f != 0);
}

static void *transposer_new(t_symbol *s, int argc, t_atom *argv) {
    t_transposer *x = (t_transposer *)pd_new(transposer_class);
    x->fftSize = 1024;           // default FFT size
    x->pitchScalar = 1.0;        // default: no pitch shift
    x->freqShift = 50.0;          // no additional frequency offset
    x->clip = 0;                 // default: folding behavior (no clipping)

    // If an argument is provided, use it as the default pitch scalar.
    if (argc > 0 && (argv[0].a_type == A_FLOAT || argv[0].a_type == A_DEFFLOAT))
        x->pitchScalar = atom_getfloat(argv);

    // Allocate fftwf buffers.
    x->FFTIn = (float *)fftwf_alloc_real(x->fftSize);
    if (!x->FFTIn) {
        pd_error(x, "[transposer~] fftwf_alloc_real failed");
        return NULL;
    }
    int halfSize = x->fftSize / 2;
    x->FFTOut = (fftwf_complex *)fftwf_alloc_complex(halfSize + 1);
    if (!x->FFTOut) {
        fftwf_free(x->FFTIn);
        pd_error(x, "[transposer~] fftwf_alloc_complex failed");
        return NULL;
    }
    // Create fftwf plans for forward (real→complex) and inverse (complex→real) transforms.
    x->FFTPlan = fftwf_plan_dft_r2c_1d(x->fftSize, x->FFTIn, x->FFTOut, FFTW_ESTIMATE);
    x->IFFTPlan = fftwf_plan_dft_c2r_1d(x->fftSize, x->FFTOut, x->FFTIn, FFTW_ESTIMATE);

    // Create a signal inlet (the first inlet is automatically created)
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    // Create a signal outlet.
    x->x_out = outlet_new(&x->x_obj, &s_signal);

    return (x);
}

static void transposer_free(t_transposer *x) {
    if (x->FFTPlan)
        fftwf_destroy_plan(x->FFTPlan);
    if (x->IFFTPlan)
        fftwf_destroy_plan(x->IFFTPlan);
    if (x->FFTIn)
        fftwf_free(x->FFTIn);
    if (x->FFTOut)
        fftwf_free(x->FFTOut);
}

void transposer_tilde_setup(void) {
    transposer_class = class_new(gensym("transposer~"),
                                 (t_newmethod)transposer_new,
                                 (t_method)transposer_free,
                                 sizeof(t_transposer),
                                 CLASS_DEFAULT,
                                 A_GIMME, 0);
    class_addmethod(transposer_class, (t_method)transposer_dsp, gensym("dsp"), A_CANT, 0);
    class_addmethod(transposer_class, (t_method)transposer_pitch, gensym("pitch"), A_FLOAT, 0);
    class_addmethod(transposer_class, (t_method)transposer_freqshift, gensym("freqshift"), A_FLOAT, 0);
    class_addmethod(transposer_class, (t_method)transposer_clip, gensym("clip"), A_FLOAT, 0);
    CLASS_MAINSIGNALIN(transposer_class, t_transposer, pitchScalar);
}

