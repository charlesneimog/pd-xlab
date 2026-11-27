#include "ipc.hpp"
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <new>
#include <vector>
#include <cstddef>
#include "apitypes.h"

// MuseSampler 0.106 still exports the legacy live-play entrypoints.
// Their declarations were removed from the current apitypes.h.
struct Api {
    void *lib = nullptr;

    ms_init_3 init = nullptr;
    ms_deinit deinit = nullptr;

    ms_get_instrument_list instruments = nullptr;
    ms_InstrumentList_get_next next = nullptr;
    ms_Instrument_get_id id = nullptr;
    ms_Instrument_get_name name = nullptr;

    ms_MuseSampler_create create = nullptr;
    ms_MuseSampler_destroy destroy = nullptr;
    ms_MuseSampler_init_2 configure = nullptr;
    ms_MuseSampler_add_track add_track = nullptr;
    ms_MuseSampler_process process = nullptr;
    ms_MuseSampler_all_notes_off all_off = nullptr;

    ms_MuseSampler_start_audition_note_5 start_note = nullptr;
    ms_MuseSampler_stop_audition_note stop_note = nullptr;
};

template <typename T> static bool load(void *lib, const char *name, T &fn) {
    fn = reinterpret_cast<T>(dlsym(lib, name));
    if (!fn) {
        fprintf(stderr, "[mscore helper] missing %s\n", name);
    }
    return fn != nullptr;
}

static bool load_api(Api &a, const char *path) {
    a.lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);

    if (!a.lib) {
        fprintf(stderr, "[mscore helper] dlopen: %s\n", dlerror());
        return false;
    }

    bool ok = true;

#define LOAD(member, symbol) ok = load(a.lib, symbol, a.member) && ok

    LOAD(init, "ms_init_3");
    LOAD(deinit, "ms_deinit");

    LOAD(instruments, "ms_get_instrument_list");
    LOAD(next, "ms_InstrumentList_get_next");
    LOAD(id, "ms_Instrument_get_id");
    LOAD(name, "ms_Instrument_get_name");

    LOAD(create, "ms_MuseSampler_create");
    LOAD(destroy, "ms_MuseSampler_destroy");
    LOAD(configure, "ms_MuseSampler_init_2");
    LOAD(add_track, "ms_MuseSampler_add_track");
    LOAD(process, "ms_MuseSampler_process");
    LOAD(all_off, "ms_MuseSampler_all_notes_off");

    LOAD(start_note, "ms_MuseSampler_start_audition_note_5");
    LOAD(stop_note, "ms_MuseSampler_stop_audition_note");

#undef LOAD

    return ok;
}

struct Engine {
    Api &api;
    void *sampler = nullptr;
    void *track = nullptr;
    int instrument = -1;
    long long position = 0;
    int sample_rate;
    explicit Engine(Api &a, int sr) : api(a), sample_rate(sr) {}
    void clear() {
        if (sampler) {
            if (track) {
                api.all_off(sampler);
            }

            api.destroy(sampler);
        }

        sampler = nullptr;
        track = nullptr;
        instrument = -1;
        position = 0;
    }
    ~Engine() {
        clear();
    }
    bool select(int wanted) {
        clear();

        sampler = api.create();

        if (!sampler) {
            return false;
        }

        if (api.configure(sampler, sample_rate, msipc::BLOCK, 2) != ms_Result_OK) {
            clear();
            return false;
        }

        track = api.add_track(sampler, wanted);

        if (!track) {
            clear();
            return false;
        }

        instrument = wanted;

        fprintf(stderr, "[helper] instrument %d ready\n", wanted);

        return true;
    }
};

