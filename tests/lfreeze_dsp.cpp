// Standalone (after CMake has built x.lfreeze~ and its dependencies):
// c++ -std=c++14 -O3 -pthread -ffunction-sections -fdata-sections -I/usr/include/pd \
// -Ibuild/_deps/loris-src/src tests/lfreeze_dsp.cpp \
// build/_deps/loris-build/libloris.a build/fftw-3.3.10-build/libfftw3.a \
// -Wl,--gc-sections -o /tmp/lfreeze_dsp_test
// /tmp/lfreeze_dsp_test [--benchmark]
#include "../src/fft/x.lfreeze~.cpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

static std::atomic<size_t> allocationCount{0};
void *operator new(size_t size) {
    if (void *storage = std::malloc(size ? size : 1)) {
        ++allocationCount;
        return storage;
    }
    throw std::bad_alloc();
}
void *operator new[](size_t size) { return ::operator new(size); }
void operator delete(void *storage) noexcept { std::free(storage); }
void operator delete[](void *storage) noexcept { std::free(storage); }
void operator delete(void *storage, size_t) noexcept { std::free(storage); }
void operator delete[](void *storage, size_t) noexcept { std::free(storage); }

t_symbol s_signal;
static t_class *const pdClassSentinel = reinterpret_cast<t_class *>(0x1234);
extern "C" t_pd *pd_new(t_class *) {
    auto *storage = static_cast<t_pd *>(std::calloc(1, sizeof(lfreeze_tilde)));
    *storage = pdClassSentinel;
    return storage;
}
extern "C" void pd_free(t_pd *x) { std::free(x); }
extern "C" t_outlet *outlet_new(t_object *, t_symbol *) { return nullptr; }
extern "C" void dsp_add(t_perfroutine, int, ...) {}
extern "C" void pd_error(const void *, const char *, ...) {}
extern "C" t_clock *clock_new(void *, t_method) { return reinterpret_cast<t_clock *>(0x4321); }
extern "C" void clock_delay(t_clock *, double) {}
extern "C" void clock_unset(t_clock *) {}
extern "C" void clock_free(t_clock *) {}
extern "C" t_float sys_getsr() { return 48000; }
extern "C" t_symbol *gensym(const char *) {
    static t_symbol symbol{};
    return &symbol;
}
extern "C" t_float atom_getfloat(const t_atom *a) { return a->a_w.w_float; }
extern "C" t_symbol *atom_getsymbol(const t_atom *a) { return a->a_w.w_symbol; }

using Frames = std::vector<std::vector<FreezeNode>>;

// Keep the original matching and sample formula independent of the optimized
// interpolation coefficients, transition cache, and phase-transfer code.
struct ReferenceNode {
    FreezeNode from, to;
    double phase;
    int toIndex;
};

struct Reference {
    Frames frames;
    std::vector<double> phases;
    std::vector<ReferenceNode> nodes;
    int frameSize, frame = 0, position = 0;
    double fadeGain = 1;
    int fadeCounter = 0, fadeSamples = 73;
    bool fadingIn = false, fadingOut = false, frozen = true;
    double sampleRate = 48000;
    std::uint32_t randomState = 12345;

    Reference(const Frames &source, int size) : frames(source), frameSize(size) {
        for (const auto &node : frames[0])
            phases.push_back(node.phase);
        prepare();
    }

