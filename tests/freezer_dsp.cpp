/* Standalone, after CMake has built FFTW:
   c++ -std=c++17 -O2 -pthread -Ibuild/fftw-3.3.10/api tests/freezer_dsp.cpp \
   build/fftw-3.3.10-build/libfftw3.a -lm -o /tmp/freezer_dsp_test
   /tmp/freezer_dsp_test
*/
#include "../src/stolen/vb_freezer_dsp.h"

#include <cassert>
#include <complex>
#include <cstdio>
#include <limits>
#include <thread>

namespace {

constexpr double twoPi = 6.283185307179586476925286766559;

// Independent small DFT: catches FFT layout, odd-size handling and IFFT scale.
std::complex<double> dft(const std::vector<double> &samples, std::size_t bin) {
    std::complex<double> result{};
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const double angle = -twoPi * static_cast<double>(bin * i) / samples.size();
        result += samples[i] * std::complex<double>(std::cos(angle), std::sin(angle));
    }
    return result;
}

std::vector<double> signal(std::size_t n) {
    std::vector<double> samples(n);
    for (std::size_t i = 0; i < n; ++i)
        samples[i] = 0.4 + 0.37 * std::sin(0.71 * i) + 0.17 * std::cos(1.19 * i) +
                     0.13 * std::cos(twoPi * (n / 2) * i / n);
    return samples;
}

void checkSpectrum(std::size_t n, double threshold) {
    vb_freezer::Processor processor;
    processor.prepare(n);
    auto input = signal(n);
    std::uint32_t seed = 37;
    const auto &output = processor.process(input, threshold, seed);
    assert(output.size() == n);
    for (double value : output)
        assert(std::isfinite(value));
    for (std::size_t bin = 0; bin <= n / 2; ++bin) {
        const double before = std::abs(dft(input, bin));
        const bool rejected = bin == 0 || (n % 2 == 0 && bin == n / 2) || before < threshold;
        const double expected = rejected ? 0 : before;
        const double actual = std::abs(dft(output, bin));
        assert(std::abs(actual - expected) < 1e-10 * n);
    }
    // An integer-length DFT loop returns to precisely the same phase after n
    // samples. Reconstruct samples at i and i+n rather than requiring a zero
    // boundary crossing (a valid periodic loop need not start at zero).
    for (std::size_t i : {std::size_t(0), n / 2, n - 1}) {
        std::complex<double> first{}, next{};
        for (std::size_t bin = 0; bin < n; ++bin) {
            const auto spectrum = dft(output, bin);
            first += spectrum * std::polar(1.0, twoPi * bin * i / n);
            next += spectrum * std::polar(1.0, twoPi * bin * (i + n) / n);
        }
        assert(std::abs(first.real() / n - output[i]) < 1e-10 * n);
        assert(std::abs(first - next) / n < 1e-10 * n);
    }
}

template <typename Exception, typename Operation> void throws(Operation operation) {
    bool caught = false;
    try {
        operation();
    } catch (const Exception &) {
        caught = true;
    }
    assert(caught);
}

void checkErrorsAndSilence() {
    vb_freezer::Processor processor;
    std::uint32_t seed = 0;
    throws<std::logic_error>([&] { processor.process({}, 0, seed); });
    throws<std::invalid_argument>([&] { processor.prepare(0); });
    throws<std::invalid_argument>([&] { processor.prepare(1); });
    throws<std::invalid_argument>([&] { processor.prepare(vb_freezer::kMaximumSize + 1); });
    processor.prepare(31);
    assert(processor.size() == 31);
    throws<std::invalid_argument>([&] { processor.prepare(1); });
    assert(processor.size() == 31);
    throws<std::invalid_argument>([&] { processor.process(std::vector<double>(30), 0, seed); });
    throws<std::invalid_argument>([&] {
        processor.process(std::vector<double>(31), std::numeric_limits<double>::infinity(), seed);
    });
    const auto &silence = processor.process(std::vector<double>(31), 0, seed);
    for (double sample : silence)
        assert(sample == 0);
    assert(seed != 0);
    const auto &rejected = processor.process(signal(31), 1e6, seed);
    for (double sample : rejected)
        assert(sample == 0);

    // The cutoff includes bins exactly at threshold, matching the Max source.
    processor.prepare(16);
    std::vector<double> impulse(16);
    impulse[0] = 1;
    const auto &cutoff = processor.process(impulse, 1, seed);
    assert(std::abs(std::abs(dft(cutoff, 3)) - 1) < 1e-12);
    impulse[0] = std::numeric_limits<double>::infinity();
    impulse[1] = std::numeric_limits<double>::quiet_NaN();
    for (double sample : processor.process(impulse, 0, seed))
        assert(sample == 0);
}

void checkRandomnessAndReuse() {
    vb_freezer::Processor processor;
    processor.prepare(127);
    std::uint32_t seed = 0;
    const auto input = signal(127);
    const auto first = processor.process(input, 0, seed);
    const auto seedAfterFirst = seed;
    const auto second = processor.process(input, 0, seed);
    assert(seed != seedAfterFirst);
    double difference = 0;
    for (std::size_t i = 0; i < first.size(); ++i)
        difference += std::abs(first[i] - second[i]);
    assert(difference > 1);
    seed = 0;
    assert(processor.process(input, 0, seed) == first);
    seed = 0;
    assert(processor.process(input, -1, seed) == first);
    const auto *outputAddress = processor.process(input, 0, seed).data();
    processor.prepare(127);
    assert(processor.process(input, 0, seed).data() == outputAddress);
    processor.prepare(9);
    assert(processor.size() == 9);
    processor.process(signal(9), 0, seed);
}

void checkConcurrentProcessors() {
    std::vector<std::thread> workers;
    for (unsigned worker = 0; worker < 4; ++worker) {
        workers.emplace_back([worker] {
            for (unsigned repeat = 0; repeat < 12; ++repeat) {
                vb_freezer::Processor processor;
                const std::size_t n = 17 + worker * 5 + repeat;
                processor.prepare(n);
                std::uint32_t seed = worker + 1;
                const auto &output = processor.process(signal(n), 0, seed);
                assert(std::abs(dft(output, 0)) < 1e-10);
                processor.prepare(n + 1);
                processor.process(signal(n + 1), 0, seed);
            }
        });
    }
    for (auto &worker : workers)
        worker.join();
}

} // namespace

int main() {
    for (std::size_t n : {2, 3, 5, 16, 31, 64, 127, 256}) {
        checkSpectrum(n, 0);
        checkSpectrum(n, 2);
    }
    checkErrorsAndSilence();
    checkRandomnessAndReuse();
    checkConcurrentProcessors();
    std::puts("vb.freezer~ DSP: magnitude, threshold, odd/even FFT, phase, reuse and concurrency "
              "checks passed");
}
