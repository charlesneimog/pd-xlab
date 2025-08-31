#include <m_pd.h>
#include <m_imp.h>

// ╭─────────────────────────────────────╮
// │            Array Objects            │
// ╰─────────────────────────────────────╯
void arrayrotate_setup(void);
void arraysum_setup(void);
void arrayappend_setup(void);

// ╭─────────────────────────────────────╮
// │            Manipulations            │
// ╰─────────────────────────────────────╯
void transposer_tilde_setup(void); // NOT WORK

// ╭─────────────────────────────────────╮
// │               Samples               │
// ╰─────────────────────────────────────╯
extern "C" void musesampler_tilde_setup(void);

// ╭─────────────────────────────────────╮
// │           Espacialização            │
// ╰─────────────────────────────────────╯
extern "C" void ambi_tilde_setup(void);

// ╭─────────────────────────────────────╮
// │  Information Theory and Statistics  │
// ╰─────────────────────────────────────╯
void kldivergence_setup(void);
void renyi_setup(void);
void euclidean_setup(void);
void entropy_setup(void);
void kalman_setup(void);

// ╭─────────────────────────────────────╮
// │                 MIR                 │
// ╰─────────────────────────────────────╯
void bock_tilde_setup(void);
void nonset_tilde_setup(void);
extern "C" void onsetsds_tilde_setup(void);

// ╭─────────────────────────────────────╮
// │               PLUGINS               │
// ╰─────────────────────────────────────╯
extern "C" void patcherize_setup(void);

// ╭─────────────────────────────────────╮
// │                UTILS                │
// ╰─────────────────────────────────────╯
void infinite0x2erecord_tilde_setup(void);

// ╭─────────────────────────────────────╮
// │           Library Objects           │
// ╰─────────────────────────────────────╯

class neimog {
  public:
    neimog() {};

    t_object Obj;

  private:
};
