#include <loris.h>
#include <m_pd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <numbers>
#include <stdexcept>
#include <thread>
#include <vector>

#include <Analyzer.h>
#include <PartialList.h>

#include "../shared/getargs.h"

static t_class *lfreeze_tilde_class;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ─────────────────────────────────────
struct FreezeNode {
    double freq;
    double amp;
    double bw;
    double phase;
};

// ─────────────────────────────────────
struct InterpValue {
    double value;
    double step;
    bool geometric;

    void advance() { value = geometric ? value * step : value + step; }
};

struct InterpNode {
    FreezeNode from;
    FreezeNode to;
    double phase;
    double phaseCorrection{0};
    int toIndex;
    InterpValue freq;
    InterpValue amp;
    InterpValue bw;
};

struct RenderJob {
    std::vector<std::vector<FreezeNode>> breakpoints;
    std::vector<t_sample> loop;
    int frameSize{4096};
    double sampleRate{44100};
    uint32_t randomState{1};
    uint64_t generation{0};
    std::atomic<bool> cancelled{false};
    std::atomic<bool> completed{false};
    char error[256]{};
};

// ─────────────────────────────────────
class lfreeze_tilde {
  public:
    t_object xobj;
    t_outlet *x_out{nullptr};
    t_float x_f{0};

    float resolution{50};
    float windowWidth{100};
    int frameSize{4096};
    int totalBufferSize{4096};
    int frames{1};
    double sampleRate{44100};

    std::vector<double> captureBuffer;
    int capturePos{0};
    bool captureActive{false};
    bool frozen{false};

    std::vector<std::vector<FreezeNode>> breakpoints;
    std::vector<std::vector<InterpNode>> transitions;
    std::vector<double> currentPhases;
    std::vector<InterpNode> interpNodes;

    int currentFrame{0};
    int samplesIntoFrame{0};

    std::vector<t_sample> loopBuffer;
    size_t loopPosition{0};
    uint32_t randomState{0x9e3779b9u};
    t_clock *clock{nullptr};
    bool renderPending{false};
    uint64_t generation{0};
    std::unique_ptr<RenderJob> job;
    std::thread worker;

    // fade
    double fadeGain{0.0};
    double fadeStartGain{0.0};
    int fadeCounter{0};
    int fadeSamples{4096};
    bool fadingIn{false};
    bool fadingOut{false};

    ~lfreeze_tilde() {
        if (job)
            job->cancelled.store(true, std::memory_order_relaxed);
        if (worker.joinable())
            worker.join();
    }
};

// ─────────────────────────────────────
static InterpValue prepare_interp(double a, double b, double samples) {
    const double eps = 1e-12;
    if (a <= eps || b <= eps) {
        return {a, (b - a) / samples, false};
    }
    // Repeated multiplication follows the same logarithmic curve without
    // evaluating logarithms and exponentials in the sample loop.
    return {a, std::exp((std::log(b) - std::log(a)) / samples), true};
}

// ─────────────────────────────────────
struct MatchCandidate {
    int fromIndex;
    int toIndex;
    double dist;
};

// ─────────────────────────────────────
static void match_partials(const std::vector<FreezeNode> &fromNodes,
                           const std::vector<FreezeNode> &toNodes, std::vector<int> &fromTo,
                           std::vector<int> &toFrom) {
    fromTo.assign(fromNodes.size(), -1);
    toFrom.assign(toNodes.size(), -1);

    if (fromNodes.empty() || toNodes.empty()) {
        return;
    }

    const double maxRatio = 1.2;
    std::vector<MatchCandidate> candidates;
    candidates.reserve(fromNodes.size() * toNodes.size());

    for (size_t i = 0; i < fromNodes.size(); ++i) {
        double f1 = fromNodes[i].freq;
        if (f1 <= 0.0) {
            continue;
        }
        for (size_t j = 0; j < toNodes.size(); ++j) {
            double f2 = toNodes[j].freq;
            if (f2 <= 0.0) {
                continue;
            }
            double ratio = (f1 > f2) ? (f1 / f2) : (f2 / f1);
            if (ratio > maxRatio) {
                continue;
            }
            double dist = fabs(log(f1 / f2));
            candidates.push_back({static_cast<int>(i), static_cast<int>(j), dist});
        }
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const MatchCandidate &a, const MatchCandidate &b) { return a.dist < b.dist; });

    for (const auto &c : candidates) {
        if (fromTo[c.fromIndex] >= 0 || toFrom[c.toIndex] >= 0) {
            continue;
        }
        fromTo[c.fromIndex] = c.toIndex;
        toFrom[c.toIndex] = c.fromIndex;
    }
}

