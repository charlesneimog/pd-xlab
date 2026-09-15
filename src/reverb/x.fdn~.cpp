#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <m_pd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static t_class *fdn_class;

#define FDN_N 8
#define FDN_DIFFUSERS 4
#define FDN_MAX_PREDELAY_MS 250.0f
#define FDN_MAX_MOD_DEPTH_SAMPLES 3.0f

enum { FDN_MATRIX_HADAMARD = 0, FDN_MATRIX_HOUSEHOLDER = 1 };

// ─────────────────────────────────────
typedef struct _fdn_tilde {
    t_object x_obj;
    t_sample f;

    double sr;

    float rt60;
    float damping;
    float wet;
    float dry;
    float input_gain;
    float lp_coeff;
    float roomsize;
    float predelay_ms;
    float diffusion;
    float moddepth;
    int matrix_type;

    float base_delay_ms[FDN_N];
    float delay_ms[FDN_N];
    int delay_samples[FDN_N];
    int buffer_size[FDN_N];
    int write_index[FDN_N];

    float feedback_gain[FDN_N];
    float lp_state[FDN_N];
    float mod_phase[FDN_N];
    float mod_freq[FDN_N];
    float mod_phase_inc[FDN_N];

    std::vector<float> buffer[FDN_N];

    float diffuser_delay_ms[FDN_DIFFUSERS];
    int diffuser_delay_samples[FDN_DIFFUSERS];
    int diffuser_write_index[FDN_DIFFUSERS];
    std::vector<float> diffuser_buffer[FDN_DIFFUSERS];

    int predelay_samples;
    int predelay_write_index;
    std::vector<float> predelay_buffer;

    t_outlet *x_out;
} t_fdn_tilde;

// ─────────────────────────────────────
static inline int fdn_wrap_index(int index, int size) {
    while (index < 0) {
        index += size;
    }
    while (index >= size) {
        index -= size;
    }
    return index;
}

// ─────────────────────────────────────
static inline float fdn_read_delay_integer(const std::vector<float> &buf, int write_index,
                                           int delay_samples) {
    int size = (int)buf.size();
    int read_index = fdn_wrap_index(write_index - delay_samples, size);
    return buf[read_index];
}

// ─────────────────────────────────────
static inline float fdn_read_delay_linear(const std::vector<float> &buf, int write_index,
                                          float delay_samples) {
    int size = (int)buf.size();
    float read_pos = (float)write_index - delay_samples;

    while (read_pos < 0.0f) {
        read_pos += (float)size;
    }
    while (read_pos >= (float)size) {
        read_pos -= (float)size;
    }

    int i0 = (int)floorf(read_pos);
    int i1 = i0 + 1;
    if (i1 >= size) {
        i1 = 0;
    }

    float frac = read_pos - (float)i0;
    return buf[i0] + frac * (buf[i1] - buf[i0]);
}

// ─────────────────────────────────────
static void fdn_update_coefficients(t_fdn_tilde *x) {
    for (int i = 0; i < FDN_N; ++i) {
        float delay_sec = (float)x->delay_samples[i] / (float)x->sr;
        x->feedback_gain[i] = powf(10.0f, -3.0f * delay_sec / x->rt60);
        x->mod_phase_inc[i] = (float)(2.0 * M_PI) * x->mod_freq[i] / (float)x->sr;
    }

    float pole = expf(-2.0f * (float)M_PI * x->damping / (float)x->sr);
    x->lp_coeff = std::clamp(1.0f - pole, 0.0f, 1.0f);
}

// ─────────────────────────────────────
static void fdn_resize_buffers(t_fdn_tilde *x) {
    int margin = (int)ceilf(FDN_MAX_MOD_DEPTH_SAMPLES) + 4;

    for (int i = 0; i < FDN_N; ++i) {
        float d_ms = x->base_delay_ms[i] * x->roomsize;
        x->delay_ms[i] = d_ms;
        x->delay_samples[i] = std::max(1, (int)roundf(d_ms * 0.001f * (float)x->sr));
        x->buffer_size[i] = x->delay_samples[i] + margin;
        x->buffer[i].assign(x->buffer_size[i], 0.0f);
        x->write_index[i] = 0;
    }
}

// ─────────────────────────────────────
static void fdn_resize_predelay(t_fdn_tilde *x) {
    int max_samples = std::max(1, (int)ceilf(FDN_MAX_PREDELAY_MS * 0.001f * (float)x->sr) + 1);
    x->predelay_buffer.assign(max_samples, 0.0f);
    x->predelay_samples =
        std::clamp((int)roundf(x->predelay_ms * 0.001f * (float)x->sr), 0, max_samples - 1);
    x->predelay_write_index = 0;
}

