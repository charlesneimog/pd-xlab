#include <m_pd.h>

#include <cmath>

#include <freeverb/earlyref.hpp>
#include <freeverb/efilter.hpp>
#include <freeverb/progenitor2.hpp>

static t_class *dragonfly_tilde_class;

class dragonfly_tilde {
  public:
    enum Algo : int {
        ALGO_EARLY = 0,
        ALGO_ROOM = 1,
    };

    t_object x_obj;
    t_sample f;
    t_outlet *x_out;

    Algo algo;

    // early reflections (used by both modes)
    fv3::earlyref_f *early;

    // room mode late reverb + input filters
    fv3::progenitor2_f *late;
    fv3::iir_1st_f *input_lpf_0;
    fv3::iir_1st_f *input_lpf_1;
    fv3::iir_1st_f *input_hpf_0;
    fv3::iir_1st_f *input_hpf_1;

    float room_early_send;
    float room_early_level;
    float room_late_level;

    float dry = 0.0f;
    float wet = 1.0f;
    double samplerate;
    int buffersize = 0;
    float *inbufL;
    float *inbufR;
    float *outbufL;
    float *outbufR;

    // room mode temp buffers
    float *lateInL;
    float *lateInR;
    float *lateOutL;
    float *lateOutR;
};

// ─────────────────────────────────────
static void dragonfly_tilde_config(dragonfly_tilde *x, t_symbol *s, int argc, t_atom *argv) {
    const char *method = s->s_name;
    if (strcmp(method, "dry") == 0) {
        float value = atom_getfloat(argv);
        if (value < 0.f) {
            value = 0.f;
        }
        if (value > 100.f) {
            value = 100.f;
        }
        x->dry = value / 100.f;
    } else if (strcmp(method, "wet") == 0) {
        float value = atom_getfloat(argv);
        if (value < 0.f) {
            value = 0.f;
        }
        if (value > 100.f) {
            value = 100.f;
        }
        x->wet = value / 100.f;
    } else if (strcmp(method, "width") == 0) {
        float value = atom_getfloat(argv);
        if (value < 0.f) {
            value = 0.f;
        }
        if (value > 100.f) {
            value = 100.f;
        }
        x->early->setwidth(value / 100.f);
    } else if (strcmp(method, "lowcut") == 0) {
        float value = atom_getfloat(argv);

        // 0 – 200 Hz
        if (value < 0.f) {
            value = 0.f;
        }
        if (value > 200.f) {
            value = 200.f;
        }

        x->early->setoutputhpf(value);

    } else if (strcmp(method, "highcut") == 0) {
        float value = atom_getfloat(argv);
        if (value < 1000.f) {
            value = 1000.f;
        }
        if (value > 10000.f) {
            value = 10000.f;
        }

        x->early->setoutputlpf(value);
    } else if (strcmp(method, "size") == 0) {
        float value = atom_getfloat(argv);
        if (value < 10.f) {
            value = 10.f;
        }
        if (value > 60.f) {
            value = 60.f;
        }
        x->early->setRSFactor(value / 10.f);
    } else if (strcmp(method, "program") == 0) {
        unsigned program = atom_getfloat(argv);
        x->early->loadPresetReflection(program);
    }

    return;
}

// ─────────────────────────────────────
static void dragonfly_tilde_free_buffers(dragonfly_tilde *x) {
    if (x->inbufL) {
        freebytes(x->inbufL, x->buffersize * (int)sizeof(float));
    }
    if (x->inbufR) {
        freebytes(x->inbufR, x->buffersize * (int)sizeof(float));
    }
    if (x->outbufL) {
        freebytes(x->outbufL, x->buffersize * (int)sizeof(float));
    }
    if (x->outbufR) {
        freebytes(x->outbufR, x->buffersize * (int)sizeof(float));
    }
    if (x->lateInL) {
        freebytes(x->lateInL, x->buffersize * (int)sizeof(float));
    }
    if (x->lateInR) {
        freebytes(x->lateInR, x->buffersize * (int)sizeof(float));
    }
    if (x->lateOutL) {
        freebytes(x->lateOutL, x->buffersize * (int)sizeof(float));
    }
    if (x->lateOutR) {
        freebytes(x->lateOutR, x->buffersize * (int)sizeof(float));
    }

    x->inbufL = nullptr;
    x->inbufR = nullptr;
    x->outbufL = nullptr;
    x->outbufR = nullptr;
    x->lateInL = nullptr;
    x->lateInR = nullptr;
    x->lateOutL = nullptr;
    x->lateOutR = nullptr;
    x->buffersize = 0;
}

