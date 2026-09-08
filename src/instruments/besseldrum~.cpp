#include "m_pd.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

static t_class *besseldrum_tilde_class = nullptr;

constexpr int kMaxModes = 128;
constexpr int kMaxOrder = 32;
constexpr int kMaxRadial = 32;
constexpr int kMaxCandidates = (kMaxOrder + 1) * kMaxRadial;
constexpr double kPi = 3.141592653589793238462643383279502884;

// The spatial modes below remain the zeros of J_m, which are exact for an
// ideal fixed circular membrane. Adding D*k^4 gives useful stiff-membrane
// dispersion, but it does not change those mode shapes or boundary conditions.
// When bending dominates, this is therefore an approximation to a circular
// plate, not an exact plate solution.

// ─────────────────────────────────────
struct Candidate {
    double alpha;
    int m;
    int n;
};

// ─────────────────────────────────────
struct Mode {
    int m;
    int n;
    int active;

    double alpha;
    double ratio;
    double frequency;
    double decay_rate;

    // Complex resonator state z = re + i*im.
    double re;
    double im;
    double a;
    double b;
    double pickup_radial;
    double spatial_scale;
    double modal_scale;
};

// Material presets (approximate starting points, not exact measurements).
// Values vary considerably with formulation, processing, humidity, and gauge.
// Keep this table together so the defaults are easy to audit and edit.
// loss0, loss1, and loss2 produce an amplitude decay rate in s^-1:
// gamma(f) = loss0 + loss1*f + loss2*f^2, with f measured in Hz.
struct MaterialPreset {
    const char *name;
    double density;   // kg/m^3
    double thickness; // m
    double young;     // Pa
    double poisson;
    double loss0;
    double loss1;
    double loss2;
};

static constexpr MaterialPreset kMaterialPresets[] = {
    {"mylar", 1390.0, 0.000250, 4.0e9, 0.38, 0.70, 1.5e-4, 2.0e-8},
    {"calfskin", 1200.0, 0.000400, 1.0e8, 0.35, 1.50, 1.2e-3, 1.0e-7},
    {"kevlar", 1440.0, 0.000250, 7.0e10, 0.36, 0.45, 1.0e-4, 1.0e-8},
    {"paper", 800.0, 0.000300, 3.0e9, 0.30, 2.00, 1.0e-3, 1.0e-7},
    {"metal", 7850.0, 0.000300, 2.0e11, 0.30, 0.20, 5.0e-5, 5.0e-9},
};

// ─────────────────────────────────────
static Candidate g_candidates[kMaxCandidates];
static int g_candidate_count = 0;
static double g_alpha01 = 2.4048255576957727686;
static bool g_table_ready = false;

// ─────────────────────────────────────
typedef struct _besseldrum_tilde {
    t_object x_obj;
    t_outlet *outlet;

    double sample_rate;
    double radius;
    double density;
    double thickness;
    double tension;
    double young;
    double poisson;
    double loss0;
    double loss1;
    double loss2;
    double gain;

    t_symbol *material;

    double strike_x;
    double strike_y;
    double pickup_x;
    double pickup_y;

    int normalize;
    int num_modes;

    Mode modes[kMaxModes];
} t_besseldrum_tilde;

// ─────────────────────────────────────
static double bessel_j(int order, double x) {
    return std::cyl_bessel_j(static_cast<double>(order), x);
}

// ─────────────────────────────────────
static bool opposite_sign(double a, double b) {
    return (a < 0.0 && b > 0.0) || (a > 0.0 && b < 0.0);
}

// ─────────────────────────────────────
static int find_bessel_zeros(int order, int count, double *out) {
    constexpr double scan_step = 0.25;
    constexpr double max_x = 500.0;

    int found = 0;
    double left = 1.0e-4;
    double f_left = bessel_j(order, left);

    while (found < count && left < max_x) {
        const double right = left + scan_step;
        const double f_right = bessel_j(order, right);

        if (opposite_sign(f_left, f_right)) {
            double a = left;
            double b = right;
            double fa = f_left;

            for (int iteration = 0; iteration < 64; ++iteration) {
                const double mid = 0.5 * (a + b);
                const double fm = bessel_j(order, mid);

                if (fm == 0.0) {
                    a = b = mid;
                    break;
                }

                if (opposite_sign(fa, fm)) {
                    b = mid;
                } else {
                    a = mid;
                    fa = fm;
                }
            }

            const double root = 0.5 * (a + b);
            out[found++] = root;

            // Resume just to the right of the zero.
            left = root + 1.0e-6;
            f_left = bessel_j(order, left);
        } else {
            left = right;
            f_left = f_right;
        }
    }

    return found;
}