// ─────────────────────────────────────
static void lfreeze_tilde_build_transition(lfreeze_tilde *x, int fromIndex) {
    int toIndex = (fromIndex + 1) % static_cast<int>(x->breakpoints.size());
    const std::vector<FreezeNode> &fromNodes = x->breakpoints[fromIndex];
    const std::vector<FreezeNode> &toNodes = x->breakpoints[toIndex];
    std::vector<InterpNode> &transition = x->transitions[fromIndex];

    std::vector<int> fromTo;
    std::vector<int> toFrom;
    match_partials(fromNodes, toNodes, fromTo, toFrom);

    transition.reserve(fromNodes.size() + toNodes.size());

    for (size_t i = 0; i < fromNodes.size(); ++i) {
        FreezeNode fromNode = fromNodes[i];
        FreezeNode toNode;
        int matchIndex = fromTo[i];
        if (matchIndex >= 0) {
            toNode = toNodes[matchIndex];
        } else {
            toNode = fromNode;
            toNode.amp = 0.0;
        }

        InterpNode interp{};
        interp.from = fromNode;
        interp.to = toNode;
        interp.phase = fromNode.phase;
        interp.toIndex = matchIndex;
        transition.push_back(interp);
    }

    for (size_t j = 0; j < toNodes.size(); ++j) {
        if (toFrom[j] >= 0) {
            continue;
        }

        FreezeNode fromNode = toNodes[j];
        fromNode.amp = 0.0;

        InterpNode interp{};
        interp.from = fromNode;
        interp.to = toNodes[j];
        interp.phase = toNodes[j].phase;
        interp.toIndex = static_cast<int>(j);
        transition.push_back(interp);
    }

    for (auto &interp : transition) {
        interp.freq = prepare_interp(interp.from.freq, interp.to.freq, x->frameSize);
        interp.amp = prepare_interp(interp.from.amp, interp.to.amp, x->frameSize);
        interp.bw = prepare_interp(interp.from.bw, interp.to.bw, x->frameSize);
    }
}

// Match each pair of frames once before rendering the cached audio cycle.
static void lfreeze_tilde_build_transitions(lfreeze_tilde *x) {
    x->transitions.clear();
    x->transitions.resize(x->breakpoints.size());
    size_t maxPartials = 0;
    size_t maxNodes = 0;
    for (size_t i = 0; i < x->breakpoints.size(); ++i) {
        lfreeze_tilde_build_transition(x, static_cast<int>(i));
        maxPartials = std::max(maxPartials, x->breakpoints[i].size());
        maxNodes = std::max(maxNodes, x->transitions[i].size());
    }
    x->currentPhases.reserve(maxPartials);
    x->interpNodes.reserve(maxNodes);
}

#include "lfreeze_phase.h"

// ─────────────────────────────────────
static void lfreeze_tilde_prepare_transition(lfreeze_tilde *x) {
    if (x->transitions.empty()) {
        return;
    }

    const auto &fromNodes = x->breakpoints[x->currentFrame];
    if (x->currentPhases.size() != fromNodes.size()) {
        x->currentPhases.resize(fromNodes.size());
        for (size_t i = 0; i < fromNodes.size(); ++i) {
            x->currentPhases[i] = fromNodes[i].phase;
        }
    }

    const auto &transition = x->transitions[x->currentFrame];
    x->interpNodes.assign(transition.begin(), transition.end());
    for (size_t i = 0; i < fromNodes.size(); ++i) {
        x->interpNodes[i].phase = x->currentPhases[i];
    }
}

