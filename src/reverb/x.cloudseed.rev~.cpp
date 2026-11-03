// CloudSeedCore DSP by Ghost Note Audio (MIT); Pd integration by pd-xlab.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <memory>

#include "DSP/ReverbController.h"
#include "Programs.h"
#include <m_pd.h>

static t_class *cloudseed_class;

// Names and indices match CloudSeedCore's Parameter namespace. All values use
// its normalized [0, 1] scale; ScaleParam supplies the physical units.
static const char *const cloudseed_parameter_names[] = {"Interpolation",
                                                        "LowCutEnabled",
                                                        "HighCutEnabled",
                                                        "InputMix",
                                                        "LowCut",
                                                        "HighCut",
                                                        "DryOut",
                                                        "EarlyOut",
                                                        "LateOut",
                                                        "TapEnabled",
                                                        "TapCount",
                                                        "TapDecay",
                                                        "TapPredelay",
                                                        "TapLength",
                                                        "EarlyDiffuseEnabled",
                                                        "EarlyDiffuseCount",
                                                        "EarlyDiffuseDelay",
                                                        "EarlyDiffuseModAmount",
                                                        "EarlyDiffuseFeedback",
                                                        "EarlyDiffuseModRate",
                                                        "LateMode",
                                                        "LateLineCount",
                                                        "LateDiffuseEnabled",
                                                        "LateDiffuseCount",
                                                        "LateLineSize",
                                                        "LateLineModAmount",
                                                        "LateDiffuseDelay",
                                                        "LateDiffuseModAmount",
                                                        "LateLineDecay",
                                                        "LateLineModRate",
                                                        "LateDiffuseFeedback",
                                                        "LateDiffuseModRate",
                                                        "EqLowShelfEnabled",
                                                        "EqHighShelfEnabled",
                                                        "EqLowpassEnabled",
                                                        "EqLowFreq",
                                                        "EqHighFreq",
                                                        "EqCutoff",
                                                        "EqLowGain",
                                                        "EqHighGain",
                                                        "EqCrossSeed",
                                                        "SeedTap",
                                                        "SeedDiffusion",
                                                        "SeedDelay",
                                                        "SeedPostDiffusion"};
static_assert(sizeof(cloudseed_parameter_names) / sizeof(*cloudseed_parameter_names) ==
                  Cloudseed::Parameter::COUNT,
              "CloudSeed parameter names must match the upstream indices");

struct CloudseedState {
    Cloudseed::ReverbController reverb;
    float mix = 1;
    bool ready = true;
    float input[2][BUFFER_SIZE];
    float output[2][BUFFER_SIZE];

    explicit CloudseedState(int samplerate) : reverb(samplerate) { reset(); }

    void reset() {
        for (int i = 0; i < Cloudseed::Parameter::COUNT; ++i)
            reverb.SetParameter(i, Cloudseed::ProgramDarkPlate[i]);
        // The Pd object starts wet only, so it can also be used on an aux bus.
        reverb.SetParameter(Cloudseed::Parameter::DryOut, 0);
        reverb.ClearBuffers();
        mix = 1;
    }
};

typedef struct _cloudseed_tilde {
    t_object x_obj;
    t_float f;
    CloudseedState *state;
} t_cloudseed_tilde;

static bool cloudseed_valid_samplerate(double samplerate) {
    return std::isfinite(samplerate) && samplerate >= 8000 && samplerate <= 192000;
}

static int cloudseed_parameter_index(const char *name) {
    for (int i = 0; i < Cloudseed::Parameter::COUNT; ++i)
        if (!std::strcmp(name, cloudseed_parameter_names[i]))
            return i;
    return -1;
}

static bool cloudseed_normalized(t_cloudseed_tilde *x, const t_atom &atom, float &value) {
    if (atom.a_type != A_FLOAT || !std::isfinite(atom_getfloat(&atom))) {
        pd_error(x, "x.cloudseed.rev~: expected a finite numeric value");
        return false;
    }
    value = std::max(0.f, std::min(1.f, static_cast<float>(atom_getfloat(&atom))));
    return true;
}

static void cloudseed_set(t_cloudseed_tilde *x, t_symbol *s, int argc, t_atom *argv) {
    int index = cloudseed_parameter_index(s->s_name);
    if (index < 0) {
        pd_error(x, "x.cloudseed.rev~: unknown parameter '%s' (send 'parameters' for names)",
                 s->s_name);
        return;
    }
    float value;
    if (argc != 1) {
        pd_error(x, "x.cloudseed.rev~: %s expects one normalized value (0..1)", s->s_name);
        return;
    }
    if (cloudseed_normalized(x, argv[0], value))
        x->state->reverb.SetParameter(index, value);
}

static void cloudseed_param(t_cloudseed_tilde *x, t_symbol *, int argc, t_atom *argv) {
    if (argc != 2) {
        pd_error(x, "x.cloudseed.rev~: param <name or index 0..44> <value 0..1>");
        return;
    }
    int index = -1;
    if (argv[0].a_type == A_SYMBOL)
        index = cloudseed_parameter_index(atom_getsymbol(argv)->s_name);
    else if (argv[0].a_type == A_FLOAT) {
        double candidate = atom_getfloat(argv);
        if (std::isfinite(candidate) && candidate >= 0 && candidate < Cloudseed::Parameter::COUNT &&
            candidate == std::floor(candidate))
            index = static_cast<int>(candidate);
    }
    if (index < 0) {
        pd_error(x, "x.cloudseed.rev~: invalid parameter name or index (send 'parameters')");
        return;
    }
    float value;
    if (cloudseed_normalized(x, argv[1], value))
        x->state->reverb.SetParameter(index, value);
}