// ─────────────────────────────────────
static void initialize_mode_table() {
    if (g_table_ready)
        return;

    g_candidate_count = 0;

    for (int m = 0; m <= kMaxOrder; ++m) {
        double zeros[kMaxRadial];
        const int found = find_bessel_zeros(m, kMaxRadial, zeros);

        for (int n = 0; n < found; ++n) {
            if (g_candidate_count >= kMaxCandidates)
                break;

            Candidate &c = g_candidates[g_candidate_count++];
            c.alpha = zeros[n];
            c.m = m;
            c.n = n + 1;
        }
    }

    std::sort(g_candidates, g_candidates + g_candidate_count,
              [](const Candidate &a, const Candidate &b) { return a.alpha < b.alpha; });

    if (g_candidate_count > 0)
        g_alpha01 = g_candidates[0].alpha;

    g_table_ready = true;
}

// ─────────────────────────────────────
static void project_to_membrane(double &x, double &y) {
    const double r = std::hypot(x, y);

    if (r > 1.0) {
        x /= r;
        y /= r;
    }
}

// ─────────────────────────────────────
static void besseldrum_clear(t_besseldrum_tilde *x) {
    for (int i = 0; i < x->num_modes; ++i) {
        x->modes[i].re = 0.0;
        x->modes[i].im = 0.0;
    }
}

// ─────────────────────────────────────
static bool finite_positive(double value) {
    return std::isfinite(value) && value > 0.0;
}

// ─────────────────────────────────────
static bool finite_nonnegative(double value) {
    return std::isfinite(value) && value >= 0.0;
}

// ─────────────────────────────────────
static double besseldrum_areal_density(const t_besseldrum_tilde *x) {
    return x->density * x->thickness;
}

// ─────────────────────────────────────
static double besseldrum_bending_stiffness(const t_besseldrum_tilde *x) {
    const double h2 = x->thickness * x->thickness;
    const double h3 = h2 * x->thickness;
    return x->young * h3 / (12.0 * (1.0 - x->poisson * x->poisson));
}

// ─────────────────────────────────────
static double besseldrum_mode_frequency(const t_besseldrum_tilde *x, double alpha) {
    // Long-double intermediates keep extreme but finite control values from
    // overflowing before the mode can be safely rejected.
    const long double density = x->density;
    const long double thickness = x->thickness;
    const long double sigma = density * thickness;
    const long double poisson = x->poisson;
    const long double bending =
        static_cast<long double>(x->young) * thickness * thickness * thickness /
        (12.0L * (1.0L - poisson * poisson));
    const long double k = static_cast<long double>(alpha) / x->radius;
    const long double k2 = k * k;
    const long double omega2 = (static_cast<long double>(x->tension) / sigma) * k2 +
                               (bending / sigma) * k2 * k2;

    if (std::isnan(omega2) || omega2 <= 0.0L)
        return 0.0;

    const long double frequency = std::sqrt(omega2) / (2.0L * kPi);
    if (!std::isfinite(frequency) ||
        frequency > static_cast<long double>(std::numeric_limits<double>::max()))
        return std::numeric_limits<double>::infinity();

    return static_cast<double>(frequency);
}

