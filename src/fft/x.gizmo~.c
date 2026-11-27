/*
 * gizmo~ -- independent Pure Data pitch shifter (C99).
 *
 * Laroche & Dolson (1999), sections 2.1-2.4: peak regions,
 * fractional spectral translation and common phase rotation (Eq. 5).
 * Laroche & Dolson (1997): identity phase-locking.
 * Dudas (2002): optional, approximate floating-formant correction.
 * Not the proprietary Cycling '74 implementation.
 *
 * [gizmo~ ratio fft_size overlap]
 * defaults: 1, 2048, 4. Right inlet: pitch ratio (float).
 * Messages: ratio <float>, semitones <float>, formant <0..1>,
 *           reset, latency (reports samples via Pd console).
 * No external FFT library. No allocation in the DSP callback.
 */
#include "m_pd.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define GIZMO_PI 3.14159265358979323846264338327950288
#define GIZMO_TAU (2.0 * GIZMO_PI)
#define GIZMO_MIN_RATIO 0.125
#define GIZMO_MAX_RATIO 8.0

typedef struct _gizmo_peak {
    int bin;
    double frequency; /* fractional FFT bin */
    double shift;     /* fractional FFT bin */
    double rotation;  /* common phase correction for the region */
} t_gizmo_peak;

typedef struct _gizmo {
    t_object obj;
    t_float f;
    t_float ratio;
    t_float formant;
    t_inlet *ratio_inlet;
    t_outlet *outlet;

    int n, half, hop, overlap, mask;
    int pos, pending, previous_count, ready;
    double *input, *output, *window;
    double *re, *im, *out_re, *out_im;
    double *mag, *smooth, *scratch;
    double *tw_re, *tw_im;
    int *reverse, *owner, *previous_owner;
    t_gizmo_peak *peaks, *previous;
    double *env_x, *env_y, *env_warp;
    int env_count;
    double env_floor;
} t_gizmo;

static t_class *gizmo_tilde_class;

