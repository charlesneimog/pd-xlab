#pragma once
#include <atomic>
#include <cstdint>

namespace msipc {

constexpr uint32_t MAGIC = 0x4d534950;
constexpr uint32_t COMMAND_CAPACITY = 256;
constexpr uint32_t AUDIO_CAPACITY = 16384;
constexpr uint32_t BLOCK = 256;
constexpr uint32_t TARGET_FRAMES = 256;

enum CommandType : int32_t { SELECT = 1, NOTE = 2, RESET = 3, LIST = 4 };
struct Command {
    int32_t type;
    int32_t pitch;
    int32_t velocity;
    int32_t cents;
};
struct Frame {
    float left;
    float right;
};
// Linux x86-64: shared lock-free atomics; single producer / single consumer per ring.
struct alignas(64) Shared {
    uint32_t magic = MAGIC;
    uint32_t sample_rate = 48000;
    std::atomic<uint32_t> stop{0};
    std::atomic<int32_t> status{0}; // 0 startup, 1 initialized, negative error
    std::atomic<uint32_t> cmd_write{0};
    std::atomic<uint32_t> cmd_read{0};
    Command commands[COMMAND_CAPACITY]{};
    std::atomic<uint32_t> audio_write{0};
    std::atomic<uint32_t> audio_read{0};
    Frame audio[AUDIO_CAPACITY]{};
};
inline bool send(Shared *s, Command cmd) {
    uint32_t w = s->cmd_write.load(std::memory_order_relaxed);
    uint32_t r = s->cmd_read.load(std::memory_order_acquire);
    if (w - r >= COMMAND_CAPACITY) {
        return false;
    }
    s->commands[w % COMMAND_CAPACITY] = cmd;
    s->cmd_write.store(w + 1, std::memory_order_release);
    return true;
}
inline bool receive(Shared *s, Command *cmd) {
    uint32_t r = s->cmd_read.load(std::memory_order_relaxed);
    if (r == s->cmd_write.load(std::memory_order_acquire)) {
        return false;
    }
    *cmd = s->commands[r % COMMAND_CAPACITY];
    s->cmd_read.store(r + 1, std::memory_order_release);
    return true;
}
inline bool push_frame(Shared *s, Frame frame) {
    uint32_t w = s->audio_write.load(std::memory_order_relaxed);
    uint32_t r = s->audio_read.load(std::memory_order_acquire);
    if (w - r >= AUDIO_CAPACITY) {
        return false;
    }
    s->audio[w % AUDIO_CAPACITY] = frame;
    s->audio_write.store(w + 1, std::memory_order_release);
    return true;
}
inline bool pop_frame(Shared *s, Frame *frame) {
    uint32_t r = s->audio_read.load(std::memory_order_relaxed);
    if (r == s->audio_write.load(std::memory_order_acquire)) {
        return false;
    }
    *frame = s->audio[r % AUDIO_CAPACITY];
    s->audio_read.store(r + 1, std::memory_order_release);
    return true;
}
} // namespace msipc