    void prepare() {
        const auto &from = frames[frame];
        const auto &to = frames[(frame + 1) % frames.size()];
        struct Candidate {
            int from, to;
            double distance;
        };
        std::vector<Candidate> candidates;
        std::vector<int> fromTo(from.size(), -1), toFrom(to.size(), -1);
        for (size_t i = 0; i < from.size(); ++i) {
            if (from[i].freq <= 0)
                continue;
            for (size_t j = 0; j < to.size(); ++j) {
                if (to[j].freq <= 0)
                    continue;
                double ratio = from[i].freq > to[j].freq ? from[i].freq / to[j].freq
                                                         : to[j].freq / from[i].freq;
                if (ratio <= 1.2)
                    candidates.push_back(
                        {int(i), int(j), std::abs(std::log(from[i].freq / to[j].freq))});
            }
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate &a, const Candidate &b) { return a.distance < b.distance; });
        for (const auto &match : candidates) {
            if (fromTo[match.from] < 0 && toFrom[match.to] < 0) {
                fromTo[match.from] = match.to;
                toFrom[match.to] = match.from;
            }
        }
        nodes.clear();
        for (size_t i = 0; i < from.size(); ++i) {
            auto target = fromTo[i] < 0 ? from[i] : to[fromTo[i]];
            if (fromTo[i] < 0)
                target.amp = 0;
            nodes.push_back({from[i], target, phases[i], fromTo[i]});
        }
        for (size_t j = 0; j < to.size(); ++j) {
            if (toFrom[j] >= 0)
                continue;
            auto origin = to[j];
            origin.amp = 0;
            nodes.push_back({origin, to[j], to[j].phase, int(j)});
        }
    }

    void finalize() {
        frame = (frame + 1) % frames.size();
        phases.resize(frames[frame].size());
        for (size_t i = 0; i < phases.size(); ++i)
            phases[i] = frames[frame][i].phase;
        for (const auto &node : nodes)
            if (node.toIndex >= 0)
                phases[node.toIndex] = node.phase;
        position = 0;
        prepare();
    }

    static double interpolate(double a, double b, double t) {
        if (a <= 1e-12 || b <= 1e-12)
            return a + (b - a) * t;
        return std::exp(std::log(a) * (1 - t) + std::log(b) * t);
    }

    void stop() {
        if (frozen) {
            fadeCounter = 0;
            fadingOut = true;
            fadingIn = false;
        }
    }

    t_sample sample() {
        if (!frozen)
            return 0;
        double output = 0, t = double(position) / frameSize;
        for (auto &node : nodes) {
            double frequency = interpolate(node.from.freq, node.to.freq, t);
            double amplitude = interpolate(node.from.amp, node.to.amp, t);
            double bandwidth = interpolate(node.from.bw, node.to.bw, t);
            randomState ^= randomState << 13;
            randomState ^= randomState >> 17;
            randomState ^= randomState << 5;
            double noise = double(randomState) * (2.0 / 4294967296.0) - 1;
            output += amplitude * (std::cos(node.phase) + bandwidth * noise * std::sin(node.phase));
            node.phase += frequency * (2 * M_PI / sampleRate);
            if (node.phase > 2 * M_PI)
                node.phase -= 2 * M_PI;
        }
        if (fadingIn) {
            fadeGain = double(fadeCounter) / fadeSamples;
            if (++fadeCounter >= fadeSamples) {
                fadeCounter = fadeSamples;
                fadeGain = 1;
                fadingIn = false;
            }
        }
        if (fadingOut) {
            fadeGain = 1 - double(fadeCounter) / fadeSamples;
            if (++fadeCounter >= fadeSamples) {
                fadeCounter = fadeSamples;
                fadeGain = 0;
                fadingOut = false;
                frozen = false;
            }
        }
        t_sample result = output * fadeGain;
        if (++position >= frameSize)
            finalize();
        return result;
    }
};

static void initialize(lfreeze_tilde &x, const Frames &frames, int frameSize, bool fade = false,
                       double sampleRate = 48000) {
    x.frameSize = frameSize;
    x.frames = frames.size();
    x.breakpoints = frames;
    x.randomState = 12345;
    x.sampleRate = sampleRate;
    assert(lfreeze_tilde_render_loop(&x, sampleRate));
    assert(x.loopBuffer.size() == size_t(frameSize) * frames.size());
    x.loopPosition = 0;
    x.frozen = true;
    x.fadeGain = fade ? 0 : 1;
    x.fadeCounter = 0;
    x.fadeSamples = 73;
    x.fadingIn = fade;
    x.fadingOut = false;
}