static double gizmo_tilde_clamp(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void gizmo_tilde_reset(t_gizmo *x) {
    if (!x->ready)
        return;
    memset(x->input, 0, (size_t)x->n * sizeof(double));
    memset(x->output, 0, (size_t)x->n * sizeof(double));
    for (int k = 0; k <= x->half; ++k)
        x->previous_owner[k] = -1;
    x->pos = x->pending = x->previous_count = 0;
}

static void gizmo_tilde_ratio(t_gizmo *x, t_floatarg f) {
    if (isfinite(f) && f > 0)
        x->ratio = (t_float)gizmo_tilde_clamp(f, GIZMO_MIN_RATIO, GIZMO_MAX_RATIO);
}

static void gizmo_tilde_semitones(t_gizmo *x, t_floatarg f) {
    if (isfinite(f))
        gizmo_tilde_ratio(x, (t_float)exp2(gizmo_tilde_clamp(f, -36, 36) / 12.0));
}

static void gizmo_tilde_formant(t_gizmo *x, t_floatarg f) {
    if (isfinite(f))
        x->formant = (t_float)gizmo_tilde_clamp(f, 0, 1);
}

static void gizmo_tilde_latency(t_gizmo *x) {
    post("gizmo~: latency %d samples (FFT %d, hop %d)", x->n, x->n, x->hop);
}

static void gizmo_tilde_release(t_gizmo *x) {
    free(x->input);
    free(x->output);
    free(x->window);
    free(x->re);
    free(x->im);
    free(x->out_re);
    free(x->out_im);
    free(x->mag);
    free(x->smooth);
    free(x->scratch);
    free(x->tw_re);
    free(x->tw_im);
    free(x->reverse);
    free(x->owner);
    free(x->previous_owner);
    free(x->peaks);
    free(x->previous);
    free(x->env_x);
    free(x->env_y);
    free(x->env_warp);
    x->ready = 0;
}

static int gizmo_tilde_allocate(t_gizmo *x) {
    size_t n = (size_t)x->n, b = (size_t)x->half + 1;
#define GIZMO_ALLOC(member, count)                                                                 \
    do {                                                                                           \
        x->member = calloc((count), sizeof(*x->member));                                           \
        if (!x->member)                                                                            \
            return 0;                                                                              \
    } while (0)
    GIZMO_ALLOC(input, n);
    GIZMO_ALLOC(output, n);
    GIZMO_ALLOC(window, n);
    GIZMO_ALLOC(re, n);
    GIZMO_ALLOC(im, n);
    GIZMO_ALLOC(out_re, n);
    GIZMO_ALLOC(out_im, n);
    GIZMO_ALLOC(mag, b);
    GIZMO_ALLOC(smooth, b);
    GIZMO_ALLOC(scratch, b);
    GIZMO_ALLOC(tw_re, n / 2);
    GIZMO_ALLOC(tw_im, n / 2);
    GIZMO_ALLOC(reverse, n);
    GIZMO_ALLOC(owner, b);
    GIZMO_ALLOC(previous_owner, b);
    GIZMO_ALLOC(peaks, b);
    GIZMO_ALLOC(previous, b);
    GIZMO_ALLOC(env_x, b);
    GIZMO_ALLOC(env_y, b);
    GIZMO_ALLOC(env_warp, b);
#undef GIZMO_ALLOC
    for (int i = 0; i < x->n; ++i) {
        x->window[i] = 0.5 - 0.5 * cos(GIZMO_TAU * i / x->n);
        int rev = 0, v = i;
        for (int len = x->n; len > 1; len >>= 1) {
            rev = (rev << 1) | (v & 1);
            v >>= 1;
        }
        x->reverse[i] = rev;
    }
    for (int i = 0; i < x->half; ++i) {
        x->tw_re[i] = cos(GIZMO_TAU * i / x->n);
        x->tw_im[i] = -sin(GIZMO_TAU * i / x->n);
    }
    x->ready = 1;
    gizmo_tilde_reset(x);
    return 1;
}

/* In-place radix-2 complex FFT. Inverse includes 1/N normalization. */
static void gizmo_tilde_fft(t_gizmo *x, double *re, double *im, int inverse) {
    for (int i = 0; i < x->n; ++i) {
        int j = x->reverse[i];
        if (j > i) {
            double a = re[i], b = im[i];
            re[i] = re[j];
            im[i] = im[j];
            re[j] = a;
            im[j] = b;
        }
    }
    for (int len = 2; len <= x->n; len <<= 1) {
        int half = len >> 1, step = x->n / len;
        for (int base = 0; base < x->n; base += len) {
            for (int j = 0; j < half; ++j) {
                int a = base + j, b = a + half, k = j * step;
                double wr = x->tw_re[k];
                double wi = inverse ? -x->tw_im[k] : x->tw_im[k];
                double tr = wr * re[b] - wi * im[b];
                double ti = wr * im[b] + wi * re[b];
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
            }
        }
    }
    if (inverse) {
        double scale = 1.0 / x->n;
        for (int i = 0; i < x->n; ++i) {
            re[i] *= scale;
            im[i] *= scale;
        }
    }
}

/* Four-neighbor peak picking, allowing endpoints and resolving flat ties. */
static int gizmo_tilde_is_peak(const double *mag, int k, int half) {
    if (mag[k] <= 0)
        return 0;
    for (int d = -2; d <= 2; ++d) {
        int j = k + d;
        if (!d || j < 0 || j > half)
            continue;
        if (mag[j] > mag[k] || (mag[j] == mag[k] && j < k))
            return 0;
    }
    return 1;
}

/* Quadratic interpolation in log magnitude, as in L&D section 2.2. */
static double gizmo_tilde_peak_frequency(const double *mag, int k, int half) {
    if (k == 0 || k == half)
        return k;
    double a = log(fmax(mag[k - 1], 1e-30));
    double b = log(fmax(mag[k], 1e-30));
    double c = log(fmax(mag[k + 1], 1e-30));
    double d = a - 2.0 * b + c;
    if (d >= -1e-12)
        return k;
    return k + gizmo_tilde_clamp(0.5 * (a - c) / d, -0.5, 0.5);
}

static int gizmo_tilde_find_peaks(t_gizmo *x) {
    int count = 0;
    for (int k = 0; k <= x->half; ++k) {
        if (!gizmo_tilde_is_peak(x->mag, k, x->half))
            continue;
        x->peaks[count].bin = k;
        x->peaks[count].frequency = gizmo_tilde_peak_frequency(x->mag, k, x->half);
        ++count;
    }
    return count;
}

/* Assign each bin to the closest peak, using midpoints as boundaries. */
static void gizmo_tilde_regions(t_gizmo *x, int count) {
    int lo = 0;
    for (int p = 0; p < count; ++p) {
        int hi = p + 1 < count ? (x->peaks[p].bin + x->peaks[p + 1].bin) / 2 : x->half;
        for (int k = lo; k <= hi; ++k)
            x->owner[k] = p;
        lo = hi + 1;
    }
}

/*
 * Eq. (5): theta[u] = theta[u-1] + delta_omega[u] * hop.
 * Previous-region lookup is a practical tracking heuristic, not specified
 * by the paper. It lets a peak retain rotation when crossing FFT bins.
 */
static void gizmo_tilde_update_phase(t_gizmo *x, int count, double ratio) {
    for (int p = 0; p < count; ++p) {
        t_gizmo_peak *peak = &x->peaks[p];
        int prev = x->previous_owner[peak->bin];
        peak->shift = (ratio - 1.0) * peak->frequency;
        peak->rotation = 0;
        if (prev >= 0 && prev < x->previous_count &&
            fabs(x->previous[prev].frequency - peak->frequency) <= 3.0) {
            peak->rotation = remainder(
                x->previous[prev].rotation + GIZMO_TAU * peak->shift * x->hop / x->n, GIZMO_TAU);
        }
    }
}

/* Linear interpolation of an amplitude envelope; constant extrapolation. */
static double gizmo_tilde_envelope_value(const double *freq, const double *amp, int count,
                                         double k) {
    if (count <= 0)
        return 0;
    if (k <= freq[0])
        return amp[0];
    if (k >= freq[count - 1])
        return amp[count - 1];
    int lo = 0, hi = count - 1;
    while (hi - lo > 1) {
        int mid = lo + (hi - lo) / 2;
        if (freq[mid] <= k)
            lo = mid;
        else
            hi = mid;
    }
    double t = (k - freq[lo]) / (freq[hi] - freq[lo]);
    return amp[lo] + t * (amp[hi] - amp[lo]);
}

/*
 * Dudas (2002), pp. 128-129: smooth magnitude spectrum, find peaks,
 * warp peak frequencies, then interpolate a new envelope.
 * The paper does not specify the exact exponential curve. Here the
 * scaling factor decays exponentially in log-frequency from ratio to 1.
 * Anchors: lowest positive peak * ratio, highest original positive peak.
 */
static void gizmo_tilde_envelope(t_gizmo *x, double ratio) {
    int b = x->half + 1;
    memcpy(x->smooth, x->mag, (size_t)b * sizeof(double));
    for (int pass = 0; pass < 3; ++pass) {
        for (int k = 0; k < b; ++k) {
            double a = x->smooth[k > 0 ? k - 1 : k];
            double c = x->smooth[k + 1 < b ? k + 1 : k];
            x->scratch[k] = 0.25 * (a + 2.0 * x->smooth[k] + c);
        }
        double *tmp = x->smooth;
        x->smooth = x->scratch;
        x->scratch = tmp;
    }
    double maxmag = 0;
    for (int k = 0; k < b; ++k)
        maxmag = fmax(maxmag, x->smooth[k]);
    x->env_floor = fmax(1e-20, maxmag * 0.001);
    x->env_count = 0;
    for (int k = 1; k < x->half; ++k) {
        if (x->smooth[k] < x->env_floor || !gizmo_tilde_is_peak(x->smooth, k, x->half))
            continue;
        int p = x->env_count++;
        x->env_x[p] = gizmo_tilde_peak_frequency(x->smooth, k, x->half);
        x->env_y[p] = x->smooth[k];
    }
    if (x->env_count < 2) {
        x->env_count = 0;
        return;
    }
    double low = x->env_x[0], high = x->env_x[x->env_count - 1];
    /* A monotonic floating envelope cannot satisfy crossed anchors. */
    if (low * ratio >= high) {
        x->env_count = 0;
        return;
    }
    double span = log(high / low), lr = log(ratio);
    for (int p = 0; p < x->env_count; ++p) {
        double f = x->env_x[p];
        double t = log(f / low) / span;
        x->env_warp[p] = f * exp(lr * (1.0 - t));
    }
}

static double gizmo_tilde_formant_gain(t_gizmo *x, double src, double dst, double amount) {
    if (amount <= 0 || x->env_count < 2)
        return 1;
    double before = gizmo_tilde_envelope_value(x->env_x, x->env_y, x->env_count, src);
    double after = gizmo_tilde_envelope_value(x->env_warp, x->env_y, x->env_count, dst);
    double gain = fmax(after, x->env_floor) / fmax(before, x->env_floor);
    /* Limit correction to +/- 12 dB to avoid amplifying spectral holes. */
    return pow(gizmo_tilde_clamp(gain, 0.25, 4.0), amount);
}

/* Add to the positive-frequency spectrum without wrapping past Nyquist. */
static void gizmo_tilde_add_bin(t_gizmo *x, int k, double re, double im) {
    if (k < 0 || k > x->half)
        return;
    if (k == 0 || k == x->half) {
        /* Conjugate contributions meet at DC and Nyquist. */
        x->out_re[k] += 2.0 * re;
    } else {
        x->out_re[k] += re;
        x->out_im[k] += im;
    }
}

/* Translate entire peak regions by a common fractional bin displacement. */
static void gizmo_tilde_shift(t_gizmo *x, int count, double formant) {
    memset(x->out_re, 0, (size_t)x->n * sizeof(double));
    memset(x->out_im, 0, (size_t)x->n * sizeof(double));
    int lo = 0;
    for (int p = 0; p < count; ++p) {
        t_gizmo_peak *peak = &x->peaks[p];
        int hi = p + 1 < count ? (peak->bin + x->peaks[p + 1].bin) / 2 : x->half;
        double dst = peak->frequency + peak->shift;
        if (dst <= x->half) {
            double gain = gizmo_tilde_formant_gain(x, peak->frequency, dst, formant);
            double cr = cos(peak->rotation) * gain;
            double ci = sin(peak->rotation) * gain;
            int delta = (int)floor(peak->shift);
            double frac = peak->shift - delta;
            for (int k = lo; k <= hi; ++k) {
                double re = x->re[k] * cr - x->im[k] * ci;
                double im = x->re[k] * ci + x->im[k] * cr;
                if (k == 0 || k == x->half) {
                    re *= 0.5;
                    im *= 0.5;
                }
                int j = k + delta;
                gizmo_tilde_add_bin(x, j, re * (1.0 - frac), im * (1.0 - frac));
                if (frac > 0)
                    gizmo_tilde_add_bin(x, j + 1, re * frac, im * frac);
            }
        }
        lo = hi + 1;
    }
    /* Hermitian symmetry gives a real-valued inverse transform. */
    for (int k = 1; k < x->half; ++k) {
        x->out_re[x->n - k] = x->out_re[k];
        x->out_im[x->n - k] = -x->out_im[k];
    }
}

static void gizmo_tilde_frame(t_gizmo *x) {
    double ratio = x->ratio, formant = x->formant;
    /* Center the FFT at the window midpoint (L&D section 1.1). */
    for (int i = 0; i < x->n; ++i) {
        int j = (i + x->half) & x->mask;
        x->re[j] = x->input[(x->pos + i) & x->mask] * x->window[i];
        x->im[j] = 0;
    }
    gizmo_tilde_fft(x, x->re, x->im, 0);
    for (int k = 0; k <= x->half; ++k)
        x->mag[k] = hypot(x->re[k], x->im[k]);
    int count = gizmo_tilde_find_peaks(x);
    if (!count) {
        x->previous_count = 0;
        for (int k = 0; k <= x->half; ++k)
            x->previous_owner[k] = -1;
        return;
    }
    gizmo_tilde_regions(x, count);
    gizmo_tilde_update_phase(x, count, ratio);
    x->env_count = 0;
    if (formant > 0 && ratio != 1.0)
        gizmo_tilde_envelope(x, ratio);
    gizmo_tilde_shift(x, count, formant);
    gizmo_tilde_fft(x, x->out_re, x->out_im, 1);
    /* Hann analysis + rectangular synthesis: sum of overlapping Hann = O/2. */
    double norm = 2.0 / x->overlap;
    for (int i = 0; i < x->n; ++i) {
        int j = (i + x->half) & x->mask;
        x->output[(x->pos + i) & x->mask] += norm * x->out_re[j];
    }
    t_gizmo_peak *tmp = x->previous;
    x->previous = x->peaks;
    x->peaks = tmp;
    int *itmp = x->previous_owner;
    x->previous_owner = x->owner;
    x->owner = itmp;
    x->previous_count = count;
}

static void gizmo_tilde_process(t_gizmo *x, const t_sample *in, t_sample *out, int n) {
    if (!x->ready) {
        memset(out, 0, (size_t)n * sizeof(*out));
        return;
    }
    for (int i = 0; i < n; ++i) {
        /* Read input first: Pd may reuse the input vector for output. */
        double sample = in[i];
        out[i] = (t_sample)x->output[x->pos];
        x->output[x->pos] = 0;
        x->input[x->pos] = isfinite(sample) ? sample : 0;
        x->pos = (x->pos + 1) & x->mask;
        if (++x->pending == x->hop) {
            x->pending = 0;
            gizmo_tilde_frame(x);
        }
    }
}

static t_int *gizmo_tilde_perform(t_int *w) {
    t_gizmo *x = (t_gizmo *)w[1];
    const t_sample *in = (const t_sample *)w[2];
    t_sample *out = (t_sample *)w[3];
    gizmo_tilde_process(x, in, out, (int)w[4]);
    return w + 5;
}

static void gizmo_tilde_dsp(t_gizmo *x, t_signal **sp) {
    gizmo_tilde_reset(x);
    dsp_add(gizmo_tilde_perform, 4, x, sp[0]->s_vec, sp[1]->s_vec, sp[0]->s_n);
}

static void gizmo_tilde_free(t_gizmo *x) {
    if (x->ratio_inlet)
        inlet_free(x->ratio_inlet);
    if (x->outlet)
        outlet_free(x->outlet);
    gizmo_tilde_release(x);
}

static int gizmo_tilde_power_of_two(int n) { return n > 0 && !(n & (n - 1)); }

static void *gizmo_tilde_new(t_symbol *s, int argc, t_atom *argv) {
    (void)s;
    t_gizmo *x = (t_gizmo *)pd_new(gizmo_tilde_class);
    if (!x) {
        return NULL;
    }
    /* Keep Pd's object header; explicitly initialize all owned members. */
    x->f = 0;
    x->ratio = 1;
    x->formant = 0;
    x->ratio_inlet = NULL;
    x->outlet = NULL;
    x->n = 2048;
    x->overlap = 4;
    x->ready = 0;
    x->input = x->output = x->window = NULL;
    x->re = x->im = x->out_re = x->out_im = NULL;
    x->mag = x->smooth = x->scratch = NULL;
    x->tw_re = x->tw_im = NULL;
    x->reverse = x->owner = x->previous_owner = NULL;
    x->peaks = x->previous = NULL;
    x->env_x = x->env_y = x->env_warp = NULL;
    if (argc > 0)
        gizmo_tilde_ratio(x, atom_getfloat(argv));
    if (argc > 1) {
        double v = atom_getfloat(argv + 1);
        if (v >= 256 && v <= 32768 && v == floor(v) && gizmo_tilde_power_of_two((int)v))
            x->n = (int)v;
        else
            pd_error(x, "gizmo~: FFT size must be a power of two (256..32768); using 2048");
    }
    if (argc > 2) {
        double v = atom_getfloat(argv + 2);
        if (v >= 4 && v <= 32 && v == floor(v) && gizmo_tilde_power_of_two((int)v))
            x->overlap = (int)v;
        else
            pd_error(x, "gizmo~: overlap must be 4, 8, 16 or 32; using 4");
    }
    x->half = x->n / 2;
    x->mask = x->n - 1;
    x->hop = x->n / x->overlap;
    if (!gizmo_tilde_allocate(x)) {
        pd_error(x, "gizmo~: unable to allocate FFT buffers");
        pd_free((t_pd *)x); /* invokes gizmo_tilde_free */
        return NULL;
    }
    x->ratio_inlet = inlet_new(&x->obj, &x->obj.ob_pd, &s_float, gensym("ratio"));
    x->outlet = outlet_new(&x->obj, &s_signal);
    return x;
}

void setup_x0x2egizmo_tilde(void) {
    gizmo_tilde_class =
        class_new(gensym("x.gizmo~"), (t_newmethod)gizmo_tilde_new, (t_method)gizmo_tilde_free,
                  sizeof(t_gizmo), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(gizmo_tilde_class, t_gizmo, f);
    class_addmethod(gizmo_tilde_class, (t_method)gizmo_tilde_dsp, gensym("dsp"), A_CANT, 0);
    class_addmethod(gizmo_tilde_class, (t_method)gizmo_tilde_ratio, gensym("ratio"), A_FLOAT, 0);
    class_addmethod(gizmo_tilde_class, (t_method)gizmo_tilde_semitones, gensym("semitones"),
                    A_FLOAT, 0);
    class_addmethod(gizmo_tilde_class, (t_method)gizmo_tilde_formant, gensym("formant"), A_FLOAT,
                    0);
    class_addmethod(gizmo_tilde_class, (t_method)gizmo_tilde_reset, gensym("reset"), 0);
    class_addmethod(gizmo_tilde_class, (t_method)gizmo_tilde_latency, gensym("latency"), 0);
}
