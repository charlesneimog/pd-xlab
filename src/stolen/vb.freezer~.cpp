// Pure Data port of vb.freezer~ by Volker Boehm (2011-2020).
// Capture a short sound and write a randomized-phase loop into a Pd array.
#include <m_pd.h>

#include "vb_freezer_dsp.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#define _USE_MATH_DEFINES
#include <cmath>

static t_class *vb_freezer_class;
static constexpr size_t vb_freezer_max_size = vb_freezer::kMaximumSize;

struct FreezerState {
    t_symbol *arrayName{&s_};
    double recordMs{250};
    double sampleRate{0};
    std::vector<double> ring;
    size_t writePosition{0};
    double threshold{0};
    double bufferGain{1};
    double freezeGain{1};
    bool busy{false};
    uint64_t generation{0};

    // Only the scheduler accesses Pd arrays or creates/destroys FFT plans.
    // The worker touches a private snapshot until completed publishes its result.
    std::vector<double> input;
    t_symbol *jobArray{nullptr};
    uint64_t jobGeneration{0};
    double jobThreshold{0}, jobBufferGain{1}, jobFreezeGain{1};
    std::unique_ptr<vb_freezer::Processor> processor;
    const std::vector<double> *result{nullptr};
    char error[256]{};
    uint32_t randomState{0x9e3779b9u};
    std::thread worker;
    std::atomic<bool> completed{false};

    ~FreezerState() {
        if (worker.joinable())
            worker.join();
    }
};

struct t_vb_freezer {
    t_object object;
    t_float signalValue;
    t_outlet *sizeOutlet;
    t_outlet *doneOutlet;
    t_clock *clock;
    FreezerState *state;
};

static t_garray *vb_freezer_array(t_vb_freezer *x, t_symbol *name, int &size, t_word *&words) {
    auto *array = reinterpret_cast<t_garray *>(pd_findbyclass(name, garray_class));
    if (!array || !garray_getfloatwords(array, &size, &words)) {
        pd_error(x, "[vb.freezer~] cannot access array '%s'", name->s_name);
        return nullptr;
    }
    return array;
}

static void vb_freezer_configure_recording(t_vb_freezer *x, double sampleRate) {
    auto &state = *x->state;
    if (sampleRate <= 0 || !std::isfinite(sampleRate))
        sampleRate = 44100;
    if (sampleRate == state.sampleRate)
        return;
    double samples = std::round(state.recordMs * 0.001 * sampleRate);
    samples = std::max(2.0, std::min(samples, double(vb_freezer_max_size)));
    state.ring.assign(static_cast<size_t>(samples), 0);
    state.writePosition = 0;
    state.sampleRate = sampleRate;
}

static void vb_freezer_process(FreezerState *state) {
    try {
        state->result =
            &state->processor->process(state->input, state->jobThreshold, state->randomState);
    } catch (const std::exception &error) {
        std::snprintf(state->error, sizeof(state->error), "%s", error.what());
    } catch (...) {
        std::snprintf(state->error, sizeof(state->error), "FFT processing failed");
    }
    state->completed.store(true, std::memory_order_release);
}

static void vb_freezer_tick(t_vb_freezer *x) {
    auto &state = *x->state;
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
    // A single-threaded WebAssembly host processes deferred jobs on its scheduler.
    if (!state.completed.load(std::memory_order_acquire))
        vb_freezer_process(&state);
#else
    if (!state.completed.load(std::memory_order_acquire)) {
        clock_delay(x->clock, 5);
        return;
    }
    if (state.worker.joinable())
        state.worker.join();
#endif
    state.busy = false;
    if (state.error[0]) {
        pd_error(x, "[vb.freezer~] %s", state.error);
        return;
    }
    // Changing the target or clearing it cancels an outstanding write.
    if (state.jobGeneration != state.generation)
        return;

    int size = 0;
    t_word *words = nullptr;
    auto *array = vb_freezer_array(x, state.jobArray, size, words);
    if (!array)
        return;
    if (size != static_cast<int>(state.input.size())) {
        pd_error(x, "[vb.freezer~] array resized during processing; result discarded");
        return;
    }
    for (int i = 0; i < size; ++i)
        words[i].w_float =
            words[i].w_float * state.jobBufferGain + (*state.result)[i] * state.jobFreezeGain;
    garray_redraw(array);
    outlet_float(x->sizeOutlet, size);
    outlet_bang(x->doneOutlet);
}

