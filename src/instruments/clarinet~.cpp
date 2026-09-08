#include "m_pd.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>

namespace {

constexpr double PI = 3.141592653589793238462643383279502884;
constexpr int MAX_BORE_MODES = 32;
constexpr int MAX_REED_MODES = 4;

struct Complex {
    double r = 0.0;
    double i = 0.0;
};

inline Complex cadd(Complex a, Complex b) { return {a.r + b.r, a.i + b.i}; }
inline Complex cmul(Complex a, Complex b) { return {a.r * b.r - a.i * b.i, a.r * b.i + a.i * b.r}; }
inline Complex cscale(Complex a, double s) { return {a.r * s, a.i * s}; }
inline Complex cdiv(Complex a, Complex b) {
    const double d = b.r * b.r + b.i * b.i;
    if (d < 1.0e-30)
        return {0.0, 0.0};
    return {(a.r * b.r + a.i * b.i) / d, (a.i * b.r - a.r * b.i) / d};
}
inline Complex cexp_complex(Complex z) {
    const double e = std::exp(z.r);
    return {e * std::cos(z.i), e * std::sin(z.i)};
}

inline double midi_to_freq(double midi) { return 440.0 * std::pow(2.0, (midi - 69.0) / 12.0); }

inline double clampd(double x, double lo, double hi) { return std::max(lo, std::min(x, hi)); }

struct BoreMode {
    // Continuous-time pole and residue parameters are converted to
    // z[k+1] = a z[k] + b U[k].
    Complex a;
    Complex b;
    Complex z;

    double ratio = 1.0;
    double q = 40.0;
    double residue_re = 1.0; // normalized to 4*f0
    double residue_im = 0.0;
    double frequency = 0.0;
    bool custom = false;
    bool active = false;
};

struct ReedMode {
    double x = 0.0; // tip-displacement modal contribution [m]
    double v = 0.0; // tip velocity contribution [m/s]

    // Exact ZOH transition of the unforced damped oscillator.
    double p11 = 0.0;
    double p12 = 0.0;
    double p21 = 0.0;
    double p22 = 0.0;

    // Contribution per Pascal of pressure difference for one sample.
    double x_gain = 0.0;
    double v_gain = 0.0;

    double frequency = 0.0;
    double q = 2.5;
    bool active = false;
};

class ClarinetModel {
  public:
    explicit ClarinetModel(double sampleRate = 48000.0) {
        setSampleRate(sampleRate);
        resetCustomModes();
        clear();
    }

    void setSampleRate(double sr) {
        if (!(sr > 1000.0) || !std::isfinite(sr))
            sr = 48000.0;
        sample_rate_ = sr;
        inv_sample_rate_ = 1.0 / sr;
        updateAir();
        updateEnvelope();
        updateReed();
        updateBore();
    }

    void clear() {
        for (int i = 0; i < MAX_BORE_MODES; ++i) {
            bore_[i].z = {};
        }
        for (int i = 0; i < MAX_REED_MODES; ++i) {
            reed_[i].x = 0.0;
            reed_[i].v = 0.0;
        }
        mouth_pressure_ = 0.0;
        last_delta_p_ = 0.0;
        last_pressure_ = 0.0;
        dc_x1_ = dc_y1_ = 0.0;
    }

    void noteOn(double frequency, double velocity01) {
        setFrequency(frequency);
        const double v = clampd(velocity01, 0.0, 1.0);
        // Keep low velocities physically capable of falling below the
        // oscillation threshold rather than forcing every note to speak.
        pressure_target_ = max_pressure_ * std::pow(v, 1.20);
    }

    void noteOff() { pressure_target_ = 0.0; }

    void setFrequency(double f) {
        if (!(f > 10.0) || !std::isfinite(f))
            return;
        fundamental_ = f;
        updateBore();
    }

    void setBreath(double x) { pressure_target_ = max_pressure_ * clampd(x, 0.0, 1.0); }

    void setPressurePa(double p) { pressure_target_ = clampd(p, 0.0, 20000.0); }

    void setMaxPressure(double p) {
        if (p > 100.0 && std::isfinite(p))
            max_pressure_ = std::min(p, 20000.0);
    }

    void setAttackMs(double ms) {
        attack_ms_ = clampd(ms, 0.1, 5000.0);
        updateEnvelope();
    }

