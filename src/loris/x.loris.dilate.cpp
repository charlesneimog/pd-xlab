#include "m_pd.h"

#include <Analyzer.h>
#include <Dilator.h>
#include <PartialList.h>
#include <Synthesizer.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

static t_class *loris_dilate;

// ─────────────────────────────────────
struct LorisDilateState {
    std::mutex mutex;
    std::condition_variable finished;
    std::vector<double> initial_times;
    std::vector<double> target_times;
    size_t worker_count{0};
    bool busy{false};
};

// ─────────────────────────────────────
struct LorisMorphJob {
    t_pdinstance *instance{nullptr};
    t_pd *owner{nullptr};
    t_symbol *destination{&s_};
    double samplerate{0.0};
    double resolution{0.0};
    double duration{0.0};
    double output_duration{0.0};
    std::vector<double> initial_times;
    std::vector<double> target_times;
    std::vector<double> input;
    std::vector<double> output;
    char error[256]{};
};

// ─────────────────────────────────────
typedef struct _loris_morph {
    t_object obj;
    t_symbol *source;
    t_symbol *destination;
    t_float samplerate;
    t_float resolution;
    LorisDilateState *state;
} t_loris_dilate;

// ─────────────────────────────────────
// Called by Pd's queue on the scheduler thread. This is the only place where
// a completed job accesses the destination array or reports through Pd.
static void loris_dilate_deliver(t_pd *obj, void *data) {
    std::unique_ptr<LorisMorphJob> job(static_cast<LorisMorphJob *>(data));
    if (!obj)
        return;

    auto *x = reinterpret_cast<t_loris_dilate *>(obj);
    x->state->busy = false;

    if (job->error[0]) {
        pd_error(x, "[x.loris.dilate] %s", job->error);
        return;
    }

    auto *dst_array = reinterpret_cast<t_garray *>(pd_findbyclass(job->destination, garray_class));
    if (!dst_array) {
        pd_error(x, "[x.loris.dilate] destination '%s' not found", job->destination->s_name);
        return;
    }

    garray_resize_long(dst_array, static_cast<long>(job->output.size()));

    int dst_size = 0;
    t_word *dst_data = nullptr;
    if (!garray_getfloatwords(dst_array, &dst_size, &dst_data)) {
        pd_error(x, "[x.loris.dilate] invalid destination '%s'", job->destination->s_name);
        return;
    }

    const int count = std::min(dst_size, static_cast<int>(job->output.size()));
    for (int i = 0; i < count; ++i)
        dst_data[i].w_float = static_cast<t_float>(job->output[static_cast<size_t>(i)]);

    garray_redraw(dst_array);
    post("[x.loris.dilate] %.2f -> %.2f seconds", job->duration, job->output_duration);
}

// ─────────────────────────────────────
// Runs without touching the Pd API or the owning Pd object. The sole exception
// is pd_queue_mess(), whose purpose is to safely transfer the result back to
// the scheduler thread.
static void loris_dilate_worker(LorisMorphJob *job, LorisDilateState *state) {
    try {
        Loris::Analyzer analyzer(job->resolution, 2.0 * job->resolution);
        Loris::PartialList partials = analyzer.analyze(job->input, job->samplerate);

        Loris::Dilator dilator;
        for (size_t i = 0; i < job->initial_times.size(); ++i)
            dilator.insert(job->initial_times[i], job->target_times[i]);
        dilator.dilate(partials.begin(), partials.end());

        if (!partials.empty()) {
            const size_t requested_size = job->output.size();
            {
                Loris::Synthesizer synth(job->samplerate, job->output);
                synth.synthesize(partials.begin(), partials.end());
            }
            // Synthesizer adds its final fade when it is destroyed.
            job->output.resize(requested_size);
        }
    } catch (const std::exception &error) {
        std::snprintf(job->error, sizeof(job->error), "%s", error.what());
    } catch (...) {
        std::snprintf(job->error, sizeof(job->error), "Loris processing failed");
    }

    // Queue first, then publish worker completion. The destructor waits for
    // this ordering before canceling queued messages, so owner cannot dangle.
    pd_queue_mess(job->instance, job->owner, job, loris_dilate_deliver);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        --state->worker_count;
        state->finished.notify_all();
    }
}

// ─────────────────────────────────────
static int loris_get_array(t_loris_dilate *x, t_symbol *name, t_garray **array, int *size,
                           t_word **data) {
    *array = reinterpret_cast<t_garray *>(pd_findbyclass(name, garray_class));
    if (!*array) {
        pd_error(x, "[x.loris.dilate] array '%s' not found", name->s_name);
        return 0;
    }

    if (!garray_getfloatwords(*array, size, data)) {
        pd_error(x, "[x.loris.dilate] invalid array '%s'", name->s_name);
        return 0;
    }

    return 1;
}