static void cloudseed_mix(t_cloudseed_tilde *x, t_floatarg value) {
    if (!std::isfinite(value)) {
        pd_error(x, "x.cloudseed.rev~: mix expects a finite value (0..1)");
        return;
    }
    x->state->mix = std::max(0.f, std::min(1.f, static_cast<float>(value)));
}

static void cloudseed_clear(t_cloudseed_tilde *x) { x->state->reverb.ClearBuffers(); }

static void cloudseed_reset(t_cloudseed_tilde *x) { x->state->reset(); }

static void cloudseed_parameters(t_cloudseed_tilde *x) {
    auto values = x->state->reverb.GetAllParameters();
    post("x.cloudseed.rev~: parameter index / name / normalized value / scaled value");
    for (int i = 0; i < Cloudseed::Parameter::COUNT; ++i)
        post("  %d %s: %g (%g)", i, cloudseed_parameter_names[i], values[i],
             Cloudseed::ScaleParam(values[i], i));
}

static t_int *cloudseed_perform(t_int *w) {
    auto *x = reinterpret_cast<t_cloudseed_tilde *>(w[1]);
    auto *inL = reinterpret_cast<t_sample *>(w[2]);
    auto *inR = reinterpret_cast<t_sample *>(w[3]);
    auto *outL = reinterpret_cast<t_sample *>(w[4]);
    auto *outR = reinterpret_cast<t_sample *>(w[5]);
    int count = static_cast<int>(w[6]);
    auto &state = *x->state;
    if (!state.ready) {
        std::fill(outL, outL + count, 0);
        std::fill(outR, outR + count, 0);
        return w + 7;
    }

    // Bounded scratch storage supports arbitrary Pd block sizes and both Pd
    // sample precisions. Copy both inputs before writing potentially aliased
    // output vectors; CloudSeedCore itself always processes 32-bit floats.
    for (int offset = 0; offset < count; offset += BUFFER_SIZE) {
        int n = std::min(BUFFER_SIZE, count - offset);
        for (int i = 0; i < n; ++i) {
            state.input[0][i] = static_cast<float>(inL[offset + i]);
            state.input[1][i] = static_cast<float>(inR[offset + i]);
        }
        state.reverb.Process(state.input[0], state.input[1], state.output[0], state.output[1], n);
        for (int i = 0; i < n; ++i) {
            // Preserve the original Pd precision for a completely dry signal.
            t_sample dryL = inL[offset + i];
            t_sample dryR = inR[offset + i];
            outL[offset + i] = state.mix * state.output[0][i] + (1 - state.mix) * dryL;
            outR[offset + i] = state.mix * state.output[1][i] + (1 - state.mix) * dryR;
        }
    }
    return w + 7;
}

static void cloudseed_dsp(t_cloudseed_tilde *x, t_signal **sp) {
    double samplerate = sp[0]->s_sr;
    x->state->ready = cloudseed_valid_samplerate(samplerate);
    if (!x->state->ready)
        pd_error(x, "x.cloudseed.rev~: supported sample rates are 8000..192000 Hz");
    else if (static_cast<int>(samplerate) != x->state->reverb.GetSamplerate())
        x->state->reverb.SetSamplerate(static_cast<int>(samplerate));

    dsp_add(cloudseed_perform, 6, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, sp[3]->s_vec,
            sp[0]->s_n);
}

static void cloudseed_free(t_cloudseed_tilde *x) { delete x->state; }

static void *cloudseed_new(t_symbol *, int argc, t_atom *argv) {
    if (argc > 1 ||
        (argc == 1 && (argv[0].a_type != A_FLOAT || !std::isfinite(atom_getfloat(argv))))) {
        pd_error(nullptr, "x.cloudseed.rev~: optional creation argument is mix (0..1)");
        return nullptr;
    }
    double samplerate = sys_getsr();
    if (!cloudseed_valid_samplerate(samplerate))
        samplerate = 48000;

    std::unique_ptr<CloudseedState> state;
    try {
        state.reset(new CloudseedState(static_cast<int>(samplerate)));
    } catch (const std::exception &error) {
        pd_error(nullptr, "x.cloudseed.rev~: cannot create reverb: %s", error.what());
        return nullptr;
    }
    auto *x = reinterpret_cast<t_cloudseed_tilde *>(pd_new(cloudseed_class));
    x->state = state.release();
    x->f = 0;
    if (argc == 1)
        cloudseed_mix(x, atom_getfloat(argv));
    signalinlet_new(&x->x_obj, 0);
    outlet_new(&x->x_obj, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return x;
}

extern "C" void setup_x0x2ecloudseed0x2erev_tilde(void) {
    Cloudseed::initPrograms();
    cloudseed_class =
        class_new(gensym("x.cloudseed.rev~"), (t_newmethod)cloudseed_new, (t_method)cloudseed_free,
                  sizeof(t_cloudseed_tilde), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(cloudseed_class, t_cloudseed_tilde, f);
    class_addmethod(cloudseed_class, (t_method)cloudseed_dsp, gensym("dsp"), A_CANT, 0);
    class_addmethod(cloudseed_class, (t_method)cloudseed_param, gensym("param"), A_GIMME, 0);
    class_addmethod(cloudseed_class, (t_method)cloudseed_mix, gensym("mix"), A_FLOAT, 0);
    class_addmethod(cloudseed_class, (t_method)cloudseed_clear, gensym("clear"), A_NULL, 0);
    class_addmethod(cloudseed_class, (t_method)cloudseed_reset, gensym("reset"), A_NULL, 0);
    class_addmethod(cloudseed_class, (t_method)cloudseed_parameters, gensym("parameters"), A_NULL,
                    0);
    class_addanything(cloudseed_class, (t_method)cloudseed_set);
}
