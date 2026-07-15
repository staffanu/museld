// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef AC3RF_DECODE_FFT_H
#define AC3RF_DECODE_FFT_H

#include <complex>
#include <iostream>
#include <valarray>

template<typename D>
class FFT {
public:
    // Cooley–Tukey FFT (in-place, divide-and-conquer), from https://rosettacode.org/wiki/Fast_Fourier_transform
    static void fft(std::valarray<std::complex<D>> &x) {
        const size_t N = x.size();
        if (N <= 1)
            return;

        std::valarray<std::complex<D>> even = x[std::slice(0, N/2, 2)];
        std::valarray<std::complex<D>> odd = x[std::slice(1, N/2, 2)];

        fft(even);
        fft(odd);

        for (size_t k = 0; k < N/2; ++k) {
            std::complex<D> t = std::polar<D>(1.0, -2 * M_PI * k / N) * odd[k];
            x[k] = even[k] + t;
            x[k+N/2] = even[k] - t;
        }
    }

    static void ifft(std::valarray<std::complex<D>> &x) {
        x = x.apply(std::conj);
        fft(x);
        x = x.apply(std::conj);
        x /= x.size();
    }
};

#endif //AC3RF_DECODE_FFT_H