static void handle(Engine &engine, msipc::Shared *shared, msipc::Command cmd) {
    switch (cmd.type) {
    case msipc::SELECT:
        engine.select(cmd.pitch);
        break;

    case msipc::NOTE:
        if (!engine.sampler || !engine.track) {
            break;
        }

        if (cmd.velocity > 0) {
            ms_AuditionStartNoteEvent_5 event{};

            event._pitch = cmd.pitch;
            event._offset_cents = cmd.cents;
            event._dynamics = cmd.velocity / 127.0;
            event._articulation = ms_NoteArticulation_None;
            event._articulation_2 = ms_NoteArticulation2_None;
            event._notehead = ms_NoteHead_Normal;
            event._active_presets = "";
            event._active_text_articulation = "";
            event._active_syllable = "";

            engine.api.start_note(engine.sampler, engine.track, event);
        } else {
            ms_AuditionStopNoteEvent event{};
            event._pitch = cmd.pitch;

            engine.api.stop_note(engine.sampler, engine.track, event);
        }
        break;

    case msipc::RESET:
        if (engine.sampler) {
            engine.api.all_off(engine.sampler);
        }
        break;

    case msipc::LIST: {
        auto instruments = engine.api.instruments();

        if (instruments) {
            while (auto inst = engine.api.next(instruments)) {
                const char *name = engine.api.name(inst);

                fprintf(stderr, "[helper] %d %s\n", engine.api.id(inst), name ? name : "");
            }
        }
        break;
    }
    }

    (void)shared;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: mscore /shared-memory-name\n");
        return 2;
    }
    int fd = shm_open(argv[1], O_RDWR, 0600);
    if (fd < 0) {
        perror("shm_open");
        return 2;
    }
    void *p = mmap(nullptr, sizeof(msipc::Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        perror("mmap");
        return 2;
    }
    auto *shared = static_cast<msipc::Shared *>(p);
    if (shared->magic != msipc::MAGIC || !shared->cmd_write.is_lock_free() ||
        !shared->audio_write.is_lock_free()) {
        fprintf(stderr, "[mscore helper] incompatible shared memory\n");
        return 2;
    }
    const char *path = getenv("MUSESAMPLER_LIBRARY");
    char default_path[4096] = {};
    if (!path) {
        const char *home = getenv("HOME");
        if (!home) {
            shared->status.store(-99);
            return 2;
        }
        snprintf(default_path, sizeof(default_path),
                 "%s/.local/share/MuseSampler/lib/libMuseSamplerCoreLib.so", home);
        path = default_path;
    }
    Api api;
    if (!load_api(api, path)) {
        shared->status.store(-98);
        return 2;
    }
    int init_result = api.init();
    if (init_result != 0) {
        fprintf(stderr, "[mscore helper] ms_init_3 = %d\n", init_result);
        shared->status.store(init_result);
        dlclose(api.lib);
        return 1;
    }
    shared->status.store(1);
    fprintf(stderr, "[mscore helper] initialized at %u Hz\n", shared->sample_rate);
    {
        Engine engine(api, shared->sample_rate);
        float left[msipc::BLOCK] = {}, right[msipc::BLOCK] = {};
        float *channels[] = {left, right};
        const struct timespec delay = {0, 1000000};
        while (!shared->stop.load(std::memory_order_acquire)) {
            msipc::Command cmd;
            while (msipc::receive(shared, &cmd)) {
                handle(engine, shared, cmd);
            }
            uint32_t fill = shared->audio_write.load(std::memory_order_acquire) -
                            shared->audio_read.load(std::memory_order_acquire);
            if (fill < msipc::TARGET_FRAMES && fill + msipc::BLOCK <= msipc::AUDIO_CAPACITY) {
                std::memset(left, 0, sizeof(left));
                std::memset(right, 0, sizeof(right));
                if (engine.sampler) {
                    ms_OutputBuffer output{channels, int(msipc::BLOCK), 2};
                    int result = api.process(engine.sampler, output, 0);
                    engine.position += msipc::BLOCK;
                    if (result != ms_Result_OK) {
                        std::memset(left, 0, sizeof(left));
                        std::memset(right, 0, sizeof(right));
                    }
                }
                for (uint32_t i = 0; i < msipc::BLOCK; ++i) {
                    msipc::push_frame(shared, {left[i], right[i]});
                }
            } else {
                nanosleep(&delay, nullptr);
            }
        }
    }
    api.deinit();
    dlclose(api.lib);
    munmap(shared, sizeof(msipc::Shared));
    return 0;
}
