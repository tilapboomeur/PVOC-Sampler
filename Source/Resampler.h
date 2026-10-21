// Reechantillonneur polyphase a sinus cardinal fenetre (Kaiser, beta = 9),
// filtre anti-repliement integre. Meme algorithme que l'application Android.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

namespace pvoc {

class Resampler {
public:
    Resampler(int srcRate, int dstRate)
        : src(srcRate), dst(dstRate),
          cutoff(std::min(1.0, (double) dstRate / srcRate) * 0.95),
          halfWidth(zeroCrossings / cutoff),
          i0beta(besselI0(beta)) {
        const int g = std::gcd(src, dst);
        phases = dst / g;
        step = src / g;
        w = (int) std::ceil(halfWidth) + 1;
        taps = 2 * w;
        if ((int64_t) phases * taps <= 8000000) {
            bank.resize((size_t) phases * taps);
            for (int p = 0; p < phases; ++p) {
                const double frac = (double) p / phases;
                for (int j = 0; j < taps; ++j)
                    bank[(size_t) p * taps + j] = (float) kernel(frac - (j - w + 1));
            }
        }
    }

    int64_t outputLength(int64_t inFrames) const {
        return (int64_t) std::floor((double) inFrames * dst / src);
    }

    /** Reechantillonne src[from..to) des echantillons de sortie, dans out (indice de sortie absolu). */
    void process(const float* in, int64_t inLen, float* out, int64_t from, int64_t to) const {
        if (!bank.empty()) {
            for (int64_t n = from; n < to; ++n) {
                const int64_t num = n * step;
                const int64_t idx = num / phases;
                const int p = (int) (num % phases);
                const int64_t base = idx - w + 1;
                const float* h = bank.data() + (size_t) p * taps;
                float acc = 0.0f;
                if (base >= 0 && base + taps <= inLen) {
                    const float* x = in + base;
                    for (int j = 0; j < taps; ++j) acc += x[j] * h[j];
                } else {
                    for (int j = 0; j < taps; ++j) {
                        const int64_t k = base + j;
                        if (k >= 0 && k < inLen) acc += in[k] * h[j];
                    }
                }
                out[n] = acc;
            }
        } else {
            for (int64_t n = from; n < to; ++n) {
                const double t = (double) (n * src) / dst;
                const int64_t k0 = std::max<int64_t>(0, (int64_t) std::ceil(t - halfWidth));
                const int64_t k1 = std::min<int64_t>(inLen - 1, (int64_t) std::floor(t + halfWidth));
                double acc = 0.0;
                for (int64_t k = k0; k <= k1; ++k) acc += in[k] * kernel(t - (double) k);
                out[n] = (float) acc;
            }
        }
    }

private:
    static constexpr int zeroCrossings = 16;
    static constexpr double beta = 9.0;
    int src, dst;
    double cutoff, halfWidth, i0beta;
    int phases = 1, step = 1, w = 1, taps = 2;
    std::vector<float> bank;

    double kernel(double d) const {
        const double u = std::fabs(d) * cutoff;
        if (u >= zeroCrossings) return 0.0;
        const double x = u / zeroCrossings;
        const double win = besselI0(beta * std::sqrt(1.0 - x * x)) / i0beta;
        const double pi = 3.14159265358979323846;
        const double sinc = u < 1e-12 ? 1.0 : std::sin(pi * u) / (pi * u);
        return sinc * win * cutoff;
    }

    static double besselI0(double x) {
        double sum = 1.0, term = 1.0;
        const double q = x * x / 4.0;
        for (int k = 1; k < 80; ++k) {
            term *= q / ((double) k * k);
            sum += term;
            if (term < 1e-14 * sum) break;
        }
        return sum;
    }
};

} // namespace pvoc
