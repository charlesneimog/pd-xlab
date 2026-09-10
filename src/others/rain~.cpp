#include "m_pd.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

static t_class *rain_tilde_class;
constexpr double PI = 3.14159265358979323846;
constexpr int MAX_DROPS = 128;

// ─────────────────────────────────────
struct Drop {
    float y1 = 0.0f;
    float y2 = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
    int samples_left = 0;
};

// ─────────────────────────────────────
typedef struct _rain_tilde {
    t_object x_obj;

    t_outlet *outlet;

    double sample_rate;

    uint32_t rng_state;

    // Parameters
    float density;    // drops / second
    float brightness; // 0..1
    float bed_gain;   // continuous noise
    float drop_gain;  // individual droplets

    // Noise filters
    float noise_lp;
    float noise_dc;

    Drop drops[MAX_DROPS];

} t_rain_tilde;

// ─────────────────────────────────────
static void rain_density(t_rain_tilde *x, t_floatarg value) {
    x->density = std::max(0.0f, std::min(static_cast<float>(value), 10000.0f));
}

// ─────────────────────────────────────
static void rain_brightness(t_rain_tilde *x, t_floatarg value) {
    x->brightness = std::max(0.0f, std::min(static_cast<float>(value), 1.0f));
}

// ─────────────────────────────────────
static void rain_bed(t_rain_tilde *x, t_floatarg value) {
    x->bed_gain = std::max(0.0f, static_cast<float>(value));
}

// ─────────────────────────────────────
static void rain_drops(t_rain_tilde *x, t_floatarg value) {
    x->drop_gain = std::max(0.0f, static_cast<float>(value));
}

// ─────────────────────────────────────
static void rain_seed(t_rain_tilde *x, t_floatarg value) {
    uint32_t seed = static_cast<uint32_t>(value);
    if (seed == 0) {
        seed = 1;
    }
    x->rng_state = seed;
}

// ─────────────────────────────────────
static inline uint32_t rain_rand(t_rain_tilde *x) {
    // xorshift32
    uint32_t s = x->rng_state;

    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;

    x->rng_state = s;

    return s;
}

// ─────────────────────────────────────
static inline float rain_rand01(t_rain_tilde *x) {
    return static_cast<float>(rain_rand(x) * (1.0 / 4294967295.0));
}

// ─────────────────────────────────────
static inline float rain_rand_bipolar(t_rain_tilde *x) { return rain_rand01(x) * 2.0f - 1.0f; }

// ─────────────────────────────────────
static void rain_trigger_drop(t_rain_tilde *x) {
    Drop *voice = nullptr;

    // Look for a free voice.
    for (int i = 0; i < MAX_DROPS; ++i) {
        if (x->drops[i].samples_left <= 0) {
            voice = &x->drops[i];
            break;
        }
    }

    // Voice stealing if necessary.
    if (!voice) {
        int index = rain_rand(x) % MAX_DROPS;
        voice = &x->drops[index];
    }

    float r1 = rain_rand01(x);
    float r2 = rain_rand01(x);
    float r3 = rain_rand01(x);

    float min_freq = 500.0f + x->brightness * 1000.0f;
    float max_freq = 3500.0f + x->brightness * 6500.0f;
    float freq_position = r1 * r1;
    float frequency = min_freq + freq_position * (max_freq - min_freq);
    frequency = std::min(frequency, static_cast<float>(x->sample_rate * 0.45));
    float decay = 0.003f + r2 * 0.032f;
    float amplitude = 0.03f + std::pow(r3, 4.0f) * 0.5f;
    amplitude *= x->drop_gain;
    if (rain_rand(x) & 1)
        amplitude = -amplitude;

    float radius = std::exp(-1.0f / (decay * static_cast<float>(x->sample_rate)));
    float omega = static_cast<float>(2.0 * PI * frequency / x->sample_rate);
    voice->a1 = 2.0f * radius * std::cos(omega);
    voice->a2 = -(radius * radius);
    voice->y1 = amplitude;
    voice->y2 = 0.0f;
    voice->samples_left = static_cast<int>(decay * x->sample_rate * 7.0);
}

