#include <m_pd.h>
#include <math.h>
#include <vector>
#include <map>

static t_class *Entropy;

// ─────────────────────────────────────
class EntropyObj {
  public:
    t_object obj;
    t_outlet *out;
};


// ─────────────────────────────────────
static void entropy_list(EntropyObj *x, t_symbol *s, int argc, t_atom *argv) {
    if (argc == 0) {
        post("[entropy_list] Lista vazia, entropia = 0");
        outlet_float(x->out, 0.0);
        return;
    }
    
    std::map<float, int> frequencias;
    int totalElementos = argc;
    
    // Contar frequência de cada número
    for (int i = 0; i < argc; i++) {
        float number = atom_getfloat(argv + i);
        frequencias[number]++;
    }
    
    // Calcular entropia
    double entropia = 0.0;
    for (const auto& [valor, freq] : frequencias) {
        double prob = static_cast<double>(freq) / totalElementos;
        entropia -= prob * log2(prob);
    }
    
    // Normalizar entre 0 e 1
    double entropiaMax = log2(totalElementos);
    double entropiaNormalizada = (totalElementos > 1) ? entropia / entropiaMax : 0.0;
    
    // Enviar a entropia normalizada como saída do objeto
    outlet_float(x->out, entropiaNormalizada);
}

// ─────────────────────────────────────
static void *entropy_new(t_symbol *s, int argc, t_atom *argv) {
    EntropyObj *x = (EntropyObj *)pd_new(Entropy);
    x->out = outlet_new(&x->obj, &s_anything);
    return (x);
}

// ─────────────────────────────────────
void entropy_setup(void) {
    Entropy = class_new(gensym("entropy"), (t_newmethod)entropy_new, 0, sizeof(EntropyObj),
                          0, A_GIMME, 0);
    class_addcreator((t_newmethod)entropy_new, gensym("entropy~"), A_GIMME, 0);
    class_addlist(Entropy, entropy_list);
}

