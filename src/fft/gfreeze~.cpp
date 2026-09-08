#include <fftw3.h>
#include <m_pd.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static t_class *gfreeze_class;

typedef struct _gfreeze {
    t_object x_obj;
    t_float x_f;
    t_outlet *x_out;

    // FFT Data
    double *fftIn;
    fftw_complex *fftOut;
    fftw_plan fftPlan;
    fftw_plan ifftPlan;

    int fftSize;
    int halfSize;
    int hopSize;
    int dspBlockSize;

    // OLA Processing Buffers
    float *analysisBuffer;
    float *olaBuffer;
    float *window;
    float *olaNorm;
    fftw_complex *fftTmp;

    // Multi-Frame Polar Memory
    float **framesMag;
    float **framesPhase;
    int numFrames;
    int framesFilled; // Add this line
    int wasFrozen;
    int writeFrame;
    float playFrame;
    float speed;
    int freeze;

    // Synthesis State Buffers
    float *outPhase;
    float *interpMag;
    float *interpPhase;
    int *peakIndex;

    int warnedBlockSize;
} t_gfreeze;

// ─────────────────────────────────────
static inline float wrap_phase(float x) {
    while (x > (float)M_PI)
        x -= 2.0f * (float)M_PI;
    while (x <= -(float)M_PI)
        x += 2.0f * (float)M_PI;
    return x;
}

// ─────────────────────────────────────
static void gfreeze_build_spectrum(t_gfreeze *x, float playFrame, int validFrames, int updatePhase,
                                   fftw_complex *dst) {
    if (validFrames < 1)
        validFrames = 1;

    int i0 = (int)playFrame;
    if (i0 < 0)
        i0 = 0;
    if (i0 >= validFrames)
        i0 = validFrames - 1;
    int i1 = (i0 + 1) % validFrames;
    float frac = playFrame - (float)i0;
    if (frac < 0.0f)
        frac = 0.0f;
    if (frac > 1.0f)
        frac = 1.0f;

    float *mag0 = x->framesMag[i0];
    float *phase0 = x->framesPhase[i0];
    float *mag1 = x->framesMag[i1];
    float *phase1 = x->framesPhase[i1];

    for (int k = 0; k <= x->halfSize; ++k) {
        x->interpMag[k] = mag0[k] * (1.0f - frac) + mag1[k] * frac;
        x->interpPhase[k] = phase0[k] + frac * wrap_phase(phase1[k] - phase0[k]);
        x->peakIndex[k] = k;
    }

    for (int k = 1; k < x->halfSize; ++k) {
        if (x->interpMag[k] > x->interpMag[k - 1] && x->interpMag[k] >= x->interpMag[k + 1]) {
            int j = k - 1;
            while (j >= 0 && x->interpMag[j] < x->interpMag[j + 1]) {
                x->peakIndex[j] = k;
                j--;
            }
            j = k + 1;
            while (j <= x->halfSize && x->interpMag[j] <= x->interpMag[j - 1]) {
                x->peakIndex[j] = k;
                j++;
            }
        }
    }

    if (updatePhase) {
        for (int k = 0; k <= x->halfSize; ++k) {
            if (x->peakIndex[k] == k) {
                float expectedAdvance =
                    (2.0f * (float)M_PI * (float)k * (float)x->hopSize) / (float)x->fftSize;
                float dPhi = wrap_phase(phase1[k] - phase0[k] - expectedAdvance);
                x->outPhase[k] = wrap_phase(x->outPhase[k] + expectedAdvance + (dPhi * x->speed));
            }
        }
    }

    for (int k = 0; k <= x->halfSize; ++k) {
        int p = x->peakIndex[k];
        float lockedPhase = x->outPhase[p] + (x->interpPhase[k] - x->interpPhase[p]);

        dst[k][0] = x->interpMag[k] * cosf(lockedPhase);
        dst[k][1] = x->interpMag[k] * sinf(lockedPhase);
    }
}