static void process(lfreeze_tilde &x, t_sample *input, t_sample *output, int count) {
    t_int w[] = {0, reinterpret_cast<t_int>(&x), reinterpret_cast<t_int>(input),
                 reinterpret_cast<t_int>(output), count};
    assert(lfreeze_tilde_perform(w) == w + 5);
}

static std::vector<t_sample> repeat(const std::vector<t_sample> &loop, int count, bool fade,
                                    int stopAt) {
    std::vector<t_sample> result(count);
    double gain = fade ? 0 : 1, stopGain = 1;
    int fadeCounter = 0;
    bool fadingIn = fade, fadingOut = false, frozen = true;
    for (int i = 0; i < count; ++i) {
        if (i == stopAt) {
            fadeCounter = 0;
            stopGain = gain;
            fadingIn = false;
            fadingOut = true;
        }
        if (!frozen)
            continue;
        if (fadingIn) {
            gain = double(fadeCounter) / 73;
            if (++fadeCounter >= 73) {
                gain = 1;
                fadingIn = false;
            }
        }
        if (fadingOut) {
            gain = stopGain * (1 - double(fadeCounter) / 73);
            if (++fadeCounter >= 73) {
                gain = 0;
                fadingOut = false;
                frozen = false;
            }
        }
        result[i] = loop[size_t(i) % loop.size()] * gain;
    }
    return result;
}

static std::vector<t_sample> render(const Frames &frames, int frameSize, int count, int block,
                                    bool aliased, bool fade, int stopAt) {
    lfreeze_tilde state;
    initialize(state, frames, frameSize, fade);
    std::vector<t_sample> input(block, 0.123), output(block), result;
    for (int offset = 0; offset < count;) {
        if (offset == stopAt)
            lfreeze_tilde_freeze(&state, 0);
        int size = std::min(block, count - offset);
        if (stopAt > offset)
            size = std::min(size, stopAt - offset);
        t_sample *destination = aliased ? input.data() : output.data();
        process(state, input.data(), destination, size);
        result.insert(result.end(), destination, destination + size);
        offset += size;
    }
    return result;
}

static void compare(const char *name, const Frames &frames, int frameSize = 128, bool fade = false,
                    int stopAt = -1) {
    const int count = frameSize * int(frames.size()) * 4 + 37;
    lfreeze_tilde cached;
    initialize(cached, frames, frameSize);
    const auto loop = cached.loopBuffer;
    const auto expected = repeat(loop, count, fade, stopAt);
    double maximumError = 0;
    for (int block : {1, 64, 100, 512}) {
        for (bool aliased : {false, true}) {
            const auto actual = render(frames, frameSize, count, block, aliased, fade, stopAt);
            for (size_t i = 0; i < actual.size(); ++i) {
                assert(std::isfinite(actual[i]));
                maximumError = std::max(maximumError, double(std::abs(actual[i] - expected[i])));
            }
        }
    }
    std::printf("%s: maximum cached playback/fade error %.9g\n", name, maximumError);
    assert(maximumError < 2e-6);
    if (stopAt >= 0)
        for (int i = stopAt + 73; i < count; ++i)
            assert(expected[i] == 0);
}

static void cachedRepetition() {
    const Frames frames{{{440, 0.3, 0.8, 0.4}}, {{470, 0.1, 0.7, 1.2}}, {{420, 0.7, 0.1, 2.5}}};
    constexpr int frameSize = 128, period = frameSize * 3;
    for (int block : {1, 64, 100, 512}) {
        for (bool aliased : {false, true}) {
            const auto result =
                render(frames, frameSize, period * 9 + 37, block, aliased, false, -1);
            assert(std::memcmp(result.data(), result.data() + period, period * sizeof(t_sample)) ==
                   0);
            for (size_t i = period; i < result.size(); ++i)
                assert(result[i] == result[i % period]);
        }
    }
    std::puts("Cached samples repeat exactly across block boundaries, including noise.");
}

