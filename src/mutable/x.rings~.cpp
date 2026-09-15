extern "C" {
#include "m_pd.h"
}

#include <cstring>
#include <xmmintrin.h>

#include <rings/dsp/part.h>
#include <rings/dsp/strummer.h>
#include <rings/dsp/string_synth_part.h>

using namespace rings;

// ─────────────────────────────────────

static constexpr size_t kRingsBlockSize = rings::kMaxBlockSize;
static constexpr size_t kRingsReverbBufferSize = 32768;
static constexpr size_t kRingsOutputFifoSize = kRingsBlockSize * 2;
static constexpr float kNoiseGateThreshold = 0.00003f;

static t_class *rings_tilde_class;

typedef struct _rings_tilde {
    t_object x_obj;
    t_float f;

    t_outlet *out_left;
    t_outlet *out_right;

    rings::Part part;
    rings::Patch patch;
    rings::PerformanceState performance;
    rings::StringSynthPart string_synth;
    rings::Strummer strummer;

    uint16_t reverb_buffer[kRingsReverbBufferSize];

    float in_fifo[kRingsBlockSize];
    size_t in_fifo_count;

    float out_fifo_left[kRingsOutputFifoSize];
    float out_fifo_right[kRingsOutputFifoSize];
    size_t out_fifo_read;
    size_t out_fifo_write;
    size_t out_fifo_count;

    float in_buffer[kRingsBlockSize];
    float out_buffer[kRingsBlockSize];
    float aux_buffer[kRingsBlockSize];

    float in_level;
    float current_tonic;
    int pending_strum;
    int easter_egg;

    int initialized;
} t_rings_tilde;

// ─────────────────────────────────────
static void rings_tilde_note(t_rings_tilde *x, t_floatarg f) {
    // Interpret incoming note as MIDI note and store the offset from tonic.
    x->performance.note = f - x->current_tonic;
}

static void rings_tilde_tonic(t_rings_tilde *x, t_floatarg f) {
    x->current_tonic = f;
    x->performance.tonic = f;
}

static void rings_tilde_fm(t_rings_tilde *x, t_floatarg f) {
    x->performance.fm = f;
}

static void rings_tilde_strum(t_rings_tilde *x) {
    x->pending_strum = 1;
}

inline float clamp01(float v) {
    return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
}