    void setReleaseMs(double ms) {
        release_ms_ = clampd(ms, 0.1, 5000.0);
        updateEnvelope();
    }

    void setTemperature(double celsius) {
        temperature_ = clampd(celsius, -20.0, 50.0);
        updateAir();
        updateBore();
    }

    void setReedFrequency(double f) {
        if (f > 50.0 && std::isfinite(f)) {
            reed_frequency_ = std::min(f, 18000.0);
            updateReed();
        }
    }

    void setReedQ(double q) {
        reed_q_ = clampd(q, 0.55, 30.0);
        updateReed();
    }

    void setReedModes(int n) {
        reed_modes_ = std::max(1, std::min(n, MAX_REED_MODES));
        updateReed();
    }

    void setOpening(double metres) {
        if (metres > 1.0e-6 && metres < 0.005 && std::isfinite(metres)) {
            opening_ = metres;
            updateReed();
        }
    }

    void setClosePressure(double pa) {
        if (pa > 100.0 && pa < 30000.0 && std::isfinite(pa)) {
            close_pressure_ = pa;
            updateReed();
        }
    }

    void setReedWidth(double metres) {
        if (metres > 1.0e-4 && metres < 0.05 && std::isfinite(metres))
            reed_width_ = metres;
    }

    void setReedArea(double m2) {
        if (m2 >= 0.0 && m2 < 0.01 && std::isfinite(m2))
            reed_area_ = m2;
    }

    void setReedMotion(double amount) { reed_motion_ = clampd(amount, 0.0, 2.0); }
    void setDischarge(double cd) { discharge_ = clampd(cd, 0.05, 1.2); }
    void setContactRestitution(double r) { contact_restitution_ = clampd(r, 0.0, 0.5); }

    void setBoreRadius(double r) {
        if (r > 0.001 && r < 0.05 && std::isfinite(r)) {
            bore_radius_ = r;
            updateBore();
        }
    }

    void setBoreModes(int n) {
        bore_modes_ = std::max(1, std::min(n, MAX_BORE_MODES));
        updateBore();
    }

    void setBoreQ(double q) {
        bore_q0_ = clampd(q, 2.0, 200.0);
        updateBore();
    }

    void setBoreQLoss(double x) {
        bore_q_loss_ = clampd(x, 0.0, 1.0);
        updateBore();
    }

    void setStretch(double x) {
        // Generic frequency-dependent shortening correction.  It is not a
        // substitute for fitted measured impedance poles.
        stretch_ = clampd(x, -0.01, 0.02);
        updateBore();
    }

    void setNoise(double n) { noise_ = clampd(n, 0.0, 1.0); }
    void setGain(double g) { gain_ = clampd(g, 0.0, 0.01); }

    bool setMode(int index1, double ratio, double q, double cre, double cim) {
        const int i = index1 - 1;
        if (i < 0 || i >= MAX_BORE_MODES)
            return false;
        if (!(ratio > 0.1) || !(q > 0.5) || !std::isfinite(ratio) || !std::isfinite(q) ||
            !std::isfinite(cre) || !std::isfinite(cim))
            return false;

        bore_[i].ratio = ratio;
        bore_[i].q = q;
        bore_[i].residue_re = cre;
        bore_[i].residue_im = cim;
        bore_[i].custom = true;
        updateBore();
        return true;
    }

    void resetCustomModes() {
        for (int i = 0; i < MAX_BORE_MODES; ++i) {
            bore_[i].custom = false;
            bore_[i].ratio = 2.0 * i + 1.0;
            bore_[i].q = bore_q0_;
            bore_[i].residue_re = 1.0;
            bore_[i].residue_im = 0.0;
        }
        updateBore();
    }