// ─────────────────────────────────────
static void gfreeze_free_frames(t_gfreeze *x) {
    if (x->framesMag) {
        for (int f = 0; f < x->numFrames; ++f)
            free(x->framesMag[f]);
        free(x->framesMag);
        x->framesMag = NULL;
    }
    if (x->framesPhase) {
        for (int f = 0; f < x->numFrames; ++f)
            free(x->framesPhase[f]);
        free(x->framesPhase);
        x->framesPhase = NULL;
    }
}

// ─────────────────────────────────────
static int gfreeze_alloc_frames(t_gfreeze *x, int numFrames) {
    float **newMag = (float **)malloc((size_t)numFrames * sizeof(float *));
    float **newPhase = (float **)malloc((size_t)numFrames * sizeof(float *));
    if (!newMag || !newPhase) {
        free(newMag);
        free(newPhase);
        return 0;
    }

    for (int f = 0; f < numFrames; ++f) {
        newMag[f] = (float *)calloc((size_t)(x->halfSize + 1), sizeof(float));
        newPhase[f] = (float *)calloc((size_t)(x->halfSize + 1), sizeof(float));
        if (!newMag[f] || !newPhase[f]) {
            for (int i = 0; i <= f; ++i) {
                free(newMag[i]);
                free(newPhase[i]);
            }
            free(newMag);
            free(newPhase);
            return 0;
        }
    }

    gfreeze_free_frames(x);
    x->framesMag = newMag;
    x->framesPhase = newPhase;
    x->numFrames = numFrames;
    x->writeFrame = 0;
    x->playFrame = 0.0f;

    // Add these reset states
    x->framesFilled = 0;
    x->writeFrame = 0;
    x->playFrame = 0.0f;

    return 1;
}

// ─────────────────────────────────────
static void gfreeze_free_processing(t_gfreeze *x) {
    if (x->analysisBuffer) {
        free(x->analysisBuffer);
        x->analysisBuffer = NULL;
    }
    if (x->olaBuffer) {
        free(x->olaBuffer);
        x->olaBuffer = NULL;
    }
    if (x->window) {
        free(x->window);
        x->window = NULL;
    }
    if (x->olaNorm) {
        free(x->olaNorm);
        x->olaNorm = NULL;
    }
    if (x->fftTmp) {
        fftw_free(x->fftTmp);
        x->fftTmp = NULL;
    }
    if (x->outPhase) {
        free(x->outPhase);
        x->outPhase = NULL;
    }
    if (x->interpMag) {
        free(x->interpMag);
        x->interpMag = NULL;
    }
    if (x->interpPhase) {
        free(x->interpPhase);
        x->interpPhase = NULL;
    }
    if (x->peakIndex) {
        free(x->peakIndex);
        x->peakIndex = NULL;
    }
}

// ─────────────────────────────────────
static int gfreeze_init_processing(t_gfreeze *x) {
    x->analysisBuffer = (float *)calloc((size_t)x->fftSize, sizeof(float));
    x->olaBuffer = (float *)calloc((size_t)x->fftSize, sizeof(float));
    x->window = (float *)malloc((size_t)x->fftSize * sizeof(float));
    x->olaNorm = (float *)calloc((size_t)x->fftSize, sizeof(float));
    x->fftTmp = (fftw_complex *)fftw_alloc_complex((size_t)(x->halfSize + 1));

    x->outPhase = (float *)calloc((size_t)(x->halfSize + 1), sizeof(float));
    x->interpMag = (float *)calloc((size_t)(x->halfSize + 1), sizeof(float));
    x->interpPhase = (float *)calloc((size_t)(x->halfSize + 1), sizeof(float));
    x->peakIndex = (int *)calloc((size_t)(x->halfSize + 1), sizeof(int));

    if (!x->analysisBuffer || !x->olaBuffer || !x->window || !x->olaNorm || !x->fftTmp ||
        !x->outPhase || !x->interpMag || !x->interpPhase || !x->peakIndex) {
        gfreeze_free_processing(x);
        return 0;
    }

    // 75% overlap sqrt-Hann
    for (int i = 0; i < x->fftSize; ++i) {
        const float w = 0.5f - 0.5f * cosf((2.0f * (float)M_PI * (float)i) / (float)x->fftSize);
        x->window[i] = sqrtf((w > 0.0f) ? w : 0.0f);
    }

    for (int i = 0; i < x->fftSize; ++i) {
        float sum = 0.0f;
        for (int offset = 0; offset < x->fftSize; offset += x->hopSize) {
            int idx = i + offset;
            if (idx >= x->fftSize)
                break;
            float w = x->window[idx];
            sum += w * w;
        }
        x->olaNorm[i] = (sum > 1.0e-8f) ? (1.0f / sum) : 0.0f;
    }
    return 1;
}