// ─────────────────────────────────────
static void dragonfly_tilde_ensure_buffers(dragonfly_tilde *x, int n) {
    if (n <= 0) {
        return;
    }

    if (x->buffersize == n && x->inbufL && x->inbufR && x->outbufL && x->outbufR) {
        // room mode needs extra buffers too
        if (x->algo != dragonfly_tilde::ALGO_ROOM ||
            (x->lateInL && x->lateInR && x->lateOutL && x->lateOutR)) {
            return;
        }
    }

    if (x->buffersize != 0) {
        dragonfly_tilde_free_buffers(x);
    }

    x->inbufL = (float *)getbytes(n * (int)sizeof(float));
    x->inbufR = (float *)getbytes(n * (int)sizeof(float));
    x->outbufL = (float *)getbytes(n * (int)sizeof(float));
    x->outbufR = (float *)getbytes(n * (int)sizeof(float));

    if (x->algo == dragonfly_tilde::ALGO_ROOM) {
        x->lateInL = (float *)getbytes(n * (int)sizeof(float));
        x->lateInR = (float *)getbytes(n * (int)sizeof(float));
        x->lateOutL = (float *)getbytes(n * (int)sizeof(float));
        x->lateOutR = (float *)getbytes(n * (int)sizeof(float));
    }

    x->buffersize = n;
}

// ─────────────────────────────────────
static void dragonfly_tilde_room_setInputLPF(dragonfly_tilde *x, float freq) {
    if (!x->input_lpf_0 || !x->input_lpf_1) {
        return;
    }
    if (freq < 0) {
        freq = 0;
    } else if (x->samplerate > 0 && freq > (float)(x->samplerate / 2.0)) {
        freq = (float)(x->samplerate / 2.0);
    }
    x->input_lpf_0->setLPF_BW(freq, (float)x->samplerate);
    x->input_lpf_1->setLPF_BW(freq, (float)x->samplerate);
}

// ─────────────────────────────────────
static void dragonfly_tilde_room_setInputHPF(dragonfly_tilde *x, float freq) {
    if (!x->input_hpf_0 || !x->input_hpf_1) {
        return;
    }
    if (freq < 0) {
        freq = 0;
    } else if (x->samplerate > 0 && freq > (float)(x->samplerate / 2.0)) {
        freq = (float)(x->samplerate / 2.0);
    }
    x->input_hpf_0->setHPF_BW(freq, (float)x->samplerate);
    x->input_hpf_1->setHPF_BW(freq, (float)x->samplerate);
}

// ─────────────────────────────────────
static t_int *dragonfly_tilde_perform_early(t_int *w) {
    dragonfly_tilde *x = (dragonfly_tilde *)(w[1]);
    t_sample *in = (t_sample *)(w[2]);
    t_sample *out = (t_sample *)(w[3]);
    int n = (int)(w[4]);

    // Pd calls the perform routine with exactly one block of size n.
    if (!x->early || !x->inbufL || !x->inbufR || !x->outbufL || !x->outbufR) {
        for (int i = 0; i < n; ++i) {
            out[i] = in[i];
        }
        return (w + 5);
    }

    for (int i = 0; i < n; ++i) {
        float s = (float)in[i];
        x->inbufL[i] = s;
        x->inbufR[i] = s;
    }

    x->early->processreplace(x->inbufL, x->inbufR, x->outbufL, x->outbufR, n);

    // stereo → mono mix
    for (int i = 0; i < n; ++i) {
        float wetmono = 0.5f * (x->outbufL[i] + x->outbufR[i]);
        out[i] = (t_sample)(x->dry * (float)in[i] + x->wet * wetmono);
    }

    return (w + 5);
}

