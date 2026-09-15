#include <algorithm>
#include <m_pd.h>
#include <string.h>

static t_class *infinite_record_class;

// Define chunk size for array growth (e.g., 15 seconds at 48kHz)
// 15 * 48000 = 720,000 samples per allocation
#define CHUNK_SECONDS 15
#define WARNING_SECONDS 0.5

struct infinite_record {
    t_object x_obj;
    t_sample x_f;

    // Array state
    t_symbol *arrayname;
    t_garray *array;
    t_word *vec;
    int vecsize;

    // Recording state
    int logical_size;
    int allocated_size;
    int sr;
    bool recording;
    bool resize_pending;

    // Fade settings
    bool fade_in_out;
    int fade_size_samples;

    // Clocks and outlets
    t_clock *clock_resize;
    t_clock *clock_redraw;
    t_clock *clock_report;
    t_outlet *outlet_report;
};

// ─────────────────────────────────────
static bool update_array_pointers(infinite_record *x) {
    if (!x->arrayname)
        return false;

    x->array = (t_garray *)pd_findbyclass(x->arrayname, garray_class);
    if (!x->array) {
        x->vec = nullptr;
        x->vecsize = 0;
        return false;
    }

    if (!garray_getfloatwords(x->array, &x->vecsize, &x->vec)) {
        x->vec = nullptr;
        x->vecsize = 0;
        return false;
    }
    return true;
}

// ─────────────────────────────────────
static void clock_cb_resize(infinite_record *x) {
    x->resize_pending = false;
    if (!x->recording)
        return;

    if (!update_array_pointers(x)) {
        pd_error(x, "[x.infinite.record~] Array '%s' lost during recording.", x->arrayname->s_name);
        x->recording = false;
        return;
    }

    int chunk_size = x->sr * CHUNK_SECONDS;
    if (chunk_size <= 0)
        chunk_size = 44100 * CHUNK_SECONDS;

    x->allocated_size += chunk_size;
    garray_resize_long(x->array, x->allocated_size);

    // Refresh pointers immediately after resizing
    update_array_pointers(x);
}

// ─────────────────────────────────────
static void clock_cb_redraw(infinite_record *x) {
    if (!x->recording)
        return;

    if (update_array_pointers(x)) {
        garray_redraw(x->array);
    }

    // Schedule next redraw at a low rate to save CPU
    clock_delay(x->clock_redraw, 500);
}

// ─────────────────────────────────────
static void clock_cb_report(infinite_record *x) {
    if (x->recording && x->sr > 0) {
        float seconds = static_cast<float>(x->logical_size) / x->sr;
        outlet_float(x->outlet_report, seconds);
        clock_delay(x->clock_report, 1000);
    }
}

// ─────────────────────────────────────
static void infinite_record_stop(infinite_record *x) {
    if (!x->recording)
        return;

    x->recording = false;
    clock_unset(x->clock_resize);
    clock_unset(x->clock_redraw);
    clock_unset(x->clock_report);

    if (update_array_pointers(x)) {
        // 1. Apply Fade directly on the array memory
        if (x->fade_in_out && x->logical_size > 0) {
            int current_fade = x->fade_size_samples;

            // Prevent fade overlapping if recording is shorter than 2x fade length
            if (current_fade > x->logical_size / 2) {
                current_fade = x->logical_size / 2;
                pd_error(x, "[x.infinite.record~] Record too short; clamped fade to %d samples",
                         current_fade);
            }

            for (int i = 0; i < current_fade; i++) {
                float progress = static_cast<float>(i) / current_fade;
                // Fade In
                x->vec[i].w_float *= progress;
                // Fade Out
                x->vec[x->logical_size - 1 - i].w_float *= progress;
            }
        }

        // 2. Shrink array precisely to logical length
        garray_resize_long(x->array, x->logical_size);

        // 3. Final Redraw
        garray_redraw(x->array);
    }

    // Invalidate pointer for DSP safety when not recording
    x->vec = nullptr;
    x->vecsize = 0;
}

// ─────────────────────────────────────
static void infinite_record_start(infinite_record *x) {
    if (x->recording)
        return;

    if (!x->arrayname) {
        pd_error(x, "[x.infinite.record~] No array name specified.");
        return;
    }

    // Refresh samplerate just in case it changed
    x->sr = static_cast<int>(sys_getsr());
    if (x->sr <= 0)
        x->sr = 44100;

    if (!update_array_pointers(x)) {
        pd_error(x, "[x.infinite.record~] Array '%s' not found.", x->arrayname->s_name);
        return;
    }

    x->logical_size = 0;
    int chunk_size = x->sr * CHUNK_SECONDS;
    x->allocated_size = chunk_size;

    // Allocate initial chunk
    garray_resize_long(x->array, x->allocated_size);
    update_array_pointers(x);

    x->recording = true;
    x->resize_pending = false;

    // Boot clocks
    clock_delay(x->clock_redraw, 500);
    clock_delay(x->clock_report, 100);
}

