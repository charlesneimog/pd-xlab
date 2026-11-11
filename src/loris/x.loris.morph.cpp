#include "m_pd.h"

#include <Analyzer.h>
#include <LinearEnvelope.h>
#include <Morpher.h>
#include <PartialList.h>
#include <PartialUtils.h>
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

static t_class *loris_morph_class;

// -----------------------------------------------------------------------------
struct LorisMorphState {
    std::mutex mutex;
    std::condition_variable finished;
    size_t worker_count{0};
    bool busy{false};
};

struct LorisMorphJob {
    t_pdinstance *instance{nullptr};
    t_pd *owner{nullptr};
    t_symbol *destination{&s_};
    double samplerate{0.0};
    double resolution{0.0};
    double frequency_amount{0.5};
    double amplitude_amount{0.5};
    double bandwidth_amount{0.5};
    std::vector<double> source;
    std::vector<double> target;
    std::vector<double> output;
    char error[256]{};
};

typedef struct _loris_morph {
    t_object obj;
    t_symbol *source;
    t_symbol *target;
    t_symbol *destination;
    t_float samplerate;
    t_float resolution;
    LorisMorphState *state;
} t_loris_morph;

// -----------------------------------------------------------------------------
// Pair the analyzed partials from low to high average frequency. Loris uses
// equal, non-zero labels as correspondences; unpaired partials retain label 0
// and are crossfaded by Morpher.
static void loris_morph_label_correspondences(Loris::PartialList &source,
                                              Loris::PartialList &target) {
    const auto frequency_less = [](const Loris::Partial &a, const Loris::Partial &b) {
        return Loris::PartialUtils::weightedAvgFrequency(a) <
               Loris::PartialUtils::weightedAvgFrequency(b);
    };

    source.sort(frequency_less);
    target.sort(frequency_less);

    auto source_it = source.begin();
    auto target_it = target.begin();
    Loris::Partial::label_type label = 1;
    while (source_it != source.end() && target_it != target.end()) {
        source_it->setLabel(label);
        target_it->setLabel(label);
        ++source_it;
        ++target_it;
        ++label;
    }

    while (source_it != source.end()) {
        source_it->setLabel(0);
        ++source_it;
    }
    while (target_it != target.end()) {
        target_it->setLabel(0);
        ++target_it;
    }
}

// Called by Pd on the scheduler thread. Array access and Pd logging are kept
// here and never performed by the worker thread.
static void loris_morph_deliver(t_pd *object, void *data) {
    std::unique_ptr<LorisMorphJob> job(static_cast<LorisMorphJob *>(data));
    if (!object)
        return;

    auto *x = reinterpret_cast<t_loris_morph *>(object);
    x->state->busy = false;

    if (job->error[0]) {
        pd_error(x, "[x.loris.morph] %s", job->error);
        return;
    }

    auto *array = reinterpret_cast<t_garray *>(pd_findbyclass(job->destination, garray_class));
    if (!array) {
        pd_error(x, "[x.loris.morph] destination '%s' not found", job->destination->s_name);
        return;
    }

    garray_resize_long(array, static_cast<long>(job->output.size()));

    int size = 0;
    t_word *samples = nullptr;
    if (!garray_getfloatwords(array, &size, &samples)) {
        pd_error(x, "[x.loris.morph] invalid destination '%s'", job->destination->s_name);
        return;
    }

    const int count = std::min(size, static_cast<int>(job->output.size()));
    for (int i = 0; i < count; ++i)
        samples[i].w_float = static_cast<t_float>(job->output[static_cast<size_t>(i)]);

    garray_redraw(array);
    post("[x.loris.morph] finished (frequency %.3f, amplitude %.3f, bandwidth %.3f)",
         job->frequency_amount, job->amplitude_amount, job->bandwidth_amount);
}