// ─────────────────────────────────────
static t_int *dragonfly_tilde_perform_room(t_int *w) {
    dragonfly_tilde *x = (dragonfly_tilde *)(w[1]);
    t_sample *in = (t_sample *)(w[2]);
    t_sample *out = (t_sample *)(w[3]);
    int n = (int)(w[4]);

    if (!x->early || !x->late || !x->input_lpf_0 || !x->input_lpf_1 || !x->input_hpf_0 ||
        !x->input_hpf_1 || !x->inbufL || !x->inbufR || !x->outbufL || !x->outbufR || !x->lateInL ||
        !x->lateInR || !x->lateOutL || !x->lateOutR) {
        for (int i = 0; i < n; ++i) {
            out[i] = in[i];
        }
        return (w + 5);
    }

    // mono -> stereo, with input filtering
    for (int i = 0; i < n; ++i) {
        float s = (float)in[i];
        x->inbufL[i] = x->input_lpf_0->process(x->input_hpf_0->process(s));
        x->inbufR[i] = x->input_lpf_1->process(x->input_hpf_1->process(s));
    }

    // early reflections
    x->early->processreplace(x->inbufL, x->inbufR, x->outbufL, x->outbufR, n);

    // late input (early send + dry feed)
    for (int i = 0; i < n; ++i) {
        x->lateInL[i] = x->room_early_send * x->outbufL[i] + x->inbufL[i];
        x->lateInR[i] = x->room_early_send * x->outbufR[i] + x->inbufR[i];
    }

    x->late->processreplace(x->lateInL, x->lateInR, x->lateOutL, x->lateOutR, n);

    for (int i = 0; i < n; ++i) {
        // build a wet stereo signal similar to the original plugin, then mix down to mono
        float wetL = x->room_early_level * x->outbufL[i] + x->room_late_level * x->lateOutL[i];
        float wetR = x->room_early_level * x->outbufR[i] + x->room_late_level * x->lateOutR[i];
        float wetmono = 0.5f * (wetL + wetR);
        out[i] = (t_sample)(x->dry * (float)in[i] + x->wet * wetmono);
    }

    return (w + 5);
}

// ─────────────────────────────────────
static void dragonfly_tilde_dsp(dragonfly_tilde *x, t_signal **sp) {
    x->samplerate = sp[0]->s_sr;

    const int n = sp[0]->s_n;
    if (n <= 0) {
        return;
    }

    dragonfly_tilde_ensure_buffers(x, n);

    if (x->early) {
        x->early->setSampleRate(x->samplerate);
    }
    if (x->late) {
        x->late->setSampleRate(x->samplerate);
    }

    // Make sure filters are configured at the current samplerate
    if (x->algo == dragonfly_tilde::ALGO_ROOM) {
        // defaults from Dragonfly Room Reverb "Medium Clear Room" preset
        dragonfly_tilde_room_setInputLPF(x, 16000.0f);
        dragonfly_tilde_room_setInputHPF(x, 4.0f);
    }

    // switch-case as requested
    switch (x->algo) {
    case dragonfly_tilde::ALGO_ROOM:
        dsp_add(dragonfly_tilde_perform_room, 4, (t_int)x, (t_int)sp[0]->s_vec, (t_int)sp[1]->s_vec,
                (t_int)sp[0]->s_n);
        break;
    case dragonfly_tilde::ALGO_EARLY:
    default:
        dsp_add(dragonfly_tilde_perform_early, 4, (t_int)x, (t_int)sp[0]->s_vec,
                (t_int)sp[1]->s_vec, (t_int)sp[0]->s_n);
        break;
    }
}