inline int clamp_int(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void rings_tilde_structure(t_rings_tilde *x, t_floatarg f) {
    x->patch.structure = clamp01(f);
}

static void rings_tilde_damping(t_rings_tilde *x, t_floatarg f) {
    x->patch.damping = clamp01(f);
}

static void rings_tilde_brightness(t_rings_tilde *x, t_floatarg f) {
    x->patch.brightness = clamp01(f);
}

static void rings_tilde_position(t_rings_tilde *x, t_floatarg f) {
    x->patch.position = clamp01(f);
}

static void rings_tilde_internal_exciter(t_rings_tilde *x, t_floatarg f) {
    x->performance.internal_exciter = (f != 0.0f);
}

static void rings_tilde_internal_strum(t_rings_tilde *x, t_floatarg f) {
    x->performance.internal_strum = (f != 0.0f);
}

static void rings_tilde_internal_note(t_rings_tilde *x, t_floatarg f) {
    x->performance.internal_note = (f != 0.0f);
}

static void rings_tilde_model(t_rings_tilde *x, t_floatarg f) {
    // RESONATOR_MODEL_MODAL,
    // RESONATOR_MODEL_SYMPATHETIC_STRING,
    // RESONATOR_MODEL_STRING,
    //
    // // Bonus!
    // RESONATOR_MODEL_FM_VOICE,
    // RESONATOR_MODEL_SYMPATHETIC_STRING_QUANTIZED,
    // RESONATOR_MODEL_STRING_AND_REVERB,
    // RESONATOR_MODEL_LAST

    int model = clamp_int((int)f, 0, rings::RESONATOR_MODEL_LAST - 1);
    x->part.set_model((rings::ResonatorModel)model);
}

// ─────────────────────────────────────
static void rings_tilde_polyphony(t_rings_tilde *x, t_floatarg f) {
    int p = clamp_int((int)f, 1, 4);
    x->part.set_polyphony(p);
    x->string_synth.set_polyphony(p);
}

// ─────────────────────────────────────
static void rings_tilde_easter(t_rings_tilde *x, t_floatarg f) {
    x->easter_egg = (f != 0.0f);
}

// ─────────────────────────────────────
static void rings_tilde_chord(t_rings_tilde *x, t_floatarg f) {
    int chord = clamp_int((int)f, 0, rings::kNumChords - 1);
    x->performance.chord = chord;
}

// ─────────────────────────────────────
static void rings_tilde_bypass(t_rings_tilde *x, t_floatarg f) {
    x->part.set_bypass(f != 0.0f);
}

// ─────────────────────────────────────
static inline void rings_tilde_reset_fifos(t_rings_tilde *x) {
    x->in_fifo_count = 0;
    x->out_fifo_read = 0;
    x->out_fifo_write = 0;
    x->out_fifo_count = 0;
}

// ─────────────────────────────────────
static inline void rings_tilde_fifo_push(t_rings_tilde *x, float left, float right) {
    if (x->out_fifo_count >= kRingsOutputFifoSize) {
        x->out_fifo_read = (x->out_fifo_read + 1) % kRingsOutputFifoSize;
        x->out_fifo_count = kRingsOutputFifoSize - 1;
    }
    x->out_fifo_left[x->out_fifo_write] = left;
    x->out_fifo_right[x->out_fifo_write] = right;
    x->out_fifo_write = (x->out_fifo_write + 1) % kRingsOutputFifoSize;
    ++x->out_fifo_count;
}

// ─────────────────────────────────────
static inline int rings_tilde_fifo_pop(t_rings_tilde *x, float *left, float *right) {
    if (x->out_fifo_count == 0) {
        *left = 0.0f;
        *right = 0.0f;
        return 0;
    }
    *left = x->out_fifo_left[x->out_fifo_read];
    *right = x->out_fifo_right[x->out_fifo_read];
    x->out_fifo_read = (x->out_fifo_read + 1) % kRingsOutputFifoSize;
    --x->out_fifo_count;
    return 1;
}

// ─────────────────────────────────────
static void rings_tilde_process_block(t_rings_tilde *x) {
    x->performance.strum = (x->pending_strum != 0);
    x->pending_strum = 0;

    if (x->easter_egg) {
        for (size_t i = 0; i < kRingsBlockSize; ++i) {
            x->in_buffer[i] = x->in_fifo[i];
        }
        x->strummer.Process(NULL, kRingsBlockSize, &x->performance);
        x->string_synth.Process(x->performance, x->patch, x->in_buffer, x->out_buffer,
                                x->aux_buffer, kRingsBlockSize);
    } else {
        for (size_t i = 0; i < kRingsBlockSize; ++i) {
            float in_sample = x->in_fifo[i];
            float error = in_sample * in_sample - x->in_level;
            x->in_level += error * (error > 0.0f ? 0.1f : 0.0001f);
            float gain = x->in_level <= kNoiseGateThreshold
                             ? (1.0f / kNoiseGateThreshold) * x->in_level
                             : 1.0f;
            x->in_buffer[i] = gain * in_sample;
        }
        x->strummer.Process(x->in_buffer, kRingsBlockSize, &x->performance);
        x->part.Process(x->performance, x->patch, x->in_buffer, x->out_buffer, x->aux_buffer,
                        kRingsBlockSize);
    }

    x->performance.strum = false;

    for (size_t i = 0; i < kRingsBlockSize; ++i) {
        rings_tilde_fifo_push(x, x->out_buffer[i], x->aux_buffer[i]);
    }
}

// ─────────────────────────────────────
static t_int *rings_tilde_perform(t_int *w) {
    t_rings_tilde *x = (t_rings_tilde *)(w[1]);
    t_sample *in = (t_sample *)(w[2]);
    t_sample *out1 = (t_sample *)(w[3]);
    t_sample *out2 = (t_sample *)(w[4]);
    int n = (int)(w[5]);

    if (!x->initialized) {
        return w + 6;
    }

    for (int i = 0; i < n; ++i) {
        x->in_fifo[x->in_fifo_count] = in[i];
        ++x->in_fifo_count;

        if (x->in_fifo_count == kRingsBlockSize) {
            rings_tilde_process_block(x);
            x->in_fifo_count = 0;
        }

        float left = 0.0f;
        float right = 0.0f;
        rings_tilde_fifo_pop(x, &left, &right);
        out1[i] = left;
        out2[i] = right;
    }

    return w + 6;
}

// ─────────────────────────────────────
static void rings_tilde_dsp(t_rings_tilde *x, t_signal **sp) {
    rings_tilde_reset_fifos(x);
    x->in_level = 0.0f;
    x->pending_strum = 0;
    dsp_add(rings_tilde_perform, 5, x,
            sp[0]->s_vec, // input
            sp[1]->s_vec, // left
            sp[2]->s_vec, // right
            sp[0]->s_n);
}

// ─────────────────────────────────────
static void *rings_tilde_new(void) {
    t_rings_tilde *x = (t_rings_tilde *)pd_new(rings_tilde_class);

    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);

    x->out_left = outlet_new(&x->x_obj, &s_signal);
    x->out_right = outlet_new(&x->x_obj, &s_signal);

    std::memset(&x->patch, 0, sizeof(x->patch));
    std::memset(&x->performance, 0, sizeof(x->performance));

    x->part.Init(x->reverb_buffer);
    x->string_synth.Init(x->reverb_buffer);
    x->strummer.Init(0.01f, rings::kSampleRate / rings::kMaxBlockSize);

    rings_tilde_reset_fifos(x);
    x->in_level = 0.0f;
    x->pending_strum = 0;
    x->easter_egg = 0;
    x->current_tonic = 60.0f;

    // Default patch (you should expose these as messages later)
    x->patch.structure = 0.25f;
    x->patch.brightness = 0.5f;
    x->patch.damping = 0.7f;
    x->patch.position = 0.5f;

    x->part.set_polyphony(1);
    x->part.set_model(rings::RESONATOR_MODEL_MODAL);

    x->performance.note = 0.0f;
    x->performance.tonic = x->current_tonic;
    x->performance.fm = 0.0f;
    x->performance.internal_exciter = true;
    x->performance.strum = false;
    x->performance.internal_strum = false;
    x->performance.internal_note = false;
    x->performance.chord = 0;

    x->initialized = 1;

    return (void *)x;
}

