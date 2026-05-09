// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#include <iostream>
#include <valarray>
#include <unistd.h>
#include <complex.h>
#include <sys/fcntl.h>
#include <chrono>
#include "../filter/HalfBand.h"
#include "../filter/FFT.h"
#include "../filter/Window.h"
#include "EfmDemodulator.h"

EfmDemodulator::EfmDemodulator(Logger &log, double input_sample_frequency, int input_block_size, bool use_simd,
    bool rf_input, int log2_decimation, int adaptive_filter_size, std::optional<std::string> retiming_debug_filename)
    : m_log(log),
      m_input_sample_frequency(input_sample_frequency),
      m_input_block_size(input_block_size),
      m_rf_input(rf_input),
      m_log2_decimation(log2_decimation),
      m_decimation_factor(1 << m_log2_decimation),
      m_remove_dc_filter("Remove DC", {1, -1}, {1, -0.9999}),
      m_low_pass_filter(nullptr),
      m_phase_adjust_filter(nullptr),
      m_timing_recovery(log, input_sample_frequency / m_decimation_factor, input_block_size / m_decimation_factor,
          adaptive_filter_size, retiming_debug_filename),
      m_timing_log_period(std::max(1, (int)std::round(input_sample_frequency / input_block_size))) {

    assert(m_log2_decimation >= 0);
    assert(m_input_block_size % m_decimation_factor == 0);

    // Buffer for filtered input.  If decimation is performed, the result is placed here and then
    // the IIR filters are applied to the data in place.
    // If no decimation, the IIR filters filter the input directly and place the output here.
    m_filtered_input.resize(m_input_block_size / m_decimation_factor);

    // Create decimation filters if needed
    if (m_log2_decimation != 0) {
        log.debug(eAudio, std::format("Decimating input by 2^{}", m_log2_decimation));
        m_decimation_filter_stages.resize(m_log2_decimation);

        for (int i = m_log2_decimation - 1; i >= 0; i--) {
            double stage_sample_freq = input_sample_frequency / (1 << i);
            const int ntaps = 31;
            std::vector<float> filter = HalfBand::kaiser<float>(ntaps, HalfBand::kBeta80dB);
            std::string description = std::format(
                "Half-band Kaiser low-pass Fs={}, ntaps={}, beta={}",
                stage_sample_freq, ntaps, HalfBand::kBeta80dB);
            m_decimation_filter_stages[i] = new FirFilterStage(
                std::format("Decimate by 2 stage {}", i + 1),
                description,
                filter,
                2, m_input_block_size >> i,
                i == m_log2_decimation - 1 ? 0 : m_decimation_filter_stages[i + 1]->filterSize() - 1,
                i == m_log2_decimation - 1 ? &m_filtered_input : m_decimation_filter_stages[i + 1]->inputBuffer(),
                use_simd);
        }
        for (const auto &stage: m_decimation_filter_stages)
            m_log.debug(eAudio, std::format("Filter stage: {}", stage->toString()));
    }

    m_low_pass_filter = makeEllipticLowpassFilter(input_sample_frequency / m_decimation_factor);

    {
        // Filter from the Sony HIL C1, called "RF EQ" in the service manual schematic. Only the first part -- the second doesn't seem to do much.
        // See the Python code elsewhere in this project for derivation of the coefficients below!
        double Fs = input_sample_frequency / m_decimation_factor;
        std::array b = {
            (float)((39176791720000.0*pow(Fs, 2) + 1.0256763e+21*Fs + 1.705e+26)/(136762495344372.0*pow(Fs, 2) + 1.03400100063e+21*Fs + 1.11017205e+27)),
            (float)((3.41e+26 - 78353583440000.0*pow(Fs, 2))/(136762495344372.0*pow(Fs, 2) + 1.03400100063e+21*Fs + 1.11017205e+27)),
            (float)((39176791720000.0*pow(Fs, 2) - 1.0256763e+21*Fs + 1.705e+26)/(136762495344372.0*pow(Fs, 2) + 1.03400100063e+21*Fs + 1.11017205e+27))
        };
        std::array a = {
            1.f,
            (float)((2.2203441e+27 - 273524990688744.0*pow(Fs, 2))/(136762495344372.0*pow(Fs, 2) + 1.03400100063e+21*Fs + 1.11017205e+27)),
            (float)((136762495344372.0*pow(Fs, 2) - 1.03400100063e+21*Fs + 1.11017205e+27)/(136762495344372.0*pow(Fs, 2) + 1.03400100063e+21*Fs + 1.11017205e+27))
        };
        m_phase_adjust_filter = new IirFilter<3>("RF EQ", b, a);
    }

    m_log.debug(eAudio, m_remove_dc_filter.toString());
    m_log.debug(eAudio, m_low_pass_filter->toString());
    m_log.debug(eAudio, m_phase_adjust_filter->toString());
}

EfmDemodulator::~EfmDemodulator() {
    delete m_phase_adjust_filter;
    delete m_low_pass_filter;

    for (const auto stage: m_decimation_filter_stages)
        delete stage;
    m_decimation_filter_stages.clear();
}