// A phase-closed pure tone is exactly one sinusoid at the nearest integral
// number of cycles. A waveform crossfade fails this test through cancellation
// and amplitude modulation, even when its first and final samples sound close.
static void constantAmplitudePhaseClosure() {
    double worstFit = 0, worstAmplitude = 0, worstPitchError = 0;
    for (double sampleRate : {44100.0, 48000.0, 96000.0}) {
        for (double frequency : {105.0, 110.0, 440.0, 997.3}) {
            for (double initialPhase : {0.0, 0.4, 2.8}) {
                constexpr int length = 4096;
                constexpr double amplitude = 0.37;
                lfreeze_tilde state;
                initialize(state, {{{frequency, amplitude, 0, initialPhase}}}, length,
                           false, sampleRate);
                const double cycles = std::round(frequency * length / sampleRate);
                const double angularFrequency = 2 * M_PI * cycles / length;
                const double renderedFrequency = cycles * sampleRate / length;
                const double pitchError = std::abs(renderedFrequency - frequency);
                assert(pitchError <= sampleRate / (2 * length) + 1e-12);
                worstPitchError = std::max(worstPitchError, pitchError);
                double cosine = 0, sine = 0;
                for (int i = 0; i < length; ++i) {
                    cosine += state.loopBuffer[i] * std::cos(angularFrequency * i);
                    sine += state.loopBuffer[i] * std::sin(angularFrequency * i);
                }
                cosine *= 2.0 / length;
                sine *= 2.0 / length;
                const double fittedAmplitude = std::hypot(cosine, sine);
                worstAmplitude = std::max(worstAmplitude, std::abs(fittedAmplitude - amplitude));
                assert(std::abs(fittedAmplitude - amplitude) < 1e-6);
                for (int i = 0; i < length; ++i) {
                    const double fitted = cosine * std::cos(angularFrequency * i) +
                                          sine * std::sin(angularFrequency * i);
                    worstFit = std::max(worstFit, std::abs(state.loopBuffer[i] - fitted));
                }
            }
        }
    }
    assert(worstFit < 1e-6);
    std::printf("Phase-closed tones preserve amplitude (fit %.9g, amplitude %.9g, "
                "maximum short-test pitch adjustment %.6g Hz).\n",
                worstFit, worstAmplitude, worstPitchError);
}

static double phaseDistance(double first, double second) {
    return std::abs(std::remainder(first - second, 2 * M_PI));
}

// Rendering a second spectral cycle independently must continue exactly from
// the first. This exercises frame-edge phase transfer, including births,
// deaths, empty frames, and partials stored in a different order each frame.
static void periodicPhaseGraph(const char *name, const Frames &frames, int frameSize = 128,
                                double sampleRate = 48000) {
    lfreeze_tilde state;
    initialize(state, frames, frameSize, false, sampleRate);
    const auto beginning = state.currentPhases;
    const size_t length = state.loopBuffer.size();
    double maximumError = 0;
    for (size_t i = 0; i < length; ++i) {
        const double sample = lfreeze_tilde_synthesize_sample(&state, 2 * M_PI / sampleRate);
        maximumError = std::max(maximumError, std::abs(sample - state.loopBuffer[i]));
    }
    assert(beginning.size() == state.currentPhases.size());
    for (size_t i = 0; i < beginning.size(); ++i)
        assert(phaseDistance(beginning[i], state.currentPhases[i]) < 1e-8);
    assert(maximumError < 1e-6);
    std::printf("%s: next spectral cycle matches cached cycle (%.9g).\n", name, maximumError);
}