// ─────────────────────────────────────
static void fdn_resize_diffusers(t_fdn_tilde *x) {
    for (int i = 0; i < FDN_DIFFUSERS; ++i) {
        int samples = std::max(1, (int)roundf(x->diffuser_delay_ms[i] * 0.001f * (float)x->sr));
        x->diffuser_delay_samples[i] = samples;
        x->diffuser_buffer[i].assign(samples, 0.0f);
        x->diffuser_write_index[i] = 0;
    }
}

// ─────────────────────────────────────
static void fdn_clear(t_fdn_tilde *x) {
    for (int i = 0; i < FDN_N; ++i) {
        std::fill(x->buffer[i].begin(), x->buffer[i].end(), 0.0f);
        x->write_index[i] = 0;
        x->lp_state[i] = 0.0f;
    }

    for (int i = 0; i < FDN_DIFFUSERS; ++i) {
        std::fill(x->diffuser_buffer[i].begin(), x->diffuser_buffer[i].end(), 0.0f);
        x->diffuser_write_index[i] = 0;
    }

    std::fill(x->predelay_buffer.begin(), x->predelay_buffer.end(), 0.0f);
    x->predelay_write_index = 0;
}

// ─────────────────────────────────────
static inline void fdn_hadamard8(const float in[FDN_N], float out[FDN_N]) {
    const float s = 0.3535533905932738f; // 1 / sqrt(8)
    out[0] = s * (in[0] + in[1] + in[2] + in[3] + in[4] + in[5] + in[6] + in[7]);
    out[1] = s * (in[0] - in[1] + in[2] - in[3] + in[4] - in[5] + in[6] - in[7]);
    out[2] = s * (in[0] + in[1] - in[2] - in[3] + in[4] + in[5] - in[6] - in[7]);
    out[3] = s * (in[0] - in[1] - in[2] + in[3] + in[4] - in[5] - in[6] + in[7]);
    out[4] = s * (in[0] + in[1] + in[2] + in[3] - in[4] - in[5] - in[6] - in[7]);
    out[5] = s * (in[0] - in[1] + in[2] - in[3] - in[4] + in[5] - in[6] + in[7]);
    out[6] = s * (in[0] + in[1] - in[2] - in[3] - in[4] - in[5] + in[6] + in[7]);
    out[7] = s * (in[0] - in[1] - in[2] + in[3] - in[4] + in[5] + in[6] - in[7]);
}

// ─────────────────────────────────────
static inline void fdn_householder8(const float in[FDN_N], float out[FDN_N]) {
    float sum = 0.0f;
    for (int i = 0; i < FDN_N; ++i) {
        sum += in[i];
    }

    float common = 0.25f * sum;
    for (int i = 0; i < FDN_N; ++i) {
        out[i] = in[i] - common;
    }
}

// ─────────────────────────────────────
static inline float fdn_predelay_tick(t_fdn_tilde *x, float in) {
    if (x->predelay_samples <= 0) {
        return in;
    }

    int size = (int)x->predelay_buffer.size();
    int read_index = fdn_wrap_index(x->predelay_write_index - x->predelay_samples, size);
    float out = x->predelay_buffer[read_index];

    x->predelay_buffer[x->predelay_write_index] = in;
    if (++x->predelay_write_index >= size) {
        x->predelay_write_index = 0;
    }

    return out;
}

// ─────────────────────────────────────
static inline float fdn_diffusion_tick(t_fdn_tilde *x, float in) {
    if (x->diffusion <= 0.0f) {
        return in;
    }

    float y = in;
    float g = 0.7f * x->diffusion;

    for (int i = 0; i < FDN_DIFFUSERS; ++i) {
        int wi = x->diffuser_write_index[i];
        float delayed = x->diffuser_buffer[i][wi];
        float ap_out = delayed - g * y;
        x->diffuser_buffer[i][wi] = y + g * ap_out;

        if (++x->diffuser_write_index[i] >= x->diffuser_delay_samples[i]) {
            x->diffuser_write_index[i] = 0;
        }

        y = ap_out;
    }

    return y;
}

