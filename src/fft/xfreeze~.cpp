#include <fftw3.h>
#include <m_pd.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static t_class *xfreeze_class;

typedef struct _xconv {
    t_object x_obj;
    t_float x_f;
    t_outlet *x_out;

    double *fftIn;
    fftw_complex *fftOut;
    fftw_plan fftPlan;
    fftw_plan ifftPlan;

    int fftSize;
    int halfSize;
    int hopSize;
    int dspBlockSize;

    float **framesRe;
    float **framesIm;

    int numFrames;
    int framesFilled;
    int writeFrame;
    int playFrame;

    float interp;
    float speed;
    int freeze;
    int phaseLock;

    float *analysisBuffer;
    float *olaBuffer;
    float *window;
    float *rotStepRe;
    float *rotStepIm;
    float *rotAccumRe;
    float *rotAccumIm;
    float *magBuffer;
    int *peakMap;
    int *peakBins;
    int rotAccumReady;

    t_symbol *matrixName;
    t_garray *matrixArray;
    t_word *matrixVec;
    int matrixSize;

    int warnedBlockSize;
} t_xfreeze;

// ─────────────────────────────────────
static int xfreeze_matrix_stride(const t_xfreeze *x) { return (x->halfSize + 1) * 2; }

// ─────────────────────────────────────
static int xfreeze_matrix_size(const t_xfreeze *x) {
    return x->numFrames * xfreeze_matrix_stride(x);
}

// ─────────────────────────────────────
static int xfreeze_matrix_ready(const t_xfreeze *x) {
    const int needed = xfreeze_matrix_size(x);
    return (x->matrixVec && x->matrixSize >= needed);
}

// ─────────────────────────────────────
static void xfreeze_matrix_unbind(t_xfreeze *x) {
    x->matrixArray = NULL;
    x->matrixVec = NULL;
    x->matrixSize = 0;
}

// ─────────────────────────────────────
static int xfreeze_matrix_bind(t_xfreeze *x, t_symbol *name) {
    x->matrixName = name;
    if (!name || name == &s_) {
        xfreeze_matrix_unbind(x);
        return 0;
    }

    t_garray *array = (t_garray *)pd_findbyclass(name, garray_class);
    if (!array) {
        xfreeze_matrix_unbind(x);
        return 0;
    }

    const int needed = xfreeze_matrix_size(x);
    garray_resize_long(array, needed);

    int vecsize = 0;
    t_word *vec = NULL;
    if (!garray_getfloatwords(array, &vecsize, &vec) || !vec || vecsize < needed) {
        xfreeze_matrix_unbind(x);
        return 0;
    }

    x->matrixArray = array;
    x->matrixVec = vec;
    x->matrixSize = vecsize;
    garray_redraw(array);
    return 1;
}

// ─────────────────────────────────────
static void xfreeze_matrix_clear(t_xfreeze *x) {
    if (!xfreeze_matrix_ready(x))
        return;

    const int total = xfreeze_matrix_size(x);
    for (int i = 0; i < total; ++i)
        x->matrixVec[i].w_float = 0.0f;

    if (x->matrixArray)
        garray_redraw(x->matrixArray);
}

// ─────────────────────────────────────
static void xfreeze_reset_rotaccum(t_xfreeze *x) {
    if (!x->rotAccumRe || !x->rotAccumIm)
        return;

    for (int k = 0; k <= x->halfSize; ++k) {
        x->rotAccumRe[k] = 1.0f;
        x->rotAccumIm[k] = 0.0f;
    }
    x->rotAccumReady = 1;
}

// ─────────────────────────────────────
static void xfreeze_compute_peak_map(t_xfreeze *x) {
    if (!x->magBuffer || !x->peakMap || !x->peakBins)
        return;

    const int half = x->halfSize;
    int peakCount = 0;
    x->peakBins[peakCount++] = 0;

    for (int k = 1; k < half; ++k) {
        const float m0 = x->magBuffer[k - 1];
        const float m1 = x->magBuffer[k];
        const float m2 = x->magBuffer[k + 1];
        if (m1 >= m0 && m1 > m2)
            x->peakBins[peakCount++] = k;
    }

    if (half > 0)
        x->peakBins[peakCount++] = half;

    int left = 0;
    for (int i = 0; i < peakCount; ++i) {
        const int peak = x->peakBins[i];
        const int right = (i == peakCount - 1) ? half : (peak + x->peakBins[i + 1]) / 2;
        for (int k = left; k <= right; ++k)
            x->peakMap[k] = peak;
        left = right + 1;
    }

    for (int k = left; k <= half; ++k)
        x->peakMap[k] = x->peakBins[peakCount - 1];
}