    double tick() {
        // Smooth the player's mouth pressure.
        const bool rising = pressure_target_ > mouth_pressure_;
        const double c = rising ? attack_coeff_ : release_coeff_;
        mouth_pressure_ = c * mouth_pressure_ + (1.0 - c) * pressure_target_;

        // Propagate the acoustic modes without this sample's volume flow.
        double p_free = 0.0;
        for (int i = 0; i < bore_modes_; ++i) {
            BoreMode &m = bore_[i];
            if (!m.active)
                continue;
            bore_free_[i] = cmul(m.a, m.z);
            p_free += 2.0 * bore_free_[i].r;
        }

        // Predict the dynamic reed as an affine function of pressure drop:
        // H(dp) = H_free + H_gain * dp
        // V(dp) = V_free + V_gain * dp
        double h_free = opening_;
        double h_gain = 0.0;
        double v_free = 0.0;
        double v_gain = 0.0;

        for (int i = 0; i < reed_modes_; ++i) {
            ReedMode &m = reed_[i];
            if (!m.active) {
                reed_x_free_[i] = m.x;
                reed_v_free_[i] = m.v;
                continue;
            }
            reed_x_free_[i] = m.p11 * m.x + m.p12 * m.v;
            reed_v_free_[i] = m.p21 * m.x + m.p22 * m.v;
            h_free += reed_x_free_[i];
            h_gain += m.x_gain;
            v_free += reed_v_free_[i];
            v_gain += m.v_gain;
        }

        const double target = mouth_pressure_ - p_free;

        // Solve dp + K*U(dp) = Pm - p_free.
        double dp = std::isfinite(last_delta_p_) ? last_delta_p_ : target;
        dp = clampd(dp, -20000.0, 20000.0);

        for (int iteration = 0; iteration < 8; ++iteration) {
            const double u = deterministicFlow(dp, h_free, h_gain, v_free, v_gain);
            const double f = dp + bore_feedthrough_ * u - target;
            if (std::abs(f) < 1.0e-5)
                break;

            const double eps = 0.1 + 1.0e-4 * std::abs(dp);
            const double up = deterministicFlow(dp + eps, h_free, h_gain, v_free, v_gain);
            const double um = deterministicFlow(dp - eps, h_free, h_gain, v_free, v_gain);
            double deriv = 1.0 + bore_feedthrough_ * (up - um) / (2.0 * eps);

            if (!std::isfinite(deriv) || std::abs(deriv) < 1.0e-7)
                deriv = 1.0;
            double step = f / deriv;
            step = clampd(step, -4000.0, 4000.0);
            dp = clampd(dp - step, -20000.0, 20000.0);
        }

        last_delta_p_ = dp;

        // Commit reed state using the converged pressure drop.
        double actual_opening = opening_;
        double tip_velocity = 0.0;
        int first_active = -1;
        for (int i = 0; i < reed_modes_; ++i) {
            ReedMode &m = reed_[i];
            if (m.active) {
                if (first_active < 0)
                    first_active = i;
                m.x = reed_x_free_[i] + m.x_gain * dp;
                m.v = reed_v_free_[i] + m.v_gain * dp;
            }
            actual_opening += m.x;
            tip_velocity += m.v;
        }

        // Unilateral lay contact.  This is a stable projection/contact
        // approximation: the channel cannot have negative height, and inward
        // tip velocity is reflected with a small restitution.
        if (actual_opening < 0.0 && first_active >= 0) {
            reed_[first_active].x += -actual_opening;
            if (tip_velocity < 0.0) {
                reed_[first_active].v += -(1.0 + contact_restitution_) * tip_velocity;
            }
            actual_opening = 0.0;
        }

        double flow = deterministicFlow(dp, h_free, h_gain, v_free, v_gain);

        // Turbulence is proportional to the current jet flow and is injected
        // after the implicit junction solve, so Newton iteration remains deterministic.
        if (noise_ > 0.0) {
            const double r = randomBipolar();
            flow += noise_ * 0.08 * std::abs(flow) * r;
        }
        flow = clampd(flow, -0.003, 0.003); // 3 L/s safety bound

        // Commit the acoustic modal states.
        double pressure = 0.0;
        bool stable = true;
        for (int i = 0; i < bore_modes_; ++i) {
            BoreMode &m = bore_[i];
            if (!m.active)
                continue;
            m.z = cadd(bore_free_[i], cscale(m.b, flow));
            pressure += 2.0 * m.z.r;
            if (!std::isfinite(m.z.r) || !std::isfinite(m.z.i) || std::abs(m.z.r) > 1.0e7 ||
                std::abs(m.z.i) > 1.0e7) {
                stable = false;
            }
        }

        if (!stable || !std::isfinite(pressure)) {
            clear();
            return 0.0;
        }

        last_pressure_ = pressure;

        // Very-low-frequency blocker only; this is not intended as a timbral EQ.
        const double dc_r = std::exp(-2.0 * PI * 15.0 * inv_sample_rate_);
        const double hp = pressure - dc_x1_ + dc_r * dc_y1_;
        dc_x1_ = pressure;
        dc_y1_ = hp;

        const double out = hp * gain_;
        return std::isfinite(out) ? out : 0.0;
    }

