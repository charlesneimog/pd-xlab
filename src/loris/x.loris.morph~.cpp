#include "m_pd.h"

t_class *loris_morph;

// ─────────────────────────────────────
typedef struct _loris_morph {
    t_object obj;
} t_loris_morph_tilde;

// ─────────────────────────────────────
void *loris_morph_new(t_symbol *s, int argc, t_atom *argv) {
    t_loris_morph_tilde *x = (t_loris_morph_tilde *)pd_new(loris_morph);

    return x;
}

// ─────────────────────────────────────
void loris_morph_free(t_loris_morph_tilde *x) {
    //
}

// ─────────────────────────────────────
extern "C" void setup_x0x2eloris0x2emorph_tilde(void) {
    loris_morph =
        class_new(gensym("x.loris.morph~"), (t_newmethod)loris_morph_new, (t_method)loris_morph_free,
                  sizeof(t_loris_morph_tilde), CLASS_DEFAULT, A_GIMME, 0);
}