// ─────────────────────────────────────
static void loris_dilate_source(t_loris_dilate *x, t_symbol *s) { x->source = s; }

// ─────────────────────────────────────
static void loris_dilate_destination(t_loris_dilate *x, t_symbol *s) { x->destination = s; }

// ─────────────────────────────────────
static bool loris_dilate_set_times(t_loris_dilate *x, const char *name, int argc, t_atom *argv,
                                   std::vector<double> &times) {
    std::vector<double> values;
    values.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        if (argv[i].a_type != A_FLOAT) {
            pd_error(x, "[x.loris.dilate] %s values must be numbers", name);
            return false;
        }
        const double value = atom_getfloat(argv + i);
        if (!std::isfinite(value)) {
            pd_error(x, "[x.loris.dilate] %s values must be finite", name);
            return false;
        }
        values.push_back(value);
    }
    times.swap(values);
    return true;
}

static void loris_dilate_initial(t_loris_dilate *x, t_symbol *, int argc, t_atom *argv) {
    loris_dilate_set_times(x, "initial", argc, argv, x->state->initial_times);
}

static void loris_dilate_target(t_loris_dilate *x, t_symbol *, int argc, t_atom *argv) {
    loris_dilate_set_times(x, "target", argc, argv, x->state->target_times);
}

// Match Dilator's piecewise time mapping so the destination array is allocated
// to the warped duration of the complete source array.
static double loris_dilate_warp_time(const std::vector<double> &initial,
                                     const std::vector<double> &target, double time) {
    const auto position = std::lower_bound(initial.begin(), initial.end(), time);
    const size_t index = static_cast<size_t>(position - initial.begin());
    if (index == 0) {
        if (initial[0] != 0.0)
            return time * target[0] / initial[0];
        return target[0] + time - initial[0];
    }
    if (index == initial.size())
        return target.back() + time - initial.back();

    const double amount = (time - initial[index - 1]) / (initial[index] - initial[index - 1]);
    return target[index - 1] + amount * (target[index] - target[index - 1]);
}

// ─────────────────────────────────────
static void loris_dilate_samplerate(t_loris_dilate *x, t_floatarg f) {
    if (std::isfinite(f) && f > 0)
        x->samplerate = f;
}

// ─────────────────────────────────────
static void loris_dilate_resolution(t_loris_dilate *x, t_floatarg f) {
    if (std::isfinite(f) && f > 0)
        x->resolution = f;
}