// ─────────────────────────────────────
static void *dragonfly_tilde_new(t_symbol *, int argc, t_atom *argv) {
    dragonfly_tilde *x = (dragonfly_tilde *)pd_new(dragonfly_tilde_class);

    outlet_new(&x->x_obj, &s_signal);

    // pd_new() does not run C++ constructors/member initializers.
    // Initialize all state explicitly to avoid undefined values (often causing silence).
    x->dry = 0.0f;
    x->wet = 1.0f;
    x->samplerate = sys_getsr();
    x->buffersize = 0;
    x->inbufL = nullptr;
    x->inbufR = nullptr;
    x->outbufL = nullptr;
    x->outbufR = nullptr;

    x->lateInL = nullptr;
    x->lateInR = nullptr;
    x->lateOutL = nullptr;
    x->lateOutR = nullptr;

    x->early = nullptr;
    x->late = nullptr;
    x->input_lpf_0 = nullptr;
    x->input_lpf_1 = nullptr;
    x->input_hpf_0 = nullptr;
    x->input_hpf_1 = nullptr;

    x->room_early_send = 0.20f;
    x->room_early_level = 1.0f;
    x->room_late_level = 1.0f;

    x->algo = dragonfly_tilde::ALGO_EARLY;

    if (argc > 0 && argv && argv[0].a_type == A_SYMBOL) {
        t_symbol *sym = atom_getsymbol(&argv[0]);
        if (sym == gensym("early")) {
            x->algo = dragonfly_tilde::ALGO_EARLY;
        } else if (sym == gensym("room")) {
            x->algo = dragonfly_tilde::ALGO_ROOM;
        } else {
            pd_error(x, "x.dragonfly~: unknown mode '%s' (use 'early' or 'room')", sym->s_name);
        }
    }

    // early reflections instance (used by both modes)
    x->early = new fv3::earlyref_f();

    x->early->setMuteOnChange(false);

    // Match Dragonfly's early-reflections defaults loosely.
    x->early->setdryr(0.1f);
    x->early->setwet(0.5f);
    x->early->setwidth(0.8f);
    x->early->setLRDelay(0.3f);
    x->early->setLRCrossApFreq(750.0f, 4);
    x->early->setDiffusionApFreq(150.0f, 4);

    if (x->algo == dragonfly_tilde::ALGO_ROOM) {
        // Configure room algorithm defaults similar to Dragonfly Room Reverb.
        x->early->loadPresetReflection(FV3_EARLYREF_PRESET_1);
        x->early->setdryr(0.0f);
        x->early->setwet(0.0f);

        x->late = new fv3::progenitor2_f();
        x->late->setMuteOnChange(false);
        x->late->setwet(0.0f);
        x->late->setdryr(0.0f);
        x->late->setwidth(1.0f);

        x->input_lpf_0 = new fv3::iir_1st_f();
        x->input_lpf_1 = new fv3::iir_1st_f();
        x->input_hpf_0 = new fv3::iir_1st_f();
        x->input_hpf_1 = new fv3::iir_1st_f();

        x->input_lpf_0->mute();
        x->input_lpf_1->mute();
        x->input_hpf_0->mute();
        x->input_hpf_1->mute();

        // Defaults borrowed from the Room preset table ("Medium Clear Room")
        // Map the plugin's param behavior to fixed settings here.
        const float size_m = 12.0f;
        const float width_pct = 100.0f;
        float predelay_ms = 8.0f;
        const float decay_s = 0.4f;
        const float diffuse_pct = 70.0f;
        const float spin_hz = 0.8f;
        const float wander_pct = 40.0f;
        const float in_high_cut_hz = 16000.0f;
        const float early_damp_hz = 10000.0f;
        const float late_damp_hz = 9400.0f;
        const float low_boost_pct = 50.0f;
        const float boost_freq_hz = 600.0f;
        const float in_low_cut_hz = 4.0f;

        x->room_early_send = 0.20f;

        x->early->setRSFactor(size_m / 10.0f);
        x->late->setRSFactor(size_m / 10.0f);

        x->early->setwidth(width_pct / 120.0f);
        x->late->setwidth(width_pct / 100.0f);

        if (predelay_ms < 0.1f) {
            predelay_ms = 0.1f;
        }
        x->late->setPreDelay(predelay_ms);
        x->late->setrt60(decay_s);

        x->late->setidiffusion1(diffuse_pct / 120.0f);
        x->late->setodiffusion1(diffuse_pct / 120.0f);

        x->late->setspin(spin_hz);
        x->late->setspin2(std::sqrt(100.0f - (10.0f - spin_hz) * (10.0f - spin_hz)) / 2.0f);

        x->late->setwander(wander_pct / 200.0f + 0.1f);
        x->late->setwander2(wander_pct / 200.0f + 0.1f);

        x->early->setoutputlpf(early_damp_hz);
        x->late->setdamp(late_damp_hz);
        x->late->setoutputdamp(late_damp_hz);

        x->late->setdamp2(boost_freq_hz);
        x->late->setbassboost(low_boost_pct / 20.0f / (float)std::pow(decay_s, 1.5f) *
                              (size_m / 10.0f));

        // set filters (will also be re-applied in dsp after samplerate is known)
        if (x->samplerate > 0) {
            dragonfly_tilde_room_setInputLPF(x, in_high_cut_hz);
            dragonfly_tilde_room_setInputHPF(x, in_low_cut_hz);
        }
    }

    if (x->samplerate > 0) {
        x->early->setSampleRate(x->samplerate);
        if (x->late) {
            x->late->setSampleRate(x->samplerate);
        }
    }

    return x;
}