// ─────────────────────────────────────
static void besseldrum_update_coefficients(t_besseldrum_tilde *x) {
    const double sr = (x->sample_rate > 1.0) ? x->sample_rate : 44100.0;
    const double nyquist = 0.5 * sr;
    const double fundamental = besseldrum_mode_frequency(x, g_alpha01);

    for (int i = 0; i < x->num_modes; ++i) {
        Mode &mode = x->modes[i];
        mode.frequency = besseldrum_mode_frequency(x, mode.alpha);
        mode.decay_rate = std::isfinite(mode.frequency)
                              ? x->loss0 + mode.frequency *
                                             (x->loss1 + x->loss2 * mode.frequency)
                              : std::numeric_limits<double>::infinity();
        if (!std::isfinite(mode.decay_rate))
            mode.decay_rate = std::numeric_limits<double>::infinity();
        mode.modal_scale = (std::isfinite(mode.frequency) && mode.frequency > 0.0 &&
                            std::isfinite(fundamental) && fundamental > 0.0)
                               ? mode.spatial_scale * fundamental / mode.frequency
                               : 0.0;

        if (!std::isfinite(mode.frequency) || mode.frequency <= 0.0 ||
            mode.frequency >= nyquist) {
            mode.active = 0;
            mode.a = 0.0;
            mode.b = 0.0;
            mode.re = 0.0;
            mode.im = 0.0;
            continue;
        }

        mode.active = 1;
        const double omega = 2.0 * kPi * mode.frequency / sr;
        const double decay = std::isfinite(mode.decay_rate)
                                 ? std::exp(-mode.decay_rate / sr)
                                 : 0.0;
        mode.a = decay * std::cos(omega);
        mode.b = decay * std::sin(omega);
    }
}

// ─────────────────────────────────────
static void besseldrum_update_pickup(t_besseldrum_tilde *x) {
    double px = x->pickup_x;
    double py = x->pickup_y;
    project_to_membrane(px, py);
    x->pickup_x = px;
    x->pickup_y = py;
    const double r = std::hypot(px, py);
    for (int i = 0; i < x->num_modes; ++i) {
        Mode &mode = x->modes[i];
        mode.pickup_radial = bessel_j(mode.m, mode.alpha * r);
    }
}

// ─────────────────────────────────────
static void besseldrum_rebuild_modes(t_besseldrum_tilde *x, int requested_modes) {
    initialize_mode_table();
    requested_modes = std::max(1, std::min(requested_modes, kMaxModes));
    requested_modes = std::min(requested_modes, g_candidate_count);
    x->num_modes = requested_modes;
    for (int i = 0; i < x->num_modes; ++i) {
        const Candidate &c = g_candidates[i];
        Mode &mode = x->modes[i];
        mode.m = c.m;
        mode.n = c.n;
        mode.active = 1;
        mode.alpha = c.alpha;
        mode.ratio = c.alpha / g_alpha01;
        mode.frequency = 0.0;
        mode.decay_rate = 0.0;
        mode.re = 0.0;
        mode.im = 0.0;
        mode.a = 0.0;
        mode.b = 0.0;
        mode.pickup_radial = 0.0;

        // Fixed circular membrane eigenfunction:
        //
        // J_m(alpha_mn r)
        //
        // with:
        //
        // J_m(alpha_mn) = 0
        //
        // For m > 0 there are two degenerate angular
        // eigenfunctions, cos(m theta) and sin(m theta),
        // hence the factor 2.
        //
        // The frequency-dependent 1 / omega impulse scaling is applied later,
        // when the physical modal frequencies are updated.

        const double j_next = bessel_j(mode.m + 1, mode.alpha);
        const double degeneracy = (mode.m == 0) ? 1.0 : 2.0;
        const double denom = j_next * j_next;
        mode.spatial_scale = (denom > 1.0e-20) ? degeneracy / denom : 0.0;
        mode.modal_scale = 0.0;
    }

    besseldrum_update_pickup(x);
    besseldrum_update_coefficients(x);
}

