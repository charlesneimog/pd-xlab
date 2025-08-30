#include <m_pd.h>
#include <string>
#include <vector>

static t_class *infinite_record_class;

class infinite_record {
  public:
    t_object x_obj;
    t_sample x_f;
    t_clock *x_clock_warning;
    t_clock *x_clock_report;

    bool recording;
    bool fade_in_out;
    int fade_size_samples;

    std::string arrayname;
    t_word *vec;
    int vecsize;
    int write_index;
    int sr;

    std::vector<float> buffer;
    t_outlet *outlet_report;
};

// ─────────────────────────────────────
static void infinite_record_report(infinite_record *x) {
    if (x->recording) {
        float seconds = 0.0f;
        seconds = static_cast<float>(x->buffer.size()) / x->sr;
        outlet_float(x->outlet_report, seconds);
    }
}

// ─────────────────────────────────────
static void infinite_record_warning(infinite_record *x) {
    if (x->recording) {
        float seconds = 0.0f;
        if (x->sr > 0)
            seconds = static_cast<float>(x->buffer.size()) / x->sr;

        post("[infinite.record~] recording audio: %.2f seconds", seconds);
        clock_delay(x->x_clock_warning, 1000);
    }
}

// ─────────────────────────────────────
static void infinite_record_stop(infinite_record *x) {
    if (!x->vec)
        return;

    t_garray *array = (t_garray *)pd_findbyclass(gensym(x->arrayname.c_str()), garray_class);
    if (!array)
        return;

    int n = x->buffer.size();

    if (x->fade_in_out) {
        if (x->fade_size_samples > n - 1) {
            pd_error(
                x,
                "[infinite.record~] fade size is higher than the record, using fade of 32 samples");
            x->fade_size_samples = 32;
        }
        logpost(x, 2, "[infinite.record~] applying fade in/out of %d samples",
                x->fade_size_samples);

        for (int i = 0; i < x->fade_size_samples; i++) {
            float progress = (float)i / x->fade_size_samples;
            x->buffer[i] *= progress;
            x->buffer[i + n - x->fade_size_samples] *= (1.0f - progress);
        }
    }

    // Sempre redimensiona a array para exatamente o tamanho do buffer
    garray_resize_long(array, n);
    garray_getfloatwords(array, &x->vecsize, &x->vec);

    for (int i = 0; i < n; i++) {
        x->vec[i].w_float = x->buffer[i];
    }

    x->write_index = n;
    garray_redraw(array);
    x->buffer.clear();
}

// ─────────────────────────────────────
static void infinite_record_methods(infinite_record *x, t_symbol *s, int argc, t_atom *argv) {
    std::string method = s->s_name;
    if (method == "start") {
        x->recording = true;
        clock_delay(x->x_clock_warning, 0);
    } else if (method == "stop") {
        x->recording = false;
        infinite_record_stop(x);
    } else if (method == "fade") {
        float f = atom_getfloat(argv);
        if (f != 0) {
            x->fade_in_out = true;
        } else {
            x->fade_in_out = false;
        }
    } else if (method == "fadesize") {
        x->fade_size_samples = atom_getfloat(argv);
    }

    return;
}

// ─────────────────────────────────────
static t_int *infinite_record_perform(t_int *w) {
    infinite_record *x = (infinite_record *)(w[1]);
    t_sample *in = (t_sample *)(w[2]);
    int n = (int)(w[3]);

    if (!x->vec || !x->recording) {
        return (w + 4);
    }

    clock_delay(x->x_clock_report, 0);
    x->buffer.insert(x->buffer.end(), in, in + n);
    return (w + 4);
}

// ─────────────────────────────────────
static void infinite_record_dsp(infinite_record *x, t_signal **sp) {
    dsp_add(infinite_record_perform, 3, x, sp[0]->s_vec, sp[0]->s_n);
}

// ─────────────────────────────────────
static void *infinite_record_new(t_symbol *s, int argc, t_atom *argv) {
    infinite_record *x = (infinite_record *)pd_new(infinite_record_class);
    if (argc < 1 || argv[0].a_type != A_SYMBOL) {
        logpost(x, 1, "[infinite.record~] Please provide an array name");
        return nullptr;
    }

    x->arrayname = atom_getsymbol(argv)->s_name;
    x->write_index = 0;
    x->vec = nullptr;
    x->vecsize = 0;

    // Get sample rate for 1-second chunks
    x->sr = sys_getsr();
    if (x->sr <= 0)
        x->sr = 44100; // default

    // Initialize array pointer
    t_garray *array = (t_garray *)pd_findbyclass(gensym(x->arrayname.c_str()), garray_class);
    if (array) {
        garray_getfloatwords(array, &x->vecsize, &x->vec);
    }
    if (!x->vec) {
        logpost(x, 1, "[infinite.record~] Array not found");
        return nullptr;
    }

    x->fade_in_out = false;
    x->fade_size_samples = 64;
    x->outlet_report = outlet_new(&x->x_obj, &s_float);

    // warning clock
    x->x_clock_warning = clock_new(x, (t_method)infinite_record_warning);
    x->x_clock_report = clock_new(x, (t_method)infinite_record_report);
    return (x);
}

// ─────────────────────────────────────
static void infinite_record_free(infinite_record *x) {}

// ─────────────────────────────────────
void infinite0x2erecord_tilde_setup(void) {
    infinite_record_class = class_new(gensym("infinite.record~"), (t_newmethod)infinite_record_new,
                                      (t_method)infinite_record_free, sizeof(infinite_record),
                                      CLASS_DEFAULT, A_GIMME, 0);

    CLASS_MAINSIGNALIN(infinite_record_class, infinite_record, x_f);
    class_addmethod(infinite_record_class, (t_method)infinite_record_dsp, gensym("dsp"), A_CANT, 0);

    class_addmethod(infinite_record_class, (t_method)infinite_record_methods, gensym("start"),
                    A_GIMME, 0);
    class_addmethod(infinite_record_class, (t_method)infinite_record_methods, gensym("stop"),
                    A_GIMME, 0);
    class_addmethod(infinite_record_class, (t_method)infinite_record_methods, gensym("fade"),
                    A_GIMME, 0);
    class_addmethod(infinite_record_class, (t_method)infinite_record_methods, gensym("fadesize"),
                    A_GIMME, 0);
}
