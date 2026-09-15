extern "C" {
#include "m_pd.h"
}

#define TSF_IMPLEMENTATION
#include "tsf.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>

// ─────────────────────────────────────────────────────────────
// x.tsf~
// TinySoundFont Pure Data external
//
// Creation:
//   [x.tsf~ piano.sf2]
//
// Basic MIDI-like input:
//   [60 100(       note on
//   [60 0(         note off
//   [60 100 2(     note on MIDI channel 2
//
// Channels exposed to Pd are 1..16.
// TinySoundFont internally uses 0..15.
// ─────────────────────────────────────────────────────────────
static t_class *tsf_tilde_class = nullptr;

struct t_tsf_tilde {
    t_object x_obj;

    tsf *soundfont;
    t_outlet *outlet;
    t_canvas *canvas;

    std::mutex *mutex;

    int sample_rate;
    int channel; // internal: 0..15
    int max_voices;

    float global_volume;

    t_symbol *filename;
};

// ─────────────────────────────────────
static bool tsf_tilde_has_soundfont(t_tsf_tilde *x) {
    if (!x->soundfont) {
        pd_error(x, "[x.tsf~] no SoundFont loaded");
        return false;
    }

    return true;
}

// ─────────────────────────────────────
static int tsf_tilde_pd_channel(t_float f) {
    // Pd-facing MIDI channel: 1..16
    int ch = static_cast<int>(f);

    ch = std::clamp(ch, 1, 16);

    return ch - 1;
}

// ─────────────────────────────────────
static int tsf_tilde_atom_channel(int argc, t_atom *argv, int index, int fallback) {
    if (argc <= index || argv[index].a_type != A_FLOAT)
        return fallback;

    return tsf_tilde_pd_channel(atom_getfloat(argv + index));
}

// ─────────────────────────────────────
static void tsf_tilde_resolve_path(t_tsf_tilde *x, const char *filename, char *result) {
    if (sys_isabsolutepath(filename)) {
        std::snprintf(result, MAXPDSTRING, "%s", filename);
        return;
    }

    canvas_makefilename(x->canvas, filename, result, MAXPDSTRING);
}

// ─────────────────────────────────────
static void tsf_tilde_initialize_channels(tsf *sf) {
    if (!sf)
        return;

    int count = tsf_get_presetcount(sf);

    if (count <= 0)
        return;

    for (int ch = 0; ch < 16; ++ch) {
        tsf_channel_set_presetindex(sf, ch, 0);
    }
}

// ─────────────────────────────────────
static void tsf_tilde_configure_soundfont(t_tsf_tilde *x, tsf *sf) {
    if (!sf)
        return;

    tsf_set_output(sf, TSF_MONO, x->sample_rate, 0.0f);

    tsf_set_volume(sf, x->global_volume);

    if (!tsf_set_max_voices(sf, x->max_voices)) {
        pd_error(x, "[x.tsf~] could not allocate %d voices", x->max_voices);
    }

    tsf_tilde_initialize_channels(sf);
}

// ─────────────────────────────────────
static void tsf_tilde_open(t_tsf_tilde *x, t_symbol *s) {
    if (!s || s == &s_) {
        pd_error(x, "[x.tsf~] open: expected a .sf2 file");
        return;
    }

    char path[MAXPDSTRING];

    tsf_tilde_resolve_path(x, s->s_name, path);

    // Load outside the audio mutex because disk access can be slow.
    tsf *new_sf = tsf_load_filename(path);

    if (!new_sf) {
        pd_error(x, "[x.tsf~] could not load SoundFont: %s", path);

        return;
    }

    if (tsf_get_presetcount(new_sf) <= 0) {
        pd_error(x, "[x.tsf~] SoundFont contains no presets: %s", path);

        tsf_close(new_sf);
        return;
    }

    tsf_tilde_configure_soundfont(x, new_sf);

    tsf *old_sf = nullptr;

    {
        std::lock_guard<std::mutex> lock(*x->mutex);

        old_sf = x->soundfont;
        x->soundfont = new_sf;
        x->filename = s;
    }

    // Safe to destroy after swapping.
    if (old_sf)
        tsf_close(old_sf);

    post("[x.tsf~] loaded %s (%d presets)", path, tsf_get_presetcount(new_sf));
}