// ─────────────────────────────────────
static void besseldrum_do_strike(t_besseldrum_tilde *x, double sx, double sy, double velocity) {
    if (!std::isfinite(sx) || !std::isfinite(sy) || !finite_positive(velocity)) {
        if (!std::isfinite(sx) || !std::isfinite(sy) || !std::isfinite(velocity))
            pd_error(x, "besseldrum~: strike values must be finite");
        return;
    }
    project_to_membrane(sx, sy);
    const double strike_r = std::hypot(sx, sy);
    const double strike_theta = std::atan2(sy, sx);
    const double pickup_theta = std::atan2(x->pickup_y, x->pickup_x);
    const double delta_theta = pickup_theta - strike_theta;
    double coupling[kMaxModes];
    double energy = 0.0;

    for (int i = 0; i < x->num_modes; ++i) {
        const Mode &mode = x->modes[i];
        if (!mode.active) {
            coupling[i] = 0.0;
            continue;
        }
        const double strike_radial = bessel_j(mode.m, mode.alpha * strike_r);
        const double angular = (mode.m == 0) ? 1.0 : std::cos(mode.m * delta_theta);
        const double c = mode.modal_scale * strike_radial * mode.pickup_radial * angular;
        coupling[i] = c;
        energy += c * c;
    }

    double scale = x->gain * velocity;
    if (x->normalize && energy > 1.0e-24) {
        scale /= std::sqrt(energy);
    }
    for (int i = 0; i < x->num_modes; ++i) {
        x->modes[i].re += scale * coupling[i];
    }
}

// ─────────────────────────────────────
static void besseldrum_bang(t_besseldrum_tilde *x) {
    besseldrum_do_strike(x, x->strike_x, x->strike_y, 1.0);
}

// ─────────────────────────────────────
static void besseldrum_float(t_besseldrum_tilde *x, t_floatarg velocity) {
    besseldrum_do_strike(x, x->strike_x, x->strike_y, static_cast<double>(velocity));
}

// ─────────────────────────────────────
static void besseldrum_strike(t_besseldrum_tilde *x, t_symbol *, int argc, t_atom *argv) {
    if (argc == 1) {
        const double velocity = atom_getfloat(argv);
        besseldrum_do_strike(x, x->strike_x, x->strike_y, velocity);
        return;
    }

    if (argc == 3) {
        const double sx = atom_getfloat(argv + 0);
        const double sy = atom_getfloat(argv + 1);
        const double velocity = atom_getfloat(argv + 2);
        besseldrum_do_strike(x, sx, sy, velocity);
        return;
    }
    pd_error(x, "besseldrum~: strike expects "
                "'strike velocity' or "
                "'strike x y velocity'");
}

// ─────────────────────────────────────
static void besseldrum_strikepos(t_besseldrum_tilde *x, t_floatarg fx, t_floatarg fy) {
    double px = static_cast<double>(fx);
    double py = static_cast<double>(fy);
    if (!std::isfinite(px) || !std::isfinite(py)) {
        pd_error(x, "besseldrum~: strikepos values must be finite");
        return;
    }
    project_to_membrane(px, py);
    x->strike_x = px;
    x->strike_y = py;
}

// ─────────────────────────────────────
static void besseldrum_pickup(t_besseldrum_tilde *x, t_floatarg fx, t_floatarg fy) {
    const double px = static_cast<double>(fx);
    const double py = static_cast<double>(fy);
    if (!std::isfinite(px) || !std::isfinite(py)) {
        pd_error(x, "besseldrum~: pickup values must be finite");
        return;
    }
    x->pickup_x = px;
    x->pickup_y = py;
    besseldrum_update_pickup(x);
}

// ─────────────────────────────────────
static void besseldrum_tune(t_besseldrum_tilde *x, t_floatarg f) {
    const double value = static_cast<double>(f);
    if (!finite_positive(value)) {
        pd_error(x, "besseldrum~: tune must be a finite value > 0 Hz");
        return;
    }

    const long double density = x->density;
    const long double thickness = x->thickness;
    const long double sigma = density * thickness;
    const long double poisson = x->poisson;
    const long double bending =
        static_cast<long double>(x->young) * thickness * thickness * thickness /
        (12.0L * (1.0L - poisson * poisson));
    const long double k = static_cast<long double>(g_alpha01) / x->radius;
    const long double k2 = k * k;
    const long double target_omega = 2.0L * kPi * value;
    long double tension = sigma * target_omega * target_omega / k2 - bending * k2;
    const long double minimum_frequency =
        std::sqrt((bending / sigma) * k2 * k2) / (2.0L * kPi);

    if (!std::isfinite(tension) ||
        tension > static_cast<long double>(std::numeric_limits<double>::max())) {
        pd_error(x, "besseldrum~: tune produced a non-finite tension");
        return;
    }
    if (tension < 0.0L) {
        const long double tolerance = 1.0e-12L * std::max(1.0L, bending * k2);
        if (tension >= -tolerance) {
            tension = 0.0L;
        } else {
            pd_error(x,
                     "besseldrum~: %.3f Hz is below the zero-tension bending "
                     "frequency %.3f Hz",
                     value, static_cast<double>(minimum_frequency));
            return;
        }
    }

    x->tension = static_cast<double>(tension);
    besseldrum_update_coefficients(x);
}

