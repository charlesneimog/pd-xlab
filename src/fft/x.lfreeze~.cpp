#include <loris.h>
#include <m_pd.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#include <PartialList.h>

#include "../shared/getargs.h"

static t_class *lfreeze_tilde_class;

// ─────────────────────────────────────
struct FreezeNode {
    double freq;
    double amp;
    double bw;
    double phase;
};

// ─────────────────────────────────────
struct InterpNode {
    FreezeNode from;
    FreezeNode to;
    double phase;
    int toIndex;
};

// ─────────────────────────────────────
class lfreeze_tilde {
  public:
    t_object xobj;
    t_outlet *x_out;
    t_float x_f;

    float resolution{50};
    float windowWidth{100};
    float frameSize{4096};
    float totalBufferSize{4096};
    float frames{1};

    std::vector<double> captureBuffer;
    int capturePos{0};
    bool captureActive{false};
    bool frozen{false};

    std::vector<std::vector<FreezeNode>> breakpoints;
    std::vector<double> currentPhases;
    std::vector<InterpNode> interpNodes;
    std::vector<FreezeNode> interpScratch;

    int currentFrame{0};
    int samplesIntoFrame{0};

    // fade
    double fadeGain{0.0};
    int fadeCounter{0};
    int fadeSamples{4096};
    bool fadingIn{false};
    bool fadingOut{false};
};

// ─────────────────────────────────────
static double log_interp(double a, double b, double t) {
    const double eps = 1e-12;
    if (a <= eps || b <= eps) {
        return a + (b - a) * t;
    }
    return exp(log(a) * (1.0 - t) + log(b) * t);
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
static void interpolatePartialSets(const std::vector<InterpNode> &nodes, double t,
                                   std::vector<FreezeNode> &out) {
    out.resize(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) {
        const FreezeNode &a = nodes[i].from;
        const FreezeNode &b = nodes[i].to;
        FreezeNode &o = out[i];
        o.freq = log_interp(a.freq, b.freq, t);
        o.amp = log_interp(a.amp, b.amp, t);
        o.bw = log_interp(a.bw, b.bw, t);
        o.phase = 0.0;
    }
}

// ─────────────────────────────────────
static void lfreeze_tilde_prepare_transition(lfreeze_tilde *x) {
    if (x->breakpoints.empty()) {
        return;
    }

    int fromIndex = x->currentFrame;
    int toIndex = (fromIndex + 1) % static_cast<int>(round(x->frames));
    const std::vector<FreezeNode> &fromNodes = x->breakpoints[fromIndex];
    const std::vector<FreezeNode> &toNodes = x->breakpoints[toIndex];

    if (x->currentPhases.size() != fromNodes.size()) {
        x->currentPhases.clear();
        x->currentPhases.reserve(fromNodes.size());
        for (const auto &node : fromNodes) {
            x->currentPhases.push_back(node.phase);
        }
    }

    std::vector<int> fromTo;
    std::vector<int> toFrom;
    match_partials(fromNodes, toNodes, fromTo, toFrom);

    x->interpNodes.clear();
    x->interpNodes.reserve(fromNodes.size() + toNodes.size());

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

        InterpNode interp;
        interp.from = fromNode;
        interp.to = toNode;
        interp.phase = x->currentPhases[i];
        interp.toIndex = matchIndex;
        x->interpNodes.push_back(interp);
    }

    for (size_t j = 0; j < toNodes.size(); ++j) {
        if (toFrom[j] >= 0) {
            continue;
        }

        FreezeNode fromNode = toNodes[j];
        fromNode.amp = 0.0;

        InterpNode interp;
        interp.from = fromNode;
        interp.to = toNodes[j];
        interp.phase = toNodes[j].phase;
        interp.toIndex = static_cast<int>(j);
        x->interpNodes.push_back(interp);
    }

    x->interpScratch.resize(x->interpNodes.size());
}

