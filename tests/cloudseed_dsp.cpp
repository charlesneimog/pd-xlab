/* Standalone, after building x.cloudseed.rev_tilde with CMake:
   c++ -std=c++17 -O2 -ffunction-sections -fdata-sections \
       -DBUFFER_SIZE=1024 -DMAX_STR_SIZE=32 -I/usr/include/pd \
       -Ibuild/cloudseedcore tests/cloudseed_dsp.cpp build/libcloudseedcore.a \
       -Wl,--gc-sections -o /tmp/cloudseed_dsp_test
   /tmp/cloudseed_dsp_test
   Add -DPD_FLOATSIZE=64 to check a double-precision Pd host.
*/
#include "../src/reverb/x.cloudseed.rev~.cpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <memory>
#include <vector>

namespace {
int errors = 0;
t_float hostRate = 48000;
}

t_symbol s_signal;
extern "C" t_pd *pd_new(t_class *) {
    return static_cast<t_pd *>(std::calloc(1, sizeof(t_cloudseed_tilde)));
}
extern "C" void pd_free(t_pd *object) { std::free(object); }
extern "C" t_inlet *signalinlet_new(t_object *, t_float) { return nullptr; }
extern "C" void inlet_free(t_inlet *) {}
extern "C" t_outlet *outlet_new(t_object *, t_symbol *) { return nullptr; }
extern "C" void outlet_free(t_outlet *) {}
extern "C" t_float sys_getsr() { return hostRate; }
extern "C" void dsp_add(t_perfroutine, int, ...) {}
extern "C" void pd_error(const void *, const char *, ...) { ++errors; }
extern "C" void post(const char *, ...) {}
extern "C" t_float atom_getfloat(const t_atom *atom) {
    return atom->a_type == A_FLOAT ? atom->a_w.w_float : 0;
}
extern "C" t_symbol *atom_getsymbol(const t_atom *atom) {
    assert(atom->a_type == A_SYMBOL);
    return atom->a_w.w_symbol;
}