// ─────────────────────────────────────
static void besseldrum_radius(t_besseldrum_tilde *x, t_floatarg f) {
    const double value = static_cast<double>(f);
    if (!finite_positive(value)) {
        pd_error(x, "besseldrum~: radius must be a finite value > 0 meters");
        return;
    }
    x->radius = value;
    besseldrum_update_coefficients(x);
}

// ─────────────────────────────────────
static void besseldrum_density(t_besseldrum_tilde *x, t_floatarg f) {
    const double value = static_cast<double>(f);
    if (!finite_positive(value)) {
        pd_error(x, "besseldrum~: density must be a finite value > 0 kg/m3");
        return;
    }
    x->density = value;
    x->material = gensym("custom");
    besseldrum_update_coefficients(x);
}

// ─────────────────────────────────────
static void besseldrum_thickness(t_besseldrum_tilde *x, t_floatarg f) {
    const double value = static_cast<double>(f);
    if (!finite_positive(value)) {
        pd_error(x, "besseldrum~: thickness must be a finite value > 0 meters");
        return;
    }
    x->thickness = value;
    x->material = gensym("custom");
    besseldrum_update_coefficients(x);
}

// ─────────────────────────────────────
static void besseldrum_tension(t_besseldrum_tilde *x, t_floatarg f) {
    const double value = static_cast<double>(f);
    if (!finite_nonnegative(value)) {
        pd_error(x, "besseldrum~: tension must be a finite value >= 0 N/m");
        return;
    }
    x->tension = value;
    besseldrum_update_coefficients(x);
}

// ─────────────────────────────────────
static void besseldrum_young(t_besseldrum_tilde *x, t_floatarg f) {
    const double value = static_cast<double>(f);
    if (!finite_nonnegative(value)) {
        pd_error(x, "besseldrum~: young must be a finite value >= 0 Pa");
        return;
    }
    x->young = value;
    x->material = gensym("custom");
    besseldrum_update_coefficients(x);
}

// ─────────────────────────────────────
static void besseldrum_poisson(t_besseldrum_tilde *x, t_floatarg f) {
    const double value = static_cast<double>(f);
    if (!std::isfinite(value) || value <= -1.0 || value >= 0.5) {
        pd_error(x, "besseldrum~: poisson must be finite and satisfy -1 < nu < 0.5");
        return;
    }
    x->poisson = value;
    x->material = gensym("custom");
    besseldrum_update_coefficients(x);
}

// ─────────────────────────────────────
static void besseldrum_loss0(t_besseldrum_tilde *x, t_floatarg f) {
    const double value = static_cast<double>(f);
    if (!finite_nonnegative(value)) {
        pd_error(x, "besseldrum~: loss0 must be finite and >= 0");
        return;
    }
    x->loss0 = value;
    x->material = gensym("custom");
    besseldrum_update_coefficients(x);
}

// ─────────────────────────────────────
static void besseldrum_loss1(t_besseldrum_tilde *x, t_floatarg f) {
    const double value = static_cast<double>(f);
    if (!finite_nonnegative(value)) {
        pd_error(x, "besseldrum~: loss1 must be finite and >= 0");
        return;
    }
    x->loss1 = value;
    x->material = gensym("custom");
    besseldrum_update_coefficients(x);
}

// ─────────────────────────────────────
static void besseldrum_loss2(t_besseldrum_tilde *x, t_floatarg f) {
    const double value = static_cast<double>(f);
    if (!finite_nonnegative(value)) {
        pd_error(x, "besseldrum~: loss2 must be finite and >= 0");
        return;
    }
    x->loss2 = value;
    x->material = gensym("custom");
    besseldrum_update_coefficients(x);
}