// ─────────────────────────────────────
static int gfreeze_init_fft(t_gfreeze *x, int fftSize) {
    x->fftSize = fftSize;
    x->halfSize = fftSize / 2;
    x->hopSize = fftSize / 4;

    x->fftIn = (double *)fftw_alloc_real((size_t)x->fftSize);
    x->fftOut = (fftw_complex *)fftw_alloc_complex((size_t)(x->halfSize + 1));
    if (!x->fftIn || !x->fftOut)
        return 0;

    memset(x->fftIn, 0, (size_t)x->fftSize * sizeof(float));
    memset(x->fftOut, 0, (size_t)(x->halfSize + 1) * sizeof(fftw_complex));

    x->fftPlan = fftw_plan_dft_r2c_1d(x->fftSize, x->fftIn, x->fftOut, FFTW_ESTIMATE);
    x->ifftPlan = fftw_plan_dft_c2r_1d(x->fftSize, x->fftOut, x->fftIn, FFTW_ESTIMATE);

    return (x->fftPlan && x->ifftPlan);
}

// ─────────────────────────────────────
static void gfreeze_freeze(t_gfreeze *x, t_floatarg f) { x->freeze = (f != 0.0f); }

// ─────────────────────────────────────
static void gfreeze_speed(t_gfreeze *x, t_floatarg f) { x->speed = (float)f; }

// ─────────────────────────────────────
static void gfreeze_frames(t_gfreeze *x, t_floatarg f) {
    int n = (int)f;
    if (n < 2) {
        n = 2;
    }
    if (n != x->numFrames) {
        if (!gfreeze_alloc_frames(x, n)) {
            pd_error(x, "[gfreeze~] Memory allocation failed for %d frames.", n);
        }
    }
}