static void vb_freezer_start(t_vb_freezer *x, bool wholeArray) {
    auto &state = *x->state;
    if (state.busy) {
        pd_error(x, "[vb.freezer~] busy; wait for processing to finish");
        return;
    }
    int size = 0;
    t_word *words = nullptr;
    if (!vb_freezer_array(x, state.arrayName, size, words))
        return;
    if (size < 2 || static_cast<size_t>(size) > vb_freezer_max_size) {
        pd_error(x, "[vb.freezer~] array size must be between 2 and 16777216 samples");
        return;
    }

    try {
        // Other Pd externals also use FFTW's global planner on the scheduler.
        // Keep planning here so their setup/free cannot race our worker.
        if (!state.processor)
            state.processor = std::make_unique<vb_freezer::Processor>();
        state.processor->prepare(static_cast<size_t>(size));
        state.input.assign(size, 0);
        if (wholeArray) {
            for (int i = 0; i < size; ++i)
                state.input[i] = words[i].w_float;
        } else {
            const size_t count = std::min(state.ring.size(), state.input.size());
            const size_t start =
                (state.writePosition + state.ring.size() - count) % state.ring.size();
            const size_t fade = std::min(size_t(1000), count / 2);
            for (size_t i = 0; i < count; ++i) {
                double window = 1;
                if (i < fade)
                    window = double(i) / fade;
                else if (i >= count - fade)
                    window = double(count - 1 - i) / fade;
                state.input[i] = state.ring[(start + i) % state.ring.size()] * window;
            }
        }

        state.jobArray = state.arrayName;
        state.jobGeneration = state.generation;
        state.jobThreshold = state.threshold;
        state.jobBufferGain = state.bufferGain;
        state.jobFreezeGain = state.freezeGain;
        state.error[0] = 0;
        state.result = nullptr;
        state.completed.store(false, std::memory_order_relaxed);
        state.busy = true;
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
        state.worker = std::thread(vb_freezer_process, &state);
#endif
        clock_delay(x->clock, 0);
    } catch (const std::exception &error) {
        state.busy = false;
        pd_error(x, "[vb.freezer~] %s", error.what());
    }
}

static void vb_freezer_bang(t_vb_freezer *x) { vb_freezer_start(x, false); }
static void vb_freezer_freeze_buf(t_vb_freezer *x) { vb_freezer_start(x, true); }

static void vb_freezer_set(t_vb_freezer *x, t_symbol *name) {
    x->state->arrayName = name;
    ++x->state->generation;
}

static void vb_freezer_clear(t_vb_freezer *x) {
    ++x->state->generation;
    int size = 0;
    t_word *words = nullptr;
    auto *array = vb_freezer_array(x, x->state->arrayName, size, words);
    if (!array)
        return;
    for (int i = 0; i < size; ++i)
        words[i].w_float = 0;
    garray_redraw(array);
}

static void vb_freezer_thresh(t_vb_freezer *x, t_floatarg value) {
    if (std::isfinite(value))
        x->state->threshold = std::max(0.0, double(value));
}
static void vb_freezer_bgain(t_vb_freezer *x, t_floatarg value) {
    if (std::isfinite(value))
        x->state->bufferGain = std::clamp(double(value), 0.0, 1.0);
}
static void vb_freezer_fgain(t_vb_freezer *x, t_floatarg value) {
    if (std::isfinite(value))
        x->state->freezeGain = std::clamp(double(value), 0.0, 1.0);
}

static void vb_freezer_info(t_vb_freezer *x) {
    auto &state = *x->state;
    post("[vb.freezer~] array: %s; capture: %lu samples (%.3g ms); %s", state.arrayName->s_name,
         static_cast<unsigned long>(state.ring.size()), state.recordMs,
         state.busy ? "processing" : "ready");
}

static t_int *vb_freezer_perform(t_int *w) {
    auto *x = reinterpret_cast<t_vb_freezer *>(w[1]);
    const auto *input = reinterpret_cast<const t_sample *>(w[2]);
    const int count = static_cast<int>(w[3]);
    auto &state = *x->state;
    for (int i = 0; i < count; ++i) {
        const double sample = input[i];
        state.ring[state.writePosition] = std::isfinite(sample) ? sample : 0;
        if (++state.writePosition == state.ring.size())
            state.writePosition = 0;
    }
    return w + 4;
}

