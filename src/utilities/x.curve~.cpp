#include "m_pd.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <vector>

namespace {

constexpr int DEFAULT_SIZE = 1024;
constexpr int MAX_SIZE = 16 * 1024 * 1024;
constexpr double PI = 3.14159265358979323846264338327950288;

enum class WindowType {
    Hann,
    Hamming,
    Blackman,
    BlackmanHarris,
    Kaiser,
    Gaussian,
    Tukey,
    FlatTop,
    Rectangular,
    Bartlett,
    Sine,
    Vorbis,
    DolphChebyshev,
    Dpss
};

typedef struct _xcurve {
    t_object object;
    t_sample *buffer;
    int size;
    int position;
    WindowType type;
    t_symbol *name;
    double parameter;
} t_xcurve;

static t_class *xcurve_class;

static double default_parameter(WindowType type) {
    switch (type) {
    case WindowType::Kaiser:
        return 8.6;
    case WindowType::Gaussian:
        return 0.4;
    case WindowType::Tukey:
        return 0.5;
    case WindowType::DolphChebyshev:
        return 100.0;
    case WindowType::Dpss:
        return 2.5;
    default:
        return 0.0;
    }
}

static bool parse_window(t_symbol *name, WindowType &type) {
    const char *s = name->s_name;
    if (!std::strcmp(s, "hann") || !std::strcmp(s, "hanning"))
        type = WindowType::Hann;
    else if (!std::strcmp(s, "hamming"))
        type = WindowType::Hamming;
    else if (!std::strcmp(s, "blackman"))
        type = WindowType::Blackman;
    else if (!std::strcmp(s, "blackman-harris") || !std::strcmp(s, "blackmanharris") ||
             !std::strcmp(s, "bh"))
        type = WindowType::BlackmanHarris;
    else if (!std::strcmp(s, "kaiser"))
        type = WindowType::Kaiser;
    else if (!std::strcmp(s, "gaussian") || !std::strcmp(s, "gauss"))
        type = WindowType::Gaussian;
    else if (!std::strcmp(s, "tukey"))
        type = WindowType::Tukey;
    else if (!std::strcmp(s, "flat-top") || !std::strcmp(s, "flattop"))
        type = WindowType::FlatTop;
    else if (!std::strcmp(s, "rectangular") || !std::strcmp(s, "rectangle") ||
             !std::strcmp(s, "rect"))
        type = WindowType::Rectangular;
    else if (!std::strcmp(s, "bartlett") || !std::strcmp(s, "triangular") ||
             !std::strcmp(s, "triangle"))
        type = WindowType::Bartlett;
    else if (!std::strcmp(s, "sine") || !std::strcmp(s, "sin"))
        type = WindowType::Sine;
    else if (!std::strcmp(s, "vorbis"))
        type = WindowType::Vorbis;
    else if (!std::strcmp(s, "dolph-chebyshev") || !std::strcmp(s, "chebyshev") ||
             !std::strcmp(s, "cheb"))
        type = WindowType::DolphChebyshev;
    else if (!std::strcmp(s, "dpss") || !std::strcmp(s, "slepian"))
        type = WindowType::Dpss;
    else
        return false;
    return true;
}

static double bessel_i0(double x) {
    double sum = 1.0;
    double term = 1.0;
    const double half = x * 0.5;
    for (int k = 1; k < 100; ++k) {
        term *= (half / k) * (half / k);
        sum += term;
        if (term <= sum * 1.0e-15)
            break;
    }
    return sum;
}

static bool is_power_of_two(size_t n) {
    return n && !(n & (n - 1));
}

static void radix2_fft(std::vector<std::complex<double>> &values, bool inverse) {
    const size_t n = values.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(values[i], values[j]);
    }

