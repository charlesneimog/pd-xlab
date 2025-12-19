#include <m_pd.h>
#include <string>

static t_class *neimog_arraysum;

// ─────────────────────────────────────
class arraysum {
  public:
    t_object obj;
    unsigned sumlast;
    std::string arrayname;
    t_outlet *out;
};

// ─────────────────────────────────────
static void arraysum_sum(arraysum *x) {
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

    int index = vecsize - x->sumlast;
    float sum = 0;
    for (int i = vecsize - x->sumlast; i < vecsize; i++) {
        sum += vec[i].w_float;
    }
    outlet_float(x->out, sum);
    return;
}

// ─────────────────────────────────────
static void *arraysum_new(t_symbol *s, t_float f) {
    arraysum *x = (arraysum *)pd_new(neimog_arraysum);
    x->arrayname = s->s_name;
    x->sumlast = f;
    x->out = outlet_new(&x->obj, &s_float);
    return (x);
}

// ─────────────────────────────────────
void arraysum_setup(void) {
    neimog_arraysum = class_new(gensym("a.sum"), (t_newmethod)arraysum_new, 0, sizeof(arraysum), 0,
                           A_SYMBOL, A_FLOAT, 0);

    class_addbang(neimog_arraysum, (t_method)arraysum_sum);
}