static void vb_freezer_dsp(t_vb_freezer *x, t_signal **signals) {
    try {
        vb_freezer_configure_recording(x, signals[0]->s_sr);
    } catch (const std::exception &error) {
        pd_error(x, "[vb.freezer~] %s", error.what());
        return;
    }
    dsp_add(vb_freezer_perform, 3, x, signals[0]->s_vec, signals[0]->s_n);
}

static void vb_freezer_free(t_vb_freezer *x) {
    if (x->clock)
        clock_free(x->clock);
    delete x->state;
}

static void *vb_freezer_new(t_symbol *, int argc, t_atom *argv) {
    auto *x = reinterpret_cast<t_vb_freezer *>(pd_new(vb_freezer_class));
    x->state = nullptr;
    x->clock = nullptr;
    x->signalValue = 0;
    try {
        if (argc > 2 || (argc > 0 && argv[0].a_type != A_SYMBOL) ||
            (argc > 1 && argv[1].a_type != A_FLOAT))
            throw std::invalid_argument("usage: vb.freezer~ array-name [record-ms]");
        x->state = new FreezerState;
        auto &state = *x->state;
        if (argc > 0)
            state.arrayName = atom_getsymbol(argv);
        if (argc > 1) {
            double ms = atom_getfloat(argv + 1);
            if (!std::isfinite(ms))
                throw std::invalid_argument("record-ms must be finite");
            if (ms > 0)
                state.recordMs = ms;
        }
        // Different instances should not generate identical randomized phases.
        state.randomState ^= static_cast<uint32_t>(reinterpret_cast<uintptr_t>(x) >> 4);
        if (!state.randomState)
            state.randomState = 1;
        vb_freezer_configure_recording(x, sys_getsr());
        x->clock = clock_new(x, reinterpret_cast<t_method>(vb_freezer_tick));
        x->sizeOutlet = outlet_new(&x->object, &s_float);
        x->doneOutlet = outlet_new(&x->object, &s_bang);
        return x;
    } catch (const std::exception &error) {
        pd_error(x, "[vb.freezer~] %s", error.what());
        pd_free(&x->object.ob_pd);
        return nullptr;
    }
}

extern "C" void setup_vb0x2efreezer_tilde(void) {
    vb_freezer_class =
        class_new(gensym("vb.freezer~"), reinterpret_cast<t_newmethod>(vb_freezer_new),
                  reinterpret_cast<t_method>(vb_freezer_free), sizeof(t_vb_freezer), CLASS_DEFAULT,
                  A_GIMME, A_NULL);
    CLASS_MAINSIGNALIN(vb_freezer_class, t_vb_freezer, signalValue);
    class_addbang(vb_freezer_class, reinterpret_cast<t_method>(vb_freezer_bang));
    class_addmethod(vb_freezer_class, reinterpret_cast<t_method>(vb_freezer_dsp), gensym("dsp"),
                    A_CANT, 0);
    class_addmethod(vb_freezer_class, reinterpret_cast<t_method>(vb_freezer_set), gensym("set"),
                    A_SYMBOL, 0);
    class_addmethod(vb_freezer_class, reinterpret_cast<t_method>(vb_freezer_thresh),
                    gensym("thresh"), A_FLOAT, 0);
    class_addmethod(vb_freezer_class, reinterpret_cast<t_method>(vb_freezer_bgain), gensym("bgain"),
                    A_FLOAT, 0);
    class_addmethod(vb_freezer_class, reinterpret_cast<t_method>(vb_freezer_fgain), gensym("fgain"),
                    A_FLOAT, 0);
    class_addmethod(vb_freezer_class, reinterpret_cast<t_method>(vb_freezer_freeze_buf),
                    gensym("freeze_buf"), A_NULL);
    class_addmethod(vb_freezer_class, reinterpret_cast<t_method>(vb_freezer_clear), gensym("clear"),
                    A_NULL);
    class_addmethod(vb_freezer_class, reinterpret_cast<t_method>(vb_freezer_info), gensym("info"),
                    A_NULL);
}
