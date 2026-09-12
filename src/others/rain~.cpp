#include "m_pd.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

// Miklavcic, Zita & Arvidsson, DAFx 2004, pp. 169–172, P_169.pdf.
// Equation numbers below refer ONLY to that paper. See rain~.md for limitations.
static t_class *rain_tilde_class;
constexpr double PI = 3.14159265358979323846;
constexpr int MAX_VOICES = 512, MAX_KERNEL = 128, MAX_PINCH = 2048, MAX_WATER = 16384;
constexpr double MAX_DENSITY = 10000;

// Sound-design layers below are perceptual additions, NOT DAFx equations.
struct Ring {
    double y1, y2, a1, a2;
};

struct TextureBand {
    double lo, hi, a, b, normalization, motion, target;
    int remaining;
};
constexpr int TEXTURE_BANDS = 6;

// Artistic material responses, not measured coefficients or paper equations.
// Keep ground/leaf for existing patches. Roof denotes a resonant metal sheet.
struct RainMaterial {
    const char *name;
    double frequencies[3];
    double damping, tone_gain, splash_gain, splash_seconds, attack_seconds;
    double cutoff, highpass, crinkle, impact_gain;
    double bed_weights[TEXTURE_BANDS];
};

static constexpr RainMaterial RAIN_MATERIALS[] = {
    {"ground", {470, 1130, 2190}, 1, 1, 1, 0.003, 0.0007, 0.8, 0, 0, 1, {1, 1, 1, 1, 1, 1}},
    {"leaf",
     {1300, 2700, 4300},
     0.7,
     0.8,
     1,
     0.003,
     0.0007,
     1.2,
     350,
     0.3,
     0.8,
     {0.3, 0.5, 0.8, 1, 1, 0.8}},
    {"roof",
     {310, 830, 1870},
     2.5,
     5,
     0.4,
     0.002,
     0.00015,
     1.1,
     600,
     0,
     0.8,
     {1.2, 1.3, 1, 0.7, 0.45, 0.25}},
    {"plastic",
     {680, 1540, 3260},
     0.65,
     1.6,
     1.1,
     0.010,
     0.0002,
     1.4,
     1100,
     0.9,
     1,
     {0.2, 0.4, 0.8, 1.2, 1.2, 1}},
    {"wood",
     {240, 560, 1210},
     1.1,
     5,
     0.35,
     0.002,
     0.00035,
     0.5,
     100,
     0,
     0.45,
     {1, 1.1, 0.8, 0.4, 0.2, 0.1}},
    {"dirt",
     {180, 390, 780},
     0.35,
     0.25,
     0.65,
     0.004,
     0.0018,
     0.3,
     70,
     0,
     0.12,
     {0.8, 1, 0.65, 0.25, 0.1, 0.05}},
    {"asphalt",
     {730, 1690, 3510},
     0.3,
     0.45,
     1,
     0.005,
     0.00035,
     1.15,
     750,
     0,
     0.65,
     {0.3, 0.55, 0.9, 1.1, 1, 0.7}},
};
constexpr int MATERIAL_COUNT = sizeof(RAIN_MATERIALS) / sizeof(RAIN_MATERIALS[0]);

struct RainVoice {
    double hard[MAX_KERNEL], water[MAX_KERNEL];
    int hard_size, water_size, hard_start, water_start, age, end;
    double amplitude, size_gain;
    Ring rings[3], plink;
    double splash_env, splash_decay, splash_lp, splash_coeff;
    double splash_low, splash_highpass_coeff, crinkle, flutter, flutter_coeff, impact_gain;
    double attack, attack_step;
    int detail_start, detail_end, plink_start;
    uint32_t noise_rng;
};

struct t_rain_tilde {
    t_object x_obj;
    t_outlet *out;
    double sample_rate, density, count_fraction;
    double radius_min, radius_max, inner, outer, height;
    double rho, c, amplitude, gain;
    double cone_length, opening_radius, splay;
    double pinch_rate, pinch[MAX_PINCH], water_delta[MAX_WATER];
    int pinch_size, water_size, active, pending;
    bool water_surface;
    uint32_t rng, texture_rng;
    bool hybrid;
    int material;
    double hybrid_mix, impact, splash, tone, plinks, bed, brightness, resonance;
    double arrival_hazard;
    double impact_smooth, splash_smooth, tone_smooth, plinks_smooth, bed_smooth;
    double brightness_smooth, intensity_smooth;
    TextureBand texture[TEXTURE_BANDS];
    double bed_weights[TEXTURE_BANDS];
    uint64_t dropped;
    RainVoice voices[MAX_VOICES];
};

// ─────────────────────────────────────
static double rain_uniform(t_rain_tilde *x) {
    uint32_t s = x->rng;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    x->rng = s;
    return s * (1.0 / 4294967296.0);
}

// ─────────────────────────────────────
static double rain_texture_random(uint32_t &state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state * (1.0 / 4294967296.0);
}