    // Getters used by Pd's print message.
    double sampleRate() const { return sample_rate_; }
    double frequency() const { return fundamental_; }
    double mouthPressure() const { return mouth_pressure_; }
    double targetPressure() const { return pressure_target_; }
    double reedFrequency() const { return reed_frequency_; }
    double reedQ() const { return reed_q_; }
    int reedModes() const { return reed_modes_; }
    double opening() const { return opening_; }
    double closePressure() const { return close_pressure_; }
    double boreRadius() const { return bore_radius_; }
    int boreModes() const { return bore_modes_; }
    double characteristicImpedance() const { return characteristic_impedance_; }
    double boreFeedthrough() const { return bore_feedthrough_; }
    const BoreMode &boreMode(int i) const { return bore_[i]; }

  private:
    double deterministicFlow(double dp, double h_free, double h_gain, double v_free,
                             double v_gain) const {
        const double opening = std::max(0.0, h_free + h_gain * dp);
        const double velocity = v_free + v_gain * dp;

        double jet = 0.0;
        const double adp = std::abs(dp);
        if (opening > 0.0 && adp > 1.0e-12) {
            const double speed = std::sqrt(2.0 * adp / air_density_);
            jet = discharge_ * reed_width_ * opening * speed;
            if (dp < 0.0)
                jet = -jet;
        }

        // Reed-induced volume flow. Positive tip velocity opens the reed.
        const double reed_flow = reed_motion_ * reed_area_ * velocity;
        return clampd(jet + reed_flow, -0.003, 0.003);
    }

    double randomBipolar() {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        const double u = static_cast<double>(rng_) / 4294967295.0;
        return 2.0 * u - 1.0;
    }

    void updateAir() {
        sound_speed_ = 331.3 + 0.606 * temperature_;
        // Ideal-gas approximation around 1 atmosphere.
        air_density_ = 1.2929 * 273.15 / (273.15 + temperature_);
        const double area = PI * bore_radius_ * bore_radius_;
        characteristic_impedance_ = air_density_ * sound_speed_ / area;
    }

    void updateEnvelope() {
        const double attack_s = std::max(attack_ms_ * 0.001, 1.0e-6);
        const double release_s = std::max(release_ms_ * 0.001, 1.0e-6);
        attack_coeff_ = std::exp(-1.0 / (attack_s * sample_rate_));
        release_coeff_ = std::exp(-1.0 / (release_s * sample_rate_));
    }

    void updateReed() {
        // Euler-Bernoulli cantilever eigenfrequency ratios (beta_n/beta_1)^2.
        static constexpr double ratios[MAX_REED_MODES] = {1.0, 6.266893025770666,
                                                          17.547481936808445, 34.38606115720301};

        // Relative static tip compliances for a clamped-free beam under a
        // distributed load, using normalized cantilever eigenfunctions.
        // Alternating signs come from the modal force projections.
        static constexpr double compliance[MAX_REED_MODES] = {
            1.5659835096336976, -0.0220978999865532, 0.0016525730933614, -0.0003076756238552};

        double sum_compliance = 0.0;
        for (int i = 0; i < reed_modes_; ++i) {
            const double f = reed_frequency_ * ratios[i];
            if (f < 0.45 * sample_rate_)
                sum_compliance += compliance[i];
        }
        if (std::abs(sum_compliance) < 1.0e-12)
            sum_compliance = compliance[0];

        for (int i = 0; i < MAX_REED_MODES; ++i) {
            ReedMode &m = reed_[i];
            m.frequency = reed_frequency_ * ratios[i];
            m.q = std::max(0.55, reed_q_ / (1.0 + 0.12 * i));
            m.active = i < reed_modes_ && m.frequency < 0.45 * sample_rate_;

            if (!m.active) {
                m.p11 = 1.0;
                m.p12 = 0.0;
                m.p21 = 0.0;
                m.p22 = 1.0;
                m.x_gain = m.v_gain = 0.0;
                continue;
            }

            const double w = 2.0 * PI * m.frequency;
            const double zeta = 1.0 / (2.0 * m.q);
            const double sigma = zeta * w;
            const double inside = std::max(1.0e-12, 1.0 - zeta * zeta);
            const double wd = w * std::sqrt(inside);
            const double e = std::exp(-sigma * inv_sample_rate_);
            const double s = std::sin(wd * inv_sample_rate_);
            const double c = std::cos(wd * inv_sample_rate_);
            const double inv_wd = 1.0 / wd;

            m.p11 = e * (c + sigma * inv_wd * s);
            m.p12 = e * (inv_wd * s);
            m.p21 = e * (-w * w * inv_wd * s);
            m.p22 = e * (c - sigma * inv_wd * s);

            // Static tip displacement caused by dp.  The scale is chosen so
            // that dp = close_pressure gives, in static equilibrium, a total
            // reed displacement of -opening_.
            const double static_gain =
                -(opening_ / close_pressure_) * (compliance[i] / sum_compliance);

            m.x_gain = (1.0 - m.p11) * static_gain;
            m.v_gain = -m.p21 * static_gain;
        }
    }