// ─────────────────────────────────────

static void dragonfly_tilde_free(dragonfly_tilde *x) {
    delete x->early;
    x->early = nullptr;

    delete x->late;
    x->late = nullptr;

    delete x->input_lpf_0;
    delete x->input_lpf_1;
    delete x->input_hpf_0;
    delete x->input_hpf_1;
    x->input_lpf_0 = nullptr;
    x->input_lpf_1 = nullptr;
    x->input_hpf_0 = nullptr;
    x->input_hpf_1 = nullptr;

    dragonfly_tilde_free_buffers(x);
}

// ─────────────────────────────────────

extern "C" void setup_x0x2edragonfly_tilde(void) {
    dragonfly_tilde_class = class_new(gensym("x.dragonfly~"), (t_newmethod)dragonfly_tilde_new,
                                      (t_method)dragonfly_tilde_free, sizeof(dragonfly_tilde),
                                      CLASS_DEFAULT, A_GIMME, A_NULL);

    CLASS_MAINSIGNALIN(dragonfly_tilde_class, dragonfly_tilde, f);

    class_addmethod(dragonfly_tilde_class, (t_method)dragonfly_tilde_dsp, gensym("dsp"), A_CANT, 0);
    class_addmethod(dragonfly_tilde_class, (t_method)dragonfly_tilde_config, gensym("wet"), A_GIMME,
                    0);
    class_addmethod(dragonfly_tilde_class, (t_method)dragonfly_tilde_config, gensym("dry"), A_GIMME,
                    0);
    class_addmethod(dragonfly_tilde_class, (t_method)dragonfly_tilde_config, gensym("width"),
                    A_GIMME, 0);
    class_addmethod(dragonfly_tilde_class, (t_method)dragonfly_tilde_config, gensym("lowcut"),
                    A_GIMME, 0);
    class_addmethod(dragonfly_tilde_class, (t_method)dragonfly_tilde_config, gensym("highcut"),
                    A_GIMME, 0);
    class_addmethod(dragonfly_tilde_class, (t_method)dragonfly_tilde_config, gensym("size"),
                    A_GIMME, 0);
    class_addmethod(dragonfly_tilde_class, (t_method)dragonfly_tilde_config, gensym("program"),
                    A_GIMME, 0);
}