static void slidingPhaseContinuity() {
    const Frames frames{{{440, 0.37, 0, 0.4}}, {{470, 0.37, 0, 1.2}},
                        {{420, 0.37, 0, 2.5}}};
    constexpr int frameSize = 4096;
    for (double sampleRate : {44100.0, 48000.0, 96000.0}) {
        lfreeze_tilde state;
        initialize(state, frames, frameSize, false, sampleRate);
        const double initialPhase = state.interpNodes[0].phase;
        const double correction = state.interpNodes[0].phaseCorrection;
        const double maximumCorrection = M_PI / state.loopBuffer.size();
        assert(std::abs(correction) <= maximumCorrection + 1e-12);
        for (size_t i = 0; i < state.loopBuffer.size(); ++i) {
            const auto &node = state.interpNodes[0];
            assert(std::abs(node.amp.value - 0.37) < 1e-10);
            assert(std::abs(node.phaseCorrection - correction) < 1e-12);
            const double expectedPhase = node.phase + node.freq.value * 2 * M_PI / sampleRate +
                                         node.phaseCorrection;
            const double actual = lfreeze_tilde_synthesize_sample(&state, 2 * M_PI / sampleRate);
            assert(std::abs(actual - state.loopBuffer[i]) < 1e-6);
            assert(phaseDistance(state.interpNodes[0].phase, expectedPhase) < 1e-10);
        }
        assert(phaseDistance(state.interpNodes[0].phase, initialPhase) < 1e-8);
        // The first sample after wrapping has the same ordinary oscillator
        // step as every other sample; no special seam window changes its gain.
        const double next = lfreeze_tilde_synthesize_sample(&state, 2 * M_PI / sampleRate);
        assert(std::abs(next - state.loopBuffer[0]) < 1e-6);
    }
    std::puts("Sliding partial phases stay continuous through every frame and the loop wrap.");
}

static void expandedLoopAndCancellation() {
    const Frames frames{{{105, 0.37, 0, 0.4}}, {{110, 0.37, 0, 1.2}}};
    for (double sampleRate : {44100.0, 48000.0, 96000.0}) {
        lfreeze_tilde state;
        state.frameSize = 128;
        state.breakpoints = frames;
        const size_t baseLength = 128 * frames.size();
        const size_t minimum = size_t(std::ceil(4 * sampleRate));
        assert(lfreeze_tilde_render_loop(&state, sampleRate, nullptr, minimum));
        assert(state.loopBuffer.size() >= minimum);
        assert(state.loopBuffer.size() < minimum + baseLength);
        assert(state.loopBuffer.size() % baseLength == 0);
        const double maximumPitchCorrection = sampleRate / (2 * state.loopBuffer.size());
        assert(maximumPitchCorrection <= 0.125 + 1e-12);
        for (const auto &transition : state.transitions)
            for (const auto &node : transition)
                assert(std::abs(node.phaseCorrection) * sampleRate / (2 * M_PI) <=
                       maximumPitchCorrection + 1e-10);
        const double initialPhase = state.interpNodes[0].phase;
        for (size_t i = 0; i < state.loopBuffer.size(); ++i) {
            const double next = lfreeze_tilde_synthesize_sample(&state, 2 * M_PI / sampleRate);
            assert(std::abs(next - state.loopBuffer[i]) < 1e-6);
        }
        assert(phaseDistance(state.interpNodes[0].phase, initialPhase) < 1e-7);
    }
    lfreeze_tilde cancelled;
    cancelled.frameSize = 4096;
    cancelled.breakpoints = frames;
    const std::atomic<bool> cancel{true};
    assert(!lfreeze_tilde_render_loop(&cancelled, 48000, &cancel));
    std::puts("Production-sized loops limit pitch adjustment to 0.125 Hz; cancellation works.");
}