// ─────────────────────────────────────
static void tsf_tilde_list(t_tsf_tilde *x, t_symbol *, int argc, t_atom *argv) {
    if (!tsf_tilde_has_soundfont(x))
        return;

    if (argc < 2) {
        pd_error(x, "[x.tsf~] expected: pitch velocity [channel]");

        return;
    }

    if (argv[0].a_type != A_FLOAT || argv[1].a_type != A_FLOAT) {
        pd_error(x, "[x.tsf~] pitch and velocity must be numbers");

        return;
    }

    int pitch = static_cast<int>(atom_getfloat(argv));

    int velocity = static_cast<int>(atom_getfloat(argv + 1));

    pitch = std::clamp(pitch, 0, 127);
    velocity = std::clamp(velocity, 0, 127);

    int channel = tsf_tilde_atom_channel(argc, argv, 2, x->channel);

    std::lock_guard<std::mutex> lock(*x->mutex);

    if (velocity == 0) {

        tsf_channel_note_off(x->soundfont, channel, pitch);

    } else {

        float vel = static_cast<float>(velocity) / 127.0f;

        if (!tsf_channel_note_on(x->soundfont, channel, pitch, vel)) {

            pd_error(x, "[x.tsf~] could not allocate voice");
        }
    }
}

// ─────────────────────────────────────
static void tsf_tilde_note(t_tsf_tilde *x, t_symbol *s, int argc, t_atom *argv) {
    tsf_tilde_list(x, s, argc, argv);
}

// ─────────────────────────────────────────────────────────────
// off <pitch>
// ─────────────────────────────────────────────────────────────

// ─────────────────────────────────────
static void tsf_tilde_off(t_tsf_tilde *x, t_floatarg f) {
    if (!tsf_tilde_has_soundfont(x))
        return;

    int pitch = std::clamp(static_cast<int>(f), 0, 127);

    std::lock_guard<std::mutex> lock(*x->mutex);

    tsf_channel_note_off(x->soundfont, x->channel, pitch);
}

// ─────────────────────────────────────────────────────────────
// channel <1..16>
// ─────────────────────────────────────────────────────────────

// ─────────────────────────────────────
static void tsf_tilde_channel(t_tsf_tilde *x, t_floatarg f) {
    x->channel = tsf_tilde_pd_channel(f);
}

// ─────────────────────────────────────────────────────────────
// preset <index>
//
// TinySoundFont preset index, NOT MIDI program number.
// ─────────────────────────────────────────────────────────────

// ─────────────────────────────────────
static void tsf_tilde_preset(t_tsf_tilde *x, t_floatarg f) {
    if (!tsf_tilde_has_soundfont(x))
        return;

    int preset = static_cast<int>(f);

    std::lock_guard<std::mutex> lock(*x->mutex);

    int count = tsf_get_presetcount(x->soundfont);

    if (preset < 0 || preset >= count) {
        pd_error(x, "[x.tsf~] preset index %d out of range 0..%d", preset, count - 1);

        return;
    }

    if (!tsf_channel_set_presetindex(x->soundfont, x->channel, preset)) {

        pd_error(x, "[x.tsf~] could not set preset %d", preset);
    }
}

// ─────────────────────────────────────────────────────────────
// program <0..127>
//
// MIDI program number in current bank.
// ─────────────────────────────────────────────────────────────

// ─────────────────────────────────────
static void tsf_tilde_program(t_tsf_tilde *x, t_floatarg f) {
    if (!tsf_tilde_has_soundfont(x))
        return;

    int program = std::clamp(static_cast<int>(f), 0, 127);

    std::lock_guard<std::mutex> lock(*x->mutex);

    if (!tsf_channel_set_presetnumber(x->soundfont, x->channel, program, 0)) {

        pd_error(x, "[x.tsf~] program %d does not exist", program);
    }
}

// ─────────────────────────────────────────────────────────────
// drums <program>
//
// Uses TinySoundFont MIDI drum-bank lookup rules.
// ─────────────────────────────────────────────────────────────

// ─────────────────────────────────────
static void tsf_tilde_drums(t_tsf_tilde *x, t_floatarg f) {
    if (!tsf_tilde_has_soundfont(x))
        return;

    int program = std::clamp(static_cast<int>(f), 0, 127);

    std::lock_guard<std::mutex> lock(*x->mutex);

    if (!tsf_channel_set_presetnumber(x->soundfont, x->channel, program, 1)) {

        pd_error(x, "[x.tsf~] drum program %d does not exist", program);
    }
}