// ─────────────────────────────────────
static void loris_dilate_process(t_loris_dilate *x, t_symbol *, int argc, t_atom *argv) {
    if (x->state->busy) {
        pd_error(x, "[x.loris.dilate] busy; wait for processing to finish");
        return;
    }

    const bool use_factor = argc == 1 && argv[0].a_type == A_FLOAT;
    if (argc != 0 && !use_factor) {
        pd_error(x, "[x.loris.dilate] use 'dilate <factor>' or 'dilate' with time lists");
        return;
    }
    const double factor = use_factor ? atom_getfloat(argv) : 1.0;
    if (use_factor && (!std::isfinite(factor) || factor <= 0)) {
        pd_error(x, "[x.loris.dilate] invalid factor");
        return;
    }

    if (!std::isfinite(x->samplerate) || x->samplerate <= 0 || !std::isfinite(x->resolution) ||
        x->resolution <= 0) {
        pd_error(x, "[x.loris.dilate] invalid configuration");
        return;
    }

    if (!x->source || !x->destination || x->source == &s_ || x->destination == &s_) {
        pd_error(x, "[x.loris.dilate] missing arrays");
        return;
    }

    if (x->source == x->destination) {
        pd_error(x, "[x.loris.dilate] source and destination must differ");
        return;
    }

    t_garray *src_array = nullptr;
    t_word *src_data = nullptr;
    int src_size = 0;
    if (!loris_get_array(x, x->source, &src_array, &src_size, &src_data))
        return;
    if (src_size <= 0)
        return;

    if (!pd_findbyclass(x->destination, garray_class)) {
        pd_error(x, "[x.loris.dilate] destination '%s' not found", x->destination->s_name);
        return;
    }

    const double duration = static_cast<double>(src_size) / x->samplerate;
    std::vector<double> initial_times;
    std::vector<double> target_times;
    double output_duration = 0.0;
    double output_samples = 0.0;
    if (use_factor) {
        initial_times = {0.0, duration};
        target_times = {0.0, duration * factor};
        output_duration = duration * factor;
        output_samples = std::ceil(static_cast<double>(src_size) * factor);
    } else {
        initial_times = x->state->initial_times;
        target_times = x->state->target_times;
        if (initial_times.empty() || initial_times.size() != target_times.size()) {
            pd_error(x, "[x.loris.dilate] initial and target need the same non-zero length");
            return;
        }
        // Dilator sorts the two sequences independently before matching them.
        std::sort(initial_times.begin(), initial_times.end());
        std::sort(target_times.begin(), target_times.end());
        output_duration = loris_dilate_warp_time(initial_times, target_times, duration);
        output_samples = std::ceil(output_duration * x->samplerate);
    }

    if (!std::isfinite(output_samples) || output_samples < 1.0 ||
        output_samples > static_cast<double>(std::numeric_limits<int>::max())) {
        pd_error(x, "[x.loris.dilate] invalid output size");
        return;
    }

    try {
        auto job = std::make_unique<LorisMorphJob>();
        job->instance = pd_this;
        job->owner = &x->obj.te_g.g_pd;
        job->destination = x->destination;
        job->samplerate = x->samplerate;
        job->resolution = x->resolution;
        job->duration = duration;
        job->output_duration = output_duration;
        job->initial_times = std::move(initial_times);
        job->target_times = std::move(target_times);
        job->input.resize(static_cast<size_t>(src_size));
        job->output.assign(static_cast<size_t>(output_samples), 0.0);
        for (int i = 0; i < src_size; ++i)
            job->input[static_cast<size_t>(i)] = src_data[i].w_float;

        {
            std::lock_guard<std::mutex> lock(x->state->mutex);
            ++x->state->worker_count;
        }
        x->state->busy = true;

        LorisMorphJob *raw_job = job.release();
        try {
            post("[x.loris.dilate] Processing! Wait...");
            std::thread(loris_dilate_worker, raw_job, x->state).detach();
        } catch (...) {
            job.reset(raw_job);
            {
                std::lock_guard<std::mutex> lock(x->state->mutex);
                --x->state->worker_count;
            }
            x->state->busy = false;
            throw;
        }
    } catch (const std::exception &error) {
        pd_error(x, "[x.loris.dilate] %s", error.what());
    } catch (...) {
        pd_error(x, "[x.loris.dilate] could not start processing");
    }
}

// ─────────────────────────────────────
static void loris_dilate_free(t_loris_dilate *x) {
    // Detached workers must finish queueing their result before the Pd object
    // can be released. pd_queue_cancel() then makes pending callbacks receive a
    // null object, while still allowing them to delete their job data.
    {
        std::unique_lock<std::mutex> lock(x->state->mutex);
        x->state->finished.wait(lock, [x] { return x->state->worker_count == 0; });
    }
    pd_queue_cancel(&x->obj.te_g.g_pd);
    delete x->state;
}

// ─────────────────────────────────────
static void *loris_dilate_new(t_symbol *, int argc, t_atom *argv) {
    auto *x = reinterpret_cast<t_loris_dilate *>(pd_new(loris_dilate));
    x->source = &s_;
    x->destination = &s_;
    x->samplerate = sys_getsr();
    x->resolution = 60.0;
    x->state = new LorisDilateState();

    if (x->samplerate <= 0)
        x->samplerate = 48000.0;

    if (argc > 0 && argv[0].a_type == A_SYMBOL)
        x->source = atom_getsymbol(argv);
    if (argc > 1 && argv[1].a_type == A_SYMBOL)
        x->destination = atom_getsymbol(argv + 1);

    return x;
}

// ─────────────────────────────────────
extern "C" void setup_x0x2eloris0x2edilate(void) {
    loris_dilate =
        class_new(gensym("x.loris.dilate"), (t_newmethod)loris_dilate_new,
                  (t_method)loris_dilate_free, sizeof(t_loris_dilate), CLASS_DEFAULT, A_GIMME, 0);

    class_addmethod(loris_dilate, (t_method)loris_dilate_process, gensym("dilate"), A_GIMME, 0);
    class_addmethod(loris_dilate, (t_method)loris_dilate_source, gensym("source"), A_SYMBOL, 0);
    class_addmethod(loris_dilate, (t_method)loris_dilate_destination, gensym("destination"),
                    A_SYMBOL, 0);
    class_addmethod(loris_dilate, (t_method)loris_dilate_initial, gensym("initial"), A_GIMME, 0);
    class_addmethod(loris_dilate, (t_method)loris_dilate_target, gensym("target"), A_GIMME, 0);
    class_addmethod(loris_dilate, (t_method)loris_dilate_samplerate, gensym("samplerate"), A_FLOAT,
                    0);
    class_addmethod(loris_dilate, (t_method)loris_dilate_resolution, gensym("resolution"), A_FLOAT,
                    0);
}
