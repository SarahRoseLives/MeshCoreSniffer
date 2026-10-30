// fft.h - small iterative radix-2 FFT used by the LoRa demodulator.
#pragma once

#include <complex>
#include <cmath>
#include <vector>
#include <utility>

namespace mc {

constexpr double PI = 3.14159265358979323846;

// In-place complex FFT.  a.size() must be a power of two.
inline void fft(std::vector<std::complex<double>>& a, bool inverse = false)
{
    const int n = static_cast<int>(a.size());
    if (n <= 1) return;

    // bit-reversal permutation
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }

    for (int len = 2; len <= n; len <<= 1) {
        const double ang = (inverse ? 2.0 : -2.0) * PI / len;
        const std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (int i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            const int half = len / 2;
            for (int j = 0; j < half; ++j) {
                const std::complex<double> u = a[i + j];
                const std::complex<double> v = a[i + j + half] * w;
                a[i + j] = u + v;
                a[i + j + half] = u - v;
                w *= wlen;
            }
        }
    }

    if (inverse) {
        const double inv = 1.0 / n;
        for (auto& x : a) x *= inv;
    }
}

} // namespace mc
