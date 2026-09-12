// Standalone: c++ -std=c++17 -O2 -ffunction-sections -fdata-sections \
// -I/usr/include/pd tests/rain_dsp.cpp -Wl,--gc-sections -o /tmp/rain_dsp_test
#include "../src/others/rain~.cpp"
#include <cassert>
#include <cstdio>
#include <limits>
#include <memory>
#include <vector>

static t_rain_tilde storage;
t_symbol s_signal;
extern "C" t_pd *pd_new(t_class *) { return reinterpret_cast<t_pd *>(&storage); }
extern "C" t_outlet *outlet_new(t_object *, t_symbol *) { return nullptr; }
extern "C" t_float sys_getsr() { return 48000; }
extern "C" void dsp_add(t_perfroutine, int, ...) {}
static int errors = 0;
extern "C" void pd_error(const void *, const char *, ...) { ++errors; }

static std::unique_ptr<t_rain_tilde> fresh() {
    auto x = std::make_unique<t_rain_tilde>(*static_cast<t_rain_tilde *>(rain_new(800)));
    rain_seed(x.get(), 1234);
    x->hybrid = false;
    x->hybrid_mix = 0; // Equation checks use strict paper mode.
    return x;
}
struct Output {
    std::vector<t_sample> left;
};
static Output render(t_rain_tilde *x, int count, int block = 64) {
    Output out{std::vector<t_sample>(count)};
    for (int pos = 0; pos < count; pos += block) {
        int n = std::min(block, count - pos);
        t_int w[] = {0, reinterpret_cast<t_int>(x), reinterpret_cast<t_int>(out.left.data() + pos),
                     n};
        assert(rain_perform(w) == w + 4);
    }
    for (int i = 0; i < count; ++i)
        assert(std::isfinite(out.left[i]));
    return out;
}
static double energy(const std::vector<t_sample> &v) {
    double s = 0;
    for (auto a : v)
        s += double(a) * a;
    return s;
}
static void sr(t_rain_tilde *x, double rate) {
    t_signal a{};
    a.s_sr = rate;
    t_signal *signals[] = {&a};
    rain_dsp(x, signals);
}
int main(int argc, char **argv) {
    // Eq. 2 matches its literal printed expression at points inside its support.
    for (double a : {0.0005, 0.002, 0.01})
        for (double distance : {0.1, 1.0, 10.0}) {
            double H = 1.7, c = 343, rho = 1.2;
            double Rs = std::hypot(distance - a, H), Rl = std::hypot(distance + a, H);
            double duration = (Rl - Rs) / c;
            assert(rain_eq2(-1, a, distance, H, rho, c) == 0);
            assert(rain_eq2(duration * 1.1, a, distance, H, rho, c) == 0);
            for (double fraction : {0.1, 0.25, 0.5, 0.75, 0.9}) {
                double t = Rs / c + duration * fraction;
                double r2 = c * c * t * t - H * H;
                double expected =
                    rho * c / PI *
                    std::acos((r2 + distance * distance - a * a) / (2 * distance * std::sqrt(r2)));
                double actual = rain_eq2(duration * fraction, a, distance, H, rho, c);
                assert(std::fabs(actual - expected) < 1e-6 + expected * 1e-5);
            }
        }
    // Independent Eq. 1 check: time-integrated pressure equals
    // rho/(2pi) * integral_disk 1/R dS for a unit step velocity.
    // Numerical polar integration of the disk provides a separate reference.
    double a = 0.01, x0 = 0.5, H = 1.7, rho = 1.2, c = 343;
    double disk = 0;
    const int nr = 128, nt = 256;
    for (int i = 0; i < nr; ++i) {
        double radius = a * (i + 0.5) / nr;
        for (int j = 0; j < nt; ++j) {
            double phi = 2 * PI * (j + 0.5) / nt;
            double R =
                std::sqrt(H * H + x0 * x0 + radius * radius + 2 * x0 * radius * std::cos(phi));
            disk += radius / R * (a / nr) * (2 * PI / nt);
        }
    }
    double integral = rain_bin(0, 1, a, x0, H, rho, c, 48000) / 48000;
    assert(std::fabs(integral - rho / (2 * PI) * disk) / integral < 0.002);

    // Subsample pulses preserve area across sample rates and arrival phases.
    auto x = fresh();
    double reference = 0;
    for (double rate : {22050, 44100, 48000, 96000, 192000}) {
        sr(x.get(), rate);
        for (double phase : {0.0, 0.3, 0.95}) {
            double kernel[MAX_KERNEL];
            int n = rain_kernel(kernel, phase, 0.001, 1, x.get());
            assert(n > 0);
            double area = 0;
            for (int k = 0; k < n; ++k)
                area += kernel[k] / rate;
            if (reference == 0)
                reference = area;
            assert(std::fabs(area - reference) / reference < 0.002);
        }
    }

    // Eq. 3 for a constant v0 has an independently available closed form.
    double l = 0.002, s = 0.004, dt = 1.0 / 192000, z = 0, v0 = 0.3, k = c / (s - l);
    for (int n = 0; n < 100; ++n) {
        double t = n * dt;
        double expected = (1 - l / s) * v0 - c * l / (s * s) * v0 * (1 - std::exp(-k * t)) / k;
        assert(std::fabs(rain_eq3(v0, z, l, s, c, dt) - expected) < 1e-12);
    }
    // Zero pinch-off velocity produces no extra water sound.
    x = fresh();
    rain_cone(x.get(), 0.002f, 0.002f, 0.5f);
    x->pinch_size = 16;
    x->pinch_rate = 48000;
    std::fill(x->pinch, x->pinch + 16, 0);
    assert(rain_prepare_water(x.get()));
    for (int i = 0; i < x->water_size; ++i)
        assert(x->water_delta[i] == 0);
    // A supplied rectangular velocity pulse is a TEST INPUT, not a paper preset.
    std::fill(x->pinch, x->pinch + 16, 0.3);
    assert(rain_prepare_water(x.get()));
    double sum = 0;
    for (int i = 0; i < x->water_size; ++i)
        sum += x->water_delta[i];
    assert(std::fabs(sum) < 1e-12); // Finite u starts and finishes at zero.
    rain_density(x.get(), 0);
    x->water_surface = true;
    rain_bang(x.get());
    auto wet = render(x.get(), 4096);
    assert(energy(wet.left) > 0 && x->active == 0);
    assert(energy(render(x.get(), 4096).left) == 0);
    // For finite v0, the added cone velocity returns to zero. Equation 1
    // therefore gives zero time-integrated EXTRA pressure, including the tail.
    x->water_surface = false;
    rain_seed(x.get(), 1234);
    rain_bang(x.get());
    auto dry = render(x.get(), 4096);
    double extra_sum = 0, extra_energy = 0;
    for (int i = 0; i < 4096; ++i) {
        double extra = double(wet.left[i]) - dry.left[i];
        extra_sum += extra;
        extra_energy += extra * extra;
    }
    assert(extra_energy > 0);
    assert(std::fabs(extra_sum) < 1e-8);

    // Bang generates one linear, finite pulse with no noise bed or ringing.
    x = fresh();
    rain_density(x.get(), 0);
    assert(energy(render(x.get(), 1024).left) == 0);
    rain_bang(x.get());
    auto pulse = render(x.get(), 64);
    assert(energy(pulse.left) > 0 && x->pending == 0 && x->active == 0);
    int nonzero = 0;
    for (auto value : pulse.left)
        nonzero += value != 0;
    assert(nonzero <= 2);
    auto one = fresh(), two = fresh();
    rain_density(one.get(), 0);
    rain_density(two.get(), 0);
    rain_amplitude(two.get(), 2);
    rain_bang(one.get());
    rain_bang(two.get());
    auto p1 = render(one.get(), 64), p2 = render(two.get(), 64);
    for (int i = 0; i < 64; ++i)
        assert(p2.left[i] == 2 * p1.left[i]);

    // Automatic hard rain remains finite through maximum density and large blocks.
    for (double rate : {22050, 48000, 192000}) {
        x = fresh();
        sr(x.get(), rate);
        rain_density(x.get(), 10000);
        auto result = render(x.get(), static_cast<int>(rate));
        assert(energy(result.left) > 0 && x->dropped == 0);
        std::printf("sr %.0f: hard rain energy %.8f\n", rate, energy(result.left));
    }
    // Reproducibility for identical configuration and buffer schedule.
    one = fresh();
    two = fresh();
    assert(render(one.get(), 48000).left == render(two.get(), 48000).left);
    // A manually scheduled pulse is independent of subsequent buffer partitioning.
    one = fresh();
    two = fresh();
    rain_density(one.get(), 0);
    rain_density(two.get(), 0);
    rain_bang(one.get());
    rain_bang(two.get());
    assert(render(one.get(), 1024, 64).left == render(two.get(), 1024, 257).left);

    x = fresh();
    int before = errors;
    rain_radius(x.get(), 0.01f, 0.001f);
    assert(errors == before + 1);
    rain_area(x.get(), 0.0001f, 1);
    assert(errors == before + 2);
    rain_height(x.get(), -1);
    assert(errors == before + 3);
    rain_cone(x.get(), 0.01f, 0.001f, 1);
    assert(errors == before + 4);
    rain_density(x.get(), std::numeric_limits<float>::quiet_NaN());
    assert(x->density == 800);
    rain_density(x.get(), -1);
    assert(x->density == 0);
    rain_seed(x.get(), 0);
    assert(x->rng == 1);
    // Explicit capacity accounting, never silent voice stealing.
    for (int i = 0; i < MAX_VOICES + 1; ++i)
        rain_trigger(x.get(), 100);
    assert(x->active == MAX_VOICES && x->dropped == 1);
    sr(x.get(), 96000);
    assert(x->active == 0);
    // Hybrid single drops have a splash tail, while paper mode stays unchanged.
    x = fresh();
    x->hybrid = true;
    x->hybrid_mix = 1;
    rain_density(x.get(), 0);
    x->intensity_smooth = 0;
    rain_bang(x.get());
    auto detailed = render(x.get(), 4800);
    double tail_energy = 0;
    for (int i = 48; i < 2000; ++i)
        tail_energy += double(detailed.left[i]) * detailed.left[i];
    assert(tail_energy > 1e-8);
    assert(x->active == 0);
    assert(energy(render(x.get(), 4800).left) == 0);
    // Below the hybrid size knee, muting added layers matches a paper bang.
    one = fresh();
    two = fresh();
    two->hybrid = true;
    two->hybrid_mix = 1;
    one->radius_min = one->radius_max = two->radius_min = two->radius_max = 0.0008;
    two->impact = two->impact_smooth = 1;
    two->splash = two->splash_smooth = 0;
    two->tone = two->tone_smooth = 0;
    two->bed = two->bed_smooth = 0;
    rain_density(one.get(), 0);
    rain_density(two.get(), 0);
    one->intensity_smooth = two->intensity_smooth = 0;
    rain_bang(one.get());
    rain_bang(two.get());
    assert(render(one.get(), 4800).left == render(two.get(), 4800).left);
    // Detail random draws do not alter a manually triggered paper kernel.
    one = fresh();
    two = fresh();
    two->hybrid = true;
    two->hybrid_mix = 1;
    rain_trigger(one.get(), 0, true);
    rain_trigger(two.get(), 0, true);
    assert(one->rng == two->rng);
    assert(one->voices[0].hard_size == two->voices[0].hard_size);
    for (int i = 0; i < one->voices[0].hard_size; ++i)
        assert(one->voices[0].hard[i] == two->voices[0].hard[i]);
    // Large drops have bounded excitation and lower, shorter resonances.
    one = fresh();
    two = fresh();
    one->hybrid = two->hybrid = true;
    one->radius_min = one->radius_max = 0.001;
    two->radius_min = two->radius_max = 0.003;
    rain_trigger(one.get(), 0, true);
    rain_trigger(two.get(), 0, true);
    const auto &small = one->voices[0];
    const auto &large = two->voices[0];
    assert(small.size_gain == 1);
    assert(large.size_gain * 9 < 2);
    assert(large.splash_env > small.splash_env && large.splash_env < 2.01 * small.splash_env);
    auto frequency = [](const Ring &r) { return std::acos(r.a1 / (2 * std::sqrt(-r.a2))); };
    for (int k = 0; k < 3; ++k) {
        assert(frequency(large.rings[k]) < frequency(small.rings[k]));
        assert(-large.rings[k].a2 < -small.rings[k].a2);
    }
    rain_trigger(one.get(), 0, true);
    assert(one->voices[0].rings[0].a1 != one->voices[1].rings[0].a1);
    // Sparse automatic hybrid rain has clusters and gaps, and is seed-reproducible.
    one = fresh();
    two = fresh();
    for (auto *r : {one.get(), two.get()}) {
        r->hybrid = true;
        r->hybrid_mix = 1;
        r->impact = r->impact_smooth = 1;
        r->splash = r->splash_smooth = r->tone = r->tone_smooth = 0;
        r->bed = r->bed_smooth = 0;
        r->radius_min = r->radius_max = 0.0005;
        sr(r, 8000);
        rain_density(r, 2);
    }
    auto sparse = render(one.get(), 240000).left;
    assert(sparse == render(two.get(), 240000).left);
    std::vector<int> onsets;
    for (int i = 0; i < int(sparse.size()); ++i)
        if (sparse[i] != 0 && (onsets.empty() || i - onsets.back() > 8))
            onsets.push_back(i);
    assert(onsets.size() > 30 && onsets.size() < 100);
    int shortest = 240000, longest = 0;
    for (size_t i = 1; i < onsets.size(); ++i) {
        shortest = std::min(shortest, onsets[i] - onsets[i - 1]);
        longest = std::max(longest, onsets[i] - onsets[i - 1]);
    }
    assert(shortest < 1000 && longest > 8000);
    // Dense hybrid mixes, all materials, wide radius range, low/high SR.
    for (double rate : {8000, 48000, 96000})
        for (int material = 0; material < MATERIAL_COUNT; ++material) {
            x = fresh();
            x->hybrid = true;
            x->hybrid_mix = 1;
            x->material = material;
            x->plinks = x->plinks_smooth = 0.2;
            rain_radius(x.get(), 0.0005f, 0.003f);
            sr(x.get(), rate);
            rain_density(x.get(), 10000);
            auto dense = render(x.get(), int(rate));
            double peak = 0;
            for (auto sample : dense.left)
                peak = std::max(peak, std::fabs(double(sample)));
            assert(peak < 1 && energy(dense.left) > 0 && x->dropped == 0);
            std::printf("hybrid %.0f Hz material %d peak %.4f\n", rate, material, peak);
        }
    // Public material messages yield distinct tails, deterministic with a fixed
    // seed. Material selection leaves strict paper output bit-identical.
    std::vector<std::vector<t_sample>> materials;
    for (const char *name : {"plastic", "roof", "wood", "dirt", "asphalt"}) {
        t_symbol symbol{};
        symbol.s_name = name;
        one = fresh();
        two = fresh();
        rain_material(two.get(), &symbol);
        assert(render(one.get(), 4096).left == render(two.get(), 4096).left);
        for (auto *r : {one.get(), two.get()}) {
            rain_clear(r);
            rain_seed(r, 4321);
            rain_material(r, &symbol);
            r->hybrid = true;
            r->hybrid_mix = 1;
            rain_density(r, 0);
            r->bed = r->bed_smooth = 0;
            rain_bang(r);
        }
        auto drop = render(one.get(), 48000).left;
        assert(drop == render(two.get(), 48000).left);
        assert(one->active == 0 && energy(drop) > 0);
        for (const auto &other : materials) {
            double difference = 0;
            for (size_t i = 48; i < drop.size(); ++i)
                difference += std::pow(double(drop[i]) - other[i], 2);
            assert(difference > 1e-8);
        }
        materials.push_back(drop);
    }
    x = fresh();
    for (const auto &pair : {std::pair<const char *, int>{"plastic-bag", 3},
                             {"dirt-floor", 5}, {"asphalt-floor", 6}}) {
        t_symbol symbol{};
        symbol.s_name = pair.first;
        rain_material(x.get(), &symbol);
        assert(x->material == pair.second);
    }
    t_symbol unknown{};
    unknown.s_name = "unknown";
    before = errors;
    rain_material(x.get(), &unknown);
    assert(errors == before + 1 && x->material == 6);
    // Tone/plink silence leaves no synthesized resonant output.
    x = fresh();
    x->hybrid = true;
    x->hybrid_mix = 1;
    x->impact = x->impact_smooth = x->splash = x->splash_smooth = 0;
    x->tone = x->tone_smooth = x->plinks = x->plinks_smooth = x->bed = x->bed_smooth = 0;
    rain_bang(x.get());
    assert(energy(render(x.get(), 4800).left) == 0);
    // Runtime controls settle and paper mode removes the background entirely.
    x = fresh();
    x->hybrid = true;
    x->hybrid_mix = 1;
    t_symbol paper{};
    paper.s_name = "paper";
    rain_model(x.get(), &paper);
    rain_density(x.get(), 0);
    render(x.get(), 48000);
    assert(x->hybrid_mix == 0 && energy(render(x.get(), 4800).left) == 0);
    rain_resonance(x.get(), 100);
    assert(x->resonance == 30);
    rain_tone(x.get(), -1);
    assert(x->tone == 0);
    rain_tone(x.get(), std::numeric_limits<float>::quiet_NaN());
    assert(x->tone == 0);
    // Optional mono float preview: plastic, roof, wood, dirt, asphalt (5s each).
    if (argc == 2) {
        FILE *file = std::fopen(argv[1], "wb");
        assert(file);
        for (int section = 0; section < 5; ++section) {
            x = fresh();
            x->hybrid = true;
            x->hybrid_mix = 1;
            const int materials[] = {3, 2, 4, 5, 6};
            x->material = materials[section];
            x->plinks = x->plinks_smooth = 0;
            rain_radius(x.get(), 0.0005f, 0.003f);
            auto preview = render(x.get(), 48000 * 5);
            for (int i = 0; i < 48000 * 5; ++i) {
                float frame[] = {float(preview.left[i])};
                assert(std::fwrite(frame, sizeof(float), 1, file) == 1);
            }
        }
        std::fclose(file);
    }

    std::puts("paper equation and DSP checks passed");
}