// ─────────────────────────────────────
static void xfreeze_clear_frames(t_xfreeze *x) {
    if (x->framesRe && x->framesIm) {
        for (int f = 0; f < x->numFrames; ++f) {
            memset(x->framesRe[f], 0, (size_t)(x->halfSize + 1) * sizeof(float));
            memset(x->framesIm[f], 0, (size_t)(x->halfSize + 1) * sizeof(float));
        }
    }

    xfreeze_matrix_clear(x);

    x->framesFilled = 0;
    x->writeFrame = 0;
    x->playFrame = 0;
    x->interp = 0.0f;
    x->rotAccumReady = 0;
    xfreeze_reset_rotaccum(x);
}

// ─────────────────────────────────────
static void xfreeze_free_frames(t_xfreeze *x) {
    if (x->framesRe) {
        for (int f = 0; f < x->numFrames; ++f) {
            free(x->framesRe[f]);
        }
        free(x->framesRe);
        x->framesRe = NULL;
    }

    if (x->framesIm) {
        for (int f = 0; f < x->numFrames; ++f) {
            free(x->framesIm[f]);
        }
        free(x->framesIm);
        x->framesIm = NULL;
    }
}

// ─────────────────────────────────────
static int xfreeze_alloc_frames(t_xfreeze *x, int numFrames) {
    float **newRe = (float **)malloc((size_t)numFrames * sizeof(float *));
    float **newIm = (float **)malloc((size_t)numFrames * sizeof(float *));
    if (!newRe || !newIm) {
        free(newRe);
        free(newIm);
        return 0;
    }

    for (int f = 0; f < numFrames; ++f) {
        newRe[f] = (float *)calloc((size_t)(x->halfSize + 1), sizeof(float));
        newIm[f] = (float *)calloc((size_t)(x->halfSize + 1), sizeof(float));
        if (!newRe[f] || !newIm[f]) {
            for (int i = 0; i <= f; ++i) {
                free(newRe[i]);
                free(newIm[i]);
            }
            free(newRe);
            free(newIm);
            return 0;
        }
    }

    xfreeze_free_frames(x);
    x->framesRe = newRe;
    x->framesIm = newIm;
    x->numFrames = numFrames;
    if (x->matrixName)
        xfreeze_matrix_bind(x, x->matrixName);
    xfreeze_clear_frames(x);
    return 1;
}

// ─────────────────────────────────────
static int xfreeze_init_fft(t_xfreeze *x, int fftSize) {
    x->fftSize = fftSize;
    x->halfSize = fftSize / 2;
    x->hopSize = fftSize / 4;

    if (x->hopSize < 1)
        x->hopSize = 1;

    x->fftIn = (double *)fftw_alloc_real((size_t)x->fftSize);
    x->fftOut = (fftw_complex *)fftw_alloc_complex((size_t)(x->halfSize + 1));
    if (!x->fftIn || !x->fftOut) {
        if (x->fftIn)
            fftw_free(x->fftIn);
        if (x->fftOut)
            fftw_free(x->fftOut);
        x->fftIn = NULL;
        x->fftOut = NULL;
        return 0;
    }

    memset(x->fftIn, 0, (size_t)x->fftSize * sizeof(float));
    memset(x->fftOut, 0, (size_t)(x->halfSize + 1) * sizeof(fftw_complex));

    x->fftPlan = fftw_plan_dft_r2c_1d(x->fftSize, x->fftIn, x->fftOut, FFTW_ESTIMATE);
    x->ifftPlan = fftw_plan_dft_c2r_1d(x->fftSize, x->fftOut, x->fftIn, FFTW_ESTIMATE);
    if (!x->fftPlan || !x->ifftPlan) {
        if (x->fftPlan)
            fftw_destroy_plan(x->fftPlan);
        if (x->ifftPlan)
            fftw_destroy_plan(x->ifftPlan);
        fftw_free(x->fftIn);
        fftw_free(x->fftOut);
        x->fftPlan = NULL;
        x->ifftPlan = NULL;
        x->fftIn = NULL;
        x->fftOut = NULL;
        return 0;
    }

    return 1;
}

