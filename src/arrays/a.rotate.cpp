#include <algorithm>
#include <m_pd.h>
#include <string>
#include <vector>

static t_class *neimog_arrayrotate;

// ─────────────────────────────────────
class arrayrotate {
  public:
    t_object obj;
    unsigned redrawat;
    unsigned redrawcount;
    std::string arrayname;
};

static void arrayrotate_redraw(arrayrotate *x, t_float f) {
    x->redrawat = f;
    return;
}

// ─────────────────────────────────────
static void arrayrotate_rotate(arrayrotate *x, t_symbol *s, int argc, t_atom *argv) {

    for (int i = 0; i < argc; i++) {
        if (argv[i].a_type != A_FLOAT) {
            pd_error(x, "All arguments must be floats");
            return;
        }
    }

    t_garray *array;
    int vecsize;
    t_word *vec;

    t_symbol *pd_symbol = gensym(x->arrayname.c_str());
    if (!(array = (t_garray *)pd_findbyclass(pd_symbol, garray_class))) {
        pd_error(x, "[Python] Array %s not found.", x->arrayname.c_str());
        return;
    } else if (!garray_getfloatwords(array, &vecsize, &vec)) {
        pd_error(x, "[Python] Bad template for tabwrite '%s'.", x->arrayname.c_str());
        return;
    }

    std::vector<float> inBuffer;
    for (int i = 0; i < vecsize; i++) {
        inBuffer.push_back(vec[i].w_float);
    }
    std::rotate(inBuffer.begin(), inBuffer.begin() + argc, inBuffer.end());

    for (int i = 0; i < vecsize; i++) {
        vec[i].w_float = inBuffer[i];
    }

    for (int i = 0; i < argc; i++) {
        int index = vecsize - argc + i;
        vec[index].w_float = atom_getfloat(argv + i);
    }

    x->redrawcount++;
    if (x->redrawcount >= x->redrawat) {
        garray_redraw(array);
        x->redrawcount = 0;
    }
    return;
}

// ─────────────────────────────────────
static void *arrayrotate_new(t_symbol *s) {
    arrayrotate *x = (arrayrotate *)pd_new(neimog_arrayrotate);
    x->arrayname = s->s_name;
    return (x);
}

// ─────────────────────────────────────
void arrayrotate_setup(void) {
    neimog_arrayrotate = class_new(gensym("a.rotate"), (t_newmethod)arrayrotate_new, 0,
                                   sizeof(arrayrotate), 0, A_SYMBOL, 0);

    class_addlist(neimog_arrayrotate, (t_method)arrayrotate_rotate);
    class_addmethod(neimog_arrayrotate, (t_method)arrayrotate_redraw, gensym("redraw"), A_FLOAT, 0);
}
