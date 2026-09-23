// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef LDSTACK_FMDEMOD_H
#define LDSTACK_FMDEMOD_H

#include <complex>
#include <cstdint>
#include <vector>

// Streaming FM demodulator for the laserdisc video carrier: mixes the RF down
// by the nominal carrier, lowpasses, decimates and takes the phase increment.
// Output samples are the instantaneous frequency in Hz at fs / decimation.
class FmDemod {
public:
    FmDemod(double sample_rate, double carrier_hz, int decimation, double cutoff_hz);

    // Demodulate `count` input samples, appending to `out`.  The output lags
    // the input by delay() input samples.
    void process(const float *in, int64_t count, std::vector<float> &out);

    [[nodiscard]] int decimation() const { return m_decimation; }
    [[nodiscard]] double outputRate() const { return m_sample_rate / m_decimation; }
    // Group delay of the lowpass in input samples
    [[nodiscard]] double delay() const { return (double)(m_taps.size() - 1) / 2; }

private:
    double m_sample_rate;
    int m_decimation;
    std::vector<float> m_taps;
    double m_carrier;
    std::vector<std::complex<float>> m_history; // mixed input, doubled ring
    int m_index;
    std::complex<double> m_phasor;             // mixer state
    std::complex<double> m_step;
    int64_t m_phasor_count;
    int m_phase;                               // input samples since the last output
    std::complex<float> m_prev;                // previous filtered output
    bool m_have_prev;
};

#endif //LDSTACK_FMDEMOD_H