static void lifecycle() {
    for (int iteration = 0; iteration < 8; ++iteration) {
        auto *x = static_cast<lfreeze_tilde *>(lfreeze_tilde_new(nullptr, 0, nullptr));
        assert(x && x->xobj.ob_pd == pdClassSentinel);
        assert(x->fadeSamples == 4096 && !x->frozen && !x->captureActive);
        assert(x->captureBuffer.size() == size_t(x->frameSize * x->frames));
        lfreeze_tilde_free(x);
        std::free(x);
    }
    lfreeze_tilde state;
    state.frameSize = 128;
    state.frames = 1;
    lfreeze_tilde_reset_state(&state);
    lfreeze_tilde_freeze(&state, 1);
    t_sample input[64], output[64];
    std::fill_n(input, 64, 0.5);
    process(state, input, output, 64);
    assert(state.captureActive && state.capturePos == 64 && !state.frozen);
    lfreeze_tilde_freeze(&state, 0);
    for (int i = 0; i < 4; ++i) {
        process(state, input, output, 64);
        for (auto sample : output)
            assert(sample == 0);
    }
    assert(!state.captureActive && state.capturePos == 64 && !state.frozen);

    const Frames frames{{{440, 0.3, 0.2, 0.4}}};
    initialize(state, frames, 128);
    assert(!state.loopBuffer.empty());
    lfreeze_tilde_freeze(&state, 1);
    assert(state.captureActive && !state.frozen && state.loopPosition == 0);
    lfreeze_tilde_frames(&state, 3);
    assert(state.frames == 3 && !state.captureActive && !state.frozen);
    assert(state.loopBuffer.empty() && state.loopPosition == 0);
    assert(state.captureBuffer.size() == 128 * 3);
}

static Frames denseFrames(int count) {
    Frames frames(3);
    for (int frame = 0; frame < 3; ++frame) {
        for (int i = 0; i < count; ++i) {
            double frequency = 70 + 15000.0 * i / count;
            frames[frame].push_back({frequency * (1 + frame * 0.001),
                                     (1 + (i % 7)) * (1 + frame * 0.05) / (count * 12),
                                     0.05 + ((i + frame) % 5) * 0.04, (i % 17) * 0.15});
        }
    }
    return frames;
}

static void playbackAllocations() {
    auto frames = denseFrames(128);
    frames[0].resize(2);
    frames[1].clear();
    lfreeze_tilde state;
    initialize(state, frames, 128);
    t_sample input[100]{}, output[100];
    const int frameBefore = state.currentFrame, positionBefore = state.samplesIntoFrame;
    const auto randomBefore = state.randomState;
    const auto phasesBefore = state.currentPhases;
    const auto loopBefore = state.loopBuffer;
    const size_t before = allocationCount.load();
    for (int block = 0; block < 40; ++block)
        process(state, input, output, 100);
    assert(allocationCount.load() == before);
    assert(state.samplesIntoFrame == positionBefore && state.currentFrame == frameBefore);
    assert(state.randomState == randomBefore && state.currentPhases == phasesBefore);
    assert(state.loopBuffer == loopBefore);
    std::puts("No allocations or spectral synthesis during cached playback.");
}

static volatile double benchmarkSink = 0;
static void benchmark(int partials) {
    const auto frames = denseFrames(partials);
    constexpr int samples = 32768, frameSize = 4096;
    Reference original(frames, frameSize);
    auto begin = std::chrono::steady_clock::now();
    double checksum = 0;
    for (int i = 0; i < samples; ++i)
        checksum += original.sample();
    double oldSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    benchmarkSink = checksum;

    lfreeze_tilde optimized;
    begin = std::chrono::steady_clock::now();
    initialize(optimized, frames, frameSize);
    const double renderSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    begin = std::chrono::steady_clock::now();
    checksum = 0;
    for (int i = 0; i < samples; ++i)
        checksum += lfreeze_tilde_synthesize_sample(&optimized, 2 * M_PI / 48000);
    const double synthesisSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    benchmarkSink = checksum;
    t_sample input[64]{}, output[64];
    begin = std::chrono::steady_clock::now();
    checksum = 0;
    constexpr int repeats = 128;
    for (int i = 0; i < samples * repeats; i += 64) {
        process(optimized, input, output, 64);
        for (auto value : output)
            checksum += value;
    }
    double newSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count() / repeats;
    benchmarkSink = checksum;
    std::printf("%d partials: render once %.1f ms; original %.1f ms, previous optimized "
                "synthesis %.1f ms, cached %.3f ms per %d samples; %.1fx faster than previous "
                "playback\n",
                partials, renderSeconds * 1000, oldSeconds * 1000, synthesisSeconds * 1000,
                newSeconds * 1000, samples, synthesisSeconds / newSeconds);
}