// ─────────────────────────────────────
static void lfreeze_tilde_finalize_transition(lfreeze_tilde *x) {
    if (x->breakpoints.empty()) {
        return;
    }

    int toIndex = (x->currentFrame + 1) % static_cast<int>(round(x->frames));
    const std::vector<FreezeNode> &toNodes = x->breakpoints[toIndex];

    std::vector<double> nextPhases(toNodes.size(), 0.0);
    std::vector<char> hasPhase(toNodes.size(), 0);

    for (const auto &interp : x->interpNodes) {
        if (interp.toIndex >= 0 && interp.toIndex < static_cast<int>(toNodes.size())) {
            nextPhases[interp.toIndex] = interp.phase;
            hasPhase[interp.toIndex] = 1;
        }
    }

    for (size_t i = 0; i < toNodes.size(); ++i) {
        if (!hasPhase[i]) {
            nextPhases[i] = toNodes[i].phase;
        }
    }

    x->currentFrame = toIndex;
    x->samplesIntoFrame = 0;
    x->currentPhases.swap(nextPhases);
    lfreeze_tilde_prepare_transition(x);
}

// ─────────────────────────────────────
static void lfreeze_tilde_analyze(lfreeze_tilde *x) {
    t_float sr = sys_getsr();
    PartialList partials;

    if (x->totalBufferSize <= 0 || x->captureBuffer.empty()) {
        return;
    }

    analyze(x->captureBuffer.data(), x->totalBufferSize, sr, &partials);

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

    x->currentFrame = 0;
    x->samplesIntoFrame = 0;

    x->currentPhases.clear();
    x->currentPhases.reserve(x->breakpoints[0].size());

    for (const auto &node : x->breakpoints[0]) {
        x->currentPhases.push_back(node.phase);
    }

    lfreeze_tilde_prepare_transition(x);

    x->frozen = true;

    // fade in
    x->fadeGain = 0.0;
    x->fadeCounter = 0;
    x->fadingIn = true;
    x->fadingOut = false;
}

// ─────────────────────────────────────
static void lfreeze_tilde_reset_state(lfreeze_tilde *x) {
    x->totalBufferSize = x->frameSize * x->frames;
    x->captureBuffer.assign(x->totalBufferSize, 0.0);
    x->capturePos = 0;
    x->captureActive = false;
    x->frozen = false;
    x->breakpoints.clear();
    x->currentPhases.clear();
    x->interpNodes.clear();
    x->interpScratch.clear();
    x->currentFrame = 0;
    x->samplesIntoFrame = 0;
}

// ─────────────────────────────────────
static void lfreeze_tilde_frames(lfreeze_tilde *x, t_floatarg f) {
    int frames = static_cast<int>(f);
    if (frames < 1) {
        frames = 1;
    }

    x->frames = frames;
    lfreeze_tilde_reset_state(x);
}

// ─────────────────────────────────────
static void lfreeze_tilde_freeze(lfreeze_tilde *x, t_floatarg f) {

    if (f <= 0.0f) {

        if (x->frozen) {
            x->fadeCounter = 0;
            x->fadingOut = true;
            x->fadingIn = false;
        }

        x->captureActive = false;
        return;
    }

    x->capturePos = 0;
    x->captureActive = true;

    x->frozen = false;

    x->breakpoints.clear();
    x->currentPhases.clear();
    x->interpNodes.clear();
    x->interpScratch.clear();

    x->currentFrame = 0;
    x->samplesIntoFrame = 0;
}