// ─────────────────────────────────────
static void besseldrum_material(t_besseldrum_tilde *x, t_symbol *symbol) {
    if (!symbol || !symbol->s_name) {
        pd_error(x, "besseldrum~: material expects a preset name");
        return;
    }

    const MaterialPreset *preset = nullptr;
    for (const MaterialPreset &candidate : kMaterialPresets) {
        if (std::strcmp(symbol->s_name, candidate.name) == 0) {
            preset = &candidate;
            break;
        }
    }

    if (!preset) {
        pd_error(x,
                 "besseldrum~: unknown material '%s' "
                 "(use mylar, calfskin, kevlar, paper, or metal)",
                 symbol->s_name);
        return;
    }

    x->density = preset->density;
    x->thickness = preset->thickness;
    x->young = preset->young;
    x->poisson = preset->poisson;
    x->loss0 = preset->loss0;
    x->loss1 = preset->loss1;
    x->loss2 = preset->loss2;
    x->material = gensym(preset->name);
    // Tension is deliberately not part of a material preset.
    besseldrum_update_coefficients(x);
}

// ─────────────────────────────────────
static void besseldrum_gain(t_besseldrum_tilde *x, t_floatarg f) {
    const double value = static_cast<double>(f);
    if (!finite_nonnegative(value)) {
        pd_error(x, "besseldrum~: gain must be finite and >= 0");
        return;
    }
    x->gain = value;
}

// ─────────────────────────────────────
static void besseldrum_modes(t_besseldrum_tilde *x, t_floatarg f) {
    const double value = static_cast<double>(f);
    if (!std::isfinite(value)) {
        pd_error(x, "besseldrum~: modes must be finite");
        return;
    }
    const int requested = value <= 1.0     ? 1
                          : value >= kMaxModes ? kMaxModes
                                               : static_cast<int>(value);
    besseldrum_rebuild_modes(x, requested);
}

// ─────────────────────────────────────
static void besseldrum_normalize(t_besseldrum_tilde *x, t_floatarg f) {
    x->normalize = (f != 0.0f);
}

// ─────────────────────────────────────
static void besseldrum_print(t_besseldrum_tilde *x) {
    const double sigma = besseldrum_areal_density(x);
    const double bending = besseldrum_bending_stiffness(x);
    post("besseldrum~:");
    post("  material: %s (preset values are approximate)",
         x->material ? x->material->s_name : "custom");
    post("  radius: %.6g m", x->radius);
    post("  density: %.6g kg/m3", x->density);
    post("  thickness: %.6g m", x->thickness);
    post("  tension: %.6g N/m", x->tension);
    post("  Young's modulus: %.6g Pa", x->young);
    post("  Poisson ratio: %.6g", x->poisson);
    post("  areal density sigma: %.6g kg/m2", sigma);
    post("  bending stiffness D: %.6g N*m", bending);
    post("  losses: gamma(f) = %.6g + %.6g*f + %.6g*f^2 [1/s]", x->loss0,
         x->loss1, x->loss2);
    if (x->num_modes > 0)
        post("  fundamental: %.3f Hz", x->modes[0].frequency);
    post("  modes: %d", x->num_modes);
    post("  gain: %.3f", x->gain);
    post("  normalize: %s", x->normalize ? "on" : "off");
    post("  strike position: %.3f %.3f", x->strike_x, x->strike_y);
    post("  pickup position: %.3f %.3f", x->pickup_x, x->pickup_y);
    post("  model note: Bessel membrane modes with a bending dispersion term; "
         "when bending dominates this is not an exact circular-plate solution");
    const int count = std::min(x->num_modes, 8);

    for (int i = 0; i < count; ++i) {
        const Mode &mode = x->modes[i];
        const char *status = "";
        if (!mode.active) {
            const double nyquist = 0.5 * ((x->sample_rate > 1.0) ? x->sample_rate : 44100.0);
            status = (std::isfinite(mode.frequency) && mode.frequency >= nyquist)
                         ? " [above Nyquist]"
                         : " [inactive: invalid physical result]";
        }

        post("  mode %d: "
             "(m=%d n=%d) "
             "alpha=%.6f ratio=%.4f "
             "f=%.2f Hz gamma=%.4g/s%s",
             i + 1, mode.m, mode.n, mode.alpha, mode.ratio, mode.frequency,
             mode.decay_rate, status);
    }
}