// ─────────────────────────────────────────────────────────────
// bank <bank>
// ─────────────────────────────────────────────────────────────

// ─────────────────────────────────────
static void tsf_tilde_bank(t_tsf_tilde *x, t_floatarg f) {
    if (!tsf_tilde_has_soundfont(x))
        return;

    int bank = std::max(0, static_cast<int>(f));

    std::lock_guard<std::mutex> lock(*x->mutex);

    tsf_channel_set_bank(x->soundfont, x->channel, bank);
}

// ─────────────────────────────────────────────────────────────
// bankpreset <bank> <program>
// ─────────────────────────────────────────────────────────────

// ─────────────────────────────────────
static void tsf_tilde_bankpreset(t_tsf_tilde *x, t_floatarg bank_f, t_floatarg program_f) {
    if (!tsf_tilde_has_soundfont(x))
        return;

    int bank = std::max(0, static_cast<int>(bank_f));

    int program = std::clamp(static_cast<int>(program_f), 0, 127);

    std::lock_guard<std::mutex> lock(*x->mutex);

    if (!tsf_channel_set_bank_preset(x->soundfont, x->channel, bank, program)) {

        pd_error(x, "[x.tsf~] bank %d program %d not found", bank, program);
    }
}

// ─────────────────────────────────────────────────────────────
// pan <0..1>
// ─────────────────────────────────────────────────────────────

// ─────────────────────────────────────
static void tsf_tilde_pan(t_tsf_tilde *x, t_floatarg f) {
    if (!tsf_tilde_has_soundfont(x))
        return;

    float pan = std::clamp(static_cast<float>(f), 0.0f, 1.0f);

    std::lock_guard<std::mutex> lock(*x->mutex);

    tsf_channel_set_pan(x->soundfont, x->channel, pan);
}

// ─────────────────────────────────────────────────────────────
// volume <linear>
// channel volume
//
// 0   silence
// 1   normal
// ─────────────────────────────────────────────────────────────

// ─────────────────────────────────────
static void tsf_tilde_volume(t_tsf_tilde *x, t_floatarg f) {
    if (!tsf_tilde_has_soundfont(x))
        return;

    float volume = std::max(0.0f, static_cast<float>(f));

    std::lock_guard<std::mutex> lock(*x->mutex);

    tsf_channel_set_volume(x->soundfont, x->channel, volume);
}

// ─────────────────────────────────────────────────────────────
// gain <linear>
//
// global volume
// ─────────────────────────────────────────────────────────────

// ─────────────────────────────────────
static void tsf_tilde_gain(t_tsf_tilde *x, t_floatarg f) {
    float gain = std::max(0.0f, static_cast<float>(f));

    x->global_volume = gain;

    if (!x->soundfont)
        return;

    std::lock_guard<std::mutex> lock(*x->mutex);

    tsf_set_volume(x->soundfont, gain);
}

// ─────────────────────────────────────────────────────────────
// pitchwheel <0..16383>
// 8192 = center
// ─────────────────────────────────────────────────────────────

// ─────────────────────────────────────
static void tsf_tilde_pitchwheel(t_tsf_tilde *x, t_floatarg f) {
    if (!tsf_tilde_has_soundfont(x))
        return;

    int value = std::clamp(static_cast<int>(f), 0, 16383);

    std::lock_guard<std::mutex> lock(*x->mutex);

    tsf_channel_set_pitchwheel(x->soundfont, x->channel, value);
}

// ─────────────────────────────────────────────────────────────
// bend <-1..1>
//
// Convenience interface:
//
// -1 = full down
//  0 = center
// +1 = full up
// ─────────────────────────────────────────────────────────────

// ─────────────────────────────────────
static void tsf_tilde_bend(t_tsf_tilde *x, t_floatarg f) {
    if (!tsf_tilde_has_soundfont(x))
        return;

    float bend = std::clamp(static_cast<float>(f), -1.0f, 1.0f);

    int wheel;

    if (bend >= 0.0f) {
        wheel = 8192 + static_cast<int>(bend * 8191.0f);
    } else {
        wheel = 8192 + static_cast<int>(bend * 8192.0f);
    }

    std::lock_guard<std::mutex> lock(*x->mutex);

    tsf_channel_set_pitchwheel(x->soundfont, x->channel, wheel);
}