// ─────────────────────────────────────
static t_int *lfreeze_tilde_perform(t_int *w) {
    lfreeze_tilde *x = (lfreeze_tilde *)(w[1]);
    t_sample *in = (t_sample *)(w[2]);
    t_sample *out = (t_sample *)(w[3]);
    int n = (int)(w[4]);

    double sr = sys_getsr();
    double twopi_over_sr = (2.0 * M_PI) / sr;

    for (int i = 0; i < n; i++) {

        double sample = in[i];

        if (x->captureActive) {

            x->captureBuffer[x->capturePos++] = sample;

            if (x->capturePos >= x->totalBufferSize) {
                x->captureActive = false;
                lfreeze_tilde_analyze(x);
            }
        }

        if (x->frozen) {

            double out_sample = 0.0;

            if (!x->interpNodes.empty()) {

                double t = static_cast<double>(x->samplesIntoFrame) / x->frameSize;

                interpolatePartialSets(x->interpNodes, t, x->interpScratch);

                for (size_t n = 0; n < x->interpNodes.size(); ++n) {

                    InterpNode &node = x->interpNodes[n];
                    const FreezeNode &params = x->interpScratch[n];

                    double noise = ((double)rand() / (double)RAND_MAX) * 2.0 - 1.0;

                    out_sample +=
                        params.amp * (cos(node.phase) + params.bw * noise * sin(node.phase));

                    node.phase += params.freq * twopi_over_sr;

                    if (node.phase > 2.0 * M_PI) {
                        node.phase -= 2.0 * M_PI;
                    }
                }
            }

            // fade in
            if (x->fadingIn) {

                x->fadeGain = (double)x->fadeCounter / (double)x->fadeSamples;

                x->fadeCounter++;

                if (x->fadeCounter >= x->fadeSamples) {
                    x->fadeCounter = x->fadeSamples;
                    x->fadeGain = 1.0;
                    x->fadingIn = false;
                }
            }

            // fade out
            if (x->fadingOut) {

                x->fadeGain = 1.0 - ((double)x->fadeCounter / (double)x->fadeSamples);

                x->fadeCounter++;

                if (x->fadeCounter >= x->fadeSamples) {

                    x->fadeCounter = x->fadeSamples;
                    x->fadeGain = 0.0;
                    x->fadingOut = false;
                    x->frozen = false;
                }
            }

            out_sample *= x->fadeGain;

            out[i] = (t_sample)out_sample;

            x->samplesIntoFrame++;

            if (x->samplesIntoFrame >= x->frameSize) {
                lfreeze_tilde_finalize_transition(x);
            }

        } else {

            out[i] = 0;
        }
    }

    return (w + 5);
}

// ─────────────────────────────────────
static void lfreeze_tilde_dsp(lfreeze_tilde *x, t_signal **sp) {
    dsp_add(lfreeze_tilde_perform, 4, x, sp[0]->s_vec, sp[1]->s_vec, sp[0]->s_n);
}

// ─────────────────────────────────────
static void *lfreeze_tilde_new(t_symbol *s, int argc, t_atom *argv) {
    lfreeze_tilde *x = (lfreeze_tilde *)pd_new(lfreeze_tilde_class);
    x->x_out = outlet_new(&x->xobj, &s_signal);

    x->windowWidth = xlab_get_float_argument(argc, argv, "-w", 100);
    x->resolution = xlab_get_float_argument(argc, argv, "-r", 50);
    x->frameSize = xlab_get_float_argument(argc, argv, "-f", 4096);
    x->frames = xlab_get_float_argument(argc, argv, "-fr", 5);

    int frameSize = static_cast<int>(round(x->frameSize));
    if (!(frameSize > 0 && (frameSize & (frameSize - 1)) == 0)) {
        pd_error(x, "[x.lfreeze~] FrameSize (-f) must be power of two");
        return NULL;
    }

    lfreeze_tilde_reset_state(x);

    analyzer_configure(x->resolution, x->windowWidth);
    return x;
}

// ─────────────────────────────────────
static void lfreeze_tilde_free(lfreeze_tilde *x) {}

// ─────────────────────────────────────
extern "C" void setup_x0x2elfreeze_tilde(void) {
    lfreeze_tilde_class =
        class_new(gensym("x.lfreeze~"), (t_newmethod)lfreeze_tilde_new, (t_method)lfreeze_tilde_free,
                  sizeof(lfreeze_tilde), CLASS_DEFAULT, A_GIMME, A_NULL);

    CLASS_MAINSIGNALIN(lfreeze_tilde_class, lfreeze_tilde, x_f);

    class_addmethod(lfreeze_tilde_class, (t_method)lfreeze_tilde_dsp, gensym("dsp"), A_CANT, 0);
    class_addmethod(lfreeze_tilde_class, (t_method)lfreeze_tilde_freeze, gensym("freeze"), A_FLOAT,
                    0);
    class_addmethod(lfreeze_tilde_class, (t_method)lfreeze_tilde_frames, gensym("frames"), A_FLOAT,
                    0);
}