// ─────────────────────────────────────
// DSP Vector Processing
// ─────────────────────────────────────
static t_int *gfreeze_perform(t_int *w) {
    t_gfreeze *x = (t_gfreeze *)(w[1]);
    t_float *in = (t_float *)(w[2]);
    t_float *out = (t_float *)(w[3]);
    int n = (int)(w[4]);

    // Bypass if blocksize mismatch
    if (n != x->fftSize || (n % x->hopSize) != 0) {
        for (int i = 0; i < n; ++i)
            out[i] = in[i];
        return (w + 5);
    }

    const int hopsPerBlock = n / x->hopSize;
    const float invFFT = 1.0f / (float)x->fftSize;
    for (int h = 0; h < hopsPerBlock; ++h) {
        const t_float *inHop = in + (h * x->hopSize);
        t_float *outHop = out + (h * x->hopSize);

        // 1. Analysis Windowing & FFT
        memmove(x->analysisBuffer, x->analysisBuffer + x->hopSize,
                (size_t)(x->fftSize - x->hopSize) * sizeof(float));
        memcpy(x->analysisBuffer + (x->fftSize - x->hopSize), inHop,
               (size_t)x->hopSize * sizeof(t_float));

        for (int i = 0; i < x->fftSize; ++i)
            x->fftIn[i] = x->analysisBuffer[i] * x->window[i];
        fftw_execute(x->fftPlan);

        if (!x->freeze) {
            // A. Record to Polar Storage (Must continue to capture input!)
            float *dstMag = x->framesMag[x->writeFrame];
            float *dstPhase = x->framesPhase[x->writeFrame];

            for (int k = 0; k <= x->halfSize; ++k) {
                float re = x->fftOut[k][0];
                float im = x->fftOut[k][1];

                // Record the true spectral data to the history buffer
                dstMag[k] = sqrtf(re * re + im * im);
                dstPhase[k] = atan2f(im, re);

                // Sync the synthesis phase to the live input to prevent phase-clicks upon freeze
                // activation
                x->outPhase[k] = dstPhase[k];

                // MUTE THE FREQUENCY DOMAIN: Enforces 100% wet silent pass-through
                // This allows the OLA window to naturally fade out the previous frozen state
                x->fftOut[k][0] = 0.0f;
                x->fftOut[k][1] = 0.0f;
            }

            // Track how many frames actually contain audio
            if (x->framesFilled < x->numFrames)
                x->framesFilled++;

            x->writeFrame = (x->writeFrame + 1) % x->numFrames;
            x->wasFrozen = 0;
        } else {
            if (!x->wasFrozen) {
                x->playFrame = (float)((x->writeFrame - 1 + x->numFrames) % x->numFrames);
                x->wasFrozen = 1;
            }
            int validFrames = x->framesFilled > 0 ? x->framesFilled : 1;
            float playFramePos = x->playFrame;
            float loopBlend = 0.0f;
            float playFrameB = 0.0f;
            int xfadeFrames = 0;

            if (validFrames > 1) {
                xfadeFrames = validFrames / 4;
                if (xfadeFrames < 1)
                    xfadeFrames = 1;
                if (xfadeFrames > (validFrames / 2))
                    xfadeFrames = validFrames / 2;

                if (x->speed >= 0.0f) {
                    float xfadeStart = (float)(validFrames - xfadeFrames);
                    if (playFramePos >= xfadeStart) {
                        loopBlend = (playFramePos - xfadeStart) / (float)xfadeFrames;
                        playFrameB = playFramePos - xfadeStart;
                    }
                } else {
                    float xfadeEnd = (float)xfadeFrames;
                    if (playFramePos < xfadeEnd) {
                        loopBlend = (xfadeEnd - playFramePos) / (float)xfadeFrames;
                        playFrameB = (float)(validFrames - xfadeFrames) + playFramePos;
                    }
                }
            }

            if (loopBlend > 0.0f && xfadeFrames > 0) {
                gfreeze_build_spectrum(x, playFramePos, validFrames, 1, x->fftTmp);
                gfreeze_build_spectrum(x, playFrameB, validFrames, 0, x->fftOut);

                float a = 1.0f - loopBlend;
                float b = loopBlend;
                for (int k = 0; k <= x->halfSize; ++k) {
                    x->fftOut[k][0] = (x->fftTmp[k][0] * a) + (x->fftOut[k][0] * b);
                    x->fftOut[k][1] = (x->fftTmp[k][1] * a) + (x->fftOut[k][1] * b);
                }
            } else {
                gfreeze_build_spectrum(x, playFramePos, validFrames, 1, x->fftOut);
            }

            x->playFrame += x->speed;
            while (x->playFrame >= (float)validFrames)
                x->playFrame -= (float)validFrames;
            while (x->playFrame < 0.0f)
                x->playFrame += (float)validFrames;
        }

        // 3. IFFT & OLA
        fftw_execute(x->ifftPlan);
        for (int i = 0; i < x->fftSize; ++i)
            x->olaBuffer[i] += (x->fftIn[i] * invFFT) * x->window[i];
        for (int i = 0; i < x->hopSize; ++i)
            outHop[i] = x->olaBuffer[i] * x->olaNorm[i];

        memmove(x->olaBuffer, x->olaBuffer + x->hopSize,
                (size_t)(x->fftSize - x->hopSize) * sizeof(float));
        memset(x->olaBuffer + (x->fftSize - x->hopSize), 0, (size_t)x->hopSize * sizeof(float));
    }
    return (w + 5);
}