// ─────────────────────────────────────────────────────────────
// pitchrange <semitones>
// ─────────────────────────────────────────────────────────────

// ─────────────────────────────────────
static void tsf_tilde_pitchrange(t_tsf_tilde *x, t_floatarg f) {
    if (!tsf_tilde_has_soundfont(x))
        return;

    float range = std::max(0.0f, static_cast<float>(f));

    std::lock_guard<std::mutex> lock(*x->mutex);

    tsf_channel_set_pitchrange(x->soundfont, x->channel, range);
}

// ─────────────────────────────────────────────────────────────
// tuning <semitones>
//
// Examples:
//
// [tuning 0.5(
// [tuning -0.25(
//
// This permits fractional semitone tuning.
// ─────────────────────────────────────────────────────────────

// ─────────────────────────────────────
static void tsf_tilde_tuning(t_tsf_tilde *x, t_floatarg f) {
    if (!tsf_tilde_has_soundfont(x))
        return;

    std::lock_guard<std::mutex> lock(*x->mutex);

    tsf_channel_set_tuning(x->soundfont, x->channel, static_cast<float>(f));
}

// ─────────────────────────────────────────────────────────────
// sustain <0|1>
// ─────────────────────────────────────────────────────────────

// ─────────────────────────────────────
static void tsf_tilde_sustain(t_tsf_tilde *x, t_floatarg f) {
    if (!tsf_tilde_has_soundfont(x))
        return;

    std::lock_guard<std::mutex> lock(*x->mutex);

    tsf_channel_set_sustain(x->soundfont, x->channel, f != 0);
}

// ─────────────────────────────────────────────────────────────
// cc <controller> <value>
// controller/value = 0..127
//
// Supported TinySoundFont controllers include volume,
// expression, pan, bank select, sustain, RPN data,
// all sound off, all notes off, etc.
// ─────────────────────────────────────────────────────────────

// ─────────────────────────────────────
static void tsf_tilde_cc(t_tsf_tilde *x, t_floatarg controller_f, t_floatarg value_f) {
    if (!tsf_tilde_has_soundfont(x))
        return;

    int controller = std::clamp(static_cast<int>(controller_f), 0, 127);

    int value = std::clamp(static_cast<int>(value_f), 0, 127);

    std::lock_guard<std::mutex> lock(*x->mutex);

    tsf_channel_midi_control(x->soundfont, x->channel, controller, value);
}

// ─────────────────────────────────────────────────────────────
// alloff
//
// Release all notes on current channel.
// ─────────────────────────────────────────────────────────────

// ─────────────────────────────────────
static void tsf_tilde_alloff(t_tsf_tilde *x) {
    if (!tsf_tilde_has_soundfont(x))
        return;

    std::lock_guard<std::mutex> lock(*x->mutex);

    tsf_channel_note_off_all(x->soundfont, x->channel);
}

// ─────────────────────────────────────────────────────────────
// alloffall
//
// Release all notes globally.
// ─────────────────────────────────────────────────────────────

// ─────────────────────────────────────
static void tsf_tilde_alloffall(t_tsf_tilde *x) {
    if (!tsf_tilde_has_soundfont(x))
        return;

    std::lock_guard<std::mutex> lock(*x->mutex);

    tsf_note_off_all(x->soundfont);
}

// ─────────────────────────────────────────────────────────────
// panic
//
// Immediately kill current channel.
// ─────────────────────────────────────────────────────────────

// ─────────────────────────────────────
static void tsf_tilde_panic(t_tsf_tilde *x) {
    if (!tsf_tilde_has_soundfont(x))
        return;

    std::lock_guard<std::mutex> lock(*x->mutex);

    tsf_channel_sounds_off_all(x->soundfont, x->channel);
}

// ─────────────────────────────────────────────────────────────
// panicall
// ─────────────────────────────────────────────────────────────

// ─────────────────────────────────────
static void tsf_tilde_panicall(t_tsf_tilde *x) {
    if (!tsf_tilde_has_soundfont(x))
        return;

    std::lock_guard<std::mutex> lock(*x->mutex);

    for (int ch = 0; ch < 16; ++ch) {
        tsf_channel_sounds_off_all(x->soundfont, ch);
    }
}