void EfmDemodulator::demodulate(const float *input_buffer, std::vector<float> &reclocked_data) {
    using clock = std::chrono::steady_clock;

    const float *iir_input;
    auto t0 = clock::now();
    if (m_decimation_filter_stages.empty()) {
        iir_input = input_buffer;
    } else {
        float *filter_input_buffer = m_decimation_filter_stages[0]->inputBuffer()->data() + m_decimation_filter_stages[0]->filterSize() - 1;
        for (int i = 0; i < m_input_block_size; i++)
            filter_input_buffer[i] = input_buffer[i];

        int n = m_input_block_size;
        for (auto stage: m_decimation_filter_stages) {
            stage->applyFilter(n);
            n /= stage->decimationFactor();
        }

        iir_input = m_filtered_input.data();
    }
    auto t1 = clock::now();

    for (int i = 0; i < m_input_block_size / m_decimation_factor; i++) {
        const float x = iir_input[i];
        float y = m_remove_dc_filter.filter(x);
        if (m_rf_input)
            y = m_phase_adjust_filter->filter(m_low_pass_filter->filter(y));
        m_filtered_input[i] = y;
    }
    auto t2 = clock::now();

    m_timing_recovery.reclock(m_filtered_input.data(), reclocked_data);
    auto t3 = clock::now();

    {
        int n = m_input_block_size;
        for (auto stage: m_decimation_filter_stages) {
            stage->moveDataToFront(n);
            n /= stage->decimationFactor();
        }
    }

    m_decimation_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    m_iir_ns        += std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
    m_reclock_ns    += std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count();

    if (++m_timing_blocks == m_timing_log_period) {
        m_log.debug(eAudio, std::format(
            "EfmDemodulator per block: decimation {:.1f} us, iir {:.1f} us, reclock {:.1f} us"
            " | per second: decimation {:.1f} ms, iir {:.1f} ms, reclock {:.1f} ms",
            m_decimation_ns / (m_timing_log_period * 1e3),
            m_iir_ns        / (m_timing_log_period * 1e3),
            m_reclock_ns    / (m_timing_log_period * 1e3),
            m_decimation_ns / 1e6,
            m_iir_ns        / 1e6,
            m_reclock_ns    / 1e6));
        m_timing_blocks = 0;
        m_decimation_ns = m_iir_ns = m_reclock_ns = 0;
    }
}

IirFilter<5> *EfmDemodulator::makeEllipticLowpassFilter(double Fs) {
    // Creates an elliptic IIR lowpass filter the same way that
    // Octave's "ellip(4, 3, 40, 1.7e6/(Fs/2))"
    //
    // The reason we cannot just copy the coefficients is that Fs
    // is unknown at compile time.  The code below is converted from
    // Octave, but everything that can be hard coded is.  So, for
    // example, the ncauer call is not implemented since the roots
    // it returns are independent of Fs.

    double w = 1.7e6 / (Fs / 2); // ellip function argument
    const int n = 4; // argument

    // Prewarp
    const double T = 2.0; // sampling frequency of 2 Hz
    w = 2 / T * tan(M_PI * w / T);

    // Generate s-plane poles, zeros and gain
    // [zero, pole, gain] = ncauer(rp, rs, n);
    std::vector zero = { std::complex(0.0, 2.9988), std::complex(0.0, 1.4209), std::complex(0.0, -2.9988), std::complex(0.0, -1.4209) };
    std::vector pole = { std::complex(-0.2273, 0.4709), std::complex(-0.0595, 0.9666), std::complex(-0.2273, -0.4709), std::complex(-0.0595, -0.9666) };
    double gain = 9.9997e-03;

    // splane frequency transform
    // [zero, pole, gain] = sftrans(zero, pole, gain, w, false);
    for (int i = 0; i < n; i++) {
        zero[i] = zero[i] * w;
        pole[i] = pole[i] * w;
    }

    // Use bilinear transform to convert poles to the z plane
    // [zero, pole, gain] = bilinear (zero, pole, gain, T);
    //   Zg = real(G * prod((2-Z*T)/T) / prod((2-P*T)/T))
    //   Zp = (2+P*T)./(2-P*T);
    //   Zz = (2+Z*T)./(2-Z*T);
    std::complex<double> Zgc = std::complex<double>(gain, 0);
    for (int i = 0; i < n; i++) {
        Zgc *= (2.0 - zero[i] * T) / (2.0 - pole[i] * T);
        pole[i] = (2.0 + pole[i] * T) / (2.0 - pole[i] * T);
        zero[i] = (2.0 + zero[i] * T) / (2.0 - zero[i] * T);
    }
    gain = Zgc.real();

    // Convert the roots to polynomials
    // b = real(gain * poly(zero))
    // a = real(poly(pole))

    auto to_poly = [n](const std::vector<std::complex<double>> &roots, double gain) -> std::array<float, n + 1> {
        std::vector<std::complex<double>> poly(n + 1);
        poly[0] = 1;
        for (int j = 0; j < n; j++)
            for (int k = j + 1; k >= 1; k--)
                poly[k] -= roots[j] * poly[k - 1];

        std::array<float, n + 1> coeffs{};
        for (int j = 0; j <= n; j++) {
            assert(abs(poly[j].imag()) < 1e-10);
            coeffs[j] = gain * poly[j].real();
        }

        return coeffs;
    };

    const auto b = to_poly(zero, gain);
    const auto a = to_poly(pole, 1.0);

    return new IirFilter<5>("Lowpass Elliptic", b, a);
}