// ─────────────────────────────────────
static void gfreeze_dsp(t_gfreeze *x, t_signal **sp) {
    x->dspBlockSize = sp[0]->s_n;
    if ((x->dspBlockSize != x->fftSize) && !x->warnedBlockSize) {
        post("[gfreeze~] warning: Pd block size (%d) must equal fftSize (%d) for optimal "
             "performance.",
             x->dspBlockSize, x->fftSize);
        x->warnedBlockSize = 1;
    }
    dsp_add(gfreeze_perform, 4, x, sp[0]->s_vec, sp[1]->s_vec, sp[0]->s_n);
}

// ─────────────────────────────────────
static void *gfreeze_new(t_symbol *s, int argc, t_atom *argv) {
    (void)s;
    t_gfreeze *x = (t_gfreeze *)pd_new(gfreeze_class);
    if (!x)
        return NULL;

    x->x_f = 0.0f;
    x->fftIn = NULL;
    x->fftOut = NULL;
    x->fftPlan = NULL;
    x->ifftPlan = NULL;
    x->analysisBuffer = NULL;
    x->olaBuffer = NULL;
    x->window = NULL;
    x->olaNorm = NULL;
    x->fftTmp = NULL;
    x->framesMag = NULL;
    x->framesPhase = NULL;
    x->outPhase = NULL;
    x->interpMag = NULL;
    x->interpPhase = NULL;
    x->peakIndex = NULL;

    x->fftSize = 2048;
    x->numFrames = 16; // Default 8 frames
    x->speed = 0.95;   // Default micro-loop scrub speed
    x->freeze = 0;
    x->warnedBlockSize = 0;

    if (argc > 0 && argv[0].a_type == A_FLOAT) {
        int reqFFT = (int)atom_getfloat(argv);
        if (reqFFT >= 64)
            x->fftSize = reqFFT;
    }

    if (!gfreeze_init_fft(x, x->fftSize)) {
        pd_error(x, "[gfreeze~] FFT init failed");
        return NULL;
    }
    if (!gfreeze_init_processing(x)) {
        pd_error(x, "[gfreeze~] Processing init failed");
        return NULL;
    }
    if (!gfreeze_alloc_frames(x, x->numFrames)) {
        pd_error(x, "[gfreeze~] Frame alloc failed");
        return NULL;
    }

    x->x_out = outlet_new(&x->x_obj, &s_signal);
    return x;
}

// ─────────────────────────────────────
static void gfreeze_free(t_gfreeze *x) {
    if (x->fftPlan)
        fftw_destroy_plan(x->fftPlan);
    if (x->ifftPlan)
        fftw_destroy_plan(x->ifftPlan);
    if (x->fftIn)
        fftw_free(x->fftIn);
    if (x->fftOut)
        fftw_free(x->fftOut);
    gfreeze_free_processing(x);
    gfreeze_free_frames(x);
}

// ─────────────────────────────────────
extern "C" void gfreeze_tilde_setup(void) {
    gfreeze_class = class_new(gensym("gfreeze~"), (t_newmethod)gfreeze_new, (t_method)gfreeze_free,
                              sizeof(t_gfreeze), CLASS_DEFAULT, A_GIMME, 0);

    CLASS_MAINSIGNALIN(gfreeze_class, t_gfreeze, x_f);
    class_addmethod(gfreeze_class, (t_method)gfreeze_dsp, gensym("dsp"), A_CANT, 0);
    class_addmethod(gfreeze_class, (t_method)gfreeze_freeze, gensym("freeze"), A_FLOAT, 0);
    class_addmethod(gfreeze_class, (t_method)gfreeze_speed, gensym("speed"), A_FLOAT, 0);
    class_addmethod(gfreeze_class, (t_method)gfreeze_frames, gensym("frames"), A_FLOAT, 0);
}