// ─────────────────────────────────────
static void xfreeze_free_processing(t_xfreeze *x) {
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
    if (x->rotStepRe) {
        free(x->rotStepRe);
        x->rotStepRe = NULL;
    }
    if (x->rotStepIm) {
        free(x->rotStepIm);
        x->rotStepIm = NULL;
    }
    if (x->rotAccumRe) {
        free(x->rotAccumRe);
        x->rotAccumRe = NULL;
    }
    if (x->rotAccumIm) {
        free(x->rotAccumIm);
        x->rotAccumIm = NULL;
    }
    if (x->magBuffer) {
        free(x->magBuffer);
        x->magBuffer = NULL;
    }
    if (x->peakMap) {
        free(x->peakMap);
        x->peakMap = NULL;
    }
    if (x->peakBins) {
        free(x->peakBins);
        x->peakBins = NULL;
    }
}

// ─────────────────────────────────────
static int xfreeze_init_processing(t_xfreeze *x) {
    x->analysisBuffer = (float *)calloc((size_t)x->fftSize, sizeof(float));
    x->olaBuffer = (float *)calloc((size_t)x->fftSize, sizeof(float));
    x->window = (float *)malloc((size_t)x->fftSize * sizeof(float));
    x->rotStepRe = (float *)malloc((size_t)(x->halfSize + 1) * sizeof(float));
    x->rotStepIm = (float *)malloc((size_t)(x->halfSize + 1) * sizeof(float));
    x->rotAccumRe = (float *)malloc((size_t)(x->halfSize + 1) * sizeof(float));
    x->rotAccumIm = (float *)malloc((size_t)(x->halfSize + 1) * sizeof(float));
    x->magBuffer = (float *)malloc((size_t)(x->halfSize + 1) * sizeof(float));
    x->peakMap = (int *)malloc((size_t)(x->halfSize + 1) * sizeof(int));
    x->peakBins = (int *)malloc((size_t)(x->halfSize + 1) * sizeof(int));

    if (!x->analysisBuffer || !x->olaBuffer || !x->window || !x->rotStepRe || !x->rotStepIm ||
        !x->rotAccumRe || !x->rotAccumIm || !x->magBuffer || !x->peakMap || !x->peakBins) {
        xfreeze_free_processing(x);
        return 0;
    }

    // sqrt-Hann analysis/synthesis pair with 4x overlap keeps OLA gain stable.
    for (int i = 0; i < x->fftSize; ++i) {
        const float w = 0.5f - 0.5f * cosf((2.0f * (float)M_PI * (float)i) / (float)x->fftSize);
        x->window[i] = sqrtf((w > 0.0f) ? w : 0.0f);
    }

    for (int k = 0; k <= x->halfSize; ++k) {
        const float angle = (2.0f * (float)M_PI * (float)k * (float)x->hopSize) / (float)x->fftSize;
        x->rotStepRe[k] = cosf(angle);
        x->rotStepIm[k] = sinf(angle);
        x->rotAccumRe[k] = 1.0f;
        x->rotAccumIm[k] = 0.0f;
        x->magBuffer[k] = 0.0f;
        x->peakMap[k] = k;
    }

    x->rotAccumReady = 0;
    return 1;
}