// ─────────────────────────────────────
static void rain_ring(Ring &r, double frequency, double seconds, double amplitude, double sr) {
    // Here I implement a normalized, strongly damped surface resonance.
    // This oscillator and its damping are sound-design additions, not Eq. 2.
    frequency = std::min(frequency, 0.42 * sr);
    double omega = 2 * PI * frequency / sr, radius = std::exp(-1 / (seconds * sr));
    r.a1 = 2 * radius * std::cos(omega);
    r.a2 = -radius * radius;
    r.y1 = amplitude * std::sin(omega);
    r.y2 = 0;
}

// ─────────────────────────────────────
static double rain_ring_sample(Ring &r) {
    double y = r.a1 * r.y1 + r.a2 * r.y2;
    r.y2 = r.y1;
    r.y1 = y;
    return y;
}

// ─────────────────────────────────────
static void rain_texture_prepare(t_rain_tilde *x) {
    const double edges[] = {120, 300, 700, 1500, 3000, 6000, 10000};
    for (int j = 0; j < TEXTURE_BANDS; ++j) {
        auto &b = x->texture[j];
        b = TextureBand{};
        b.a = std::exp(-2 * PI * std::min(edges[j], 0.45 * x->sample_rate) / x->sample_rate);
        b.b = std::exp(-2 * PI * std::min(edges[j + 1], 0.45 * x->sample_rate) / x->sample_rate);
        double variance = (1 - b.a) / (1 + b.a) + (1 - b.b) / (1 + b.b) -
                          2 * (1 - b.a) * (1 - b.b) / (1 - b.a * b.b);
        b.normalization = variance > 1e-9 ? 1 / std::sqrt(variance) : 0;
    }
}

// ─────────────────────────────────────
static double rain_background(t_rain_tilde *x, double smooth) {
    // Here I implement the diffuse background: six filtered-noise bands with
    // independent, slow envelope motion. This is a sound-design layer, not a
    // replacement for any event or equation in the paper.
    double result = 0;
    for (int j = 0; j < TEXTURE_BANDS; ++j) {
        auto &b = x->texture[j];
        if (b.remaining <= 0) {
            b.target = 2 * rain_texture_random(x->texture_rng) - 1;
            b.remaining = std::max(
                1, int((0.15 + 0.85 * rain_texture_random(x->texture_rng)) * x->sample_rate));
        }
        --b.remaining;
        b.motion += smooth * (b.target - b.motion);
        double noise = 2 * rain_texture_random(x->texture_rng) - 1;
        b.lo += (1 - b.a) * (noise - b.lo);
        b.hi += (1 - b.b) * (noise - b.hi);
        double high = double(j) / (TEXTURE_BANDS - 1);
        double weight = 1 - high * (1 - x->brightness_smooth) * 0.9;
        x->bed_weights[j] +=
            smooth * (RAIN_MATERIALS[x->material].bed_weights[j] - x->bed_weights[j]);
        weight *= x->bed_weights[j];
        result += (b.hi - b.lo) * b.normalization * weight * (1 + 0.22 * b.motion);
    }
    return result * 0.07 * x->bed_smooth * x->intensity_smooth;
}

// ─────────────────────────────────────
// This implements Equation 2, with A(vterm) = 1. tau = t - Rs/c.
// Eq. 2 is the analytic evaluation of Equation 1 for a circular, uniform
// step-velocity source (Section 2.1). The full pressure is A(vterm)*this value.
// We require x0 > a: the printed expression is singular on the listener axis
// and its stated Rs does not cover a source disk containing that axis.
static double rain_eq2(double tau, double a, double x0, double H, double rho, double c) {
    double Rs = std::hypot(x0 - a, H), Rl = std::hypot(x0 + a, H);
    // Algebraic rationalization of Rl-Rs avoids cancellation for small drops.
    double duration = 4 * x0 * a / ((Rl + Rs) * c);
    if (tau <= 0 || tau >= duration)
        return 0;
    // c^2*t^2-H^2 = (x0-a)^2 + c*tau*(2*Rs+c*tau).
    double radial2 = (x0 - a) * (x0 - a) + c * tau * (2 * Rs + c * tau);
    double cosine = (radial2 + x0 * x0 - a * a) / (2 * x0 * std::sqrt(radial2));
    // Roundoff guard only; this does not introduce a different pressure model.
    cosine = std::max(-1.0, std::min(cosine, 1.0));
    return rho * c / PI * std::acos(cosine);
}

