#include "ipc.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits>
#include <m_pd.h>
#include <new>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

t_class *musesamplerClass = nullptr;
extern "C" void x0x2emuse_tilde_setup();

struct State {
    msipc::Shared *shared = nullptr;
    pid_t child = -1;
    int sample_rate = 0;
    int block_size = 0;
    int instrument = -1;
    std::string shm_name;
    bool launched = false;
    unsigned drop_count = 0;
};

struct Object {
    t_object object;
    State *state;
};

static std::string helper_path() {
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void *>(&x0x2emuse_tilde_setup), &info) || !info.dli_fname) {
        return "mscore";
    }
    std::string file = info.dli_fname;
    size_t slash = file.find_last_of('/');
    if (slash == std::string::npos) {
        return "./mscore";
    }
    return file.substr(0, slash + 1) + "mscore";
}

static void shutdown(State *s) {
    if (!s) {
        return;
    }
    if (s->shared) {
        s->shared->stop.store(1, std::memory_order_release);
    }
    if (s->child > 0) {
        // Called only during DSP graph rebuild/object destruction, never in perform().
        int status = 0;
        pid_t result = waitpid(s->child, &status, WNOHANG);
        if (result == 0) {
            kill(s->child, SIGTERM);
            waitpid(s->child, &status, 0);
        }
    }
    if (s->shared) {
        munmap(s->shared, sizeof(msipc::Shared));
        s->shared = nullptr;
    }
    if (!s->shm_name.empty()) {
        shm_unlink(s->shm_name.c_str());
    }
    s->shm_name.clear();
    s->child = -1;
    s->launched = false;
}

static bool start(Object *x, int sample_rate) {
    State *s = x->state;
    shutdown(s);
    static std::atomic<unsigned> serial{0};
    char name[128];
    snprintf(name, sizeof(name), "/pd-musesampler-%ld-%u", long(getpid()), ++serial);
    s->shm_name = name;
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        pd_error(x, "[musesampler~] shm_open: %s", strerror(errno));
        return false;
    }
    if (ftruncate(fd, sizeof(msipc::Shared)) < 0) {
        pd_error(x, "[musesampler~] ftruncate: %s", strerror(errno));
        close(fd);
        shm_unlink(name);
        s->shm_name.clear();
        return false;
    }
    void *p = mmap(nullptr, sizeof(msipc::Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        pd_error(x, "[musesampler~] mmap: %s", strerror(errno));
        shm_unlink(name);
        s->shm_name.clear();
        return false;
    }
    s->shared = new (p) msipc::Shared;
    s->shared->sample_rate = sample_rate;
    std::string helper = helper_path();
    char *args[] = {const_cast<char *>(helper.c_str()), name, nullptr};
    pid_t pid = -1;
    int err = posix_spawn(&pid, helper.c_str(), nullptr, nullptr, args, environ);
    if (err != 0) {
        pd_error(x, "[musesampler~] posix_spawn(%s): %s", helper.c_str(), strerror(err));
        shutdown(s);
        return false;
    }
    s->child = pid;
    s->launched = true;
    if (s->instrument >= 0) {
        msipc::send(s->shared, {msipc::SELECT, s->instrument, 0, 0});
    }
    post("[musesampler~] started helper pid %d", int(pid));
    return true;
}

static void command(Object *x, msipc::Command c) {
    State *s = x->state;
    if (s && s->shared && !msipc::send(s->shared, c)) {
        pd_error(x, "[musesampler~] command ring full");
    }
}

static void instruments(Object *x) { command(x, {msipc::LIST, 0, 0, 0}); }
static void get(Object *x, t_symbol *key) {
    if (key == gensym("instruments")) {
        instruments(x);
    }
}
static void select_instrument(Object *x, t_floatarg v) {
    if (!std::isfinite(v) || v < 0 || v > std::numeric_limits<int>::max() || std::trunc(v) != v) {
        pd_error(x, "[musesampler~] expected integer instrument ID");
        return;
    }
    State *s = x->state;
    s->instrument = int(v);
    command(x, {msipc::SELECT, s->instrument, 0, 0});
}
static void set(Object *x, t_symbol *, int argc, t_atom *argv) {
    if (argc == 2 && argv[0].a_type == A_SYMBOL && atom_getsymbol(argv) == gensym("instrument")) {
        --argc;
        ++argv;
    }
    if (argc == 1 && argv[0].a_type == A_FLOAT) {
        select_instrument(x, atom_getfloat(argv));
    } else {
        pd_error(x, "[musesampler~] expected 'set instrument <id>'");
    }
}
static void note(Object *x, t_floatarg pitch, t_floatarg velocity, t_floatarg cents) {
    if (!std::isfinite(pitch) || !std::isfinite(velocity) || !std::isfinite(cents) || pitch < 0 ||
        pitch > 127 || velocity < 0 || velocity > 127 || cents < -100 || cents > 100) {
        pd_error(x, "[musesampler~] invalid note arguments");
        return;
    }
    command(x, {msipc::NOTE, int(pitch), int(velocity), int(cents)});
}
static void note_off(Object *x, t_floatarg pitch, t_floatarg) { note(x, pitch, 0, 0); }
static void list(Object *x, t_symbol *, int argc, t_atom *argv) {
    if (argc == 2 && argv[0].a_type == A_FLOAT && argv[1].a_type == A_FLOAT) {
        note(x, atom_getfloat(argv), atom_getfloat(argv + 1), 0);
    }
}
static void reset(Object *x) { command(x, {msipc::RESET, 0, 0, 0}); }