// ─────────────────────────────────────
static t_int *xfreeze_perform(t_int *w) {
    t_xfreeze *x = (t_xfreeze *)(w[1]);
    t_float *in = (t_float *)(w[2]);
    t_float *out = (t_float *)(w[3]);
    int n = (int)(w[4]);

    if (n != x->fftSize || (n % x->hopSize) != 0) {
        for (int i = 0; i < n; ++i)
            out[i] = x->freeze ? in[i] : 0.0f;
        return (w + 5);
    }

    const int hopsPerBlock = n / x->hopSize;
    const float invFFT = 1.0f / (float)x->fftSize;
    const float olaScale = 0.5f;
    const int useMatrix = xfreeze_matrix_ready(x);
    const int matrixStride = useMatrix ? xfreeze_matrix_stride(x) : 0;
    t_word *matrixVec = useMatrix ? x->matrixVec : NULL;
    const int useFrames = (x->framesRe && x->framesIm);

    for (int h = 0; h < hopsPerBlock; ++h) {
        const t_float *inHop = in + (h * x->hopSize);
        t_float *outHop = out + (h * x->hopSize);

        memmove(x->analysisBuffer, x->analysisBuffer + x->hopSize,
                (size_t)(x->fftSize - x->hopSize) * sizeof(float));
        memcpy(x->analysisBuffer + (x->fftSize - x->hopSize), inHop,
               (size_t)x->hopSize * sizeof(t_float));

        for (int i = 0; i < x->fftSize; ++i)
            x->fftIn[i] = x->analysisBuffer[i] * x->window[i];
        fftw_execute(x->fftPlan);

        if (!x->freeze) {
            if (useMatrix && matrixVec) {
                const int base = x->writeFrame * matrixStride;
                for (int k = 0; k <= x->halfSize; ++k) {
                    const float re = x->fftOut[k][0];
                    const float im = x->fftOut[k][1];
                    const int idx = base + (k * 2);
                    matrixVec[idx].w_float = re;
                    matrixVec[idx + 1].w_float = im;
                }
            } else if (useFrames) {
                float *dstRe = x->framesRe[x->writeFrame];
                float *dstIm = x->framesIm[x->writeFrame];
                for (int k = 0; k <= x->halfSize; ++k) {
                    dstRe[k] = x->fftOut[k][0];
                    dstIm[k] = x->fftOut[k][1];
                }
            }

            x->writeFrame = (x->writeFrame + 1) % x->numFrames;
            if (x->framesFilled < x->numFrames)
                x->framesFilled++;

            memset(x->olaBuffer, 0, (size_t)x->fftSize * sizeof(float));
            for (int i = 0; i < x->hopSize; ++i)
                outHop[i] = 0.0f;
            continue;
        }

        if (!x->rotAccumReady)
            xfreeze_reset_rotaccum(x);

        if (x->framesFilled <= 0 || (!useMatrix && !useFrames)) {
            for (int i = 0; i < x->hopSize; ++i)
                outHop[i] = 0.0f;
            continue;
        }

        const int frameCount = x->framesFilled;
        const int i0 = x->playFrame % frameCount;
        const int i1 = (i0 + 1) % frameCount;
        const float a = x->interp;
        const float oneMinusA = 1.0f - a;

        for (int k = 0; k <= x->halfSize; ++k) {
            float re0 = 0.0f;
            float im0 = 0.0f;
            float re1 = 0.0f;
            float im1 = 0.0f;

            if (useMatrix && matrixVec) {
                const int idx0 = (i0 * matrixStride) + (k * 2);
                const int idx1 = (i1 * matrixStride) + (k * 2);
                re0 = matrixVec[idx0].w_float;
                im0 = matrixVec[idx0 + 1].w_float;
                re1 = matrixVec[idx1].w_float;
                im1 = matrixVec[idx1 + 1].w_float;
            } else if (useFrames) {
                re0 = x->framesRe[i0][k];
                im0 = x->framesIm[i0][k];
                re1 = x->framesRe[i1][k];
                im1 = x->framesIm[i1][k];
            }

            const float baseRe = oneMinusA * re0 + a * re1;
            const float baseIm = oneMinusA * im0 + a * im1;
            x->fftOut[k][0] = baseRe;
            x->fftOut[k][1] = baseIm;
            if (x->phaseLock)
                x->magBuffer[k] = sqrtf(baseRe * baseRe + baseIm * baseIm);
        }

        if (x->phaseLock)
            xfreeze_compute_peak_map(x);

        for (int k = 0; k <= x->halfSize; ++k) {
            const int stepIndex = x->phaseLock ? x->peakMap[k] : k;
            const float accRe = x->rotAccumRe[k];
            const float accIm = x->rotAccumIm[k];
            const float baseRe = x->fftOut[k][0];
            const float baseIm = x->fftOut[k][1];
            x->fftOut[k][0] = baseRe * accRe - baseIm * accIm;
            x->fftOut[k][1] = baseRe * accIm + baseIm * accRe;

            const float stepRe = x->rotStepRe[stepIndex];
            const float stepIm = x->rotStepIm[stepIndex];
            x->rotAccumRe[k] = accRe * stepRe - accIm * stepIm;
            x->rotAccumIm[k] = accRe * stepIm + accIm * stepRe;
        }

        fftw_execute(x->ifftPlan);
        for (int i = 0; i < x->fftSize; ++i) {
            const float sample = x->fftIn[i] * invFFT;
            x->olaBuffer[i] += sample * x->window[i] * olaScale;
        }

        for (int i = 0; i < x->hopSize; ++i)
            outHop[i] = x->olaBuffer[i];

        memmove(x->olaBuffer, x->olaBuffer + x->hopSize,
                (size_t)(x->fftSize - x->hopSize) * sizeof(float));
        memset(x->olaBuffer + (x->fftSize - x->hopSize), 0, (size_t)x->hopSize * sizeof(float));

        x->interp += x->speed;
        while (x->interp >= 1.0f) {
            x->interp -= 1.0f;
            x->playFrame = (x->playFrame + 1) % frameCount;
        }
    }

    return (w + 5);
}