// ─────────────────────────────────────
// Numerical sampling note: millimeter drops produce pulses shorter than an
// audio sample. Integrate Eq. 2 over each sample interval instead of missing
// a pulse between sample points. Eight-point Gauss-Legendre quadrature is a
// numerical approximation, NOT an extra equation claimed to be in the paper.
static double rain_bin(double lo, double hi, double a, double x0, double H, double rho, double c,
                       double sr) {
    double Rs = std::hypot(x0 - a, H), Rl = std::hypot(x0 + a, H);
    double duration = 4 * x0 * a / ((Rl + Rs) * c);
    lo = std::max(0.0, lo);
    hi = std::min(duration, hi);
    if (hi <= lo)
        return 0;
    static constexpr double nodes[] = {0.1834346424956498, 0.5255324099163290, 0.7966664774136267,
                                       0.9602898564975363};
    static constexpr double weights[] = {0.3626837833783620, 0.3137066458778873, 0.2223810344533745,
                                         0.1012285362903763};
    double mid = (lo + hi) / 2, half = (hi - lo) / 2, sum = 0;
    for (int i = 0; i < 4; ++i)
        sum += weights[i] * (rain_eq2(mid - half * nodes[i], a, x0, H, rho, c) +
                             rain_eq2(mid + half * nodes[i], a, x0, H, rho, c));
    return sum * half * sr;
}

// ─────────────────────────────────────
static int rain_kernel(double *out, double fraction, double a, double x0, const t_rain_tilde *x) {
    double Rs = std::hypot(x0 - a, x->height), Rl = std::hypot(x0 + a, x->height);
    double duration = 4 * x0 * a / ((Rl + Rs) * x->c);
    int count = static_cast<int>(std::ceil(duration * x->sample_rate + fraction));
    if (count > MAX_KERNEL)
        return 0;
    for (int i = 0; i < count; ++i)
        out[i] = rain_bin((i - fraction) / x->sample_rate, (i + 1 - fraction) / x->sample_rate, a,
                          x0, x->height, x->rho, x->c, x->sample_rate);
    return count;
}

// ─────────────────────────────────────
// This implements Equation 3's exponential convolution using a supplied v0.
// z(t)=integral_0^t v0(t-tau)*exp(-c*tau/(s-l)) d tau.
// u=(1-l/s)*v0 - c*l/s^2*z. The l/c delay is applied when creating a voice.
// The recurrence is the EXACT integral update for piecewise-constant v0.
static double rain_eq3(double v0, double &z, double l, double s, double c, double dt) {
    double u = (1 - l / s) * v0 - c * l / (s * s) * z;
    double k = c / (s - l), decay = std::exp(-k * dt);
    z = decay * z + v0 * (-std::expm1(-k * dt)) / k;
    return u;
}

// ─────────────────────────────────────
static bool rain_prepare_water(t_rain_tilde *x) {
    x->water_size = 0;
    if (!x->pinch_size || x->cone_length <= 0)
        return false;
    // Section 2.2: s=sqrt(A/pi)/lambda; A=pi*opening_radius^2.
    double s = x->opening_radius / x->splay;
    double z = 0, previous = 0, scale = 0;
    for (int i = 0; i < x->pinch_size; ++i)
        scale = std::max(scale, std::fabs(x->pinch[i]));
    int source_frames = static_cast<int>(std::ceil(x->pinch_size * x->sample_rate / x->pinch_rate));
    if (source_frames > MAX_WATER - 2)
        return false;
    for (int n = 0; n < MAX_WATER - 1; ++n) {
        int index = static_cast<int>(std::floor(n * x->pinch_rate / x->sample_rate));
        double v0 = index < x->pinch_size ? x->pinch[index] : 0;
        double u = rain_eq3(v0, z, x->cone_length, s, x->c, 1 / x->sample_rate);
        // This implements Equation 1 via superposition of Equation 2:
        // approximate u by sample-held steps; each jump Delta-u excites the
        // circular unit-step pressure response. No sinusoid is substituted.
        x->water_delta[n] = u - previous;
        previous = u;
        if (n >= source_frames && std::fabs(u) <= scale * 1e-9) {
            // Numerical tail tolerance, not an additional physical decay.
            x->water_delta[n + 1] = -u;
            x->water_size = n + 2;
            return true;
        }
    }
    return false; // Refuse a waveform we cannot represent within storage limits.
}

// ─────────────────────────────────────
static void rain_clear(t_rain_tilde *x) {
    //
    x->active = 0;
}