    for (size_t length = 2; length <= n; length <<= 1) {
        const double angle = (inverse ? 2.0 : -2.0) * PI / static_cast<double>(length);
        const std::complex<double> step(std::cos(angle), std::sin(angle));
        for (size_t i = 0; i < n; i += length) {
            std::complex<double> phase(1.0, 0.0);
            for (size_t j = 0; j < length / 2; ++j) {
                const std::complex<double> even = values[i + j];
                const std::complex<double> odd = values[i + j + length / 2] * phase;
                values[i + j] = even + odd;
                values[i + j + length / 2] = even - odd;
                phase *= step;
            }
        }
    }

    if (inverse) {
        const double scale = 1.0 / static_cast<double>(n);
        for (auto &value : values)
            value *= scale;
    }
}

// Forward DFT using Bluestein's algorithm for non-power-of-two window sizes.
static void forward_dft(std::vector<std::complex<double>> &values) {
    const size_t n = values.size();
    if (n < 2)
        return;
    if (is_power_of_two(n)) {
        radix2_fft(values, false);
        return;
    }

    size_t fft_size = 1;
    while (fft_size < n * 2 - 1)
        fft_size <<= 1;

    std::vector<std::complex<double>> a(fft_size);
    std::vector<std::complex<double>> b(fft_size);
    for (size_t i = 0; i < n; ++i) {
        const double angle = PI * static_cast<double>(i) * static_cast<double>(i) /
                             static_cast<double>(n);
        const std::complex<double> negative_chirp(std::cos(angle), -std::sin(angle));
        const std::complex<double> positive_chirp(std::cos(angle), std::sin(angle));
        a[i] = values[i] * negative_chirp;
        b[i] = positive_chirp;
        if (i)
            b[fft_size - i] = positive_chirp;
    }

    radix2_fft(a, false);
    radix2_fft(b, false);
    for (size_t i = 0; i < fft_size; ++i)
        a[i] *= b[i];
    radix2_fft(a, true);

    for (size_t i = 0; i < n; ++i) {
        const double angle = PI * static_cast<double>(i) * static_cast<double>(i) /
                             static_cast<double>(n);
        values[i] = a[i] * std::complex<double>(std::cos(angle), -std::sin(angle));
    }
}

static void fill_dolph_chebyshev(t_sample *buffer, int size, double attenuation) {
    if (size == 1) {
        buffer[0] = 1.0f;
        return;
    }

    attenuation = std::clamp(attenuation, 45.0, 200.0);
    const int order = size - 1;
    const double beta =
        std::cosh(std::acosh(std::pow(10.0, attenuation / 20.0)) / static_cast<double>(order));
    std::vector<std::complex<double>> spectrum(size);

    for (int k = 0; k < size; ++k) {
        const double x = beta * std::cos(PI * k / static_cast<double>(size));
        double value;
        if (x > 1.0)
            value = std::cosh(order * std::acosh(x));
        else if (x < -1.0)
            value = (size & 1 ? 1.0 : -1.0) * std::cosh(order * std::acosh(-x));
        else
            value = std::cos(order * std::acos(x));

        if (!(size & 1)) {
            const double angle = PI * k / static_cast<double>(size);
            spectrum[k] = value * std::complex<double>(std::cos(angle), std::sin(angle));
        } else {
            spectrum[k] = value;
        }
    }

    forward_dft(spectrum);
    if (size & 1) {
        const int half = (size + 1) / 2;
        int output = 0;
        for (int i = half - 1; i > 0; --i)
            buffer[output++] = static_cast<t_sample>(spectrum[i].real());
        for (int i = 0; i < half; ++i)
            buffer[output++] = static_cast<t_sample>(spectrum[i].real());
    } else {
        const int half = size / 2 + 1;
        int output = 0;
        for (int i = half - 1; i > 0; --i)
            buffer[output++] = static_cast<t_sample>(spectrum[i].real());
        for (int i = 1; i < half; ++i)
            buffer[output++] = static_cast<t_sample>(spectrum[i].real());
    }
}