// ─────────────────────────────────────
static void xfreeze_dsp(t_xfreeze *x, t_signal **sp) {
    x->dspBlockSize = sp[0]->s_n;
    if ((x->dspBlockSize != x->fftSize || (x->dspBlockSize % x->hopSize) != 0) &&
        !x->warnedBlockSize) {
        post("[xfreeze~] warning: block size (%d) must equal fftSize (%d) and be divisible by hop "
             "(%d); "
             "passing signal through",
             x->dspBlockSize, x->fftSize, x->hopSize);
        x->warnedBlockSize = 1;
    }
    if (x->matrixName && !xfreeze_matrix_ready(x))
        xfreeze_matrix_bind(x, x->matrixName);
    dsp_add(xfreeze_perform, 4, x, sp[0]->s_vec, sp[1]->s_vec, sp[0]->s_n);
}

// ─────────────────────────────────────
static void xfreeze_freeze(t_xfreeze *x, t_floatarg f) {
    const int newState = (f != 0.0f);
    if (newState && !x->freeze) {
        x->playFrame = 0;
        x->interp = 0.0f;
        x->rotAccumReady = 0;
        xfreeze_reset_rotaccum(x);
        if (x->olaBuffer)
            memset(x->olaBuffer, 0, (size_t)x->fftSize * sizeof(float));
    }
    if (!newState && x->freeze) {
        x->rotAccumReady = 0;
        xfreeze_reset_rotaccum(x);
        if (x->olaBuffer)
            memset(x->olaBuffer, 0, (size_t)x->fftSize * sizeof(float));
    }
    x->freeze = newState;
}

// ─────────────────────────────────────
static void xfreeze_frames(t_xfreeze *x, t_floatarg f) {
    int n = (int)f;
    if (n < 2)
        n = 2;
    if (n > 512)
        n = 512;

    if (n == x->numFrames)
        return;

    if (!xfreeze_alloc_frames(x, n)) {
        pd_error(x, "[xfreeze~] could not allocate frame buffers");
    }
}

// ─────────────────────────────────────
static void xfreeze_speed(t_xfreeze *x, t_floatarg f) {
    if (f < 0.0f)
        f = 0.0f;
    x->speed = (float)f;
}

// ─────────────────────────────────────
static void xfreeze_phaselock(t_xfreeze *x, t_floatarg f) { x->phaseLock = (f != 0.0f); }

// ─────────────────────────────────────
static void xfreeze_array(t_xfreeze *x, t_symbol *s) {
    if (!s || s == &s_ || s == gensym("none")) {
        x->matrixName = NULL;
        xfreeze_matrix_unbind(x);
        return;
    }

    if (!xfreeze_matrix_bind(x, s)) {
        pd_error(x, "[xfreeze~] array '%s' not found or invalid", s->s_name);
    }
}

// ─────────────────────────────────────
static void xfreeze_reset(t_xfreeze *x) {
    xfreeze_clear_frames(x);
    if (x->analysisBuffer)
        memset(x->analysisBuffer, 0, (size_t)x->fftSize * sizeof(float));
    if (x->olaBuffer)
        memset(x->olaBuffer, 0, (size_t)x->fftSize * sizeof(float));
}

