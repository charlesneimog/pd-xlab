
#include <OpenScofo.hpp>

#include <m_pd.h>

#include <g_canvas.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

static t_class *x_onset_cut_class;

static constexpr int FFT_SIZE = 2048;
static constexpr int HOP_SIZE = 512;
static constexpr int FADE_SIZE = 64;
static constexpr int EXTRA_MS = 250;
static constexpr double DB_THRESHOLD = -60.0;
static constexpr int SILENCE_MS = 100;
static constexpr int PRE_ONSET_MS = 20;

// ─────────────────────────────────────
typedef struct _x_onset_cut {
    t_object x_obj;
    t_canvas *canvas;
    OpenScofo::OpenScofo *openscofo;
    double sr;
    double db;
} t_x_onset_cut;

// ─────────────────────────────────────
static void write_u16(std::ofstream &file, uint16_t value) {
    char bytes[2] = {(char)(value & 0xff), (char)((value >> 8) & 0xff)};
    file.write(bytes, 2);
}

static void write_u32(std::ofstream &file, uint32_t value) {
    write_u16(file, (uint16_t)(value & 0xffff));
    write_u16(file, (uint16_t)(value >> 16));
}

// ─────────────────────────────────────
static bool write_wav(const fs::path &path, const std::vector<float> &samples, int sr) {
    if (samples.size() > (UINT32_MAX - 36) / 2)
        return false;

    std::ofstream file(path, std::ios::binary);
    if (!file)
        return false;

    uint32_t data_size = (uint32_t)samples.size() * 2;

    file.write("RIFF", 4);
    write_u32(file, 36 + data_size);
    file.write("WAVE", 4);

    file.write("fmt ", 4);
    write_u32(file, 16);
    write_u16(file, 1); // PCM
    write_u16(file, 1); // Mono
    write_u32(file, (uint32_t)sr);
    write_u32(file, (uint32_t)sr * 2);
    write_u16(file, 2);
    write_u16(file, 16);

    file.write("data", 4);
    write_u32(file, data_size);

    for (float sample : samples) {
        float v = std::clamp(sample, -1.0f, 1.0f);
        int16_t pcm = (int16_t)std::lrint(v * (v < 0 ? 32768.0f : 32767.0f));
        write_u16(file, (uint16_t)pcm);
    }

    return file.good();
}

// ─────────────────────────────────────
static void x_onset_cut_cut(t_x_onset_cut *x, t_symbol *array, t_symbol *tag) {
    t_garray *garray = (t_garray *)pd_findbyclass(array, garray_class);

    if (!garray) {
        pd_error(x, "x.onset.cut~: array '%s' not found", array->s_name);
        return;
    }

    int size = 0;
    t_word *words = nullptr;

    if (!garray_getfloatwords(garray, &size, &words)) {
        pd_error(x, "x.onset.cut~: invalid array");
        return;
    }

    if (size < FFT_SIZE) {
        pd_error(x, "x.onset.cut~: array is too short");
        return;
    }

    std::vector<float> samples(size);
    for (int i = 0; i < size; ++i)
        samples[i] = words[i].w_float;

    char path[MAXPDSTRING];
    canvas_makefilename(x->canvas, tag->s_name, path, MAXPDSTRING);

    fs::path directory(path);

    try {
        fs::create_directories(directory);
    } catch (const fs::filesystem_error &e) {
        pd_error(x, "x.onset.cut~: %s", e.what());
        return;
    }

    // ── OpenScofo
    OpenScofo::OpenScofo scofo(x->sr, FFT_SIZE, HOP_SIZE);

    scofo.RequestDescriptor(scofo.GetDescriptorsEnum("db"));
    scofo.RequestDescriptor(scofo.GetDescriptorsEnum("onset"));

    // ── Segmentation
    static constexpr int SILENCE_MS = 100;
    static constexpr int PRE_ONSET_MS = 20;

    const int silence_required = (int)(SILENCE_MS * x->sr / 1000.0);
    const int pre_onset = (int)(PRE_ONSET_MS * x->sr / 1000.0);

    std::vector<std::pair<int, int>> segments;

    bool active = false;
    int begin = 0;
    int silence_samples = 0;

    for (int i = 0; i + HOP_SIZE <= size; i += HOP_SIZE) {
        if (!scofo.ProcessBlock(samples.data() + i, HOP_SIZE))
            continue;

        auto desc = scofo.GetDescription();

        const double db = desc.dB;
        const bool onset = desc.Onset;

        if (!active) {
            if (onset && db > x->db) {
                begin = std::max(0, i - pre_onset);
                active = true;
                silence_samples = 0;
            }
        } else {
            if (db < x->db) {
                silence_samples += HOP_SIZE;

                if (silence_samples >= silence_required) {
                    int end = std::min(i + HOP_SIZE, size);
                    segments.emplace_back(begin, end);

                    active = false;
                    silence_samples = 0;
                }
            } else {
                silence_samples = 0;
            }
        }
    }

    if (active)
        segments.emplace_back(begin, size);

    // ── Export
    const int extra = (int)(EXTRA_MS * x->sr / 1000.0);
    int exported = 0;

    for (size_t idx = 0; idx < segments.size(); ++idx) {
        auto [start, end] = segments[idx];

        int next_start = (idx + 1 < segments.size()) ? segments[idx + 1].first : size;

        end = std::min({end + extra, next_start, size});

        if (end <= start)
            continue;

        std::vector<float> chunk(samples.begin() + start, samples.begin() + end);

        int n = (int)chunk.size();
        int fade = std::min(FADE_SIZE, n / 2);

        for (int i = 0; i < fade; ++i) {
            float gain = fade > 1 ? (float)i / (fade - 1) : 0.0f;
            chunk[i] *= gain;
            chunk[n - 1 - i] *= gain;
        }

        char filename[64];
        snprintf(filename, sizeof(filename), "_%04zu.wav", idx);

        fs::path output = directory / (std::string(tag->s_name) + filename);

        if (!write_wav(output, chunk, (int)x->sr)) {
            pd_error(x, "x.onset.cut~: cannot write '%s'", output.string().c_str());
            continue;
        }

        exported++;
    }

    post("x.onset.cut~: exported %d segments", exported);
}

// ─────────────────────────────────────
static void x_onset_cut_db(t_x_onset_cut *x, t_floatarg db) {
    //
    x->db = db;
}

// ─────────────────────────────────────
static void *x_onset_cut_new(t_symbol *s, int argc, t_atom *argv) {
    t_x_onset_cut *x = (t_x_onset_cut *)pd_new(x_onset_cut_class);
    x->canvas = canvas_getcurrent();
    x->sr = sys_getsr();
    if (x->sr <= 0) {
        x->sr = 48000;
    }

    x->db = DB_THRESHOLD;
    x->openscofo = nullptr;
    return x;
}

// ─────────────────────────────────────
static void x_onset_cut_free(t_x_onset_cut *x) { delete x->openscofo; }

// ─────────────────────────────────────
extern "C" void setup_x0x2eonset0x2ecut_tilde(void) {
    x_onset_cut_class =
        class_new(gensym("x.onset.cut~"), (t_newmethod)x_onset_cut_new, (t_method)x_onset_cut_free,
                  sizeof(t_x_onset_cut), CLASS_DEFAULT, A_GIMME, 0);
    class_addmethod(x_onset_cut_class, (t_method)x_onset_cut_cut, gensym("cut"), A_SYMBOL, A_SYMBOL,
                    0);
    class_addmethod(x_onset_cut_class, (t_method)x_onset_cut_db, gensym("db"), A_FLOAT, 0);
}
