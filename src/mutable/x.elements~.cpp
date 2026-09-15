extern "C" {
#include "m_pd.h"
}

#include <cstring>
#include <xmmintrin.h>

#include <elements/dsp/part.h>

using namespace elements;

// ─────────────────────────────────────

static t_class *elements_tilde_class;

typedef struct _elements_tilde {
    t_object x_obj;
    t_float f;

    Part part;
    uint16_t reverb_buffer[32768];

    float in_silence[16];
    float out_main[16];
    float out_aux[16];

    // performance
    PerformanceState x_perf;

    float x_note;
    float x_velocity;
    float x_gate;
    float x_modulation;
    float x_last_gate;

    t_outlet *x_level_out;

    t_outlet *out_left;
    t_outlet *out_right;

    float phase;
} t_elements_tilde;

// ─────────────────────────────────────
void elements_list(t_elements_tilde *x, t_symbol *s, int ac, t_atom *av) {
    float f = atom_getfloat(av);
    float vel = atom_getfloat(av + 1);
    x->x_note = f;
    x->x_velocity = vel > 0 ? vel / 127.f : 0.f;
    x->x_gate = vel > 0 ? 1.f : 0.f;
}

// ─────────────────────────────────────
void elements_note(t_elements_tilde *x, t_floatarg note, t_floatarg vel) {
    x->x_note = note;
    x->x_velocity = vel > 0 ? vel / 127.f : 0.f;
    x->x_gate = vel > 0 ? 1.f : 0.f;
}

// ─────────────────────────────────────
void elements_gate(t_elements_tilde *x, t_floatarg f) {
    x->x_gate = f != 0;
}

// ─────────────────────────────────────
void elements_velocity(t_elements_tilde *x, t_floatarg f) {
    x->x_velocity = f;
}

// ─────────────────────────────────────
void elements_level(t_elements_tilde *x, t_floatarg f) {
    x->x_perf.strength = f;
}

// ─────────────────────────────────────
void elements_pitchbend(t_elements_tilde *x, t_floatarg f) {
    // assume [-1, 1] range
    x->x_modulation = f * 2.0f; // ~2 semitones
}

// ─────────────────────────────────────
void elements_aftertouch(t_elements_tilde *x, t_floatarg f) {
    x->x_perf.strength = f;
}

// ─────────────────────────────────────
void elements_geometry(t_elements_tilde *x, t_floatarg f) {
    x->part.mutable_patch()->resonator_geometry = f;
}

// ─────────────────────────────────────
void elements_brightness(t_elements_tilde *x, t_floatarg f) {
    x->part.mutable_patch()->resonator_brightness = f;
}

// ─────────────────────────────────────
void elements_damping(t_elements_tilde *x, t_floatarg f) {
    x->part.mutable_patch()->resonator_damping = f;
}

// ─────────────────────────────────────
void elements_position(t_elements_tilde *x, t_floatarg f) {
    x->part.mutable_patch()->resonator_position = f;
}

// ─────────────────────────────────────
void elements_space(t_elements_tilde *x, t_floatarg f) {
    x->part.mutable_patch()->space = f;
}

// ─────────────────────────────────────
static t_int *elements_tilde_perform(t_int *w) {
    t_elements_tilde *x = (t_elements_tilde *)(w[1]);
    t_sample *out1 = (t_sample *)(w[2]);
    t_sample *out2 = (t_sample *)(w[3]);
    int n = (int)(w[4]);

    int remaining = n;
    int offset = 0;

    while (remaining > 0) {
        int block = remaining >= 16 ? 16 : remaining;

        x->x_perf.note = x->x_note;
        x->x_perf.modulation = x->x_modulation;
        x->x_perf.gate = x->x_gate;

        float strike[16];
        float blow[16];

        for (int i = 0; i < block; ++i) {
            strike[i] = (x->x_gate && !x->x_last_gate) ? x->x_velocity : 0.0f;
            blow[i] = x->x_gate ? x->x_velocity * 0.25f : 0.0f;
        }

        x->x_last_gate = x->x_gate;

        x->part.Process(x->x_perf, blow, strike, x->out_main, x->out_aux, block);

        for (int j = 0; j < block; ++j) {
            out1[offset + j] = x->out_main[j];
            out2[offset + j] = x->out_aux[j];
        }

        offset += block;
        remaining -= block;
    }

    // one-shot trigger behavior
    x->x_gate = 0;

    return w + 5;
}

// ─────────────────────────────────────

static void elements_tilde_dsp(t_elements_tilde *x, t_signal **sp) {
    dsp_add(elements_tilde_perform, 4, x, sp[0]->s_vec, sp[1]->s_vec, sp[0]->s_n);
}

// ─────────────────────────────────────

static void *elements_tilde_new(void) {
    t_elements_tilde *x = (t_elements_tilde *)pd_new(elements_tilde_class);

    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);

    std::memset(x->in_silence, 0, sizeof(x->in_silence));

    x->part.Init(x->reverb_buffer);

    Patch *p = x->part.mutable_patch();

    // Minimal sane defaults (important — otherwise silence or instability)
    p->exciter_strike_level = 0.5f;
    p->resonator_geometry = 0.4f;
    p->resonator_brightness = 0.7f;
    p->resonator_damping = 0.5f;
    p->resonator_position = 0.3f;
    p->space = 0.2f;

    x->out_left = outlet_new(&x->x_obj, &s_signal);
    x->out_right = outlet_new(&x->x_obj, &s_signal);

    return (void *)x;
}

// ─────────────────────────────────────

extern "C" void setup_x0x2eelements_tilde(void) {
    elements_tilde_class = class_new(gensym("x.elements~"), (t_newmethod)elements_tilde_new, 0,
                                     sizeof(t_elements_tilde), CLASS_DEFAULT, A_NULL);

    CLASS_MAINSIGNALIN(elements_tilde_class, t_elements_tilde, f);

    class_addlist(elements_tilde_class, (t_method)elements_list);
    class_addmethod(elements_tilde_class, (t_method)elements_note, gensym("note"), A_FLOAT, A_FLOAT,
                    0);

    class_addmethod(elements_tilde_class, (t_method)elements_tilde_dsp, gensym("dsp"), A_CANT, 0);

    class_addmethod(elements_tilde_class, (t_method)elements_note, gensym("note"), A_FLOAT, A_FLOAT,
                    0);
    class_addmethod(elements_tilde_class, (t_method)elements_gate, gensym("gate"), A_FLOAT, 0);
    class_addmethod(elements_tilde_class, (t_method)elements_velocity, gensym("velocity"), A_FLOAT,
                    0);
    class_addmethod(elements_tilde_class, (t_method)elements_level, gensym("level"), A_FLOAT, 0);

    class_addmethod(elements_tilde_class, (t_method)elements_pitchbend, gensym("bend"), A_FLOAT, 0);
    class_addmethod(elements_tilde_class, (t_method)elements_aftertouch, gensym("aftertouch"),
                    A_FLOAT, 0);

    class_addmethod(elements_tilde_class, (t_method)elements_geometry, gensym("geometry"), A_FLOAT,
                    0);

    class_addmethod(elements_tilde_class, (t_method)elements_brightness, gensym("brightness"),
                    A_FLOAT, 0);

    class_addmethod(elements_tilde_class, (t_method)elements_damping, gensym("damping"), A_FLOAT,
                    0);

    class_addmethod(elements_tilde_class, (t_method)elements_position, gensym("position"), A_FLOAT,
                    0);

    class_addmethod(elements_tilde_class, (t_method)elements_space, gensym("space"), A_FLOAT, 0);
}