// ─────────────────────────────────────
static t_int *rain_perform(t_int *w) {
    auto *x = reinterpret_cast<t_rain_tilde *>(w[1]);
    auto *out = reinterpret_cast<t_sample *>(w[2]);

    int n = static_cast<int>(w[3]);

    float density = x->density;
    float sr = static_cast<float>(x->sample_rate);

    /*
        Poisson approximation.

        Probability of a drop occurring during one sample.
    */

    float trigger_probability = density / sr;

    trigger_probability = std::min(trigger_probability, 0.95f);

    /*
        Continuous rain bed.

        brightness controls low-pass cutoff.
    */

    float cutoff = 1500.0f + x->brightness * 13500.0f;

    cutoff = std::min(cutoff, sr * 0.45f);

    float lp_coeff = std::exp(-2.0f * static_cast<float>(PI) * cutoff / sr);

    // ~100 Hz DC / low-frequency remover.
    float dc_coeff = std::exp(-2.0f * static_cast<float>(PI) * 100.0f / sr);

    for (int i = 0; i < n; ++i) {

        // ----------------------------------------------------
        // Continuous noise
        // ----------------------------------------------------

        float noise = rain_rand_bipolar(x);
        x->noise_lp = (1.0f - lp_coeff) * noise + lp_coeff * x->noise_lp;
        x->noise_dc = (1.0f - dc_coeff) * x->noise_lp + dc_coeff * x->noise_dc;

        float bed = x->noise_lp - x->noise_dc;

        bed *= x->bed_gain;

        if (rain_rand01(x) < trigger_probability)
            rain_trigger_drop(x);

        float drop_signal = 0.0f;
        for (int j = 0; j < MAX_DROPS; ++j) {
            Drop &v = x->drops[j];
            if (v.samples_left <= 0)
                continue;

            float y = v.a1 * v.y1 + v.a2 * v.y2;
            v.y2 = v.y1;
            v.y1 = y;
            drop_signal += y;
            --v.samples_left;
        }

        float signal = bed + drop_signal;
        signal = signal / (1.0f + std::fabs(signal));
        out[i] = signal;
    }

    return w + 4;
}

// ─────────────────────────────────────
static void rain_dsp(t_rain_tilde *x, t_signal **sp) {
    x->sample_rate = sp[0]->s_sr;

    dsp_add(rain_perform, 3, x, sp[0]->s_vec, sp[0]->s_n);
}

// ─────────────────────────────────────
static void *rain_new(t_floatarg density) {
    auto *x = reinterpret_cast<t_rain_tilde *>(pd_new(rain_tilde_class));
    x->sample_rate = sys_getsr();
    if (x->sample_rate <= 0)
        x->sample_rate = 44100.0;
    x->rng_state = 0x12345678u ^ static_cast<uint32_t>(reinterpret_cast<uintptr_t>(x));
    x->density = density > 0 ? density : 800.0f;
    x->brightness = 0.65f;
    x->bed_gain = 0.15f;
    x->drop_gain = 0.3f;
    x->noise_lp = 0.0f;
    x->noise_dc = 0.0f;
    for (auto &drop : x->drops)
        drop = Drop{};
    x->outlet = outlet_new(&x->x_obj, &s_signal);
    return x;
}

// ─────────────────────────────────────
extern "C" void rain_tilde_setup(void) {
    rain_tilde_class = class_new(gensym("rain~"), reinterpret_cast<t_newmethod>(rain_new), nullptr,
                                 sizeof(t_rain_tilde), CLASS_DEFAULT, A_DEFFLOAT, 0);
    class_addmethod(rain_tilde_class, reinterpret_cast<t_method>(rain_dsp), gensym("dsp"), A_CANT,
                    0);
    class_addmethod(rain_tilde_class, reinterpret_cast<t_method>(rain_density), gensym("density"),
                    A_FLOAT, 0);
    class_addmethod(rain_tilde_class, reinterpret_cast<t_method>(rain_brightness),
                    gensym("brightness"), A_FLOAT, 0);
    class_addmethod(rain_tilde_class, reinterpret_cast<t_method>(rain_bed), gensym("bed"), A_FLOAT,
                    0);
    class_addmethod(rain_tilde_class, reinterpret_cast<t_method>(rain_drops), gensym("drops"),
                    A_FLOAT, 0);
    class_addmethod(rain_tilde_class, reinterpret_cast<t_method>(rain_seed), gensym("seed"),
                    A_FLOAT, 0);
}