// ─────────────────────────────────────
static void rain_trigger(t_rain_tilde *x, double arrival, bool manual = false) {
    if (x->active == MAX_VOICES) {
        ++x->dropped;
        return;
    }
    // Section 3.1: positions uniform in an annulus. Inverse area CDF is derived
    // from that specification: x0=sqrt(inner^2+U*(outer^2-inner^2)).
    double x0 = std::sqrt(x->inner * x->inner +
                          rain_uniform(x) * (x->outer * x->outer - x->inner * x->inner));

    // Section 3.1 allows user-specified or randomized radii, but gives no size
    // distribution. Equal radius limits specify one size; uniform is our stated
    // numerical sampling choice when a range is supplied.
    double a = x->radius_min + rain_uniform(x) * (x->radius_max - x->radius_min);
    RainVoice &v = x->voices[x->active];
    v = RainVoice{};
    const auto &material = RAIN_MATERIALS[x->material];
    v.impact_gain = material.impact_gain;
    double hard_time = std::hypot(x0 - a, x->height) / x->c;
    double water_time =
        x->water_surface ? (std::hypot(x0 - x->opening_radius, x->height) + x->cone_length) / x->c
                         : hard_time;
    double first = std::min(hard_time, water_time);
    double hard_arrival = arrival + (hard_time - first) * x->sample_rate;
    v.hard_start = static_cast<int>(std::floor(hard_arrival));
    v.hard_size = rain_kernel(v.hard, hard_arrival - v.hard_start, a, x0, x);
    v.amplitude = x->amplitude;

    // Here I soften the radius-dependent gain in hybrid mode. Above a 1 mm
    // radius, the approximately quadratic excitation approaches twice the
    // reference level smoothly. This is event-level sound design, not clipping
    // or a change to Equation 2. Smaller drops and model paper are unchanged.
    double relative_area = (a / 0.001) * (a / 0.001);
    v.size_gain = relative_area <= 1 ? 1 : (2 - 1 / relative_area) / relative_area;
    v.water_size = 0;
    v.water_start = 0;
    v.end = v.hard_start + v.hard_size;
    if (x->water_surface) {
        double water_arrival = arrival + (water_time - first) * x->sample_rate;
        v.water_start = static_cast<int>(std::floor(water_arrival));
        v.water_size =
            rain_kernel(v.water, water_arrival - v.water_start, x->opening_radius, x0, x);
        v.end = std::max(v.end, v.water_start + v.water_size + x->water_size - 1);
    }
    if (!v.hard_size || (x->water_surface && !v.water_size)) {
        ++x->dropped;
        return;
    }

    // Here I add perceptual detail to selected impacts. All paper impacts still
    // render; at dense rates only ~800/s receive longer tails to bound CPU use.
    // Manual bangs always receive detail. Selection uses a separate PRNG so
    // enabling these layers never changes paper-event positions or sizes.
    if (x->hybrid && (manual || rain_texture_random(x->texture_rng) <
                                    std::min(1.0, 800 / std::max(1.0, x->density)))) {
        v.detail_start = v.hard_start;
        double pulse_area = 0;
        for (int k = 0; k < v.hard_size; ++k)
            pulse_area += v.hard[k] / x->sample_rate;
        // Artistic excitation gain from pulse area, referenced to 48 kHz;
        // it is not a calibrated pressure/energy relationship from the paper.
        double body = pulse_area * 48000 * v.amplitude * 5 * v.size_gain;
        double size = std::min(1.0, a / 0.003);
        v.noise_rng = static_cast<uint32_t>(rain_texture_random(x->texture_rng) * 4294967296.0);
        if (!v.noise_rng)
            v.noise_rng = 1;
        v.splash_env = body * material.splash_gain;
        double splash_seconds = material.splash_seconds * (1 + 1.667 * size);
        v.splash_decay = std::exp(-1 / (splash_seconds * x->sample_rate));
        double cutoff = (1500 + 6500 * x->brightness_smooth) * material.cutoff;
        v.splash_coeff =
            std::exp(-2 * PI * std::min(cutoff, 0.45 * x->sample_rate) / x->sample_rate);
        v.splash_highpass_coeff =
            std::exp(-2 * PI * std::min(material.highpass, 0.45 * x->sample_rate) / x->sample_rate);
        v.crinkle = material.crinkle;
        v.flutter_coeff = std::exp(-2 * PI * 900 / x->sample_rate);
        v.attack_step = 1 / std::max(1.0, material.attack_seconds * x->sample_rate);
        // Here I vary each drop's resonances instead of repeating a nearly
        // fixed chord. Larger drops emphasize lower, more damped modes. These
        // frequency/decay distributions are perceptual choices, not paper math.
        double size_pitch = 1 / std::sqrt(std::max(0.5, a / 0.001));
        double jitter = 0.75 + 0.5 * rain_texture_random(x->texture_rng);
        double decay = x->resonance * 0.001 * material.damping *
                       (0.65 + 0.55 * rain_texture_random(x->texture_rng));
        double longest_decay = 0;
        for (int k = 0; k < 3; ++k) {
            double mode_jitter = 0.85 + 0.3 * rain_texture_random(x->texture_rng);
            double mode_decay =
                decay * (0.7 + 0.3 * rain_texture_random(x->texture_rng)) / (1 + 0.6 * k);
            mode_decay /= std::sqrt(std::max(1.0, a / 0.001));
            longest_decay = std::max(longest_decay, mode_decay);
            double strength = body * material.tone_gain *
                              (0.45 + 0.35 * rain_texture_random(x->texture_rng)) / (1 + k);
            if (rain_texture_random(x->texture_rng) < 0.5)
                strength = -strength;
            rain_ring(v.rings[k], material.frequencies[k] * size_pitch * jitter * mode_jitter,
                      mode_decay, strength, x->sample_rate);
        }
        v.detail_end =
            v.detail_start + int(7 * std::max(splash_seconds, longest_decay) * x->sample_rate);
        // Here I implement an optional, rare water plink. It is deliberately
        // quiet, short and low pitched; it is NOT the paper's Eq. 3 cone model.
        // Explicit plinks level enables it independently of surface water.
        if (x->plinks > 0 && rain_texture_random(x->texture_rng) < 0.08) {
            v.plink_start =
                v.detail_start +
                int((0.004 + 0.008 * rain_texture_random(x->texture_rng)) * x->sample_rate);
            rain_ring(v.plink, 650 + 950 * rain_texture_random(x->texture_rng), 0.004, body * 0.3,
                      x->sample_rate);
            v.detail_end = std::max(v.detail_end, v.plink_start + int(0.028 * x->sample_rate));
        }
        v.end = std::max(v.end, v.detail_end);
    }
    v.age = 0;
    ++x->active;
}