// ─────────────────────────────────────
static inline float fdn_tick(t_fdn_tilde *x, float in) {
    float tank_in = fdn_predelay_tick(x, in);
    tank_in = fdn_diffusion_tick(x, tank_in);
    tank_in *= x->input_gain;

    float out_taps[FDN_N];
    float fb_input[FDN_N];
    float fb[FDN_N];

    for (int i = 0; i < FDN_N; ++i) {
        float d;

        if (x->moddepth <= 0.0f) {
            d = fdn_read_delay_integer(x->buffer[i], x->write_index[i], x->delay_samples[i]);
        } else {
            float mod = sinf(x->mod_phase[i]) * x->moddepth;
            d = fdn_read_delay_linear(x->buffer[i], x->write_index[i],
                                      (float)x->delay_samples[i] + mod);

            x->mod_phase[i] += x->mod_phase_inc[i];
            if (x->mod_phase[i] >= (float)(2.0 * M_PI)) {
                x->mod_phase[i] -= (float)(2.0 * M_PI);
            }
        }

        out_taps[i] = d;

        x->lp_state[i] += x->lp_coeff * (d - x->lp_state[i]);
        fb_input[i] = x->lp_state[i] * x->feedback_gain[i];
    }

    if (x->matrix_type == FDN_MATRIX_HADAMARD) {
        fdn_hadamard8(fb_input, fb);
    } else {
        fdn_householder8(fb_input, fb);
    }

    for (int i = 0; i < FDN_N; ++i) {
        x->buffer[i][x->write_index[i]] = tank_in + fb[i];

        if (++x->write_index[i] >= x->buffer_size[i]) {
            x->write_index[i] = 0;
        }
    }

    float reverb = 0.25f * (out_taps[0] - out_taps[1] + out_taps[2] - out_taps[3] + out_taps[4] -
                            out_taps[5] + out_taps[6] - out_taps[7]);

    return x->dry * in + x->wet * reverb;
}

// ─────────────────────────────────────
static void fdn_tilde_set(t_fdn_tilde *x, t_symbol *s, int argc, t_atom *argv) {
    if (!s) {
        return;
    }

    const char *method = s->s_name;
    if (!strcmp(method, "rt60")) {
        if (argc == 1) {
            x->rt60 = std::clamp(atom_getfloat(argv), 0.05f, 30.0f);
            fdn_update_coefficients(x);
        }
    } else if (!strcmp(method, "damping")) {
        if (argc == 1) {
            x->damping = std::clamp(atom_getfloat(argv), 20.0f, 20000.0f);
            fdn_update_coefficients(x);
        }
    } else if (!strcmp(method, "wet")) {
        if (argc == 1) {
            x->wet = std::clamp(atom_getfloat(argv), 0.0f, 2.0f);
        }
    } else if (!strcmp(method, "dry")) {
        if (argc == 1) {
            x->dry = std::clamp(atom_getfloat(argv), 0.0f, 2.0f);
        }
    } else if (!strcmp(method, "input")) {
        if (argc == 1) {
            x->input_gain = std::clamp(atom_getfloat(argv), 0.0f, 2.0f);
        }
    } else if (!strcmp(method, "roomsize")) {
        if (argc == 1) {
            x->roomsize = std::clamp(atom_getfloat(argv), 0.5f, 2.0f);
            fdn_resize_buffers(x);
            fdn_update_coefficients(x);
        }
    } else if (!strcmp(method, "predelay")) {
        if (argc == 1) {
            int max_samples = (int)x->predelay_buffer.size();
            x->predelay_ms = std::clamp(atom_getfloat(argv), 0.0f, FDN_MAX_PREDELAY_MS);
            x->predelay_samples = std::clamp((int)roundf(x->predelay_ms * 0.001f * (float)x->sr), 0,
                                             std::max(0, max_samples - 1));
        }
    } else if (!strcmp(method, "diffusion")) {
        if (argc == 1) {
            x->diffusion = std::clamp(atom_getfloat(argv), 0.0f, 1.0f);
        }
    } else if (!strcmp(method, "moddepth")) {
        if (argc == 1) {
            x->moddepth = std::clamp(atom_getfloat(argv), 0.0f, FDN_MAX_MOD_DEPTH_SAMPLES);
        }
    } else if (!strcmp(method, "matrix")) {
        if (argc == 1 && argv->a_type == A_SYMBOL) {
            t_symbol *name = atom_getsymbol(argv);
            if (name == gensym("hadamard")) {
                x->matrix_type = FDN_MATRIX_HADAMARD;
            } else if (name == gensym("householder")) {
                x->matrix_type = FDN_MATRIX_HOUSEHOLDER;
            }
        }
    } else if (!strcmp(method, "clear")) {
        fdn_clear(x);
    }
}

// ─────────────────────────────────────
static t_int *fdn_tilde_perform(t_int *w) {
    t_fdn_tilde *x = (t_fdn_tilde *)w[1];
    t_sample *in = (t_sample *)w[2];
    t_sample *out = (t_sample *)w[3];
    int n = (int)w[4];

    while (n--) {
        *out++ = (t_sample)fdn_tick(x, (float)*in++);
    }

    return (w + 5);
}

