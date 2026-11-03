// Portable DSP for vb.freezer~. Adapted from Volker Boehm's Max external
// (2011, updates through 2020): retain FFT magnitudes and randomize phases.
#ifndef VB_FREEZER_DSP_H
#define VB_FREEZER_DSP_H

#include <fftw3.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <vector>

namespace vb_freezer {

inline constexpr std::size_t kMaximumSize = std::size_t{1} << 24;

namespace detail {

// FFTW execution is safe for independent plans. Planning and plan destruction
// share this lock across all Processor instances, including different workers.
inline std::mutex &plannerMutex() {
    static std::mutex mutex;
    return mutex;
}

struct FftwFree {
    void operator()(void *memory) const noexcept { fftw_free(memory); }
};

struct Storage {
    const std::size_t size;
    std::unique_ptr<double, FftwFree> input;
    std::unique_ptr<fftw_complex, FftwFree> spectrum;
    std::vector<double> output;
    fftw_plan forward = nullptr;
    fftw_plan inverse = nullptr;

    explicit Storage(std::size_t n)
        : size(n), input(fftw_alloc_real(n)), spectrum(fftw_alloc_complex(n / 2 + 1)), output(n) {
        if (!input || !spectrum)
            throw std::bad_alloc();
        std::lock_guard<std::mutex> lock(plannerMutex());
        forward =
            fftw_plan_dft_r2c_1d(static_cast<int>(n), input.get(), spectrum.get(), FFTW_ESTIMATE);
        if (forward)
            inverse = fftw_plan_dft_c2r_1d(static_cast<int>(n), spectrum.get(), output.data(),
                                           FFTW_ESTIMATE);
        if (!forward || !inverse) {
            if (forward)
                fftw_destroy_plan(forward);
            if (inverse)
                fftw_destroy_plan(inverse);
            throw std::runtime_error("vb.freezer~: FFTW could not create transform plans");
        }
    }

    ~Storage() {
        std::lock_guard<std::mutex> lock(plannerMutex());
        fftw_destroy_plan(forward);
        fftw_destroy_plan(inverse);
    }

    Storage(const Storage &) = delete;
    Storage &operator=(const Storage &) = delete;
};

inline std::uint32_t nextRandom(std::uint32_t &state) noexcept {
    // A per-instance state avoids rand()'s shared state and worker contention.
    if (state == 0)
        state = 0x6d2b79f5u;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

} // namespace detail

// This class makes no Pd calls. A Processor is not reentrant; independent
// instances may run concurrently. In a Pd host, create/destroy plans on the
// scheduler so planning is also serialized with other FFTW-based externals.
class Processor {
  public:
    Processor() = default;
    ~Processor() = default;
    Processor(const Processor &) = delete;
    Processor &operator=(const Processor &) = delete;

    std::size_t size() const noexcept { return storage_ ? storage_->size : 0; }

    // Arbitrary even and odd sizes are supported. Failure retains the old plans.
    void prepare(std::size_t n) {
        if (n < 2 || n > kMaximumSize)
            throw std::invalid_argument("vb.freezer~: FFT size must be between 2 and 16777216");
        if (size() == n)
            return;
        auto replacement = std::make_unique<detail::Storage>(n);
        storage_.swap(replacement);
    }

    // Threshold is an unnormalized FFT magnitude, as in the original external.
    // Nonfinite input samples are treated as silence. The returned reference
    // remains valid until prepare changes the size or this Processor is freed.
    const std::vector<double> &process(const std::vector<double> &input, double threshold,
                                       std::uint32_t &randomState) {
        if (!storage_)
            throw std::logic_error("vb.freezer~: prepare must be called before process");
        auto &storage = *storage_;
        if (input.size() != storage.size)
            throw std::invalid_argument("vb.freezer~: input length does not match FFT size");
        if (!std::isfinite(threshold))
            throw std::invalid_argument("vb.freezer~: threshold must be finite");
        threshold = std::max(0.0, threshold);

        for (std::size_t i = 0; i < storage.size; ++i)
            storage.input.get()[i] = std::isfinite(input[i]) ? input[i] : 0.0;
        fftw_execute(storage.forward);

        constexpr double twoPi = 6.283185307179586476925286766559;
        constexpr double randomScale = 1.0 / 4294967296.0;
        auto *spectrum = storage.spectrum.get();
        const std::size_t positiveEnd = (storage.size + 1) / 2;
        spectrum[0][0] = spectrum[0][1] = 0;
        for (std::size_t bin = 1; bin < positiveEnd; ++bin) {
            const double magnitude = std::hypot(spectrum[bin][0], spectrum[bin][1]);
            const double phase = twoPi * (detail::nextRandom(randomState) * randomScale - 0.5);
            if (magnitude >= threshold) {
                spectrum[bin][0] = magnitude * std::cos(phase);
                spectrum[bin][1] = magnitude * std::sin(phase);
            } else {
                spectrum[bin][0] = spectrum[bin][1] = 0;
            }
        }
        if (storage.size % 2 == 0)
            spectrum[storage.size / 2][0] = spectrum[storage.size / 2][1] = 0;

        fftw_execute(storage.inverse);
        const double scale = 1.0 / static_cast<double>(storage.size);
        for (double &sample : storage.output)
            sample *= scale;
        return storage.output;
    }

  private:
    std::unique_ptr<detail::Storage> storage_;
};

} // namespace vb_freezer

#endif // VB_FREEZER_DSP_H