// ─────────────────────────────────────
static void lfreeze_tilde_finalize_transition(lfreeze_tilde *x) {
    if (x->breakpoints.empty()) {
        return;
    }

    int toIndex = (x->currentFrame + 1) % static_cast<int>(x->breakpoints.size());
    const std::vector<FreezeNode> &toNodes = x->breakpoints[toIndex];

    x->currentPhases.resize(toNodes.size());
    for (size_t i = 0; i < toNodes.size(); ++i) {
        x->currentPhases[i] = toNodes[i].phase;
    }

    for (const auto &interp : x->interpNodes) {
        if (interp.toIndex >= 0 && interp.toIndex < static_cast<int>(toNodes.size())) {
            x->currentPhases[interp.toIndex] = interp.phase;
        }
    }

    x->currentFrame = toIndex;
    x->samplesIntoFrame = 0;
    lfreeze_tilde_prepare_transition(x);
}

// ─────────────────────────────────────
static double lfreeze_tilde_synthesize_sample(lfreeze_tilde *x, double twopi_over_sr) {
    double sample = 0;
    for (auto &node : x->interpNodes) {
        // Private random state keeps background rendering independent of other
        // instances and of Pd's random-number generators.
        x->randomState ^= x->randomState << 13;
        x->randomState ^= x->randomState >> 17;
        x->randomState ^= x->randomState << 5;
        double noise = double(x->randomState) * (2.0 / 4294967296.0) - 1.0;
        sample += node.amp.value * (cos(node.phase) + node.bw.value * noise * sin(node.phase));
        node.phase += node.freq.value * twopi_over_sr + node.phaseCorrection;
        if (node.phase > 2.0 * M_PI) {
            node.phase -= 2.0 * M_PI;
        }

        node.freq.advance();
        node.amp.advance();
        node.bw.advance();
    }
    if (++x->samplesIntoFrame >= x->frameSize)
        lfreeze_tilde_finalize_transition(x);
    return sample;
}

// Solve oscillator phases around the complete cached cycle before rendering.
// Each partial keeps its spectral amplitude: no waveform crossfade or gain dip
// is needed at the loop boundary.
static bool lfreeze_tilde_render_loop(lfreeze_tilde *x, double sr,
                                      const std::atomic<bool> *cancelled = nullptr,
                                      size_t minimumSamples = 0) {
    if (cancelled && cancelled->load(std::memory_order_relaxed))
        return false;
    if (x->breakpoints.empty()) {
        x->loopBuffer.clear();
        return false;
    }
    if (x->frameSize < 1 || !std::isfinite(sr) || sr <= 0)
        throw std::invalid_argument("invalid loop size or sample rate");

    const size_t baseFrames = x->breakpoints.size();
    const size_t baseSamples = size_t(x->frameSize) * baseFrames;
    const size_t repeats =
        minimumSamples > baseSamples ? 1 + (minimumSamples - 1) / baseSamples : 1;
    if (repeats > std::numeric_limits<size_t>::max() / baseSamples)
        throw std::length_error("rendered loop is too large");
    const size_t size = baseSamples * repeats;
    lfreeze_tilde_build_transitions(x);

    // A longer cache lets phase closure use very small frequency corrections.
    // Reuse the base matches instead of matching every repeated frame again.
    const size_t renderedFrames = baseFrames * repeats;
    x->breakpoints.reserve(renderedFrames);
    x->transitions.reserve(renderedFrames);
    for (size_t frame = baseFrames; frame < renderedFrames; ++frame) {
        if (cancelled && cancelled->load(std::memory_order_relaxed))
            return false;
        x->breakpoints.push_back(x->breakpoints[frame % baseFrames]);
        x->transitions.push_back(x->transitions[frame % baseFrames]);
    }
    if (!lfreeze_tilde_close_phases(x, sr, cancelled))
        return false;

    x->currentFrame = x->samplesIntoFrame = 0;
    x->currentPhases.clear();
    for (const auto &node : x->breakpoints[0])
        x->currentPhases.push_back(node.phase);
    if (!x->randomState)
        x->randomState = 1;
    lfreeze_tilde_prepare_transition(x);
    x->loopBuffer.resize(size);
    const double twopi_over_sr = (2.0 * M_PI) / sr;
    for (size_t i = 0; i < size; ++i) {
        if ((i & 255) == 0 && cancelled && cancelled->load(std::memory_order_relaxed))
            return false;
        x->loopBuffer[i] = static_cast<t_sample>(lfreeze_tilde_synthesize_sample(x, twopi_over_sr));
    }
    x->loopPosition = 0;
    return true;
}