int main(int argc, char **argv) {
    compare("constant partials", {{{440, 0.3, 0.2, 0.4}, {1000, 0.2, 0, 1.5}}});
    compare("interpolated partials",
            {{{440, 0.3, 0.2, 0.4}}, {{470, 0.1, 0.7, 1.2}}, {{420, 0.7, 0.1, 2.5}}});
    compare("zero and near-zero endpoints", {{{440, 0, 0, 0.4}, {1000, 0.3, 1e-13, 1.5}},
                                             {{460, 0.4, 0.6, 2.2}, {980, 1e-13, 0.4, 2.4}},
                                             {{420, 0.2, 0, 1.1}, {1040, 0.5, 0.1, 0.6}}});
    compare("births, deaths, duplicate frequencies",
            {{{220, 0.1, 0, 0.2}, {220, 0.3, 0.4, 1}, {700, 0.2, 0.2, 2}},
             {{225, 0.2, 0.1, 1}, {225, 0.4, 0, 2}, {1400, 0.2, 0.2, 3}},
             {{220, 0.2, 0.2, 2}, {3500, 0.1, 0.6, 3}}});
    compare("empty frames", {{{440, 0.2, 0.2, 0.3}}, {}, {{1000, 0.3, 0.1, 0.4}}});
    compare("empty freeze", {{}});
    compare("full-length interpolation", {{{440, 0.3, 0.2, 0.4}}, {{470, 0.1, 0.7, 1.2}}}, 4096);
    compare("fade in and out", {{{440, 0.3, 0.2, 0.4}}, {{470, 0.1, 0.7, 1.2}}}, 128, true, 650);
    compare("stop during fade in", {{{440, 0.3, 0.2, 0.4}}, {{470, 0.1, 0.7, 1.2}}}, 128, true, 25);
    cachedRepetition();
    constantAmplitudePhaseClosure();
    slidingPhaseContinuity();
    periodicPhaseGraph("Persistent partials and frame permutations",
                       {{{440, 0.3, 0, 0.4}, {1000, 0.2, 0, 1.5}},
                        {{980, 0.4, 0, 0.7}, {470, 0.1, 0, 1.2}},
                        {{420, 0.5, 0, 2.5}, {1020, 0.3, 0, 2.2}}});
    periodicPhaseGraph("Closed tracks that permute after one loop",
                       {{{415, 0.3, 0, 0.4}, {438, 0.2, 0, 1.5}},
                        {{440, 0.4, 0, 0.7}, {448, 0.1, 0, 1.2}},
                        {{422, 0.5, 0, 2.5}, {448, 0.3, 0, 2.2}}});
    periodicPhaseGraph("Births, deaths, and duplicate frequencies",
                       {{{220, 0.1, 0, 0.2}, {220, 0.3, 0, 1}, {700, 0.2, 0, 2}},
                        {{225, 0.2, 0, 1}, {225, 0.4, 0, 2}, {1400, 0.2, 0, 3}},
                        {{220, 0.2, 0, 2}, {3500, 0.1, 0, 3}}});
    periodicPhaseGraph("Empty frames", {{{440, 0.2, 0, 0.3}}, {}, {{1000, 0.3, 0, 0.4}}});
    for (int frameSize : {1, 2, 4, 16, 128, 4096})
        periodicPhaseGraph("Short and long frames", {{{440, 0.37, 0, 0.4}}}, frameSize);
    expandedLoopAndCancellation();
    lifecycle();
    playbackAllocations();
    std::puts("Passed: phase closure, constant amplitude, cached loops, sample rates, cancellation, "
              "block sizes, aliased buffers, fades, and lifecycle.");
    if (argc > 1 && std::strcmp(argv[1], "--benchmark") == 0) {
        benchmark(128);
        benchmark(512);
    }
}