static void fill_dpss(t_sample *buffer, int size, double time_bandwidth) {
    if (size == 1) {
        buffer[0] = 1.0f;
        return;
    }

    time_bandwidth = std::clamp(time_bandwidth, 0.01, size * 0.5 - 1.0e-6);
    const double bandwidth = time_bandwidth / static_cast<double>(size);
    const double cosine = std::cos(2.0 * PI * bandwidth);
    std::vector<double> diagonal(size);
    std::vector<double> off_diagonal(size - 1);
    double lower_bound = 0.0;

    for (int i = 0; i < size; ++i) {
        const double centered = (size - 1 - 2.0 * i) * 0.5;
        diagonal[i] = centered * centered * cosine;
    }
    for (int i = 0; i < size - 1; ++i)
        off_diagonal[i] = (i + 1.0) * (size - i - 1.0) * 0.5;
    for (int i = 0; i < size; ++i) {
        const double radius = (i ? off_diagonal[i - 1] : 0.0) +
                              (i + 1 < size ? off_diagonal[i] : 0.0);
        lower_bound = std::min(lower_bound, diagonal[i] - radius);
    }

    const double shift = -lower_bound + 1.0;
    std::vector<double> vector(size, 1.0 / std::sqrt(static_cast<double>(size)));
    std::vector<double> next(size);
    for (int iteration = 0; iteration < 200; ++iteration) {
        double norm = 0.0;
        for (int i = 0; i < size; ++i) {
            double value = (diagonal[i] + shift) * vector[i];
            if (i)
                value += off_diagonal[i - 1] * vector[i - 1];
            if (i + 1 < size)
                value += off_diagonal[i] * vector[i + 1];
            next[i] = value;
            norm += value * value;
        }
        norm = std::sqrt(norm);
        if (norm == 0.0)
            break;
        double difference = 0.0;
        for (int i = 0; i < size; ++i) {
            next[i] /= norm;
            difference = std::max(difference, std::abs(next[i] - vector[i]));
        }
        vector.swap(next);
        if (difference < 1.0e-13)
            break;
    }

    for (int i = 0; i < size; ++i)
        buffer[i] = static_cast<t_sample>(vector[i]);
}

static void normalize_window(t_sample *buffer, int size) {
    double peak = 0.0;
    for (int i = 0; i < size; ++i)
        peak = std::max(peak, static_cast<double>(buffer[i]));
    if (peak <= 0.0)
        return;

    // x.curve~ is an envelope generator, so its output is intentionally constrained to [0, 1].
    for (int i = 0; i < size; ++i) {
        const double normalized = static_cast<double>(buffer[i]) / peak;
        buffer[i] = static_cast<t_sample>(std::clamp(normalized, 0.0, 1.0));
    }
}