namespace {
struct Free {
    void operator()(t_cloudseed_tilde *object) const {
        cloudseed_free(object);
        std::free(object);
    }
};
using Object = std::unique_ptr<t_cloudseed_tilde, Free>;

t_atom number(t_float value) {
    t_atom atom;
    SETFLOAT(&atom, value);
    return atom;
}

Object fresh(t_float mix = 1) {
    auto argument = number(mix);
    Object object(static_cast<t_cloudseed_tilde *>(cloudseed_new(nullptr, 1, &argument)));
    assert(object && object->state);
    return object;
}

void sampleRate(t_cloudseed_tilde *object, t_float rate) {
    t_sample dummy[64]{};
    t_signal a{}, b{}, c{}, d{};
    t_signal *signals[] = {&a, &b, &c, &d};
    for (auto *signal : signals) {
        signal->s_n = 64;
        signal->s_sr = rate;
        signal->s_vec = dummy;
    }
    cloudseed_dsp(object, signals);
}

struct Stereo {
    std::vector<t_sample> left, right;
    explicit Stereo(int count) : left(count), right(count) {}
};

Stereo process(t_cloudseed_tilde *object, Stereo &input, int block = 64, bool inPlace = false) {
    Stereo output(static_cast<int>(input.left.size()));
    for (int position = 0; position < static_cast<int>(input.left.size()); position += block) {
        const int count = std::min(block, static_cast<int>(input.left.size()) - position);
        auto *left = inPlace ? input.left.data() + position : output.left.data() + position;
        auto *right = inPlace ? input.right.data() + position : output.right.data() + position;
        t_int arguments[] = {0,
                             reinterpret_cast<t_int>(object),
                             reinterpret_cast<t_int>(input.left.data() + position),
                             reinterpret_cast<t_int>(input.right.data() + position),
                             reinterpret_cast<t_int>(left),
                             reinterpret_cast<t_int>(right), count};
        assert(cloudseed_perform(arguments) == arguments + 7);
        for (int index = 0; index < count; ++index) {
            assert(std::isfinite(left[index]));
            assert(std::isfinite(right[index]));
        }
    }
    return inPlace ? input : output;
}

Stereo silence(t_cloudseed_tilde *object, int count, int block = 64) {
    Stereo input(count);
    return process(object, input, block);
}

double energy(const std::vector<t_sample> &samples, int begin = 0) {
    double result = 0;
    for (int index = begin; index < static_cast<int>(samples.size()); ++index)
        result += double(samples[index]) * samples[index];
    return result;
}

std::vector<double> parameters(t_cloudseed_tilde *object) {
    const auto *values = object->state->reverb.GetAllParameters();
    return {values, values + Cloudseed::Parameter::COUNT};
}

void namedParameter(t_cloudseed_tilde *object, const char *name, t_float value) {
    t_symbol symbol{};
    symbol.s_name = name;
    auto atom = number(value);
    cloudseed_set(object, &symbol, 1, &atom);
}

void checkImpulseAndClear() {
    auto object = fresh();
    assert(object->state->mix == 1);
    assert(parameters(object.get())[Cloudseed::Parameter::DryOut] == 0);
    auto quiet = silence(object.get(), 2048);
    assert(energy(quiet.left) == 0 && energy(quiet.right) == 0);
    Stereo input(48000);
    input.left[0] = 1;
    input.right[37] = -0.5;
    auto wet = process(object.get(), input);
    assert(energy(wet.left, 1024) > 1e-8);
    assert(energy(wet.right, 1024) > 1e-8);
    double stereoDifference = 0;
    for (std::size_t index = 0; index < wet.left.size(); ++index)
        stereoDifference += std::pow(double(wet.left[index]) - wet.right[index], 2);
    assert(stereoDifference > 1e-8);
    assert(energy(silence(object.get(), 4096).left) > 0);
    const auto before = parameters(object.get());
    cloudseed_clear(object.get());
    assert(parameters(object.get()) == before);
    quiet = silence(object.get(), 48000);
    assert(energy(quiet.left) == 0 && energy(quiet.right) == 0);
}

void checkDryAndAliasing() {
    for (bool inPlace : {false, true}) {
        auto object = fresh(0);
        Stereo input(8195);
        for (int index = 0; index < static_cast<int>(input.left.size()); ++index) {
            // These values retain extra mantissa bits on a double-precision
            // host. The dry path must not round-trip through CloudSeed float.
            input.left[index] = t_sample(std::sin(index * 0.13) * 0.123456789123);
            input.right[index] = t_sample(std::cos(index * 0.07) * 0.987654321987);
        }
        const Stereo expected = input;
        const auto dry = process(object.get(), input, 4099, inPlace);
        assert(dry.left == expected.left && dry.right == expected.right);
        cloudseed_mix(object.get(), 1);
        // A zero dry mix still feeds the reverb, so unmuting has an existing tail.
        assert(energy(silence(object.get(), 48000).left) > 0);
    }
}

void checkMessagesAndSampleRate() {
    auto object = fresh();
    const auto defaults = parameters(object.get());
    namedParameter(object.get(), "LateLineDecay", 0.375);
    assert(parameters(object.get())[Cloudseed::Parameter::LateLineDecay] == 0.375);
    t_atom arguments[] = {number(Cloudseed::Parameter::LateLineSize), number(0.625)};
    cloudseed_param(object.get(), nullptr, 2, arguments);
    assert(parameters(object.get())[Cloudseed::Parameter::LateLineSize] == 0.625);
    t_symbol parameterName{};
    parameterName.s_name = "InputMix";
    SETSYMBOL(arguments, &parameterName);
    arguments[1] = number(0.25);
    cloudseed_param(object.get(), nullptr, 2, arguments);
    assert(parameters(object.get())[Cloudseed::Parameter::InputMix] == 0.25);

    const auto configured = parameters(object.get());
    int before = errors;
    namedParameter(object.get(), "UnknownParameter", 0.5);
    namedParameter(object.get(), "LateLineDecay", std::numeric_limits<t_float>::quiet_NaN());
    for (t_float index : {t_float(-1), t_float(Cloudseed::Parameter::COUNT), t_float(1.5)}) {
        arguments[0] = number(index);
        cloudseed_param(object.get(), nullptr, 2, arguments);
    }
    cloudseed_param(object.get(), nullptr, 0, nullptr);
    arguments[0] = number(Cloudseed::Parameter::LateLineDecay);
    SETSYMBOL(arguments + 1, &parameterName);
    cloudseed_param(object.get(), nullptr, 2, arguments);
    assert(errors == before + 7);
    assert(parameters(object.get()) == configured);
    cloudseed_mix(object.get(), std::numeric_limits<t_float>::infinity());
    assert(object->state->mix == 1);

    cloudseed_mix(object.get(), 0.375);
    for (t_float rate : {t_float(8000), t_float(44100), t_float(96000), t_float(192000)}) {
        sampleRate(object.get(), rate);
        assert(object->state->reverb.GetSamplerate() == rate);
        assert(parameters(object.get()) == configured && object->state->mix == 0.375);
        Stereo input(16391);
        input.left[0] = input.right[0] = 1;
        auto output = process(object.get(), input, 4099);
        assert(energy(output.left) > 0 && energy(output.right) > 0);
    }
    cloudseed_reset(object.get());
    assert(parameters(object.get()) == defaults);
    assert(object->state->mix == 1);

    before = errors;
    sampleRate(object.get(), 4000);
    assert(errors == before + 1);
    Stereo input(4099);
    std::fill(input.left.begin(), input.left.end(), 1);
    std::fill(input.right.begin(), input.right.end(), -1);
    auto muted = process(object.get(), input, 4099);
    assert(energy(muted.left) == 0 && energy(muted.right) == 0);
    sampleRate(object.get(), 48000);
    assert(object->state->ready && object->state->reverb.GetSamplerate() == 48000);
    auto resumed = process(object.get(), input);
    assert(energy(resumed.left) > 0 && energy(resumed.right) > 0);
}

void checkDelayBoundaries() {
    auto object = fresh();
    // Maximum allpass delay and modulation exercise the circular-buffer
    // boundaries repaired in the CMake copy of the upstream core.
    for (const char *name : {"EarlyDiffuseEnabled", "EarlyDiffuseCount", "EarlyDiffuseDelay",
                            "EarlyDiffuseModAmount", "EarlyDiffuseModRate",
                            "LateDiffuseEnabled", "LateDiffuseCount", "LateDiffuseDelay",
                            "LateDiffuseModAmount", "LateDiffuseModRate",
                            "LateLineCount", "LateLineSize", "LateLineModAmount",
                            "LateLineModRate"})
        namedParameter(object.get(), name, 1);
    for (t_float rate : {t_float(8000), t_float(192000)}) {
        sampleRate(object.get(), rate);
        cloudseed_clear(object.get());
        Stereo input(static_cast<int>(rate / 2) + 17);
        input.left[0] = input.right[0] = 1;
        process(object.get(), input, 4099);
    }
}
} // namespace

int main() {
    Cloudseed::initPrograms();
    checkImpulseAndClear();
    checkDryAndAliasing();
    checkMessagesAndSampleRate();
    checkDelayBoundaries();
    std::printf("x.cloudseed.rev~ DSP (%d-bit Pd): wet stereo tail, clear, exact dry, "
                "in-place/large blocks, parameter errors and sample rate retention passed\n",
                PD_FLOATSIZE);
}