// FFTW planning inside Loris stays on Pd's scheduler, alongside other FFT-based
// externals. The more expensive additive rendering below runs in a worker.
static void lfreeze_tilde_analyze(lfreeze_tilde *x, double sr) {

    if (x->totalBufferSize <= 0 || x->captureBuffer.empty()) {
        return;
    }

    Loris::Analyzer analyzer(x->resolution, x->windowWidth);
    PartialList partials = analyzer.analyze(x->captureBuffer.data(),
                                            x->captureBuffer.data() + x->captureBuffer.size(), sr);

    x->breakpoints.clear();
    x->breakpoints.resize(x->frames);

    for (int frame = 0; frame < x->frames; ++frame) {
        double time = ((static_cast<double>(frame) + 0.5) * x->frameSize) / sr;
        std::vector<FreezeNode> nodes;

        for (PartialList::const_iterator it = partials.begin(); it != partials.end(); ++it) {
            const Partial &p = *it;
            if (partial_startTime(&p) <= time && partial_endTime(&p) >= time) {
                double a = partial_amplitudeAt(&p, time);

                FreezeNode node;
                node.freq = partial_frequencyAt(&p, time);
                node.amp = a;
                node.bw = partial_bandwidthAt(&p, time);
                node.phase = partial_phaseAt(&p, time);

                nodes.push_back(node);
            }
        }

        std::sort(nodes.begin(), nodes.end(),
                  [](const FreezeNode &a, const FreezeNode &b) { return a.freq < b.freq; });

        x->breakpoints[frame] = std::move(nodes);
    }
}

static void lfreeze_tilde_render_job(RenderJob *job) {
    try {
        lfreeze_tilde renderer;
        renderer.frameSize = job->frameSize;
        renderer.sampleRate = job->sampleRate;
        renderer.randomState = job->randomState;
        renderer.breakpoints.swap(job->breakpoints);
        // At least four seconds keeps each persistent partial's phase-closing
        // correction within 0.125 Hz, including short single-frame freezes.
        if (lfreeze_tilde_render_loop(&renderer, job->sampleRate, &job->cancelled,
                                      static_cast<size_t>(std::ceil(4 * job->sampleRate))))
            job->loop.swap(renderer.loopBuffer);
    } catch (const std::exception &error) {
        std::snprintf(job->error, sizeof(job->error), "%s", error.what());
    } catch (...) {
        std::snprintf(job->error, sizeof(job->error), "loop rendering failed");
    }
    job->completed.store(true, std::memory_order_release);
}

static void lfreeze_tilde_tick(lfreeze_tilde *x) {
    if (x->job) {
        if (!x->job->completed.load(std::memory_order_acquire)) {
            clock_delay(x->clock, 5);
            return;
        }
        if (x->worker.joinable())
            x->worker.join();
        if (x->job->generation == x->generation && !x->job->cancelled.load()) {
            if (x->job->error[0]) {
                pd_error(x, "[x.lfreeze~] %s", x->job->error);
            } else if (!x->job->loop.empty()) {
                x->loopBuffer.swap(x->job->loop);
                x->loopPosition = 0;
                x->fadeGain = 0;
                x->fadeCounter = 0;
                x->fadingIn = true;
                x->fadingOut = false;
                x->frozen = true;
            }
        }
        x->job.reset();
    }
    if (!x->renderPending)
        return;
    x->renderPending = false;
    try {
        lfreeze_tilde_analyze(x, x->sampleRate);
        auto job = std::make_unique<RenderJob>();
        job->frameSize = x->frameSize;
        job->sampleRate = x->sampleRate;
        job->generation = x->generation;
        job->randomState = x->randomState;
        job->breakpoints.swap(x->breakpoints);
        x->job = std::move(job);
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
        lfreeze_tilde_render_job(x->job.get());
#else
        x->worker = std::thread(lfreeze_tilde_render_job, x->job.get());
#endif
        clock_delay(x->clock, 5);
    } catch (const std::exception &error) {
        x->job.reset();
        pd_error(x, "[x.lfreeze~] %s", error.what());
    }
}

static void lfreeze_tilde_cancel_render(lfreeze_tilde *x) {
    ++x->generation;
    x->renderPending = false;
    if (x->job)
        x->job->cancelled.store(true, std::memory_order_relaxed);
}

