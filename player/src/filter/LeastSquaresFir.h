// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSELD_LEAST_SQUARES_FIR_H
#define MUSELD_LEAST_SQUARES_FIR_H

#include <cmath>
#include <complex>
#include <functional>
#include <stdexcept>
#include <vector>

// Weighted least-squares FIR design on a frequency grid, for complex (or, with a
// symmetric specification, real) filters whose bands are not symmetric about zero
// or whose pass band is not flat: the MUSE band-pass that decimates needs its
// negative-frequency image 50 dB down but the EFM below the band only 30, and the
// low-pass after a decimated quadrature demodulation compensates the
// discriminator's sinc.  The taps h[-M..M] minimize sum over the grid of
// weight(f) * |H(f) - desired(f)|^2, where H(f) = sum h[k] e^{-j 2 pi f k / Fs};
// frequencies not covered by a band are don't-care.  The normal equations are
// Hermitian Toeplitz and solved by Gaussian elimination (N is a few dozen).
namespace LeastSquaresFir {

    struct Band {
        double low, high;                         // Hz, in [-Fs/2, Fs/2)
        std::function<double(double)> desired;    // of the frequency in Hz (real-valued)
        double weight;
    };

    inline std::function<double(double)> constant(double v) {
        return [v](double) { return v; };
    }

    inline std::vector<std::complex<double>> design(int ntaps, double Fs, const std::vector<Band> &bands,
                                                    int grid = 4096) {
        if ((ntaps & 1) == 0)
            throw std::invalid_argument("LeastSquaresFir::design: the tap count must be odd");
        const int M = (ntaps - 1) / 2;
        using C = std::complex<double>;
        // t[d] = sum w e^{j 2 pi f d / Fs} for d = 0 .. 2M (G[m][k] = t[m - k], conj for d < 0),
        // g[m] = sum w D e^{j 2 pi f m / Fs}
        std::vector<C> t(2 * M + 1, C(0)), g(ntaps, C(0));
        for (int i = 0; i < grid; i++) {
            const double f = -Fs / 2 + Fs * i / grid;
            for (const auto &band : bands) {
                if (f < band.low || f >= band.high)
                    continue;
                const double w = band.weight, d = band.desired(f);
                const double omega = 2 * M_PI * f / Fs;
                for (int dd = 0; dd <= 2 * M; dd++)
                    t[dd] += w * std::polar(1.0, omega * dd);
                for (int m = -M; m <= M; m++)
                    g[m + M] += w * d * std::polar(1.0, omega * m);
                break;
            }
        }
        std::vector<std::vector<C>> G(ntaps, std::vector<C>(ntaps));
        for (int m = 0; m < ntaps; m++)
            for (int k = 0; k < ntaps; k++)
                G[m][k] = m >= k ? t[m - k] : std::conj(t[k - m]);
        // Gaussian elimination with partial pivoting on [G | g]
        for (int col = 0; col < ntaps; col++) {
            int pivot = col;
            for (int r = col + 1; r < ntaps; r++)
                if (std::abs(G[r][col]) > std::abs(G[pivot][col]))
                    pivot = r;
            std::swap(G[col], G[pivot]);
            std::swap(g[col], g[pivot]);
            if (std::abs(G[col][col]) == 0)
                throw std::runtime_error("LeastSquaresFir::design: singular normal equations");
            for (int r = col + 1; r < ntaps; r++) {
                const C factor = G[r][col] / G[col][col];
                for (int c = col; c < ntaps; c++)
                    G[r][c] -= factor * G[col][c];
                g[r] -= factor * g[col];
            }
        }
        std::vector<C> h(ntaps);
        for (int r = ntaps - 1; r >= 0; r--) {
            C s = g[r];
            for (int c = r + 1; c < ntaps; c++)
                s -= G[r][c] * h[c];
            h[r] = s / G[r][r];
        }
        return h;
    }

    // The real filter of a specification symmetric about zero
    inline std::vector<double> designReal(int ntaps, double Fs, const std::vector<Band> &bands, int grid = 4096) {
        auto h = design(ntaps, Fs, bands, grid);
        std::vector<double> r(h.size());
        for (size_t i = 0; i < h.size(); i++)
            r[i] = h[i].real();
        return r;
    }

    // The response at f of taps h[-M..M]
    inline std::complex<double> response(const std::vector<std::complex<double>> &h, double Fs, double f) {
        const int M = ((int)h.size() - 1) / 2;
        std::complex<double> s(0);
        for (int k = -M; k <= M; k++)
            s += h[k + M] * std::polar(1.0, -2 * M_PI * f * k / Fs);
        return s;
    }
}

#endif // MUSELD_LEAST_SQUARES_FIR_H
