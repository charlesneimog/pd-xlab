#ifndef XL_LFREEZE_PHASE_H
#define XL_LFREEZE_PHASE_H

#include <atomic>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace lfreeze_phase_detail {

// The renderer advances frequency after using its current value. Integrate
// those N discrete values, rather than the continuous interpolation curve.
static long double frequency_sum(const InterpValue &frequency, int samples) {
    const long double n = samples;
    const long double first = frequency.value;
    if (!frequency.geometric)
        return first * n + frequency.step * n * (n - 1.0L) / 2.0L;
    if (frequency.step == 1.0)
        return first * n;
    const long double logStep = std::log(static_cast<long double>(frequency.step));
    return first * std::expm1(n * logStep) / std::expm1(logStep);
}

struct Vertex {
    FreezeNode *breakpoint;
    InterpNode *transition;
    size_t next;
    size_t previous;
    long double advance;
    bool assigned;
};

} // namespace lfreeze_phase_detail

// Close oscillator trajectories before rendering a cached cycle. Include this
// file after lfreeze_tilde and its transition builders. Transitions must already
// contain fresh interpolation values, and frame count must be final.
//
// Matched partials form disjoint directed chains and cycles. Chains retain their
// initial analysis phase. Cycles receive the smallest uniform frequency offset
// that makes their total phase advance an integer number of revolutions, even
// when matching permutes partials over several loop traversals. Births join the
// resulting destination phase. Amplitudes and audio samples are never modified.
// Returns false on cancellation or invalid prepared transition data.
static bool lfreeze_tilde_close_phases(lfreeze_tilde *x, double sr,
                                      const std::atomic<bool> *cancelled = nullptr) {
    using lfreeze_phase_detail::Vertex;
    const auto isCancelled = [cancelled]() {
        return cancelled && cancelled->load(std::memory_order_relaxed);
    };
    if (isCancelled())
        return false;
    if (!std::isfinite(sr) || sr <= 0.0 || x->frameSize <= 0 ||
        x->transitions.size() != x->breakpoints.size())
        return false;

    const long double twopi = 2.0L * std::acos(-1.0L);
    const long double radiansPerHz = twopi / sr;
    const size_t none = std::numeric_limits<size_t>::max();
    const size_t frames = x->breakpoints.size();
    std::vector<size_t> offsets(frames + 1, 0);
    for (size_t frame = 0; frame < frames; ++frame) {
        if (isCancelled())
            return false;
        offsets[frame + 1] = offsets[frame] + x->breakpoints[frame].size();
        if (x->transitions[frame].size() < x->breakpoints[frame].size())
            return false;
        for (auto &transition : x->transitions[frame])
            transition.phaseCorrection = 0.0;
    }

    std::vector<Vertex> vertices;
    vertices.reserve(offsets.back());
    for (size_t frame = 0; frame < frames; ++frame) {
        if (isCancelled())
            return false;
        const size_t nextFrame = (frame + 1) % frames;
        for (size_t node = 0; node < x->breakpoints[frame].size(); ++node) {
            auto &transition = x->transitions[frame][node];
            size_t next = none;
            if (transition.toIndex >= 0) {
                if (static_cast<size_t>(transition.toIndex) >= x->breakpoints[nextFrame].size())
                    return false;
                next = offsets[nextFrame] + static_cast<size_t>(transition.toIndex);
            }
            const long double advance =
                lfreeze_phase_detail::frequency_sum(transition.freq, x->frameSize) * radiansPerHz;
            if (!std::isfinite(advance))
                return false;
            vertices.push_back({&x->breakpoints[frame][node], &transition, next, none,
                                advance, false});
        }
    }
    for (size_t node = 0; node < vertices.size(); ++node) {
        if ((node & 255) == 0 && isCancelled())
            return false;
        const size_t next = vertices[node].next;
        if (next != none) {
            // Partial matching must be one-to-one.
            if (vertices[next].previous != none)
                return false;
            vertices[next].previous = node;
        }
    }

    const auto assignPhase = [twopi](Vertex &vertex, long double phase) {
        const double wrapped = static_cast<double>(std::remainder(phase, twopi));
        vertex.breakpoint->phase = wrapped;
        vertex.transition->from.phase = wrapped;
        vertex.transition->phase = wrapped;
        vertex.assigned = true;
    };

    // An unmatched birth is a root. Propagate through the wrap as necessary;
    // chains require no frequency correction because they begin/end at silence.
    for (size_t start = 0; start < vertices.size(); ++start) {
        if ((start & 255) == 0 && isCancelled())
            return false;
        if (vertices[start].previous != none || vertices[start].assigned)
            continue;
        long double phase = vertices[start].breakpoint->phase;
        for (size_t node = start; node != none; node = vertices[node].next) {
            if (isCancelled())
                return false;
            auto &vertex = vertices[node];
            if (vertex.assigned)
                return false;
            assignPhase(vertex, phase);
            phase = std::remainder(phase + vertex.advance, twopi);
        }
    }

    // Every remaining vertex belongs to a closed cycle. A cycle can contain
    // several traversals of the stored audio loop when seam matching permutes
    // partials, so solve the whole cycle rather than each frame-zero node.
    std::vector<size_t> cycle;
    for (size_t start = 0; start < vertices.size(); ++start) {
        if ((start & 255) == 0 && isCancelled())
            return false;
        if (vertices[start].assigned)
            continue;
        cycle.clear();
        long double totalAdvance = 0.0L;
        size_t node = start;
        do {
            if (isCancelled())
                return false;
            if (node == none || vertices[node].assigned || cycle.size() >= vertices.size())
                return false;
            cycle.push_back(node);
            totalAdvance += vertices[node].advance;
            node = vertices[node].next;
        } while (node != start);

        const double correction = static_cast<double>(
            -std::remainder(totalAdvance, twopi) /
            (static_cast<long double>(cycle.size()) * x->frameSize));
        long double phase = vertices[start].breakpoint->phase;
        for (size_t index : cycle) {
            if (isCancelled())
                return false;
            auto &vertex = vertices[index];
            vertex.transition->phaseCorrection = correction;
            assignPhase(vertex, phase);
            phase = std::remainder(phase + vertex.advance +
                                       static_cast<long double>(correction) * x->frameSize,
                                   twopi);
        }
    }

    for (size_t frame = 0; frame < frames; ++frame) {
        if (isCancelled())
            return false;
        const size_t nextFrame = (frame + 1) % frames;
        auto &transition = x->transitions[frame];
        const size_t fromCount = x->breakpoints[frame].size();
        for (size_t node = 0; node < transition.size(); ++node) {
            auto &edge = transition[node];
            if (edge.toIndex < 0) {
                // Deaths retain the phase propagated from their source.
                edge.to.phase = static_cast<double>(std::remainder(
                    edge.phase +
                        lfreeze_phase_detail::frequency_sum(edge.freq, x->frameSize) * radiansPerHz,
                    twopi));
                continue;
            }
            if (static_cast<size_t>(edge.toIndex) >= x->breakpoints[nextFrame].size())
                return false;
            edge.to.phase = x->breakpoints[nextFrame][edge.toIndex].phase;
            if (node >= fromCount) {
                // Births start at zero amplitude and reach the assigned target
                // phase after exactly one interpolation frame.
                const long double advance =
                    lfreeze_phase_detail::frequency_sum(edge.freq, x->frameSize) * radiansPerHz;
                edge.phase = static_cast<double>(std::remainder(edge.to.phase - advance, twopi));
                edge.from.phase = edge.phase;
            }
        }
    }
    return !isCancelled();
}

#endif // XL_LFREEZE_PHASE_H
