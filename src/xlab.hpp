#include <m_pd.h>

#include <m_imp.h>

void arrayrotate_setup(void);
void arraysum_setup(void);
void arrayappend_setup(void);

void kldivergence_setup(void);
void renyi_setup(void);
void euclidean_setup(void);
void entropy_setup(void);
void kalman_setup(void);

// ╭─────────────────────────────────────╮
// │                UTILS                │
// ╰─────────────────────────────────────╯
void infinite0x2erecord_tilde_setup(void);

// ╭─────────────────────────────────────╮
// │           Library Objects           │
// ╰─────────────────────────────────────╯

class xlab {
  public:
    xlab() {};
    t_object Obj;

  private:
};