static void fill_window(t_xcurve *x) {
    const int size = x->size;
    if (size == 1) {
        x->buffer[0] = 1.0f;
        x->position = size;
        return;
    }

    const double denominator = static_cast<double>(size - 1);
    for (int i = 0; i < size; ++i) {
        const double phase = 2.0 * PI * i / denominator;
        const double unit = i / denominator;
        double value = 0.0;

        switch (x->type) {
        case WindowType::Hann:
            value = 0.5 - 0.5 * std::cos(phase);
            break;
        case WindowType::Hamming:
            value = 0.54 - 0.46 * std::cos(phase);
            break;
        case WindowType::Blackman:
            value = 0.42 - 0.5 * std::cos(phase) + 0.08 * std::cos(2.0 * phase);
            break;
        case WindowType::BlackmanHarris:
            value = 0.35875 - 0.48829 * std::cos(phase) + 0.14128 * std::cos(2.0 * phase) -
                    0.01168 * std::cos(3.0 * phase);
            break;
        case WindowType::Kaiser: {
            const double beta = std::clamp(x->parameter, 0.0, 50.0);
            const double position = 2.0 * unit - 1.0;
            value = bessel_i0(beta * std::sqrt(std::max(0.0, 1.0 - position * position))) /
                    bessel_i0(beta);
            break;
        }
        case WindowType::Gaussian: {
            const double sigma = std::clamp(x->parameter, 0.01, 1.0);
            const double position = (i - denominator * 0.5) / (sigma * denominator * 0.5);
            value = std::exp(-0.5 * position * position);
            break;
        }
        case WindowType::Tukey: {
            const double alpha = std::clamp(x->parameter, 0.0, 1.0);
            if (alpha == 0.0)
                value = 1.0;
            else if (unit < alpha * 0.5)
                value = 0.5 * (1.0 + std::cos(PI * (2.0 * unit / alpha - 1.0)));
            else if (unit <= 1.0 - alpha * 0.5)
                value = 1.0;
            else
                value = 0.5 * (1.0 + std::cos(PI * (2.0 * unit / alpha - 2.0 / alpha + 1.0)));
            break;
        }
        case WindowType::FlatTop:
            value = 0.21557895 - 0.41663158 * std::cos(phase) +
                    0.277263158 * std::cos(2.0 * phase) - 0.083578947 * std::cos(3.0 * phase) +
                    0.006947368 * std::cos(4.0 * phase);
            break;
        case WindowType::Rectangular:
            value = 1.0;
            break;
        case WindowType::Bartlett:
            value = 1.0 - std::abs((i - denominator * 0.5) / (denominator * 0.5));
            break;
        case WindowType::Sine:
            value = std::sin(PI * unit);
            break;
        case WindowType::Vorbis: {
            const double sine = std::sin(PI * (i + 0.5) / static_cast<double>(size));
            value = std::sin(PI * 0.5 * sine * sine);
            break;
        }
        case WindowType::DolphChebyshev:
        case WindowType::Dpss:
            break;
        }
        x->buffer[i] = static_cast<t_sample>(value);
    }

    if (x->type == WindowType::DolphChebyshev)
        fill_dolph_chebyshev(x->buffer, size, x->parameter);
    else if (x->type == WindowType::Dpss)
        fill_dpss(x->buffer, size, x->parameter);

    normalize_window(x->buffer, size);
    x->position = size;
}

static bool resize_buffer(t_xcurve *x, int size) {
    if (size < 1 || size > MAX_SIZE) {
        pd_error(x, "[x.curve~] size must be between 1 and %d samples", MAX_SIZE);
        return false;
    }
    if (size == x->size)
        return true;

    t_sample *buffer = static_cast<t_sample *>(getbytes(size * sizeof(t_sample)));
    if (!buffer) {
        pd_error(x, "[x.curve~] could not allocate %d samples", size);
        return false;
    }
    if (x->buffer)
        freebytes(x->buffer, x->size * sizeof(t_sample));
    x->buffer = buffer;
    x->size = size;
    return true;
}

static bool configure(t_xcurve *x, t_symbol *name, int size, bool has_parameter,
                      double parameter) {
    WindowType type;
    if (!parse_window(name, type)) {
        pd_error(x, "[x.curve~] unknown curve '%s'", name->s_name);
        return false;
    }
    if (!resize_buffer(x, size))
        return false;

    x->type = type;
    x->name = name;
    x->parameter = has_parameter ? parameter : default_parameter(type);
    fill_window(x);
    return true;
}

static void xcurve_bang(t_xcurve *x) {
    x->position = 0;
}

static void xcurve_anything(t_xcurve *x, t_symbol *selector, int argc, t_atom *argv) {
    int size = x->size;
    bool has_parameter = false;
    double parameter = 0.0;

    if (argc > 2) {
        pd_error(x, "[x.curve~] usage: %s <size> [parameter]", selector->s_name);
        return;
    }
    if (argc > 0) {
        if (argv[0].a_type != A_FLOAT) {
            pd_error(x, "[x.curve~] usage: %s <size> [parameter]", selector->s_name);
            return;
        }
        size = static_cast<int>(atom_getfloat(argv));
    }
    if (argc > 1) {
        if (argv[1].a_type != A_FLOAT) {
            pd_error(x, "[x.curve~] parameter must be a number");
            return;
        }
        has_parameter = true;
        parameter = atom_getfloat(argv + 1);
    }
    if (configure(x, selector, size, has_parameter, parameter))
        xcurve_bang(x);
}

