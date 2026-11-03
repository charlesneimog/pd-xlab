// Standalone (after CMake has fetched the dependencies):
// c++ -std=c++17 -O2 -ffunction-sections -fdata-sections -I/usr/include/pd \
// -Ibuild/_deps/signalsmith-stretch-src/include \
// -Ibuild/_deps/signalsmith-linear-src/include tests/pv_dsp.cpp \
// -Wl,--gc-sections -o /tmp/pv_dsp_test
#include "../src/single/x.pv~/x.pv~.cpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>

t_symbol s_signal;
extern "C" t_pd *pd_new(t_class *) {
    return reinterpret_cast<t_pd *>(std::calloc(1, sizeof(pv_tilde)));
}
extern "C" t_outlet *outlet_new(t_object *, t_symbol *) { return nullptr; }
extern "C" void dsp_add(t_perfroutine, int, ...) {}
extern "C" void pd_error(const void *, const char *, ...) {}
extern "C" void logpost(const void *, int, const char *, ...) {}
extern "C" t_symbol *gensym(const char *) {
    static t_symbol symbol{};
    return &symbol;
}
extern "C" t_float atom_getfloat(const t_atom *a) { return a->a_w.w_float; }
extern "C" t_symbol *atom_getsymbol(const t_atom *a) { return a->a_w.w_symbol; }

static void start(pv_tilde *x, int block) {
    t_signal input{}, output{};
    input.s_n = output.s_n = block;
    input.s_sr = output.s_sr = 48000;
    t_signal *signals[] = {&input, &output};
    pv_tilde_dsp(x, signals);
}

static void process(pv_tilde *x, t_sample *input, t_sample *output, int block) {
    t_int w[] = {0, reinterpret_cast<t_int>(x), reinterpret_cast<t_int>(input),
                 reinterpret_cast<t_int>(output), block};
    assert(pv_tilde_perform(w) == w + 5);
}

static std::vector<t_sample> render(int block, bool aliased) {
    auto *x = static_cast<pv_tilde *>(pv_tilde_new(nullptr, 0, nullptr));
    start(x, block);
    pv_tilde_float(x, 700);
    std::vector<t_sample> input(block), output(block), rendered;
    for (int pos = 0; pos < 32768; pos += block) {
        for (int i = 0; i < block; ++i) {
            double t = (pos + i) / 48000.0;
            input[i] = 0.4 * std::sin(2 * M_PI * 440 * t) + 0.2 * std::sin(2 * M_PI * 173 * t);
        }
        auto *result = aliased ? input.data() : output.data();
        process(x, input.data(), result, block);
        rendered.insert(rendered.end(), result, result + block);
    }
    rendered.resize(32768);

    // A DSP restart with silent input must not replay old input or OLA tails.
    start(x, block);
    std::fill(input.begin(), input.end(), 0);
    for (int pos = 0; pos < 8192; pos += block) {
        process(x, input.data(), output.data(), block);
        for (auto sample : output)
            assert(std::isfinite(sample) && std::abs(sample) < 1e-6);
    }
    pv_tilde_free(x);
    std::free(x);
    return rendered;
}

static void startup() {
    // A cosine starts away from zero. Starting the vocoder must not turn that
    // edge into a click after its internal latency, even when transposed.
    for (int cents : {0, 700, 1200, -1200}) {
        auto *x = static_cast<pv_tilde *>(pv_tilde_new(nullptr, 0, nullptr));
        start(x, 64);
        pv_tilde_float(x, cents);
        t_sample input[64], output[64], previous = 0;
        double maxStep = 0, energy = 0;
        for (int pos = 0; pos < 8192; pos += 64) {
            for (int i = 0; i < 64; ++i)
                input[i] = 0.6 * std::cos(2 * M_PI * 440 * (pos + i) / 48000.0);
            process(x, input, output, 64);
            for (auto sample : output) {
                assert(std::isfinite(sample));
                maxStep = std::max(maxStep, double(std::abs(sample - previous)));
                energy += sample * sample;
                previous = sample;
            }
        }
        std::printf("startup %d cents: max step %.6f\n", cents, maxStep);
        assert(energy > 10 && maxStep < 0.1);
        pv_tilde_free(x);
        std::free(x);
    }
}

int main() {
    startup();
    const auto reference = render(64, false);
    double energy = 0;
    for (auto sample : reference) {
        assert(std::isfinite(sample));
        energy += sample * sample;
    }
    assert(energy > 100);

    // Processing must preserve the same stream regardless of Pd block size,
    // including blocks which do not divide the FFT size and larger blocks.
    for (int block : {64, 128, 256, 100, 2048}) {
        for (bool aliased : {false, true}) {
            const auto output = render(block, aliased);
            double error = 0;
            for (size_t i = 0; i < reference.size(); ++i) {
                assert(std::isfinite(output[i]));
                error = std::max(error, double(std::abs(output[i] - reference[i])));
            }
            std::printf("block %d, aliased %d: max error %.9g\n", block, aliased, error);
            assert(error < 1e-5);
        }
    }
    std::puts(
        "Passed: smooth startup, continuous output, aliased buffers, and clean DSP restarts.");
}