// ─────────────────────────────────────
static void infinite_record_float(infinite_record *x, t_float f) {
    if (f == 0.0f)
        infinite_record_stop(x);
    else if (f == 1.0f)
        infinite_record_start(x);
    else
        pd_error(x, "[x.infinite.record~] Input must be 0 or 1");
}

// ─────────────────────────────────────
static void infinite_record_arrayname(infinite_record *x, t_symbol *s) {
    if (x->arrayname != NULL && strcmp(x->arrayname->s_name, s->s_name) == 0) {
        return;
    }

    if (x->recording) {
        pd_error(x, "[x.infinite.record~] Array switched during recording. Stopping previous.");
        infinite_record_stop(x);
    }
    x->arrayname = s;
    update_array_pointers(x);
}

// ─────────────────────────────────────
static void infinite_record_fade(infinite_record *x, t_floatarg f) { x->fade_in_out = (f != 0.0f); }

// ─────────────────────────────────────
static void infinite_record_fadesize(infinite_record *x, t_floatarg f) {
    x->fade_size_samples = std::max(1, static_cast<int>(f));
}

// ─────────────────────────────────────
static t_int *infinite_record_perform(t_int *w) {
    infinite_record *x = (infinite_record *)(w[1]);
    t_sample *in = (t_sample *)(w[2]);
    int n = (int)(w[3]);

    if (!x->recording || !x->vec) {
        return (w + 4);
    }

    // Determine how many samples we can safely write
    int available = x->vecsize - x->logical_size;
    int to_copy = (n < available) ? n : available;

    for (int i = 0; i < to_copy; i++) {
        x->vec[x->logical_size + i].w_float = in[i];
    }

    x->logical_size += to_copy;

    int warning_margin = x->sr * WARNING_SECONDS;
    if (x->logical_size + warning_margin >= x->vecsize && !x->resize_pending) {
        x->resize_pending = true;
        clock_delay(x->clock_resize, 0);
    }

    return (w + 4);
}

// ─────────────────────────────────────
static void infinite_record_dsp(infinite_record *x, t_signal **sp) {
    dsp_add(infinite_record_perform, 3, x, sp[0]->s_vec, sp[0]->s_n);
}

// ─────────────────────────────────────
static void *infinite_record_new(t_symbol *s, int argc, t_atom *argv) {
    infinite_record *x = (infinite_record *)pd_new(infinite_record_class);

    x->arrayname = nullptr;
    if (argc > 0 && argv[0].a_type == A_SYMBOL) {
        x->arrayname = atom_getsymbol(argv);
    }

    x->vec = nullptr;
    x->vecsize = 0;
    x->logical_size = 0;
    x->allocated_size = 0;
    x->recording = false;
    x->resize_pending = false;

    x->fade_in_out = false;
    x->fade_size_samples = 64;

    x->sr = static_cast<int>(sys_getsr());
    if (x->sr <= 0)
        x->sr = 44100;

    // Setup inlet/outlet
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, gensym("symbol"), gensym("arrayname"));
    x->outlet_report = outlet_new(&x->x_obj, &s_float);

    // Instantiate clocks
    x->clock_resize = clock_new(x, (t_method)clock_cb_resize);
    x->clock_redraw = clock_new(x, (t_method)clock_cb_redraw);
    x->clock_report = clock_new(x, (t_method)clock_cb_report);

    // Initial resolution attempt if name was provided
    update_array_pointers(x);

    return (x);
}

// ─────────────────────────────────────
static void infinite_record_free(infinite_record *x) {
    clock_free(x->clock_resize);
    clock_free(x->clock_redraw);
    clock_free(x->clock_report);
}

// ─────────────────────────────────────
extern "C" void setup_x0x2ei0x2erecord_tilde(void) {
    infinite_record_class = class_new(
        gensym("x.infinite.record~"), (t_newmethod)infinite_record_new,
        (t_method)infinite_record_free, sizeof(infinite_record), CLASS_DEFAULT, A_GIMME, 0);

    class_addcreator((t_newmethod)infinite_record_new, gensym("x.irecord~"), A_GIMME, 0);

    CLASS_MAINSIGNALIN(infinite_record_class, infinite_record, x_f);

    class_addfloat(infinite_record_class, infinite_record_float);
    class_addmethod(infinite_record_class, (t_method)infinite_record_dsp, gensym("dsp"), A_CANT, 0);

    class_addmethod(infinite_record_class, (t_method)infinite_record_arrayname, gensym("arrayname"),
                    A_SYMBOL, 0);
    class_addmethod(infinite_record_class, (t_method)infinite_record_start, gensym("start"),
                    A_NULL);
    class_addmethod(infinite_record_class, (t_method)infinite_record_stop, gensym("stop"), A_NULL);
    class_addmethod(infinite_record_class, (t_method)infinite_record_fade, gensym("fade"), A_FLOAT,
                    0);
    class_addmethod(infinite_record_class, (t_method)infinite_record_fadesize, gensym("fadesize"),
                    A_FLOAT, 0);
}