static void xcurve_set(t_xcurve *x, t_symbol *, int argc, t_atom *argv) {
    if (argc < 1 || argv[0].a_type != A_SYMBOL) {
        pd_error(x, "[x.curve~] usage: set <curve> [size] [parameter]");
        return;
    }
    xcurve_anything(x, atom_getsymbol(argv), argc - 1, argv + 1);
}

static void xcurve_size(t_xcurve *x, t_floatarg requested) {
    const int size = static_cast<int>(requested);
    if (resize_buffer(x, size)) {
        fill_window(x);
        xcurve_bang(x);
    }
}

static void xcurve_parameter(t_xcurve *x, t_floatarg parameter) {
    x->parameter = parameter;
    fill_window(x);
    xcurve_bang(x);
}

static t_int *xcurve_perform(t_int *w) {
    t_xcurve *x = reinterpret_cast<t_xcurve *>(w[1]);
    t_sample *output = reinterpret_cast<t_sample *>(w[2]);
    int block_size = static_cast<int>(w[3]);
    int position = x->position;

    while (block_size--) {
        if (position < x->size)
            *output++ = x->buffer[position++];
        else
            *output++ = 0.0f;
    }
    x->position = position;
    return w + 4;
}

static void xcurve_dsp(t_xcurve *x, t_signal **signals) {
    dsp_add(xcurve_perform, 3, x, signals[0]->s_vec, signals[0]->s_n);
}

static void *xcurve_new(t_symbol *, int argc, t_atom *argv) {
    t_xcurve *x = reinterpret_cast<t_xcurve *>(pd_new(xcurve_class));
    x->buffer = nullptr;
    x->size = 0;
    x->position = 0;
    x->type = WindowType::Hann;
    x->name = gensym("hann");
    x->parameter = default_parameter(x->type);

    t_symbol *name = x->name;
    int size = DEFAULT_SIZE;
    bool has_parameter = false;
    double parameter = 0.0;
    int index = 0;

    if (argc > index && argv[index].a_type == A_SYMBOL)
        name = atom_getsymbol(argv + index++);
    if (argc > index && argv[index].a_type == A_FLOAT)
        size = static_cast<int>(atom_getfloat(argv + index++));
    if (argc > index && argv[index].a_type == A_FLOAT) {
        parameter = atom_getfloat(argv + index++);
        has_parameter = true;
    }
    if (index != argc)
        pd_error(x, "[x.curve~] usage: x.curve~ [curve] [size] [parameter]");

    if (!configure(x, name, size, has_parameter, parameter))
        configure(x, gensym("hann"), DEFAULT_SIZE, false, 0.0);
    outlet_new(&x->object, &s_signal);
    return x;
}

static void xcurve_free(t_xcurve *x) {
    if (x->buffer)
        freebytes(x->buffer, x->size * sizeof(t_sample));
}

} // namespace

extern "C" void setup_x0x2ecurve_tilde(void) {
    xcurve_class = class_new(gensym("x.curve~"), reinterpret_cast<t_newmethod>(xcurve_new),
                             reinterpret_cast<t_method>(xcurve_free), sizeof(t_xcurve),
                             CLASS_DEFAULT, A_GIMME, 0);
    class_addbang(xcurve_class, xcurve_bang);
    class_addmethod(xcurve_class, reinterpret_cast<t_method>(xcurve_dsp), gensym("dsp"), A_CANT,
                    0);
    class_addmethod(xcurve_class, reinterpret_cast<t_method>(xcurve_set), gensym("set"), A_GIMME,
                    0);
    class_addmethod(xcurve_class, reinterpret_cast<t_method>(xcurve_size), gensym("size"), A_FLOAT,
                    0);
    class_addmethod(xcurve_class, reinterpret_cast<t_method>(xcurve_parameter), gensym("parameter"),
                    A_FLOAT, 0);
    class_addmethod(xcurve_class, reinterpret_cast<t_method>(xcurve_parameter), gensym("param"),
                    A_FLOAT, 0);
    class_addanything(xcurve_class, xcurve_anything);
}