// ─────────────────────────────────────
static t_int *besseldrum_perform(t_int *w) {
    auto *x = reinterpret_cast<t_besseldrum_tilde *>(w[1]);
    auto *out = reinterpret_cast<t_sample *>(w[2]);
    const int n = static_cast<int>(w[3]);

    for (int sample = 0; sample < n; ++sample) {
        double sum = 0.0;
        for (int i = 0; i < x->num_modes; ++i) {
            Mode &mode = x->modes[i];
            if (!mode.active)
                continue;

            const double re = mode.re;
            const double im = mode.im;
            const double new_re = mode.a * re - mode.b * im;
            const double new_im = mode.b * re + mode.a * im;

            mode.re = new_re;
            mode.im = new_im;
            sum += new_im;
        }
        out[sample] = static_cast<t_sample>(sum);
    }

    // Kill tiny denormal values after long decays.
    for (int i = 0; i < x->num_modes; ++i) {
        Mode &mode = x->modes[i];
        if (std::abs(mode.re) < 1.0e-20 && std::abs(mode.im) < 1.0e-20) {
            mode.re = 0.0;
            mode.im = 0.0;
        }
    }

    return w + 4;
}

// ─────────────────────────────────────
static void besseldrum_dsp(t_besseldrum_tilde *x, t_signal **sp) {
    x->sample_rate = static_cast<double>(sp[0]->s_sr);
    besseldrum_update_coefficients(x);
    dsp_add(besseldrum_perform, 3, x, sp[0]->s_vec, sp[0]->s_n);
}

// ─────────────────────────────────────
static void besseldrum_free(t_besseldrum_tilde *x) {
    if (x->outlet)
        outlet_free(x->outlet);
}

// ─────────────────────────────────────
static void *besseldrum_new(t_symbol *, int argc, t_atom *argv) {
    auto *x = reinterpret_cast<t_besseldrum_tilde *>(pd_new(besseldrum_tilde_class));

    x->sample_rate = sys_getsr();

    if (!std::isfinite(x->sample_rate) || x->sample_rate <= 1.0)
        x->sample_rate = 44100.0;

    // Defaults: the first material preset plus independent geometry/tension.
    const MaterialPreset &default_material = kMaterialPresets[0];
    x->radius = 0.20;
    x->density = default_material.density;
    x->thickness = default_material.thickness;
    x->tension = 800.0;
    x->young = default_material.young;
    x->poisson = default_material.poisson;
    x->loss0 = default_material.loss0;
    x->loss1 = default_material.loss1;
    x->loss2 = default_material.loss2;
    x->material = gensym(default_material.name);
    x->gain = 0.20;
    x->normalize = 1;
    x->num_modes = 0;

    x->strike_x = -0.25;
    x->strike_y = 0.0;

    x->pickup_x = 0.30;
    x->pickup_y = 0.20;

    int requested_modes = 32;
    double requested_tuning = 0.0;

    // Backward-compatible creation form: [besseldrum~ fundamental modes].
    // The fundamental now tunes the physical model by changing tension.
    if (argc >= 1) {
        const double f = atom_getfloat(argv + 0);

        if (finite_positive(f))
            requested_tuning = f;
    }

    if (argc >= 2) {
        const double modes = atom_getfloat(argv + 1);
        if (std::isfinite(modes)) {
            requested_modes = modes <= 1.0     ? 1
                              : modes >= kMaxModes ? kMaxModes
                                                   : static_cast<int>(modes);
        }
    }

    x->outlet = outlet_new(&x->x_obj, &s_signal);
    besseldrum_rebuild_modes(x, requested_modes);
    if (requested_tuning > 0.0)
        besseldrum_tune(x, static_cast<t_floatarg>(requested_tuning));
    return x;
}