// ─────────────────────────────────────────────────────────────
// reset
//
// Stop notes + reset TinySoundFont channel state.
// ─────────────────────────────────────────────────────────────

// ─────────────────────────────────────
static void tsf_tilde_reset(t_tsf_tilde *x) {
    if (!tsf_tilde_has_soundfont(x))
        return;

    std::lock_guard<std::mutex> lock(*x->mutex);

    tsf_reset(x->soundfont);

    tsf_tilde_initialize_channels(x->soundfont);

    tsf_set_volume(x->soundfont, x->global_volume);
}

// ─────────────────────────────────────────────────────────────
// maxvoices <n>
// ─────────────────────────────────────────────────────────────

// ─────────────────────────────────────
static void tsf_tilde_maxvoices(t_tsf_tilde *x, t_floatarg f) {
    int voices = std::max(1, static_cast<int>(f));

    if (!x->soundfont) {
        x->max_voices = voices;
        return;
    }

    std::lock_guard<std::mutex> lock(*x->mutex);

    if (!tsf_set_max_voices(x->soundfont, voices)) {

        pd_error(x, "[x.tsf~] could not allocate %d voices", voices);

        return;
    }

    x->max_voices = voices;
}

// ─────────────────────────────────────
static void tsf_tilde_voices(t_tsf_tilde *x) {
    if (!tsf_tilde_has_soundfont(x)) {
        return;
    }

    std::lock_guard<std::mutex> lock(*x->mutex);
    post("[x.tsf~] active voices: %d", tsf_active_voice_count(x->soundfont));
}

// ─────────────────────────────────────
static void tsf_tilde_presets(t_tsf_tilde *x) {
    if (!tsf_tilde_has_soundfont(x))
        return;

    std::lock_guard<std::mutex> lock(*x->mutex);
    int count = tsf_get_presetcount(x->soundfont);
    post("[x.tsf~] presets: %d", count);
    for (int i = 0; i < count; ++i) {
        const char *name = tsf_get_presetname(x->soundfont, i);
        post("[x.tsf~] %d: %s", i, name ? name : "(unnamed)");
    }
}

// ─────────────────────────────────────
static void tsf_tilde_info(t_tsf_tilde *x) {
    if (!tsf_tilde_has_soundfont(x))
        return;

    std::lock_guard<std::mutex> lock(*x->mutex);
    int ch = x->channel;
    int preset_index = tsf_channel_get_preset_index(x->soundfont, ch);
    int bank = tsf_channel_get_preset_bank(x->soundfont, ch);
    int program = tsf_channel_get_preset_number(x->soundfont, ch);
    const char *name = tsf_get_presetname(x->soundfont, preset_index);

    post("[x.tsf~] channel: %d", ch + 1);
    post("[x.tsf~] preset index: %d (%s)", preset_index, name ? name : "(unnamed)");
    post("[x.tsf~] bank/program: %d/%d", bank, program);
    post("[x.tsf~] pan: %.3f", tsf_channel_get_pan(x->soundfont, ch));
    post("[x.tsf~] volume: %.3f", tsf_channel_get_volume(x->soundfont, ch));
    post("[x.tsf~] pitch wheel: %d", tsf_channel_get_pitchwheel(x->soundfont, ch));
    post("[x.tsf~] pitch range: %.3f", tsf_channel_get_pitchrange(x->soundfont, ch));
    post("[x.tsf~] tuning: %.3f", tsf_channel_get_tuning(x->soundfont, ch));
    post("[x.tsf~] active voices: %d", tsf_active_voice_count(x->soundfont));
}

// ─────────────────────────────────────
static t_int *tsf_tilde_perform(t_int *w) {
    auto *x = reinterpret_cast<t_tsf_tilde *>(w[1]);
    auto *out = reinterpret_cast<t_sample *>(w[2]);
    int n = static_cast<int>(w[3]);
    if (!x->soundfont) {
        std::memset(out, 0, sizeof(t_sample) * n);
        return w + 4;
    }

    {
        std::lock_guard<std::mutex> lock(*x->mutex);
        tsf_render_float(x->soundfont, out, n, 0);
    }
    return w + 4;
}