// ─────────────────────────────────────
static t_int *rain_perform(t_int *w) {
    auto *x = reinterpret_cast<t_rain_tilde *>(w[1]);
    auto *left = reinterpret_cast<t_sample *>(w[2]);
    int n = static_cast<int>(w[3]);
    if (x->hybrid) {
        // Here I randomize the rhythm with exponential arrival intervals
        // (a Poisson process), allowing both clusters and gaps at the requested
        // mean density. Unlike the paper buffer-count scheme below, sparse rain
        // no longer repeats at nearly fixed intervals. This is a hybrid addition.
        if (x->density > 0) {
            if (x->arrival_hazard < 0)
                x->arrival_hazard = -std::log1p(-rain_uniform(x));
            double duration = n / x->sample_rate, elapsed = 0;
            while (x->arrival_hazard < x->density * (duration - elapsed)) {
                elapsed += x->arrival_hazard / x->density;
                rain_trigger(x, elapsed * x->sample_rate);
                x->arrival_hazard = -std::log1p(-rain_uniform(x));
            }
            x->arrival_hazard -= x->density * (duration - elapsed);
        }
    } else {
        // Section 3.1 / Fig. 3: the strict paper path retains uniform arrivals
        // within each buffer and fractional bookkeeping for the event count.
        x->count_fraction += x->density * n / x->sample_rate;
        int count = static_cast<int>(x->count_fraction);
        x->count_fraction -= count;
        for (int i = 0; i < count; ++i)
            rain_trigger(x, rain_uniform(x) * n);
    }
    while (x->pending > 0) {
        rain_trigger(x, 0, true);
        --x->pending;
    }
    double smooth = 1 - std::exp(-1 / (0.03 * x->sample_rate));
    double intensity = std::sqrt(x->density / 800);
    for (int i = 0; i < n; ++i) {
        // Here I smooth the sound-design controls to avoid abrupt gain changes.
        // In model paper, a settled zero hybrid mix restores the equations.
        x->hybrid_mix += smooth * ((x->hybrid ? 1.0 : 0.0) - x->hybrid_mix);
        if (!x->hybrid && x->hybrid_mix < 1e-9)
            x->hybrid_mix = 0;
        x->impact_smooth += smooth * (x->impact - x->impact_smooth);
        x->splash_smooth += smooth * (x->splash - x->splash_smooth);
        x->tone_smooth += smooth * (x->tone - x->tone_smooth);
        x->plinks_smooth += smooth * (x->plinks - x->plinks_smooth);
        x->bed_smooth += smooth * (x->bed - x->bed_smooth);
        x->brightness_smooth += smooth * (x->brightness - x->brightness_smooth);
        x->intensity_smooth += smooth * (intensity - x->intensity_smooth);
        double l = 0;
        for (int j = 0; j < x->active;) {
            RainVoice &v = x->voices[j];
            double p = 0;
            int hard = v.age - v.hard_start;
            if (hard >= 0 && hard < v.hard_size)
                p = v.amplitude * v.hard[hard];
            int water = v.age - v.water_start;
            if (water >= 0 && v.water_size) {
                int begin = std::max(0, water - x->water_size + 1);
                int end = std::min(v.water_size - 1, water);
                for (int k = begin; k <= end; ++k)
                    p += v.water[k] * x->water_delta[water - k];
            }
            // Here I implement the short filtered-noise splash, with a soft
            // attack and exponential tail. It is a perceptual layer, NOT Eq. 2.
            double detail = 0;
            if (v.age >= v.detail_start && v.age < v.detail_end) {
                double noise = 2 * rain_texture_random(v.noise_rng) - 1;
                v.splash_lp += (1 - v.splash_coeff) * (noise - v.splash_lp);
                v.splash_low += (1 - v.splash_highpass_coeff) * (v.splash_lp - v.splash_low);
                // Irregular amplitude flutter gives thin plastic a crinkly tail.
                // All voices consume the same random sequence for material comparisons.
                double motion = 2 * rain_texture_random(v.noise_rng) - 1;
                v.flutter += (1 - v.flutter_coeff) * (motion - v.flutter);
                double texture =
                    (v.splash_lp - v.splash_low) *
                    (1 - v.crinkle + v.crinkle * std::min(2.0, 6 * std::fabs(v.flutter)));
                v.attack = std::min(1.0, v.attack + v.attack_step);
                detail = x->splash_smooth * texture * v.splash_env * v.attack;
                v.splash_env *= v.splash_decay;
                // Here I mix the quiet surface resonances separately from splash.
                for (auto &ring : v.rings)
                    detail += x->tone_smooth * rain_ring_sample(ring) * v.attack;
                if (v.age >= v.plink_start)
                    detail += x->plinks_smooth * rain_ring_sample(v.plink);
            }
            // Here I blend the paper transient with the perceptual layers.
            // model paper restores its full level and removes all added layers.
            p = p * (1 + x->hybrid_mix * (x->impact_smooth * v.size_gain * v.impact_gain - 1)) +
                x->hybrid_mix * detail;
            l += p; // Section 3.1: linear superposition at the mono listener.
            ++v.age;
            if (v.age >= v.end) {
                --x->active;
                if (j < x->active)
                    v = x->voices[x->active];
            } else
                ++j;
        }
        if (x->hybrid_mix > 0) {
            l += x->hybrid_mix * rain_background(x, smooth);
        }
        // Output gain is playback conversion, NOT part of Equations 1–3.
        // Hybrid additions are artistic levels; only model paper is pressure-only.
        left[i] = static_cast<t_sample>(l * x->gain);
    }
    return w + 4;
}