// ─────────────────────────────────────
extern "C" void setup_x0x2erings_tilde(void) {
    rings_tilde_class = class_new(gensym("x.rings~"), (t_newmethod)rings_tilde_new, 0,
                                  sizeof(t_rings_tilde), CLASS_DEFAULT, A_NULL);

    CLASS_MAINSIGNALIN(rings_tilde_class, t_rings_tilde, f);

    class_addmethod(rings_tilde_class, (t_method)rings_tilde_dsp, gensym("dsp"), A_CANT, 0);

    class_addmethod(rings_tilde_class, (t_method)rings_tilde_note, gensym("note"), A_FLOAT, 0);
    class_addmethod(rings_tilde_class, (t_method)rings_tilde_tonic, gensym("tonic"), A_FLOAT, 0);
    class_addmethod(rings_tilde_class, (t_method)rings_tilde_fm, gensym("fm"), A_FLOAT, 0);

    class_addmethod(rings_tilde_class, (t_method)rings_tilde_strum, gensym("strum"), A_NULL);
    class_addmethod(rings_tilde_class, (t_method)rings_tilde_internal_exciter, gensym("internal"),
                    A_FLOAT, 0);
    class_addmethod(rings_tilde_class, (t_method)rings_tilde_internal_strum,
                    gensym("internal_strum"), A_FLOAT, 0);
    class_addmethod(rings_tilde_class, (t_method)rings_tilde_internal_note, gensym("internal_note"),
                    A_FLOAT, 0);

    class_addmethod(rings_tilde_class, (t_method)rings_tilde_structure, gensym("structure"),
                    A_FLOAT, 0);
    class_addmethod(rings_tilde_class, (t_method)rings_tilde_brightness, gensym("brightness"),
                    A_FLOAT, 0);
    class_addmethod(rings_tilde_class, (t_method)rings_tilde_damping, gensym("damping"), A_FLOAT,
                    0);
    class_addmethod(rings_tilde_class, (t_method)rings_tilde_position, gensym("position"), A_FLOAT,
                    0);

    class_addmethod(rings_tilde_class, (t_method)rings_tilde_chord, gensym("chord"), A_FLOAT, 0);
    class_addmethod(rings_tilde_class, (t_method)rings_tilde_easter, gensym("easter"), A_FLOAT, 0);
    class_addmethod(rings_tilde_class, (t_method)rings_tilde_bypass, gensym("bypass"), A_FLOAT, 0);

    class_addmethod(rings_tilde_class, (t_method)rings_tilde_model, gensym("model"), A_FLOAT, 0);
    class_addmethod(rings_tilde_class, (t_method)rings_tilde_polyphony, gensym("polyphony"),
                    A_FLOAT, 0);
}