// ─────────────────────────────────────
static void tsf_tilde_dsp(t_tsf_tilde *x, t_signal **sp) {
    int sr = static_cast<int>(sp[0]->s_sr);
    if (sr != x->sample_rate) {
        x->sample_rate = sr;
        if (x->soundfont) {
            std::lock_guard<std::mutex> lock(*x->mutex);
            tsf_set_output(x->soundfont, TSF_MONO, sr, 0.0f);
            tsf_set_volume(x->soundfont, x->global_volume);
        }
    }
    dsp_add(tsf_tilde_perform, 3, x, sp[0]->s_vec, sp[0]->s_n);
}

// ─────────────────────────────────────
static void *tsf_tilde_new(t_symbol *s) {
    auto *x = reinterpret_cast<t_tsf_tilde *>(pd_new(tsf_tilde_class));

    x->soundfont = nullptr;
    x->outlet = nullptr;
    x->canvas = canvas_getcurrent();
    x->mutex = nullptr;
    x->channel = 0;

    x->sample_rate = static_cast<int>(sys_getsr());
    if (x->sample_rate <= 0)
        x->sample_rate = 44100;

    x->max_voices = 256;
    x->global_volume = 1.0f;
    x->filename = &s_;

    x->mutex = new (std::nothrow) std::mutex();

    if (!x->mutex) {
        pd_error(x, "[x.tsf~] could not create mutex");
        return x;
    }
    x->outlet = outlet_new(&x->x_obj, &s_signal);

    if (s && s != &s_) {
        char dir[MAXPDSTRING];
        char *name = nullptr;
        int fd = canvas_open(x->canvas, s->s_name, "", dir, &name, MAXPDSTRING, 1);
        if (fd < 0) {
            pd_error(x, "[x.tsf~] could not find SoundFont: %s", s->s_name);
            return x;
        }

        sys_close(fd);
        char path[MAXPDSTRING];
        std::snprintf(path, sizeof(path), "%s/%s", dir, name);
        x->soundfont = tsf_load_filename(path);

        if (!x->soundfont) {
            pd_error(x, "[x.tsf~] could not load SoundFont: %s", path);
            return x;
        }

        x->filename = s;
        tsf_set_output(x->soundfont, TSF_MONO, x->sample_rate, 0.0f);
        tsf_set_volume(x->soundfont, x->global_volume);

        if (!tsf_set_max_voices(x->soundfont, x->max_voices)) {
            pd_error(x, "[x.tsf~] could not allocate %d voices", x->max_voices);
        }

        for (int channel = 0; channel < 16; ++channel) {
            tsf_channel_set_presetindex(x->soundfont, channel, 0);
        }

        post("[x.tsf~] loaded: %s", path);
        post("[x.tsf~] presets: %d", tsf_get_presetcount(x->soundfont));
    }

    return x;
}

// ─────────────────────────────────────
static void tsf_tilde_free(t_tsf_tilde *x) {
    if (x->mutex) {
        tsf *old_sf = nullptr;
        {
            std::lock_guard<std::mutex> lock(*x->mutex);
            old_sf = x->soundfont;
            x->soundfont = nullptr;
        }
        if (old_sf) {
            tsf_close(old_sf);
        }
        delete x->mutex;
        x->mutex = nullptr;
    }
}

