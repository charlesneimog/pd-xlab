#include "dddd.h"
#include "m_pd.h"

#include "Analyzer.h"

static t_class *loris_anal_class;

// ─────────────────────────────────────
typedef struct _loris_anal {
    t_object x_obj;
    t_sample f;

    t_outlet *out;

} t_loris_anal;

// ─────────────────────────────────────
static void loris_anal_anal(t_loris_anal *x, t_symbol *s, int argc, t_atom *argv) {
    if (argc < 1 || argv[0].a_type != A_SYMBOL) {
        pd_error(x, "[x.loris.anal] expected array name");
        return;
    }

    t_symbol *name = atom_getsymbol(argv);
    auto *array = reinterpret_cast<t_garray *>(pd_findbyclass(name, garray_class));

    if (!array) {
        pd_error(x, "[x.loris.anal] array '%s' not found", name->s_name);
        return;
    }

    int size = 0;
    t_word *samples = nullptr;

    if (!garray_getfloatwords(array, &size, &samples) || size <= 0) {
        pd_error(x, "[x.loris.anal] invalid array '%s'", name->s_name);
        return;
    }

    double sr = sys_getsr();
    if (sr <= 0) {
        pd_error(x, "[x.loris.anal] invalid sample rate");
        return;
    }

    std::vector<double> audio(size);

    for (int i = 0; i < size; i++) {
        audio[i] = samples[i].w_float;
    }

    Loris::Analyzer analyzer(50.0);
    Loris::PartialList partials = analyzer.analyze(audio, sr);

    // Start transfer
    outlet_anything(x->out, gensym("begin"), 0, nullptr);

    for (Loris::PartialList::const_iterator p = partials.begin(); p != partials.end(); ++p) {

        t_atom label;
        SETFLOAT(&label, p->label());

        outlet_anything(x->out, gensym("partial"), 1, &label);

        // 64 breakpoints per message
        t_atom atoms[64 * 5];
        int count = 0;

        for (Loris::Partial::const_iterator b = p->begin(); b != p->end(); ++b) {

            const Loris::Breakpoint &bp = b.breakpoint();
            int i = count * 5;

            SETFLOAT(&atoms[i + 0], b.time());
            SETFLOAT(&atoms[i + 1], bp.frequency());
            SETFLOAT(&atoms[i + 2], bp.amplitude());
            SETFLOAT(&atoms[i + 3], bp.phase());
            SETFLOAT(&atoms[i + 4], bp.bandwidth());

            count++;

            if (count == 64) {
                outlet_anything(x->out, gensym("points"), count * 5, atoms);
                count = 0;
            }
        }

        if (count > 0) {
            outlet_anything(x->out, gensym("points"), count * 5, atoms);
        }
    }

    // Finish transfer
    outlet_anything(x->out, gensym("finish"), 0, nullptr);
}

// ─────────────────────────────────────
static void loris_anal_free(t_loris_anal *x) {}

// ─────────────────────────────────────
static void *loris_anal_new(t_symbol *, int argc, t_atom *argv) {
    auto *x = reinterpret_cast<t_loris_anal *>(pd_new(loris_anal_class));
    x->out = outlet_new(&x->x_obj, gensym("anything"));

    return x;
}

// ─────────────────────────────────────
extern "C" void setup_x0x2eloris0x2eanal(void) {
    loris_anal_class =
        class_new(gensym("x.loris.anal"), (t_newmethod)loris_anal_new, (t_method)loris_anal_free,
                  sizeof(t_loris_anal), CLASS_DEFAULT, A_GIMME, 0);

    class_addmethod(loris_anal_class, (t_method)loris_anal_anal, gensym("anal"), A_GIMME, 0);
}
