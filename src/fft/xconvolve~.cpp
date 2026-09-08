#include <fftw3.h>
#include <m_pd.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static t_class *xconv_class;

// ─────────────────────────────────────
typedef struct _xconv {
    t_object x_obj;
    t_float x_f;

    t_inlet *x_inH;
    t_outlet *x_out;

    int blockSize;
    int fftSize;
    int nBins;

    double *xTime;
    double *hTime;
    double *yTime;
    float *overlap;

    fftw_complex *X;
    fftw_complex *H;
    fftw_complex *Y;

    fftw_plan fftPlanX;
    fftw_plan fftPlanH;
    fftw_plan ifftPlan;
} t_xconv;

// ─────────────────────────────────────
static int next_power_of_two(int n) {
    int p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

// ─────────────────────────────────────
static void xconv_clear(t_xconv *x) {
    if (x->fftPlanX)
        fftw_destroy_plan(x->fftPlanX);
    if (x->fftPlanH)
        fftw_destroy_plan(x->fftPlanH);
    if (x->ifftPlan)
        fftw_destroy_plan(x->ifftPlan);

    if (x->xTime)
        fftw_free(x->xTime);
    if (x->hTime)
        fftw_free(x->hTime);
    if (x->yTime)
        fftw_free(x->yTime);
    if (x->overlap)
        free(x->overlap);

    if (x->X)
        fftw_free(x->X);
    if (x->H)
        fftw_free(x->H);
    if (x->Y)
        fftw_free(x->Y);

    x->fftPlanX = NULL;
    x->fftPlanH = NULL;
    x->ifftPlan = NULL;

    x->xTime = NULL;
    x->hTime = NULL;
    x->yTime = NULL;
    x->overlap = NULL;

    x->X = NULL;
    x->H = NULL;
    x->Y = NULL;
}

// ─────────────────────────────────────
static void xconv_alloc(t_xconv *x, int blockSize) {
    xconv_clear(x);

    x->blockSize = blockSize;
    x->fftSize = next_power_of_two(2 * blockSize - 1);
    x->nBins = x->fftSize / 2 + 1;

    x->xTime = (double *)fftw_alloc_real(x->fftSize);
    x->hTime = (double *)fftw_alloc_real(x->fftSize);
    x->yTime = (double *)fftw_alloc_real(x->fftSize);

    x->overlap = (float *)calloc(x->blockSize, sizeof(float));

    x->X = (fftw_complex *)fftw_alloc_complex(x->nBins);
    x->H = (fftw_complex *)fftw_alloc_complex(x->nBins);
    x->Y = (fftw_complex *)fftw_alloc_complex(x->nBins);

    x->fftPlanX = fftw_plan_dft_r2c_1d(x->fftSize, x->xTime, x->X, FFTW_MEASURE);
    x->fftPlanH = fftw_plan_dft_r2c_1d(x->fftSize, x->hTime, x->H, FFTW_MEASURE);
    x->ifftPlan = fftw_plan_dft_c2r_1d(x->fftSize, x->Y, x->yTime, FFTW_MEASURE);
}

// ─────────────────────────────────────
static t_int *xconv_perform(t_int *w) {
    t_xconv *x = (t_xconv *)(w[1]);
    t_float *audio = (t_float *)(w[2]);
    t_float *h = (t_float *)(w[3]);
    t_float *out = (t_float *)(w[4]);
    int n = (int)(w[5]);
    int N = x->blockSize;
    int F = x->fftSize;
    if (n != N) {
        return (w + 6);
    }
    memset(x->xTime, 0, sizeof(float) * F);
    memset(x->hTime, 0, sizeof(float) * F);
    for (int i = 0; i < N; i++) {
        x->xTime[i] = audio[i];
        x->hTime[i] = h[i];
    }

    fftw_execute(x->fftPlanX);
    fftw_execute(x->fftPlanH);

    for (int k = 0; k < x->nBins; k++) {
        float xr = x->X[k][0];
        float xi = x->X[k][1];
        float hr = x->H[k][0];
        float hi = x->H[k][1];
        x->Y[k][0] = xr * hr - xi * hi;
        x->Y[k][1] = xr * hi + xi * hr;
    }

    fftw_execute(x->ifftPlan);

    float scale = 1.0f / (float)F;
    for (int i = 0; i < N; i++) {
        out[i] = x->yTime[i] * scale + x->overlap[i];
    }
    for (int i = 0; i < N - 1; i++) {
        x->overlap[i] = x->yTime[i + N] * scale;
    }
    x->overlap[N - 1] = 0.0f;

    return (w + 6);
}

// ─────────────────────────────────────
static void xconv_dsp(t_xconv *x, t_signal **sp) {
    int blockSize = sp[0]->s_n;

    if (x->blockSize != blockSize) {
        xconv_alloc(x, blockSize);
    }

    dsp_add(xconv_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, blockSize);
}

// ─────────────────────────────────────
static void *xconv_new(t_symbol *s, int argc, t_atom *argv) {
    t_xconv *x = (t_xconv *)pd_new(xconv_class);

    x->x_f = 0;
    x->x_inH = inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    x->x_out = outlet_new(&x->x_obj, &s_signal);

    x->blockSize = 0;
    x->fftSize = 0;
    x->nBins = 0;

    x->xTime = NULL;
    x->hTime = NULL;
    x->yTime = NULL;
    x->overlap = NULL;

    x->X = NULL;
    x->H = NULL;
    x->Y = NULL;

    x->fftPlanX = NULL;
    x->fftPlanH = NULL;
    x->ifftPlan = NULL;
    return x;
}

// ─────────────────────────────────────
static void xconv_free(t_xconv *x) {
    //
    xconv_clear(x);
}

// ─────────────────────────────────────
extern "C" void xconv_tilde_setup(void) {
    xconv_class = class_new(gensym("xconv~"), (t_newmethod)xconv_new, (t_method)xconv_free,
                            sizeof(t_xconv), CLASS_DEFAULT, A_GIMME, 0);

    CLASS_MAINSIGNALIN(xconv_class, t_xconv, x_f);
    class_addmethod(xconv_class, (t_method)xconv_dsp, gensym("dsp"), A_CANT, 0);
}
