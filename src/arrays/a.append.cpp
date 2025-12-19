#include <m_pd.h>
#include <string>

#define NEIMOG_MAXARRAYSIZE 10000000

static t_class *neimog_arrayappend;

// ─────────────────────────────────────
class arrayappend {
  public:
    t_object obj;
    std::string arrayname;
    int index;
    int arraysize;
    int resize_step;
};

// ─────────────────────────────────────
static void arrayappend_float(arrayappend *x, t_float f) {
    t_garray *array;
    int vecsize;
    t_word *vec;

    t_symbol *pd_symbol = gensym(x->arrayname.c_str());

    if (!(array = (t_garray *)pd_findbyclass(pd_symbol, garray_class))) {
        pd_error(x, "[a.append] Array %s not found.", x->arrayname.c_str());
        return;
    } else if (!garray_getfloatwords(array, &vecsize, &vec)) {
        pd_error(x, "[a.append] Bad template for tabwrite '%s'.", x->arrayname.c_str());
        return;
    }

    x->arraysize = garray_npoints(array);
    if (x->arraysize + x->resize_step > NEIMOG_MAXARRAYSIZE) {
        pd_error(x, "[a.append] Array too big.");
        x->index = 0;
        return;
    }

    if (x->arraysize - 1 < x->index) {
        pd_error(x, "[a.append] Array too small.");
        return;
    }

    if (x->arraysize - 1 == x->index) {
        garray_resize_long(array, x->resize_step + 100);
        garray_redraw(array);
        if (!(array = (t_garray *)pd_findbyclass(pd_symbol, garray_class))) {
            pd_error(x, "[a.append] Array %s not found.", x->arrayname.c_str());
            return;
        } else if (!garray_getfloatwords(array, &vecsize, &vec)) {
            pd_error(x, "[a.append] Bad template for tabwrite '%s'.", x->arrayname.c_str());
            return;
        }
        if (!(array = (t_garray *)pd_findbyclass(pd_symbol, garray_class))) {
            pd_error(x, "[a.append] a.arrayappend %s not found.", x->arrayname.c_str());
            return;
        }

        x->arraysize = garray_npoints(array);
    }

    // vec[x->index].w_float = f;
    x->index++;
}

// ─────────────────────────────────────
static void *arrayappend_new(t_symbol *s) {
    arrayappend *x = (arrayappend *)pd_new(neimog_arrayappend);
    t_garray *array;
    x->arrayname = s->s_name;
    x->index = 0;
    x->resize_step = 100;
    if (!(array = (t_garray *)pd_findbyclass(s, garray_class))) {
        pd_error(x, "[a.append] a.arrayappend %s not found.", x->arrayname.c_str());
        return NULL;
    }

    x->arraysize = garray_npoints(array);

    garray_setsaveit(array, 0);
    return x;
}

// ─────────────────────────────────────
void arrayappend_setup(void) {
    neimog_arrayappend = class_new(gensym("a.append"), (t_newmethod)arrayappend_new, 0,
                                   sizeof(arrayappend), 0, A_SYMBOL, 0);
    class_addfloat(neimog_arrayappend, (t_method)arrayappend_float);
}
