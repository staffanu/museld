// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include "FmDemod.h"

#include <cmath>
#include <numbers>

namespace {
    // Kaiser-windowed sinc lowpass
    double besselI0(double x) {
        double sum = 1, term = 1;
        for (int k = 1; k < 50; k++) {
            term *= (x / (2 * k)) * (x / (2 * k));
            sum += term;
            if (term < 1e-12 * sum) break;
        }
        return sum;
    }
}

FmDemod::FmDemod(double sample_rate, double carrier_hz, int decimation, double cutoff_hz)
    : m_sample_rate(sample_rate), m_decimation(decimation), m_index(0), m_phasor(1, 0), m_phasor_count(0),
      m_phase(0), m_prev(0), m_have_prev(false) {
    // 63 taps: the audio carriers 4-5 MHz below the video carrier are ~50 dB
    // down by the output Nyquist frequency, enough for sync slicing
    const int n = 63;
    const double beta = 6.0;
    const double fc = cutoff_hz / sample_rate;
    m_taps.resize(n);
    double sum = 0;
    for (int i = 0; i < n; i++) {
        const double x = i - (n - 1) / 2.0;
        const double sinc = x == 0 ? 2 * fc : std::sin(2 * std::numbers::pi * fc * x) / (std::numbers::pi * x);
        const double w = besselI0(beta * std::sqrt(1 - (2.0 * i / (n - 1) - 1) * (2.0 * i / (n - 1) - 1))) / besselI0(beta);
        m_taps[i] = (float)(sinc * w);
        sum += m_taps[i];
    }
    for (auto &t : m_taps)
        t = (float)(t / sum);
    m_history.assign(2 * n, {0, 0});
    m_carrier = carrier_hz;
    const double w0 = -2 * std::numbers::pi * carrier_hz / sample_rate;
    m_step = {std::cos(w0), std::sin(w0)};
}

void FmDemod::process(const float *in, int64_t count, std::vector<float> &out) {
    const int n = (int)m_taps.size();
    const float *h = m_taps.data();
    const double out_rate = outputRate();
    for (int64_t i = 0; i < count; i++) {
        // Mix down into a doubled ring, so the newest n samples are always
        // contiguous at m_history[m_index + 1 .. m_index + n]
        const std::complex<float> mixed((float)(in[i] * m_phasor.real()), (float)(in[i] * m_phasor.imag()));
        m_index = (m_index + 1) % n;
        m_history[m_index] = mixed;
        m_history[m_index + n] = mixed;
        m_phasor *= m_step;
        if (++m_phasor_count == 4096) {
            m_phasor /= std::abs(m_phasor);
            m_phasor_count = 0;
        }
        if (++m_phase < m_decimation)
            continue;
        m_phase = 0;
        // Filter at the output rate only
        float re = 0, im = 0;
        const std::complex<float> *x = m_history.data() + m_index + 1; // oldest first
        for (int j = 0; j < n; j++) {
            re += h[j] * x[n - 1 - j].real();
            im += h[j] * x[n - 1 - j].imag();
        }
        const std::complex<float> z(re, im);
        if (m_have_prev) {
            const std::complex<float> d = z * std::conj(m_prev);
            out.push_back((float)(m_carrier + std::atan2(d.imag(), d.real()) / (2 * std::numbers::pi) * out_rate));
        }
        m_prev = z;
        m_have_prev = true;
    }
}