    void updateBore() {
        updateAir();

        const double base_residue = 4.0 * fundamental_; // c/L for L=c/(4 f0)
        bore_feedthrough_ = 0.0;

        for (int i = 0; i < MAX_BORE_MODES; ++i) {
            BoreMode &m = bore_[i];

            if (!m.custom) {
                const double odd = 2.0 * i + 1.0;
                m.ratio = odd * (1.0 + stretch_ * i);
                m.q = std::max(2.0, bore_q0_ / (1.0 + bore_q_loss_ * i));
                m.residue_re = 1.0;
                m.residue_im = 0.0;
            }

            m.frequency = fundamental_ * m.ratio;
            m.active = i < bore_modes_ && m.frequency > 5.0 && m.frequency < 0.47 * sample_rate_;

            if (!m.active) {
                m.a = {};
                m.b = {};
                continue;
            }

            const double alpha = PI * m.frequency / m.q;
            const double omega = 2.0 * PI * m.frequency;
            const Complex pole{-alpha, omega};
            const Complex a = cexp_complex(cscale(pole, inv_sample_rate_));
            const Complex a_minus_one{a.r - 1.0, a.i};
            const Complex integral = cdiv(a_minus_one, pole);

            const Complex residue{base_residue * m.residue_re, base_residue * m.residue_im};

            m.a = a;
            m.b = cscale(cmul(residue, integral), characteristic_impedance_);
            bore_feedthrough_ += 2.0 * m.b.r;
        }
    }

  private:
    double sample_rate_ = 48000.0;
    double inv_sample_rate_ = 1.0 / 48000.0;

    double temperature_ = 20.0;
    double sound_speed_ = 343.42;
    double air_density_ = 1.204;

    double fundamental_ = 146.832;
    double bore_radius_ = 0.0075;
    int bore_modes_ = 16;
    double bore_q0_ = 42.0;
    double bore_q_loss_ = 0.075;
    double stretch_ = 0.0012;
    double characteristic_impedance_ = 0.0;
    double bore_feedthrough_ = 0.0;

    BoreMode bore_[MAX_BORE_MODES];
    Complex bore_free_[MAX_BORE_MODES];

    double reed_frequency_ = 2200.0;
    double reed_q_ = 2.8;
    int reed_modes_ = 2;
    double opening_ = 0.00040;
    double close_pressure_ = 7000.0;
    double reed_width_ = 0.012;
    double reed_area_ = 8.0e-5;
    double reed_motion_ = 0.20;
    double discharge_ = 0.72;
    double contact_restitution_ = 0.03;

    ReedMode reed_[MAX_REED_MODES];
    double reed_x_free_[MAX_REED_MODES]{};
    double reed_v_free_[MAX_REED_MODES]{};

    double max_pressure_ = 6500.0;
    double pressure_target_ = 0.0;
    double mouth_pressure_ = 0.0;
    double attack_ms_ = 12.0;
    double release_ms_ = 70.0;
    double attack_coeff_ = 0.0;
    double release_coeff_ = 0.0;

    double noise_ = 0.06;
    double gain_ = 0.00012;

    double last_delta_p_ = 0.0;
    double last_pressure_ = 0.0;
    double dc_x1_ = 0.0;
    double dc_y1_ = 0.0;