// ─────────────────────────────────────
static void rain_density(t_rain_tilde *x, t_floatarg value) {
    if (std::isfinite(value)) {
        x->density = std::max(0.0, std::min(double(value), MAX_DENSITY));
        x->count_fraction = 0;
    }
}

// ─────────────────────────────────────
static void rain_bang(t_rain_tilde *x) {
    if (x->pending < MAX_VOICES)
        ++x->pending;
    else
        pd_error(x, "rain~: pending drop capacity exceeded");
}

// ─────────────────────────────────────
static void rain_seed(t_rain_tilde *x, t_floatarg value) {
    if (!std::isfinite(value))
        return;
    double v = std::fmod(std::trunc(double(value)), 4294967296.0);
    if (v < 0)
        v += 4294967296.0;
    x->arrival_hazard = -1;
    x->rng = static_cast<uint32_t>(v);
    if (!x->rng)
        x->rng = 1;
    x->texture_rng = x->rng ^ 0x9e3779b9u;
    if (!x->texture_rng)
        x->texture_rng = 1;
}

// ─────────────────────────────────────
static void rain_radius(t_rain_tilde *x, t_floatarg lo, t_floatarg hi) {
    if (!std::isfinite(lo) || !std::isfinite(hi) || lo <= 0 || hi < lo || hi > 0.02 ||
        hi >= x->inner) {
        pd_error(x, "rain~: radius min max in meters: 0 < min <= max <= 0.02, max < inner radius");
        return;
    }
    x->radius_min = lo;
    x->radius_max = hi;
}

// ─────────────────────────────────────
static void rain_area(t_rain_tilde *x, t_floatarg inner, t_floatarg outer) {
    if (!std::isfinite(inner) || !std::isfinite(outer) ||
        inner <= std::max(x->radius_max, x->opening_radius) || outer < inner || outer > 1000) {
        pd_error(x,
                 "rain~: area inner outer in meters: inner > source radii, inner <= outer <= 1000");
        return;
    }
    x->inner = inner;
    x->outer = outer;
}

// ─────────────────────────────────────
static void rain_height(t_rain_tilde *x, t_floatarg value) {
    if (std::isfinite(value) && value > 0 && value <= 1000)
        x->height = value;
    else
        pd_error(x, "rain~: height must be in (0, 1000] meters");
}

// ─────────────────────────────────────
static void rain_amplitude(t_rain_tilde *x, t_floatarg value) {
    // Section 2.1 names A(vterm) but DOES NOT give its function of velocity.
    // Accept A itself (surface step velocity), rather than inventing a law.
    if (std::isfinite(value) && value >= 0 && value <= 1000)
        x->amplitude = value;
    else
        pd_error(x, "rain~: amplitude A(vterm) must be in [0,1000] m/s");
}

// ─────────────────────────────────────
static void rain_gain(t_rain_tilde *x, t_floatarg value) {
    if (std::isfinite(value) && value >= 0 && value <= 1000000)
        x->gain = value;
    else
        pd_error(x, "rain~: gain must be in [0,1000000]");
}

// ─────────────────────────────────────
static void rain_surface(t_rain_tilde *x, t_symbol *name) {
    if (!std::strcmp(name->s_name, "hard"))
        x->water_surface = false;
    else if (!std::strcmp(name->s_name, "water") && x->water_size)
        x->water_surface = true;
    else
        pd_error(x, "rain~: use surface hard, or configure cone and pinch before surface water");
}

