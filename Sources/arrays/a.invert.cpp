#include <m_pd.h>
#include <string>

static t_class *neimog_arrayinvert;

// ─────────────────────────────────────
class arrayinvert {
  public:
    t_object obj;
    unsigned sumlast;
    std::string arrayname;
    t_outlet *out;
};

// ─────────────────────────────────────
static void arrayinvert_bang(arrayinvert *x) {
    t_garray *array;
    int vecsize;
    t_word *vec;

    t_symbol *pd_symbol = gensym(x->arrayname.c_str());
    if (!(array = (t_garray *)pd_findbyclass(pd_symbol, garray_class))) {
        pd_error(x, "[a.invert] array %s not found.", x->arrayname.c_str());
        return;
    } else if (!garray_getfloatwords(array, &vecsize, &vec)) {
        pd_error(x, "[a.invert] Bad template for tabwrite '%s'.", x->arrayname.c_str());
        return;
    }

    return;
}

// ─────────────────────────────────────
static void *arrayinvert_new(t_symbol *s, t_float f) {
    arrayinvert *x = (arrayinvert *)pd_new(neimog_arrayinvert);
    return (x);
}

// ─────────────────────────────────────
void arrayinvert_setup(void) {
    neimog_arrayinvert = class_new(gensym("a.invert"), (t_newmethod)arrayinvert_new, 0,
                                   sizeof(arrayinvert), 0, A_SYMBOL, A_FLOAT, 0);

    class_addbang(neimog_arrayinvert, (t_method)arrayinvert_bang);
}