    std::uint32_t rng_ = 0x7f4a7c15u;
};

} // namespace

// -----------------------------------------------------------------------------
// Pure Data wrapper
// -----------------------------------------------------------------------------

typedef struct _clarinet_tilde {
    t_object x_obj;
    ClarinetModel *model;
    t_outlet *out;
} t_clarinet_tilde;

static t_class *clarinet_tilde_class = nullptr;

static void clarinet_tilde_list(t_clarinet_tilde *x, t_symbol *, int argc, t_atom *argv) {
    if (!x->model)
        return;
    if (argc != 2) {
        pd_error(x, "[clarinet~] list expects: <midi-pitch> <velocity 0..127>");
        return;
    }
    const double midi = atom_getfloat(argv);
    const double velocity = atom_getfloat(argv + 1);
    if (velocity <= 0.0) {
        x->model->noteOff();
        return;
    }
    x->model->noteOn(midi_to_freq(midi), clampd(velocity / 127.0, 0.0, 1.0));
}

static void clarinet_tilde_freq(t_clarinet_tilde *x, t_floatarg f) {
    if (x->model && f > 0)
        x->model->setFrequency(f);
}
static void clarinet_tilde_breath(t_clarinet_tilde *x, t_floatarg f) {
    if (x->model)
        x->model->setBreath(f);
}
static void clarinet_tilde_pressure(t_clarinet_tilde *x, t_floatarg f) {
    if (x->model)
        x->model->setPressurePa(f);
}
static void clarinet_tilde_maxpressure(t_clarinet_tilde *x, t_floatarg f) {
    if (x->model)
        x->model->setMaxPressure(f);
}
static void clarinet_tilde_attack(t_clarinet_tilde *x, t_floatarg f) {
    if (x->model)
        x->model->setAttackMs(f);
}
static void clarinet_tilde_release(t_clarinet_tilde *x, t_floatarg f) {
    if (x->model)
        x->model->setReleaseMs(f);
}
static void clarinet_tilde_temperature(t_clarinet_tilde *x, t_floatarg f) {
    if (x->model)
        x->model->setTemperature(f);
}
static void clarinet_tilde_reedfreq(t_clarinet_tilde *x, t_floatarg f) {
    if (x->model)
        x->model->setReedFrequency(f);
}
static void clarinet_tilde_reedq(t_clarinet_tilde *x, t_floatarg f) {
    if (x->model)
        x->model->setReedQ(f);
}
static void clarinet_tilde_reedmodes(t_clarinet_tilde *x, t_floatarg f) {
    if (x->model)
        x->model->setReedModes(static_cast<int>(f));
}
static void clarinet_tilde_opening(t_clarinet_tilde *x, t_floatarg f) {
    if (x->model)
        x->model->setOpening(f);
}
static void clarinet_tilde_closepressure(t_clarinet_tilde *x, t_floatarg f) {
    if (x->model)
        x->model->setClosePressure(f);
}
static void clarinet_tilde_reedwidth(t_clarinet_tilde *x, t_floatarg f) {
    if (x->model)
        x->model->setReedWidth(f);
}
static void clarinet_tilde_reedarea(t_clarinet_tilde *x, t_floatarg f) {
    if (x->model)
        x->model->setReedArea(f);
}
static void clarinet_tilde_reedmotion(t_clarinet_tilde *x, t_floatarg f) {
    if (x->model)
        x->model->setReedMotion(f);
}
static void clarinet_tilde_discharge(t_clarinet_tilde *x, t_floatarg f) {
    if (x->model)
        x->model->setDischarge(f);
}
static void clarinet_tilde_contact(t_clarinet_tilde *x, t_floatarg f) {
    if (x->model)
        x->model->setContactRestitution(f);
}
static void clarinet_tilde_boreradius(t_clarinet_tilde *x, t_floatarg f) {
    if (x->model)
        x->model->setBoreRadius(f);
}
static void clarinet_tilde_modes(t_clarinet_tilde *x, t_floatarg f) {
    if (x->model)
        x->model->setBoreModes(static_cast<int>(f));
}
static void clarinet_tilde_boreq(t_clarinet_tilde *x, t_floatarg f) {
    if (x->model)
        x->model->setBoreQ(f);
}
static void clarinet_tilde_qloss(t_clarinet_tilde *x, t_floatarg f) {
    if (x->model)
        x->model->setBoreQLoss(f);
}
static void clarinet_tilde_stretch(t_clarinet_tilde *x, t_floatarg f) {
    if (x->model)
        x->model->setStretch(f);
}
static void clarinet_tilde_noise(t_clarinet_tilde *x, t_floatarg f) {
    if (x->model)
        x->model->setNoise(f);
}
static void clarinet_tilde_gain(t_clarinet_tilde *x, t_floatarg f) {
    if (x->model)
        x->model->setGain(f);
}

