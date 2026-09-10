#include "m_pd.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>

static t_class *rperlin_class;

typedef struct _rperlin {
    t_object x_obj;

    t_outlet *outlet;

    float min;
    float max;

    double position;
    double step;

    uint32_t seed;
} t_rperlin;

// ─────────────────────────────────────
static inline uint32_t rperlin_hash(int x, uint32_t seed) {
    uint32_t h = static_cast<uint32_t>(x) ^ seed;

    h ^= h >> 16;
    h *= 0x7feb352d;
    h ^= h >> 15;
    h *= 0x846ca68b;
    h ^= h >> 16;

    return h;
}

// ─────────────────────────────────────
static inline double rperlin_gradient(int x, uint32_t seed) {
    uint32_t h = rperlin_hash(x, seed);

    // Gradient in approximately [-1, 1]
    return static_cast<double>(h) / static_cast<double>(UINT32_MAX) * 2.0 - 1.0;
}

// ─────────────────────────────────────
static inline double rperlin_fade(double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

// ─────────────────────────────────────
static inline double rperlin_lerp(double a, double b, double t) { return a + t * (b - a); }

// ─────────────────────────────────────
static double rperlin_noise(double x, uint32_t seed) {
    int x0 = static_cast<int>(std::floor(x));
    int x1 = x0 + 1;
    double local = x - static_cast<double>(x0);
    double g0 = rperlin_gradient(x0, seed);
    double g1 = rperlin_gradient(x1, seed);

    double n0 = g0 * local;
    double n1 = g1 * (local - 1.0);
    double u = rperlin_fade(local);
    double value = rperlin_lerp(n0, n1, u) * 2.0;
    return std::clamp(value, -1.0, 1.0);
}

// ─────────────────────────────────────
static void rperlin_bang(t_rperlin *x) {
    double noise = rperlin_noise(x->position, x->seed);
    double normalized = noise * 0.5 + 0.5;
    double output = x->min + normalized * (x->max - x->min);
    outlet_float(x->outlet, static_cast<t_float>(output));
    x->position += x->step;
}

// ─────────────────────────────────────
static void rperlin_range(t_rperlin *x, t_floatarg min, t_floatarg max) {
    if (min <= max) {
        x->min = min;
        x->max = max;
    } else {
        x->min = max;
        x->max = min;
    }
}

// ─────────────────────────────────────
static void rperlin_step(t_rperlin *x, t_floatarg step) {
    x->step = std::max(0.000001, static_cast<double>(step));
}

// ─────────────────────────────────────
static void rperlin_seed(t_rperlin *x, t_floatarg seed) {
    uint32_t s = static_cast<uint32_t>(seed);
    if (s == 0) {
        s = 1;
    }

    x->seed = s;
    x->position = static_cast<double>(rperlin_hash(static_cast<int>(s), s) % 100000) / 100.0;
}

// ─────────────────────────────────────
static void rperlin_reset(t_rperlin *x) {
    uint32_t h = rperlin_hash(static_cast<int>(x->seed), x->seed);

    x->position = static_cast<double>(h % 100000) / 100.0;
}

// ─────────────────────────────────────
static void *rperlin_new(t_floatarg min, t_floatarg max) {
    auto *x = reinterpret_cast<t_rperlin *>(pd_new(rperlin_class));
    if (min <= max) {
        x->min = min;
        x->max = max;
    } else {
        x->min = max;
        x->max = min;
    }

    if (min == 0.0f && max == 0.0f) {
        x->min = 0.0f;
        x->max = 1.0f;
    }

    std::random_device rd;
    x->seed = static_cast<uint32_t>(rd());
    if (x->seed == 0) {
        x->seed = 1;
    }
    x->position = static_cast<double>(rd() % 100000) / 100.0;
    x->step = 0.05;
    x->outlet = outlet_new(&x->x_obj, &s_float);
    return x;
}

// ─────────────────────────────────────
extern "C" void rperlin_setup(void) {
    rperlin_class = class_new(gensym("rperlin"), reinterpret_cast<t_newmethod>(rperlin_new),
                              nullptr, sizeof(t_rperlin), CLASS_DEFAULT, A_DEFFLOAT, A_DEFFLOAT, 0);
    class_addbang(rperlin_class, reinterpret_cast<t_method>(rperlin_bang));
    class_addmethod(rperlin_class, reinterpret_cast<t_method>(rperlin_range), gensym("range"),
                    A_FLOAT, A_FLOAT, 0);
    class_addmethod(rperlin_class, reinterpret_cast<t_method>(rperlin_step), gensym("step"),
                    A_FLOAT, 0);
    class_addmethod(rperlin_class, reinterpret_cast<t_method>(rperlin_seed), gensym("seed"),
                    A_FLOAT, 0);
    class_addmethod(rperlin_class, reinterpret_cast<t_method>(rperlin_reset), gensym("reset"),
                    A_NULL, 0);
}