// ─────────────────────────────────────
static void lfreeze_tilde_reset_state(lfreeze_tilde *x) {
    lfreeze_tilde_cancel_render(x);
    x->totalBufferSize = x->frameSize * x->frames;
    x->captureBuffer.assign(x->totalBufferSize, 0.0);
    x->capturePos = 0;
    x->captureActive = false;
    x->frozen = false;
    x->breakpoints.clear();
    x->transitions.clear();
    x->currentPhases.clear();
    x->interpNodes.clear();
    x->loopBuffer.clear();
    x->loopPosition = 0;
    x->fadeGain = 0;
    x->fadingIn = x->fadingOut = false;
    x->currentFrame = 0;
    x->samplesIntoFrame = 0;
}

// ─────────────────────────────────────
static void lfreeze_tilde_frames(lfreeze_tilde *x, t_floatarg f) {
    if (!std::isfinite(f) || double(f) > std::numeric_limits<int>::max() / x->frameSize) {
        pd_error(x, "[x.lfreeze~] invalid frame count");
        return;
    }
    int frames = static_cast<int>(std::max(t_float(1), f));
    if (frames < 1) {
        frames = 1;
    }

    x->frames = frames;
    lfreeze_tilde_reset_state(x);
}

// ─────────────────────────────────────
static void lfreeze_tilde_freeze(lfreeze_tilde *x, t_floatarg f) {
    lfreeze_tilde_cancel_render(x);

    if (f <= 0.0f) {

        if (x->frozen) {
            x->fadeStartGain = x->fadeGain;
            x->fadeCounter = 0;
            x->fadingOut = true;
            x->fadingIn = false;
        }

        x->captureActive = false;
        return;
    }

    // The worker never reads this buffer, so a new capture can start while a
    // cancelled render finishes without waiting on the audio thread.
    x->capturePos = 0;
    x->captureActive = true;

    x->frozen = false;

    x->breakpoints.clear();
    x->transitions.clear();
    x->currentPhases.clear();
    x->interpNodes.clear();
    x->loopBuffer.clear();
    x->loopPosition = 0;

    x->currentFrame = 0;
    x->samplesIntoFrame = 0;
}

// ─────────────────────────────────────
static t_int *lfreeze_tilde_perform(t_int *w) {
    auto *x = reinterpret_cast<lfreeze_tilde *>(w[1]);
    const auto *in = reinterpret_cast<const t_sample *>(w[2]);
    auto *out = reinterpret_cast<t_sample *>(w[3]);
    const int n = static_cast<int>(w[4]);

    // Full-volume playback is just a buffer copy, split only at the loop wrap.
    if (x->frozen && !x->captureActive && !x->fadingIn && !x->fadingOut && !x->loopBuffer.empty()) {
        int offset = 0;
        while (offset < n) {
            const size_t count =
                std::min(x->loopBuffer.size() - x->loopPosition, size_t(n - offset));
            std::copy_n(x->loopBuffer.data() + x->loopPosition, count, out + offset);
            offset += static_cast<int>(count);
            x->loopPosition += count;
            if (x->loopPosition == x->loopBuffer.size())
                x->loopPosition = 0;
        }
        return w + 5;
    }

    for (int i = 0; i < n; ++i) {
        // Read before writing, including when Pd aliases the inlet and outlet.
        const double input = in[i];
        if (x->captureActive) {
            x->captureBuffer[x->capturePos++] = std::isfinite(input) ? input : 0;
            if (x->capturePos == x->totalBufferSize) {
                x->captureActive = false;
                x->renderPending = true;
                clock_delay(x->clock, 0);
            }
        }
        if (!x->frozen || x->loopBuffer.empty()) {
            out[i] = 0;
            continue;
        }

        double sample = x->loopBuffer[x->loopPosition++];
        if (x->loopPosition == x->loopBuffer.size())
            x->loopPosition = 0;
        if (x->fadingIn) {
            x->fadeGain = double(x->fadeCounter) / x->fadeSamples;
            if (++x->fadeCounter >= x->fadeSamples) {
                x->fadeGain = 1;
                x->fadingIn = false;
            }
        }
        if (x->fadingOut) {
            x->fadeGain = x->fadeStartGain * (1 - double(x->fadeCounter) / x->fadeSamples);
            if (++x->fadeCounter >= x->fadeSamples) {
                x->fadeGain = 0;
                x->fadingOut = false;
                x->frozen = false;
            }
        }
        out[i] = static_cast<t_sample>(sample * x->fadeGain);
    }
    return w + 5;
}