static void clarinet_tilde_mode(t_clarinet_tilde *x, t_symbol *, int argc, t_atom *argv) {
    if (!x->model)
        return;
    if (argc != 5) {
        pd_error(x, "[clarinet~] mode expects: <index> <ratio> <Q> <residue-re> <residue-im>");
        return;
    }
    const int index = atom_getint(argv);
    const double ratio = atom_getfloat(argv + 1);
    const double q = atom_getfloat(argv + 2);
    const double cre = atom_getfloat(argv + 3);
    const double cim = atom_getfloat(argv + 4);
    if (!x->model->setMode(index, ratio, q, cre, cim)) {
        pd_error(x, "[clarinet~] invalid mode parameters");
    }
}

static void clarinet_tilde_resetmodes(t_clarinet_tilde *x) {
    if (x->model)
        x->model->resetCustomModes();
}

static void clarinet_tilde_clear(t_clarinet_tilde *x) {
    if (x->model)
        x->model->clear();
}

static void clarinet_tilde_print(t_clarinet_tilde *x) {
    if (!x->model)
        return;
    post("clarinet~ modal physical model:");
    post("  sample rate: %.1f Hz", x->model->sampleRate());
    post("  fundamental: %.3f Hz", x->model->frequency());
    post("  mouth pressure: %.1f Pa (target %.1f Pa)", x->model->mouthPressure(),
         x->model->targetPressure());
    post("  reed: %.1f Hz, Q %.2f, %d modes", x->model->reedFrequency(), x->model->reedQ(),
         x->model->reedModes());
    post("  opening: %.6f m, closing pressure: %.1f Pa", x->model->opening(),
         x->model->closePressure());
    post("  bore radius: %.6f m, %d modes, Zc %.3e", x->model->boreRadius(), x->model->boreModes(),
         x->model->characteristicImpedance());
    post("  discrete instantaneous bore impedance K: %.3e", x->model->boreFeedthrough());

    const int n = std::min(8, x->model->boreModes());
    for (int i = 0; i < n; ++i) {
        const BoreMode &m = x->model->boreMode(i);
        post("  mode %d: %.2f Hz, ratio %.5f, Q %.2f, C=(%.3f, %.3f)%s", i + 1, m.frequency,
             m.ratio, m.q, m.residue_re, m.residue_im, m.custom ? " custom" : "");
    }
}

static t_int *clarinet_tilde_perform(t_int *w) {
    auto *x = reinterpret_cast<t_clarinet_tilde *>(w[1]);
    auto *out = reinterpret_cast<t_sample *>(w[2]);
    const int n = static_cast<int>(w[3]);

    if (!x->model) {
        std::fill(out, out + n, static_cast<t_sample>(0));
        return w + 4;
    }

    for (int i = 0; i < n; ++i)
        out[i] = static_cast<t_sample>(x->model->tick());

    return w + 4;
}

static void clarinet_tilde_dsp(t_clarinet_tilde *x, t_signal **sp) {
    if (x->model && sp[0]->s_sr > 1000.0)
        x->model->setSampleRate(sp[0]->s_sr);

    dsp_add(clarinet_tilde_perform, 3, x, sp[0]->s_vec, sp[0]->s_n);
}

static void *clarinet_tilde_new(t_symbol *, int argc, t_atom *argv) {
    auto *x = reinterpret_cast<t_clarinet_tilde *>(pd_new(clarinet_tilde_class));
    x->model = nullptr;
    x->out = nullptr;

    double sr = sys_getsr();
    if (!(sr > 1000.0))
        sr = 48000.0;

    try {
        x->model = new ClarinetModel(sr);
    } catch (const std::bad_alloc &) {
        pd_error(x, "[clarinet~] could not allocate physical model");
    } catch (...) {
        pd_error(x, "[clarinet~] unexpected construction error");
    }

    if (x->model) {
        if (argc >= 1) {
            const double f = atom_getfloat(argv);
            if (f > 0.0)
                x->model->setFrequency(f);
        }
        if (argc >= 2) {
            const int modes = atom_getint(argv + 1);
            if (modes > 0)
                x->model->setBoreModes(modes);
        }
    }

    x->out = outlet_new(&x->x_obj, &s_signal);
    return x;
}

