#include "m_pd.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define BUF_MAX 1024

typedef struct _xclick {
    t_object x_obj;

    int x_nsamples;
    int x_nleft;
    int x_position;

    t_float x_buffer[BUF_MAX];

    t_float x_sr;
    int x_waveform; // 0=click, 1=sine, 2=rect, 3=triangle, 4=noise
} t_xclick;

static t_class *xclick_class;

// ─────────────────────────────────────
static void fill_waveform(t_xclick *x) {
    int N = BUF_MAX;
    float sr = x->x_sr > 0 ? x->x_sr : 44100.0f;

    switch (x->x_waveform) {
    case 0: {
        int samples = (int)(sr * 0.002f);
        if (samples < 1)
            samples = 1;
        if (samples > BUF_MAX)
            samples = BUF_MAX;

        x->x_nsamples = samples;
        for (int i = 0; i < samples; ++i) {
            float phase = (float)i / (float)samples;
            float env = expf(-8.0f * phase);
            x->x_buffer[i] = (i & 1 ? -1.0f : 1.0f) * env;
        }
        break;
    }

    case 1:
        x->x_nsamples = N;
        for (int i = 0; i < N; ++i)
            x->x_buffer[i] = sinf(2.0f * (float)M_PI * 440.0f * i / sr);
        break;

    case 2: // rectangle
        x->x_nsamples = N;
        for (int i = 0; i < N; ++i) {
            float phase = fmodf(i * 440.0f / sr, 1.0f);
            x->x_buffer[i] = phase < 0.5f ? 1.0f : -1.0f;
        }
        break;

    case 3: // triangle
        x->x_nsamples = N;
        for (int i = 0; i < N; ++i) {
            float phase = fmodf(i * 440.0f / sr, 1.0f);
            x->x_buffer[i] = 4.0f * fabsf(phase - 0.5f) - 1.0f;
        }
        break;

    case 4: // noise
        x->x_nsamples = N;
        for (int i = 0; i < N; ++i)
            x->x_buffer[i] = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
        break;
    }

    x->x_nleft = 0;
    x->x_position = 0;
}

// ─────────────────────────────────────
static void xclick_bang(t_xclick *x) {
    // Restart from the beginning every time a bang arrives.
    x->x_position = 0;
    x->x_nleft = x->x_nsamples;
}

// ─────────────────────────────────────
static void xclick_setwave(t_xclick *x, t_symbol *s) {
    if (!s)
        return;

    if (!strcmp(s->s_name, "-osc"))
        x->x_waveform = 1;
    else if (!strcmp(s->s_name, "-rect"))
        x->x_waveform = 2;
    else if (!strcmp(s->s_name, "-triangle"))
        x->x_waveform = 3;
    else if (!strcmp(s->s_name, "-noise"))
        x->x_waveform = 4;
    else
        x->x_waveform = 0;

    fill_waveform(x);
}

// ─────────────────────────────────────
static t_int *xclick_perform(t_int *w) {
    t_xclick *x = (t_xclick *)(w[1]);
    int n = (int)(w[2]);
    t_sample *out = (t_sample *)(w[3]);

    while (n--) {
        if (x->x_nleft > 0) {
            int pos = x->x_position++;
            float env = (float)x->x_nleft / (float)x->x_nsamples;
            *out++ = x->x_buffer[pos] * env;
            --x->x_nleft;
        } else {
            *out++ = 0.0f;
        }
    }
    return w + 4;
}

// ─────────────────────────────────────
static void xclick_dsp(t_xclick *x, t_signal **sp) {
    x->x_sr = sp[0]->s_sr;
    fill_waveform(x);
    dsp_add(xclick_perform, 3, x, sp[0]->s_n, sp[0]->s_vec);
}

// ─────────────────────────────────────
static void *xclick_new(t_symbol *s, int ac, t_atom *av) {
    t_xclick *x = (t_xclick *)pd_new(xclick_class);
    x->x_sr = sys_getsr();
    x->x_nsamples = 1;
    x->x_nleft = 0;
    x->x_position = 0;
    x->x_waveform = 0;
    outlet_new(&x->x_obj, &s_signal);
    if (ac > 0 && av[0].a_type == A_SYMBOL)
        xclick_setwave(x, atom_getsymbol(av));
    else
        fill_waveform(x);
    return x;
}

// ─────────────────────────────────────
extern "C" void setup_x0x2eclick(void) {
    xclick_class = class_new(gensym("x.click"), (t_newmethod)xclick_new, 0, sizeof(t_xclick),
                             CLASS_DEFAULT, A_GIMME, 0);

    class_addbang(xclick_class, (t_method)xclick_bang);
    class_addmethod(xclick_class, (t_method)xclick_dsp, gensym("dsp"), A_CANT, 0);
    class_addmethod(xclick_class, (t_method)xclick_setwave, gensym("wave"), A_SYMBOL, 0);
}