// -----------------------------------------------------------------------------
// The worker owns all input/output sample storage and does not touch the Pd API
// except for queueing its completed result back to the scheduler thread.
static void loris_morph_worker(LorisMorphJob *job, LorisMorphState *state) {
    try {
        Loris::Analyzer source_analyzer(job->resolution, 2.0 * job->resolution);
        Loris::Analyzer target_analyzer(job->resolution, 2.0 * job->resolution);
        Loris::PartialList source_partials = source_analyzer.analyze(job->source, job->samplerate);
        Loris::PartialList target_partials = target_analyzer.analyze(job->target, job->samplerate);

        loris_morph_label_correspondences(source_partials, target_partials);

        Loris::LinearEnvelope frequency_envelope(job->frequency_amount);
        Loris::LinearEnvelope amplitude_envelope(job->amplitude_amount);
        Loris::LinearEnvelope bandwidth_envelope(job->bandwidth_amount);
        Loris::Morpher morpher(frequency_envelope, amplitude_envelope, bandwidth_envelope);
        morpher.morph(source_partials.begin(), source_partials.end(), target_partials.begin(),
                      target_partials.end());

        if (!morpher.partials().empty()) {
            const size_t requested_size = job->output.size();
            {
                Loris::Synthesizer synthesizer(job->samplerate, job->output);
                synthesizer.synthesize(morpher.partials().begin(), morpher.partials().end());
            }
            // Synthesizer may append its final fade; the Pd array has the
            // duration of the longer input.
            job->output.resize(requested_size);
        }
    } catch (const std::exception &error) {
        std::snprintf(job->error, sizeof(job->error), "%s", error.what());
    } catch (...) {
        std::snprintf(job->error, sizeof(job->error), "Loris processing failed");
    }

    pd_queue_mess(job->instance, job->owner, job, loris_morph_deliver);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        --state->worker_count;
        state->finished.notify_all();
    }
}

// -----------------------------------------------------------------------------
static int loris_morph_get_array(t_loris_morph *x, t_symbol *name, int *size, t_word **samples) {
    auto *array = reinterpret_cast<t_garray *>(pd_findbyclass(name, garray_class));
    if (!array) {
        pd_error(x, "[x.loris.morph] array '%s' not found", name->s_name);
        return 0;
    }
    if (!garray_getfloatwords(array, size, samples)) {
        pd_error(x, "[x.loris.morph] invalid array '%s'", name->s_name);
        return 0;
    }
    return 1;
}

static bool loris_morph_read_amount(t_loris_morph *x, const t_atom &atom, double &amount) {
    if (atom.a_type != A_FLOAT) {
        pd_error(x, "[x.loris.morph] morph amounts must be numbers");
        return false;
    }

    amount = atom_getfloat(&atom);
    if (!std::isfinite(amount) || amount < 0.0 || amount > 1.0) {
        pd_error(x, "[x.loris.morph] morph amounts must be between 0 and 1");
        return false;
    }
    return true;
}