static t_int *perform(t_int *w) {
    auto *x = reinterpret_cast<Object *>(w[1]);
    auto *left = reinterpret_cast<t_sample *>(w[2]);
    auto *right = reinterpret_cast<t_sample *>(w[3]);
    const int frames = int(w[4]);
    auto *s = x->state;
    if (!s || !s->shared || s->shared->status.load(std::memory_order_acquire) != 1) {
        std::fill_n(left, frames, 0);
        std::fill_n(right, frames, 0);
        return w + 5;
    }
    for (int i = 0; i < frames; ++i) {
        msipc::Frame frame{};
        if (!msipc::pop_frame(s->shared, &frame)) {
            ++s->drop_count;
        }
        left[i] = frame.left;
        right[i] = frame.right;
    }
    return w + 5;
}

static void dsp(Object *x, t_signal **sp) {
    State *s = x->state;
    const int sr = int(sp[0]->s_sr);
    const int frames = sp[0]->s_n;
    if (sr > 0 && (s->sample_rate != sr || !s->launched)) {
        s->sample_rate = sr;
        start(x, sr);
    }
    s->block_size = frames;
    dsp_add(perform, 4, x, sp[0]->s_vec, sp[1]->s_vec, t_int(frames));
}

static void status(Object *x) {
    State *s = x->state;

    if (!s || !s->shared) {
        post("[musesampler~] helper not running");
        return;
    }

    auto *sh = s->shared;

    post("Status: %d", sh->status.load());
    post("Commands written: %u", sh->cmd_write.load());
    post("Commands read: %u", sh->cmd_read.load());
    post("Audio written: %u", sh->audio_write.load());
    post("Audio read: %u", sh->audio_read.load());
    post("Underruns: %u", s->drop_count);
}

static void *create(t_symbol *, int argc, t_atom *argv) {
    auto *x = reinterpret_cast<Object *>(pd_new(musesamplerClass));
    x->state = new State;
    outlet_new(&x->object, &s_signal);
    outlet_new(&x->object, &s_signal);
    if (argc == 2 && argv[0].a_type == A_SYMBOL && argv[1].a_type == A_FLOAT &&
        (atom_getsymbol(argv) == gensym("-inst") || atom_getsymbol(argv) == gensym("-i"))) {
        select_instrument(x, atom_getfloat(argv + 1));
    } else if (argc) {
        pd_error(x, "[musesampler~] expected '-inst <id>'");
    }
    return x;
}
static void destroy(Object *x) {
    shutdown(x->state);
    delete x->state;
}

extern "C" void x0x2emuse_tilde_setup() {
    musesamplerClass = class_new(gensym("x.muse~"), (t_newmethod)create, (t_method)destroy,
                                 sizeof(Object), CLASS_DEFAULT, A_GIMME, 0);
    class_addmethod(musesamplerClass, (t_method)dsp, gensym("dsp"), A_CANT, 0);
    class_addmethod(musesamplerClass, (t_method)instruments, gensym("instruments"), A_NULL);
    class_addmethod(musesamplerClass, (t_method)get, gensym("get"), A_SYMBOL, 0);
    class_addmethod(musesamplerClass, (t_method)set, gensym("set"), A_GIMME, 0);
    class_addmethod(musesamplerClass, (t_method)note, gensym("noteon"), A_FLOAT, A_FLOAT,
                    A_DEFFLOAT, 0);
    class_addmethod(musesamplerClass, (t_method)note_off, gensym("noteoff"), A_FLOAT, A_DEFFLOAT,
                    0);
    class_addmethod(musesamplerClass, (t_method)reset, gensym("reset"), A_NULL);
    class_addmethod(musesamplerClass, (t_method)status, gensym("status"), A_NULL);
    class_addlist(musesamplerClass, (t_method)list);
}