static void clarinet_tilde_free(t_clarinet_tilde *x) {
    delete x->model;
    x->model = nullptr;
    if (x->out) {
        outlet_free(x->out);
        x->out = nullptr;
    }
}

extern "C" void clarinet_tilde_setup(void) {
    clarinet_tilde_class =
        class_new(gensym("clarinet~"), reinterpret_cast<t_newmethod>(clarinet_tilde_new),
                  reinterpret_cast<t_method>(clarinet_tilde_free), sizeof(t_clarinet_tilde),
                  CLASS_DEFAULT, A_GIMME, 0);

    class_addmethod(clarinet_tilde_class, reinterpret_cast<t_method>(clarinet_tilde_dsp),
                    gensym("dsp"), A_CANT, 0);

    class_addlist(clarinet_tilde_class, reinterpret_cast<t_method>(clarinet_tilde_list));

#define ADD_FLOAT_METHOD(name, fn)                                                                 \
    class_addmethod(clarinet_tilde_class, reinterpret_cast<t_method>(fn), gensym(name), A_FLOAT, 0)

    ADD_FLOAT_METHOD("freq", clarinet_tilde_freq);
    ADD_FLOAT_METHOD("breath", clarinet_tilde_breath);
    ADD_FLOAT_METHOD("pressure", clarinet_tilde_pressure);
    ADD_FLOAT_METHOD("maxpressure", clarinet_tilde_maxpressure);
    ADD_FLOAT_METHOD("attack", clarinet_tilde_attack);
    ADD_FLOAT_METHOD("release", clarinet_tilde_release);
    ADD_FLOAT_METHOD("temperature", clarinet_tilde_temperature);
    ADD_FLOAT_METHOD("reedfreq", clarinet_tilde_reedfreq);
    ADD_FLOAT_METHOD("reedq", clarinet_tilde_reedq);
    ADD_FLOAT_METHOD("reedmodes", clarinet_tilde_reedmodes);
    ADD_FLOAT_METHOD("opening", clarinet_tilde_opening);
    ADD_FLOAT_METHOD("closepressure", clarinet_tilde_closepressure);
    ADD_FLOAT_METHOD("reedwidth", clarinet_tilde_reedwidth);
    ADD_FLOAT_METHOD("reedarea", clarinet_tilde_reedarea);
    ADD_FLOAT_METHOD("reedmotion", clarinet_tilde_reedmotion);
    ADD_FLOAT_METHOD("discharge", clarinet_tilde_discharge);
    ADD_FLOAT_METHOD("contact", clarinet_tilde_contact);
    ADD_FLOAT_METHOD("boreradius", clarinet_tilde_boreradius);
    ADD_FLOAT_METHOD("modes", clarinet_tilde_modes);
    ADD_FLOAT_METHOD("boreq", clarinet_tilde_boreq);
    ADD_FLOAT_METHOD("qloss", clarinet_tilde_qloss);
    ADD_FLOAT_METHOD("stretch", clarinet_tilde_stretch);
    ADD_FLOAT_METHOD("noise", clarinet_tilde_noise);
    ADD_FLOAT_METHOD("gain", clarinet_tilde_gain);

#undef ADD_FLOAT_METHOD

    class_addmethod(clarinet_tilde_class, reinterpret_cast<t_method>(clarinet_tilde_mode),
                    gensym("mode"), A_GIMME, 0);

    // class_addmethod(clarinet_tilde_class, reinterpret_cast<t_method>(clarinet_tilde_resetmodes),
    //                 gensym("resetmodes"), 0);
    //
    // class_addmethod(clarinet_tilde_class, reinterpret_cast<t_method>(clarinet_tilde_clear),
    //                 gensym("clear"), 0);
    //
    // class_addmethod(clarinet_tilde_class, reinterpret_cast<t_method>(clarinet_tilde_print),
    //                 gensym("print"), 0);

    post("clarinet~: multimodal reed + modal input-impedance physical model");
}