// ─────────────────────────────────────
extern "C" void besseldrum_tilde_setup(void) {
    initialize_mode_table();

    besseldrum_tilde_class =
        class_new(gensym("besseldrum~"), reinterpret_cast<t_newmethod>(besseldrum_new),
                  reinterpret_cast<t_method>(besseldrum_free), sizeof(t_besseldrum_tilde),
                  CLASS_DEFAULT, A_GIMME, 0);

    class_addmethod(besseldrum_tilde_class, reinterpret_cast<t_method>(besseldrum_dsp),
                    gensym("dsp"), A_CANT, 0);

    class_addbang(besseldrum_tilde_class, reinterpret_cast<t_method>(besseldrum_bang));
    class_addfloat(besseldrum_tilde_class, reinterpret_cast<t_method>(besseldrum_float));

    class_addmethod(besseldrum_tilde_class, reinterpret_cast<t_method>(besseldrum_strike),
                    gensym("strike"), A_GIMME, 0);

    class_addmethod(besseldrum_tilde_class, reinterpret_cast<t_method>(besseldrum_strikepos),
                    gensym("strikepos"), A_FLOAT, A_FLOAT, 0);

    class_addmethod(besseldrum_tilde_class, reinterpret_cast<t_method>(besseldrum_pickup),
                    gensym("pickup"), A_FLOAT, A_FLOAT, 0);

    class_addmethod(besseldrum_tilde_class, reinterpret_cast<t_method>(besseldrum_tune),
                    gensym("tune"), A_FLOAT, 0);

    // Compatibility alias: unlike the old implementation, freq changes
    // tension and never overrides the physical frequency calculation.
    class_addmethod(besseldrum_tilde_class, reinterpret_cast<t_method>(besseldrum_tune),
                    gensym("freq"), A_FLOAT, 0);

    class_addmethod(besseldrum_tilde_class, reinterpret_cast<t_method>(besseldrum_radius),
                    gensym("radius"), A_FLOAT, 0);

    class_addmethod(besseldrum_tilde_class, reinterpret_cast<t_method>(besseldrum_density),
                    gensym("density"), A_FLOAT, 0);

    class_addmethod(besseldrum_tilde_class, reinterpret_cast<t_method>(besseldrum_thickness),
                    gensym("thickness"), A_FLOAT, 0);

    class_addmethod(besseldrum_tilde_class, reinterpret_cast<t_method>(besseldrum_tension),
                    gensym("tension"), A_FLOAT, 0);

    class_addmethod(besseldrum_tilde_class, reinterpret_cast<t_method>(besseldrum_young),
                    gensym("young"), A_FLOAT, 0);

    class_addmethod(besseldrum_tilde_class, reinterpret_cast<t_method>(besseldrum_poisson),
                    gensym("poisson"), A_FLOAT, 0);

    class_addmethod(besseldrum_tilde_class, reinterpret_cast<t_method>(besseldrum_loss0),
                    gensym("loss0"), A_FLOAT, 0);

    class_addmethod(besseldrum_tilde_class, reinterpret_cast<t_method>(besseldrum_loss1),
                    gensym("loss1"), A_FLOAT, 0);

    class_addmethod(besseldrum_tilde_class, reinterpret_cast<t_method>(besseldrum_loss2),
                    gensym("loss2"), A_FLOAT, 0);

    class_addmethod(besseldrum_tilde_class, reinterpret_cast<t_method>(besseldrum_material),
                    gensym("material"), A_SYMBOL, 0);

    class_addmethod(besseldrum_tilde_class, reinterpret_cast<t_method>(besseldrum_gain),
                    gensym("gain"), A_FLOAT, 0);

    class_addmethod(besseldrum_tilde_class, reinterpret_cast<t_method>(besseldrum_modes),
                    gensym("modes"), A_FLOAT, 0);

    class_addmethod(besseldrum_tilde_class, reinterpret_cast<t_method>(besseldrum_normalize),
                    gensym("normalize"), A_FLOAT, 0);

    class_addmethod(besseldrum_tilde_class, reinterpret_cast<t_method>(besseldrum_clear),
                    gensym("clear"), A_NULL, 0);

    class_addmethod(besseldrum_tilde_class, reinterpret_cast<t_method>(besseldrum_print),
                    gensym("print"), A_NULL, 0);

    post("besseldrum~: circular membrane "
         "modal synthesizer (%d modes max)",
         kMaxModes);
}