// ─────────────────────────────────────
static void rain_cone(t_rain_tilde *x, t_floatarg l, t_floatarg radius, t_floatarg splay) {
    if (!std::isfinite(l) || !std::isfinite(radius) || !std::isfinite(splay) || l <= 0 || l > 1 ||
        radius <= 0 || radius > 0.02 || radius >= x->inner || splay <= 0 || radius / splay <= l) {
        pd_error(x,
                 "rain~: cone l radius lambda requires l>0, radius<=0.02 m and s=radius/lambda>l");
        return;
    }
    rain_clear(x);
    x->cone_length = l;
    x->opening_radius = radius;
    x->splay = splay;
    if (!rain_prepare_water(x)) {
        x->water_surface = false;
        if (x->pinch_size)
            pd_error(x, "rain~: cone response exceeds water buffer capacity");
    }
}

// ─────────────────────────────────────
static void rain_pinch(t_rain_tilde *x, t_symbol *name, t_floatarg rate) {
    auto *array = reinterpret_cast<t_garray *>(pd_findbyclass(name, garray_class));
    int size = 0;
    t_word *words = nullptr;
    if (!array || !garray_getfloatwords(array, &size, &words) || size < 1 || size > MAX_PINCH ||
        !std::isfinite(rate) || rate < 0) {
        pd_error(x, "rain~: pinch requires a float array of 1..2048 velocity samples and optional "
                    "sample rate");
        return;
    }
    double input_rate = rate > 0 ? rate : x->sample_rate;
    if (input_rate < 1000 || input_rate > 384000) {
        pd_error(x, "rain~: pinch sample rate must be 1000..384000 Hz");
        return;
    }
    for (int i = 0; i < size; ++i)
        if (!std::isfinite(words[i].w_float) || std::fabs(words[i].w_float) > 1000) {
            pd_error(x, "rain~: pinch samples must be finite velocities within +/-1000 m/s");
            return;
        }
    rain_clear(x);
    x->pinch_size = size;
    x->pinch_rate = input_rate;
    for (int i = 0; i < size; ++i)
        x->pinch[i] = words[i].w_float;
    if (!rain_prepare_water(x)) {
        x->water_surface = false;
        pd_error(x, "rain~: configure cone; waveform plus tail must fit the water buffer");
    }
}

// ─────────────────────────────────────
static void rain_air(t_rain_tilde *x, t_floatarg rho, t_floatarg c) {
    if (!std::isfinite(rho) || !std::isfinite(c) || rho <= 0 || rho > 10 || c < 250 || c > 450) {
        pd_error(x, "rain~: air rho c requires 0<rho<=10 kg/m^3 and 250<=c<=450 m/s");
        return;
    }
    rain_clear(x);
    x->rho = rho;
    x->c = c;
    if (x->pinch_size && !rain_prepare_water(x)) {
        x->water_surface = false;
        pd_error(x, "rain~: water response exceeds buffer capacity");
    }
}

// ─────────────────────────────────────
static void rain_status(t_rain_tilde *x) {
    post("rain~: DAFx 2004 Eq. 1/2/3; %d active; %llu capacity-dropped events", x->active,
         static_cast<unsigned long long>(x->dropped));
}

// ─────────────────────────────────────
static void rain_model(t_rain_tilde *x, t_symbol *name) {
    if (!std::strcmp(name->s_name, "hybrid"))
        x->hybrid = true;
    else if (!std::strcmp(name->s_name, "paper"))
        x->hybrid = false;
    else
        pd_error(x, "rain~: model must be hybrid or paper");
}

// ─────────────────────────────────────
static void rain_material(t_rain_tilde *x, t_symbol *name) {
    const char *selected = name->s_name;
    if (!std::strcmp(selected, "plastic-bag"))
        selected = "plastic";
    if (!std::strcmp(selected, "dirt-floor"))
        selected = "dirt";
    if (!std::strcmp(selected, "asphalt-floor"))
        selected = "asphalt";
    for (int i = 0; i < MATERIAL_COUNT; ++i)
        if (!std::strcmp(selected, RAIN_MATERIALS[i].name)) {
            x->material = i;
            return;
        }
    pd_error(x, "rain~: material must be plastic, roof, wood, dirt, asphalt, ground or leaf");
}

// ─────────────────────────────────────
// Independent layer levels; clamps are UI limits, not paper equations.
#define LAYER(name)                                                                                \
    static void rain_##name(t_rain_tilde *x, t_floatarg value) {                                   \
        if (std::isfinite(value))                                                                  \
            x->name = std::max(0.0, std::min(double(value), 1.0));                                 \
    }
LAYER(impact)
LAYER(splash)
LAYER(tone)
LAYER(plinks)
LAYER(bed)
LAYER(brightness)
#undef LAYER

// ─────────────────────────────────────
static void rain_resonance(t_rain_tilde *x, t_floatarg value) {
    if (std::isfinite(value))
        x->resonance = std::max(1.0, std::min(double(value), 30.0));
}

// ─────────────────────────────────────
static void rain_legacy(t_rain_tilde *x, t_symbol *, int, t_atom *) {
    pd_error(
        x, "rain~: unsupported control; use impact/splash/tone/plinks/bed or radius, area, height, "
           "amplitude and gain (see rain~.md)");
}