// ─────────────────────────────────────
static void *xfreeze_new(t_symbol *s, int argc, t_atom *argv) {
    (void)s;

    t_xfreeze *x = (t_xfreeze *)pd_new(xfreeze_class);
    if (!x)
        return NULL;

    x->x_f = 0.0f;
    x->x_out = NULL;

    x->fftIn = NULL;
    x->fftOut = NULL;
    x->fftPlan = NULL;
    x->ifftPlan = NULL;

    x->framesRe = NULL;
    x->framesIm = NULL;
    x->analysisBuffer = NULL;
    x->olaBuffer = NULL;
    x->window = NULL;
    x->rotStepRe = NULL;
    x->rotStepIm = NULL;
    x->rotAccumRe = NULL;
    x->rotAccumIm = NULL;
    x->magBuffer = NULL;
    x->peakMap = NULL;
    x->peakBins = NULL;
    x->matrixName = NULL;
    x->matrixArray = NULL;
    x->matrixVec = NULL;
    x->matrixSize = 0;

    x->fftSize = 1024;
    x->halfSize = x->fftSize / 2;
    x->dspBlockSize = 0;

    x->numFrames = 4;
    x->framesFilled = 0;
    x->writeFrame = 0;
    x->playFrame = 0;
    x->interp = 0.0f;
    x->speed = 0.1f;
    x->freeze = 0;
    x->phaseLock = 1;
    x->rotAccumReady = 0;
    x->warnedBlockSize = 0;

    if (argc > 0 && argv[0].a_type == A_FLOAT) {
        int requestedFFT = (int)atom_getfloat(argv);
        if (requestedFFT >= 64)
            x->fftSize = requestedFFT;
    }

    if (argc > 1 && argv[1].a_type == A_FLOAT) {
        int requestedFrames = (int)atom_getfloat(argv + 1);
        if (requestedFrames >= 2)
            x->numFrames = requestedFrames;
    }

    x->halfSize = x->fftSize / 2;

    if (!xfreeze_init_fft(x, x->fftSize)) {
        pd_error(x, "[xfreeze~] failed to initialize FFT buffers/plans");
        return NULL;
    }

    if (!xfreeze_init_processing(x)) {
        pd_error(x, "[xfreeze~] failed to initialize processing buffers");
        if (x->fftPlan)
            fftw_destroy_plan(x->fftPlan);
        if (x->ifftPlan)
            fftw_destroy_plan(x->ifftPlan);
        if (x->fftIn)
            fftw_free(x->fftIn);
        if (x->fftOut)
            fftw_free(x->fftOut);
        x->fftPlan = NULL;
        x->ifftPlan = NULL;
        x->fftIn = NULL;
        x->fftOut = NULL;
        return NULL;
    }

    if (!xfreeze_alloc_frames(x, x->numFrames)) {
        pd_error(x, "[xfreeze~] failed to allocate frame buffers");
        if (x->fftPlan)
            fftw_destroy_plan(x->fftPlan);
        if (x->ifftPlan)
            fftw_destroy_plan(x->ifftPlan);
        if (x->fftIn)
            fftw_free(x->fftIn);
        if (x->fftOut)
            fftw_free(x->fftOut);
        xfreeze_free_processing(x);
        x->fftPlan = NULL;
        x->ifftPlan = NULL;
        x->fftIn = NULL;
        x->fftOut = NULL;
        return NULL;
    }

    x->x_out = outlet_new(&x->x_obj, &s_signal);
    return x;
}

// ─────────────────────────────────────
static void xfreeze_free(t_xfreeze *x) {
    if (x->fftPlan)
        fftw_destroy_plan(x->fftPlan);
    if (x->ifftPlan)
        fftw_destroy_plan(x->ifftPlan);
    if (x->fftIn)
        fftw_free(x->fftIn);
    if (x->fftOut)
        fftw_free(x->fftOut);
    xfreeze_free_processing(x);
    xfreeze_free_frames(x);
}

// ─────────────────────────────────────
extern "C" void xfreeze_tilde_setup(void) {
    xfreeze_class = class_new(gensym("xfreeze~"), (t_newmethod)xfreeze_new, (t_method)xfreeze_free,
                              sizeof(t_xfreeze), CLASS_DEFAULT, A_GIMME, 0);

    CLASS_MAINSIGNALIN(xfreeze_class, t_xfreeze, x_f);
    class_addmethod(xfreeze_class, (t_method)xfreeze_dsp, gensym("dsp"), A_CANT, 0);

    class_addmethod(xfreeze_class, (t_method)xfreeze_freeze, gensym("freeze"), A_FLOAT, 0);
    class_addmethod(xfreeze_class, (t_method)xfreeze_frames, gensym("frames"), A_FLOAT, 0);
    class_addmethod(xfreeze_class, (t_method)xfreeze_speed, gensym("speed"), A_FLOAT, 0);
    class_addmethod(xfreeze_class, (t_method)xfreeze_phaselock, gensym("phaselock"), A_FLOAT, 0);
    class_addmethod(xfreeze_class, (t_method)xfreeze_array, gensym("array"), A_SYMBOL, 0);
    class_addmethod(xfreeze_class, (t_method)xfreeze_reset, gensym("reset"), A_NULL);
}