// ─────────────────────────────────────
static void lfreeze_tilde_dsp(lfreeze_tilde *x, t_signal **sp) {
    if (x->sampleRate != sp[0]->s_sr) {
        x->sampleRate = sp[0]->s_sr;
        lfreeze_tilde_reset_state(x);
    }
    dsp_add(lfreeze_tilde_perform, 4, x, sp[0]->s_vec, sp[1]->s_vec, sp[0]->s_n);
}

// ─────────────────────────────────────
static void *lfreeze_tilde_new(t_symbol *s, int argc, t_atom *argv) {
    lfreeze_tilde *x = (lfreeze_tilde *)pd_new(lfreeze_tilde_class);
    // Pd allocates raw storage; construct the vectors and initialize DSP state
    // while preserving the object header installed by pd_new().
    t_object object = x->xobj;
    new (x) lfreeze_tilde;
    x->xobj = object;

    x->windowWidth = xlab_get_float_argument(argc, argv, "-w", 100);
    x->resolution = xlab_get_float_argument(argc, argv, "-r", 50);
    double frameSizeArg = std::round(xlab_get_float_argument(argc, argv, "-f", 4096));
    double framesArg = xlab_get_float_argument(argc, argv, "-fr", 5);
    if (!std::isfinite(frameSizeArg) || frameSizeArg < 1 ||
        frameSizeArg > std::numeric_limits<int>::max()) {
        pd_error(x, "[x.lfreeze~] invalid frame size");
        pd_free(&x->xobj.ob_pd);
        return nullptr;
    }
    int frameSize = static_cast<int>(frameSizeArg);
    if ((frameSize & (frameSize - 1)) != 0) {
        pd_error(x, "[x.lfreeze~] FrameSize (-f) must be power of two");
        pd_free(&x->xobj.ob_pd);
        return NULL;
    }
    x->frameSize = frameSize;
    if (!std::isfinite(framesArg) || framesArg > std::numeric_limits<int>::max() / frameSize ||
        !std::isfinite(x->resolution) || x->resolution <= 0 || !std::isfinite(x->windowWidth) ||
        x->windowWidth <= 0) {
        pd_error(x, "[x.lfreeze~] invalid analysis parameters or frame count");
        pd_free(&x->xobj.ob_pd);
        return nullptr;
    }
    x->frames = static_cast<int>(std::max(1.0, framesArg));
    x->sampleRate = sys_getsr() > 0 ? sys_getsr() : 44100;
    x->randomState ^= static_cast<uint32_t>(reinterpret_cast<uintptr_t>(x) >> 4);
    x->clock = clock_new(x, reinterpret_cast<t_method>(lfreeze_tilde_tick));
    x->x_out = outlet_new(&x->xobj, &s_signal);

    lfreeze_tilde_reset_state(x);

    return x;
}

// ─────────────────────────────────────
static void lfreeze_tilde_free(lfreeze_tilde *x) {
    if (x->clock)
        clock_free(x->clock);
    x->~lfreeze_tilde();
}

// ─────────────────────────────────────
extern "C" void setup_x0x2elfreeze_tilde(void) {
    lfreeze_tilde_class = class_new(gensym("x.lfreeze~"), (t_newmethod)lfreeze_tilde_new,
                                    (t_method)lfreeze_tilde_free, sizeof(lfreeze_tilde),
                                    CLASS_DEFAULT, A_GIMME, A_NULL);

    CLASS_MAINSIGNALIN(lfreeze_tilde_class, lfreeze_tilde, x_f);

    class_addmethod(lfreeze_tilde_class, (t_method)lfreeze_tilde_dsp, gensym("dsp"), A_CANT, 0);
    class_addmethod(lfreeze_tilde_class, (t_method)lfreeze_tilde_freeze, gensym("freeze"), A_FLOAT,
                    0);
    class_addmethod(lfreeze_tilde_class, (t_method)lfreeze_tilde_frames, gensym("frames"), A_FLOAT,
                    0);
}