// ─────────────────────────────────────
static void rain_dsp(t_rain_tilde *x, t_signal **sp) {
    double sr = sp[0]->s_sr;
    if (!std::isfinite(sr) || sr < 1000 || sr > 384000) {
        pd_error(x, "rain~: supported sample rates are 1000..384000 Hz");
        return;
    }
    if (sr != x->sample_rate) {
        rain_clear(x);
        x->sample_rate = sr;
        rain_texture_prepare(x);
        if (x->pinch_size && !rain_prepare_water(x)) {
            x->water_surface = false;
            pd_error(x, "rain~: resampled water response exceeds buffer capacity");
        }
    }
    dsp_add(rain_perform, 3, x, sp[0]->s_vec, sp[0]->s_n);
}

// ─────────────────────────────────────
static void *rain_new(t_floatarg density) {
    auto *x = reinterpret_cast<t_rain_tilde *>(pd_new(rain_tilde_class));
    x->sample_rate = sys_getsr();
    if (x->sample_rate < 1000 || x->sample_rate > 384000 || !std::isfinite(x->sample_rate))
        x->sample_rate = 44100;
    // Scene/playback defaults, NOT measured values supplied by the paper.
    x->density = 800;
    if (density > 0)
        rain_density(x, density);
    x->radius_min = 0.0005;
    x->radius_max = 0.002; // Varied scene input for the hybrid default, not a paper distribution.
    x->inner = 0.5;
    x->outer = 5;
    x->height = 1.7;
    x->rho = 1.2;
    x->c = 343;
    x->amplitude = 1;
    x->gain = 1;
    x->cone_length = 0;
    x->opening_radius = 0.001;
    x->splay = 1;
    x->pinch_rate = x->sample_rate;
    x->pinch_size = x->water_size = 0;
    x->water_surface = false;
    x->active = x->pending = 0;
    x->count_fraction = 0;
    x->dropped = 0;
    x->rng = 0x12345678u ^ static_cast<uint32_t>(reinterpret_cast<uintptr_t>(x));
    if (!x->rng)
        x->rng = 1;
    x->texture_rng = x->rng ^ 0x9e3779b9u;
    if (!x->texture_rng)
        x->texture_rng = 1;
    x->hybrid = true;
    x->arrival_hazard = -1;
    x->hybrid_mix = 1;
    x->material = 0;
    for (int j = 0; j < TEXTURE_BANDS; ++j)
        x->bed_weights[j] = RAIN_MATERIALS[x->material].bed_weights[j];
    x->impact = x->impact_smooth = 0.25;
    x->splash = x->splash_smooth = 0.8;
    x->tone = x->tone_smooth = 0.06;
    x->plinks = x->plinks_smooth = 0;
    x->bed = x->bed_smooth = 0.12;
    x->brightness = x->brightness_smooth = 0.45;
    x->resonance = 6;
    x->intensity_smooth = std::sqrt(x->density / 800);
    rain_texture_prepare(x);
    x->out = outlet_new(&x->x_obj, &s_signal);
    return x;
}

// ─────────────────────────────────────
extern "C" void rain_tilde_setup() {
    rain_tilde_class = class_new(gensym("rain~"), reinterpret_cast<t_newmethod>(rain_new), nullptr,
                                 sizeof(t_rain_tilde), CLASS_DEFAULT, A_DEFFLOAT, 0);
    class_addbang(rain_tilde_class, reinterpret_cast<t_method>(rain_bang));

#define METHOD(name, ...)                                                                          \
    class_addmethod(rain_tilde_class, reinterpret_cast<t_method>(rain_##name), gensym(#name),      \
                    __VA_ARGS__, 0)
    METHOD(dsp, A_CANT);
    METHOD(density, A_FLOAT);
    METHOD(seed, A_FLOAT);
    METHOD(radius, A_FLOAT, A_FLOAT);
    METHOD(area, A_FLOAT, A_FLOAT);
    METHOD(height, A_FLOAT);
    METHOD(amplitude, A_FLOAT);
    METHOD(gain, A_FLOAT);
    METHOD(surface, A_SYMBOL);
    METHOD(cone, A_FLOAT, A_FLOAT, A_FLOAT);
    METHOD(pinch, A_SYMBOL, A_DEFFLOAT);
    METHOD(air, A_FLOAT, A_FLOAT);
    METHOD(model, A_SYMBOL);
    METHOD(material, A_SYMBOL);
    METHOD(impact, A_FLOAT);
    METHOD(splash, A_FLOAT);
    METHOD(tone, A_FLOAT);
    METHOD(plinks, A_FLOAT);
    METHOD(bed, A_FLOAT);
    METHOD(brightness, A_FLOAT);
    METHOD(resonance, A_FLOAT);
#undef METHOD
    class_addmethod(rain_tilde_class, reinterpret_cast<t_method>(rain_status), gensym("status"),
                    A_NULL);
    const char *removed[] = {"pitch", "drops", "distance", "saturation"};
    for (auto name : removed)
        class_addmethod(rain_tilde_class, reinterpret_cast<t_method>(rain_legacy), gensym(name),
                        A_GIMME, 0);
}