static void loris_morph_process(t_loris_morph *x, t_symbol *, int argc, t_atom *argv) {
    if (x->state->busy) {
        pd_error(x, "[x.loris.morph] busy; wait for processing to finish");
        return;
    }

    if (argc != 1 && argc != 3) {
        pd_error(x, "[x.loris.morph] use 'morph <amount>' or "
                    "'morph <frequency> <amplitude> <bandwidth>'");
        return;
    }

    double frequency_amount = 0.0;
    double amplitude_amount = 0.0;
    double bandwidth_amount = 0.0;
    if (!loris_morph_read_amount(x, argv[0], frequency_amount))
        return;
    if (argc == 1) {
        amplitude_amount = frequency_amount;
        bandwidth_amount = frequency_amount;
    } else if (!loris_morph_read_amount(x, argv[1], amplitude_amount) ||
               !loris_morph_read_amount(x, argv[2], bandwidth_amount)) {
        return;
    }

    if (!std::isfinite(x->samplerate) || x->samplerate <= 0 || !std::isfinite(x->resolution) ||
        x->resolution <= 0) {
        pd_error(x, "[x.loris.morph] invalid configuration");
        return;
    }
    if (!x->source || !x->target || !x->destination || x->source == &s_ || x->target == &s_ ||
        x->destination == &s_) {
        pd_error(x, "[x.loris.morph] missing source, target, or destination array");
        return;
    }

    int source_size = 0;
    int target_size = 0;
    t_word *source_samples = nullptr;
    t_word *target_samples = nullptr;
    if (!loris_morph_get_array(x, x->source, &source_size, &source_samples) ||
        !loris_morph_get_array(x, x->target, &target_size, &target_samples)) {
        return;
    }
    if (source_size <= 0 || target_size <= 0) {
        pd_error(x, "[x.loris.morph] source and target arrays must not be empty");
        return;
    }
    if (!pd_findbyclass(x->destination, garray_class)) {
        pd_error(x, "[x.loris.morph] destination '%s' not found", x->destination->s_name);
        return;
    }

    const size_t output_size = static_cast<size_t>(std::max(source_size, target_size));
    if (output_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
        pd_error(x, "[x.loris.morph] invalid output size");
        return;
    }

    try {
        auto job = std::make_unique<LorisMorphJob>();
        job->instance = pd_this;
        job->owner = &x->obj.te_g.g_pd;
        job->destination = x->destination;
        job->samplerate = x->samplerate;
        job->resolution = x->resolution;
        job->frequency_amount = frequency_amount;
        job->amplitude_amount = amplitude_amount;
        job->bandwidth_amount = bandwidth_amount;
        job->source.resize(static_cast<size_t>(source_size));
        job->target.resize(static_cast<size_t>(target_size));
        job->output.assign(output_size, 0.0);

        for (int i = 0; i < source_size; ++i)
            job->source[static_cast<size_t>(i)] = source_samples[i].w_float;
        for (int i = 0; i < target_size; ++i)
            job->target[static_cast<size_t>(i)] = target_samples[i].w_float;

        {
            std::lock_guard<std::mutex> lock(x->state->mutex);
            ++x->state->worker_count;
        }
        x->state->busy = true;

        LorisMorphJob *raw_job = job.release();
        try {
            post("[x.loris.morph] processing...");
            std::thread(loris_morph_worker, raw_job, x->state).detach();
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
        pd_error(x, "[x.loris.morph] %s", error.what());
    } catch (...) {
        pd_error(x, "[x.loris.morph] could not start processing");
    }
}

// -----------------------------------------------------------------------------
static void loris_morph_source(t_loris_morph *x, t_symbol *name) { x->source = name; }

static void loris_morph_target(t_loris_morph *x, t_symbol *name) { x->target = name; }

static void loris_morph_destination(t_loris_morph *x, t_symbol *name) { x->destination = name; }

static void loris_morph_samplerate(t_loris_morph *x, t_floatarg value) {
    if (std::isfinite(value) && value > 0)
        x->samplerate = value;
    else
        pd_error(x, "[x.loris.morph] samplerate must be positive");
}

static void loris_morph_resolution(t_loris_morph *x, t_floatarg value) {
    if (std::isfinite(value) && value > 0)
        x->resolution = value;
    else
        pd_error(x, "[x.loris.morph] resolution must be positive");
}

static void loris_morph_free(t_loris_morph *x) {
    // A detached worker must finish queueing before Pd can release the owner.
    // Canceling afterward makes a pending callback receive a null owner while
    // still allowing the callback to destroy its job data.
    {
        std::unique_lock<std::mutex> lock(x->state->mutex);
        x->state->finished.wait(lock, [x] { return x->state->worker_count == 0; });
    }
    pd_queue_cancel(&x->obj.te_g.g_pd);
    delete x->state;
}

static void *loris_morph_new(t_symbol *, int argc, t_atom *argv) {
    auto *x = reinterpret_cast<t_loris_morph *>(pd_new(loris_morph_class));
    x->source = &s_;
    x->target = &s_;
    x->destination = &s_;
    x->samplerate = sys_getsr();
    x->resolution = 60.0;

    try {
        x->state = new LorisMorphState();
    } catch (...) {
        pd_error(x, "[x.loris.morph] could not allocate state");
        return nullptr;
    }

    if (x->samplerate <= 0)
        x->samplerate = 48000.0;
    if (argc > 0 && argv[0].a_type == A_SYMBOL)
        x->source = atom_getsymbol(argv);
    if (argc > 1 && argv[1].a_type == A_SYMBOL)
        x->target = atom_getsymbol(argv + 1);
    if (argc > 2 && argv[2].a_type == A_SYMBOL)
        x->destination = atom_getsymbol(argv + 2);

    return x;
}

// -----------------------------------------------------------------------------
extern "C" void setup_x0x2eloris0x2emorph(void) {
    loris_morph_class =
        class_new(gensym("x.loris.morph"), (t_newmethod)loris_morph_new, (t_method)loris_morph_free,
                  sizeof(t_loris_morph), CLASS_DEFAULT, A_GIMME, 0);

    class_addmethod(loris_morph_class, (t_method)loris_morph_process, gensym("morph"), A_GIMME, 0);
    class_addmethod(loris_morph_class, (t_method)loris_morph_source, gensym("source"), A_SYMBOL, 0);
    class_addmethod(loris_morph_class, (t_method)loris_morph_target, gensym("target"), A_SYMBOL, 0);
    class_addmethod(loris_morph_class, (t_method)loris_morph_destination, gensym("destination"),
                    A_SYMBOL, 0);
    class_addmethod(loris_morph_class, (t_method)loris_morph_samplerate, gensym("samplerate"),
                    A_FLOAT, 0);
    class_addmethod(loris_morph_class, (t_method)loris_morph_resolution, gensym("resolution"),
                    A_FLOAT, 0);
}