// ─────────────────────────────────────
static void fdn_tilde_dsp(t_fdn_tilde *x, t_signal **sp) {
    double new_sr = sp[0]->s_sr > 0 ? sp[0]->s_sr : 44100.0;

    if (new_sr != x->sr || x->predelay_buffer.empty()) {
        x->sr = new_sr;
        fdn_resize_buffers(x);
        fdn_resize_predelay(x);
        fdn_resize_diffusers(x);
        fdn_update_coefficients(x);
        fdn_clear(x);
    }

    dsp_add(fdn_tilde_perform, 4, x, sp[0]->s_vec, sp[1]->s_vec, sp[0]->s_n);
}

// ─────────────────────────────────────
static void *fdn_tilde_new(t_symbol *s, int argc, t_atom *argv) {
    t_fdn_tilde *x = (t_fdn_tilde *)pd_new(fdn_class);

    x->sr = sys_getsr() > 0 ? sys_getsr() : 44100.0;

    x->rt60 = 1.2f;
    x->damping = 10000.0f;
    x->wet = 0.25f;
    x->dry = 1.0f;
    x->input_gain = 0.25f;
    x->lp_coeff = 0.5f;
    x->roomsize = 1.0f;
    x->predelay_ms = 0.0f;
    x->diffusion = 0.35f;
    x->moddepth = 0.3f;
    x->matrix_type = FDN_MATRIX_HOUSEHOLDER;

    float defaults[FDN_N] = {29.7f, 37.1f, 41.1f, 43.7f, 53.1f, 61.7f, 67.9f, 71.3f};
    float mod_freqs[FDN_N] = {0.07f, 0.09f, 0.11f, 0.13f, 0.17f, 0.19f, 0.21f, 0.23f};
    float diff_ms[FDN_DIFFUSERS] = {4.7f, 8.3f, 12.9f, 17.1f};

    for (int i = 0; i < FDN_N; ++i) {
        x->base_delay_ms[i] = defaults[i];
        x->delay_ms[i] = defaults[i];
        x->delay_samples[i] = 1;
        x->buffer_size[i] = 1;
        x->write_index[i] = 0;
        x->feedback_gain[i] = 0.0f;
        x->lp_state[i] = 0.0f;
        x->mod_phase[i] = (float)i * 0.731f;
        x->mod_freq[i] = mod_freqs[i];
        x->mod_phase_inc[i] = 0.0f;
    }

    for (int i = 0; i < FDN_DIFFUSERS; ++i) {
        x->diffuser_delay_ms[i] = diff_ms[i];
        x->diffuser_delay_samples[i] = 1;
        x->diffuser_write_index[i] = 0;
    }

    x->predelay_samples = 0;
    x->predelay_write_index = 0;

    fdn_resize_buffers(x);
    fdn_resize_predelay(x);
    fdn_resize_diffusers(x);
    fdn_update_coefficients(x);
    fdn_clear(x);

    x->x_out = outlet_new(&x->x_obj, &s_signal);

    return x;
}

// ─────────────────────────────────────
static void fdn_tilde_free(t_fdn_tilde *x) { outlet_free(x->x_out); }

// ─────────────────────────────────────
extern "C" void setup_x0x2efdn_tilde(void) {
    fdn_class = class_new(gensym("x.fdn~"), (t_newmethod)fdn_tilde_new, (t_method)fdn_tilde_free,
                          sizeof(t_fdn_tilde), CLASS_DEFAULT, A_GIMME, 0);

    CLASS_MAINSIGNALIN(fdn_class, t_fdn_tilde, f);

    class_addmethod(fdn_class, (t_method)fdn_tilde_dsp, gensym("dsp"), A_CANT, 0);

    class_addmethod(fdn_class, (t_method)fdn_tilde_set, gensym("rt60"), A_GIMME, 0);
    class_addmethod(fdn_class, (t_method)fdn_tilde_set, gensym("damping"), A_GIMME, 0);
    class_addmethod(fdn_class, (t_method)fdn_tilde_set, gensym("wet"), A_GIMME, 0);
    class_addmethod(fdn_class, (t_method)fdn_tilde_set, gensym("dry"), A_GIMME, 0);
    class_addmethod(fdn_class, (t_method)fdn_tilde_set, gensym("input"), A_GIMME, 0);
    class_addmethod(fdn_class, (t_method)fdn_tilde_set, gensym("roomsize"), A_GIMME, 0);
    class_addmethod(fdn_class, (t_method)fdn_tilde_set, gensym("predelay"), A_GIMME, 0);
    class_addmethod(fdn_class, (t_method)fdn_tilde_set, gensym("diffusion"), A_GIMME, 0);
    class_addmethod(fdn_class, (t_method)fdn_tilde_set, gensym("moddepth"), A_GIMME, 0);
    class_addmethod(fdn_class, (t_method)fdn_tilde_set, gensym("matrix"), A_GIMME, 0);
    class_addmethod(fdn_class, (t_method)fdn_tilde_set, gensym("clear"), A_GIMME, 0);
}