// ─────────────────────────────────────
extern "C" void setup_x0x2etsf_tilde(void) {
    tsf_tilde_class = class_new(gensym("x.tsf~"), reinterpret_cast<t_newmethod>(tsf_tilde_new),
                                reinterpret_cast<t_method>(tsf_tilde_free), sizeof(t_tsf_tilde),
                                CLASS_DEFAULT, A_DEFSYM, 0);
    class_addlist(tsf_tilde_class, reinterpret_cast<t_method>(tsf_tilde_list));
    class_addmethod(tsf_tilde_class, reinterpret_cast<t_method>(tsf_tilde_note), gensym("note"),
                    A_GIMME, 0);
    class_addmethod(tsf_tilde_class, reinterpret_cast<t_method>(tsf_tilde_off), gensym("off"),
                    A_FLOAT, 0);
    class_addmethod(tsf_tilde_class, reinterpret_cast<t_method>(tsf_tilde_open), gensym("open"),
                    A_SYMBOL, 0);
    class_addmethod(tsf_tilde_class, reinterpret_cast<t_method>(tsf_tilde_open), gensym("load"),
                    A_SYMBOL, 0);
    class_addmethod(tsf_tilde_class, reinterpret_cast<t_method>(tsf_tilde_channel),
                    gensym("channel"), A_FLOAT, 0);
    class_addmethod(tsf_tilde_class, reinterpret_cast<t_method>(tsf_tilde_preset), gensym("preset"),
                    A_FLOAT, 0);
    class_addmethod(tsf_tilde_class, reinterpret_cast<t_method>(tsf_tilde_program),
                    gensym("program"), A_FLOAT, 0);
    class_addmethod(tsf_tilde_class, reinterpret_cast<t_method>(tsf_tilde_drums), gensym("drums"),
                    A_FLOAT, 0);
    class_addmethod(tsf_tilde_class, reinterpret_cast<t_method>(tsf_tilde_bank), gensym("bank"),
                    A_FLOAT, 0);
    class_addmethod(tsf_tilde_class, reinterpret_cast<t_method>(tsf_tilde_bankpreset),
                    gensym("bankpreset"), A_FLOAT, A_FLOAT, 0);
    class_addmethod(tsf_tilde_class, reinterpret_cast<t_method>(tsf_tilde_pan), gensym("pan"),
                    A_FLOAT, 0);
    class_addmethod(tsf_tilde_class, reinterpret_cast<t_method>(tsf_tilde_volume), gensym("volume"),
                    A_FLOAT, 0);
    class_addmethod(tsf_tilde_class, reinterpret_cast<t_method>(tsf_tilde_gain), gensym("gain"),
                    A_FLOAT, 0);
    class_addmethod(tsf_tilde_class, reinterpret_cast<t_method>(tsf_tilde_pitchwheel),
                    gensym("pitchwheel"), A_FLOAT, 0);
    class_addmethod(tsf_tilde_class, reinterpret_cast<t_method>(tsf_tilde_bend), gensym("bend"),
                    A_FLOAT, 0);
    class_addmethod(tsf_tilde_class, reinterpret_cast<t_method>(tsf_tilde_pitchrange),
                    gensym("pitchrange"), A_FLOAT, 0);
    class_addmethod(tsf_tilde_class, reinterpret_cast<t_method>(tsf_tilde_tuning), gensym("tuning"),
                    A_FLOAT, 0);
    class_addmethod(tsf_tilde_class, reinterpret_cast<t_method>(tsf_tilde_sustain),
                    gensym("sustain"), A_FLOAT, 0);
    class_addmethod(tsf_tilde_class, reinterpret_cast<t_method>(tsf_tilde_cc), gensym("cc"),
                    A_FLOAT, A_FLOAT, 0);
    class_addmethod(tsf_tilde_class, reinterpret_cast<t_method>(tsf_tilde_alloff), gensym("alloff"),
                    A_NULL, 0);
    class_addmethod(tsf_tilde_class, reinterpret_cast<t_method>(tsf_tilde_alloffall),
                    gensym("alloffall"), A_NULL, 0);
    class_addmethod(tsf_tilde_class, reinterpret_cast<t_method>(tsf_tilde_panic), gensym("panic"),
                    A_NULL, 0);
    class_addmethod(tsf_tilde_class, reinterpret_cast<t_method>(tsf_tilde_panicall),
                    gensym("panicall"), A_NULL, 0);
    class_addmethod(tsf_tilde_class, reinterpret_cast<t_method>(tsf_tilde_reset), gensym("reset"),
                    A_NULL, 0);
    class_addmethod(tsf_tilde_class, reinterpret_cast<t_method>(tsf_tilde_maxvoices),
                    gensym("maxvoices"), A_FLOAT, 0);
    class_addmethod(tsf_tilde_class, reinterpret_cast<t_method>(tsf_tilde_voices), gensym("voices"),
                    A_NULL, 0);
    class_addmethod(tsf_tilde_class, reinterpret_cast<t_method>(tsf_tilde_presets),
                    gensym("presets"), A_NULL, 0);
    class_addmethod(tsf_tilde_class, reinterpret_cast<t_method>(tsf_tilde_info), gensym("info"),
                    A_NULL, 0);
    class_addmethod(tsf_tilde_class, reinterpret_cast<t_method>(tsf_tilde_dsp), gensym("dsp"),
                    A_CANT, 0);
}
