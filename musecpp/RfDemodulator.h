//
// Created by staffanu on 12/10/23.
//

#ifndef MUSECPP_RFDEMODULATOR_H
#define MUSECPP_RFDEMODULATOR_H

#include <fcntl.h>
#include <unistd.h>
#include <fstream>
#include <fmt/format.h>
#include <cassert>
#include <complex>
#include <array>
#include <algorithm>
#include <atomic>
#include <deque>
#include <condition_variable>
#include "Logger.h"
#include "MuseTypes.h"

namespace RfDemodulatorHelper {
    // FIR passband filter to filter out noise over 22.5 MHz and also the pilot signal and EFM.
    // The pilot signal isn't completely supressed but that doesn't seem to matter.
    // Computed using gnuradio:
    // firdes.complex_band_pass(1.0, 62.5e6, 2.7e6, 22.3e6, 2e6, window.WIN_RECTANGULAR)
    static constexpr std::pair<std::array<float, 32>, std::array<float, 32>> initializeBandpassFilter() {
        std::array<std::complex<float>, 32> bandpass_filter_def = {
                std::complex<float>(0.0065876576118171215, 0.020274756476283073),
                std::complex<float>(-0.004714661277830601, 0.00342539488337934),
                std::complex<float>(0.01447536051273346, 0.010516997426748276),
                std::complex<float>(-0.00879573542624712, 0.02707030437886715),
                std::complex<float>(-0.013138349168002605, -3.2994755372328655e-08),
                std::complex<float>(0.005763731896877289, 0.017739124596118927),
                std::complex<float>(-0.03205157443881035, 0.02328665927052498),
                std::complex<float>(-0.021089427173137665, -0.015322495251893997),
                std::complex<float>(-0.005935038439929485, 0.018265916034579277),
                std::complex<float>(-0.061971265822649, -2.5511641865705315e-07),
                std::complex<float>(-0.017557775601744652, -0.05403804033994675),
                std::complex<float>(-0.015810636803507805, 0.01148699875921011),
                std::complex<float>(-0.11814974993467331, -0.0858415812253952),
                std::complex<float>(0.08165450394153595, -0.2513030767440796),
                std::complex<float>(0.3123723268508911, 1.3405565368884709e-06),
                std::complex<float>(0.08165234327316284, 0.2513037919998169),
                std::complex<float>(-0.1181504875421524, 0.08584056794643402),
                std::complex<float>(-0.015810538083314896, -0.011487134732306004),
                std::complex<float>(-0.01755823940038681, 0.05403788760304451),
                std::complex<float>(-0.061971265822649, -2.76787233133291e-07),
                std::complex<float>(-0.005934881512075663, -0.018265968188643456),
                std::complex<float>(-0.02108955755829811, 0.015322314575314522),
                std::complex<float>(-0.03205139935016632, -0.023286905139684677),
                std::complex<float>(0.005763866938650608, -0.017739081755280495),
                std::complex<float>(-0.013138349168002605, -6.724289391968341e-08),
                std::complex<float>(-0.008795528672635555, -0.02707037143409252),
                std::complex<float>(0.014475440606474876, -0.010516887530684471),
                std::complex<float>(-0.0047146352007985115, -0.0034254309721291065),
                std::complex<float>(0.006587812211364508, -0.020274706184864044),
                std::complex<float>(0, 0), // padding to make size multiple of 8
                std::complex<float>(0, 0),
                std::complex<float>(0, 0),
        };

        std::array<float, 32> bandpass_filter_re{};
        std::array<float, 32> bandpass_filter_im{};
        for (int i = 0; i < bandpass_filter_def.size(); i++) {
            bandpass_filter_re[i] = bandpass_filter_def[bandpass_filter_def.size() - i - 1].real();
            bandpass_filter_im[i] = bandpass_filter_def[bandpass_filter_def.size() - i - 1].imag();
        }

        return std::make_pair(bandpass_filter_re, bandpass_filter_im);
    }

    // FIR lowpass filter for the demodulated signal
    // Computed using gnuradio: firdes.low_pass(1.0, 62.5e6, 9.8e6, 2e6, window.WIN_RECTANGULAR)
    static constexpr std::array<float, 32> initializeLowpassFilter() {
        return std::array<float, 32>{
                0.021318137645721436,
                0.005827637854963541,
                -0.01789254881441593,
                -0.028463421389460564,
                -0.013138349168002605,
                0.01865200139582157,
                0.03961782529950142,
                0.02606804110109806,
                -0.01920594647526741,
                -0.061971265822649,
                -0.0568188801407814,
                0.019542962312698364,
                0.14604157209396362,
                0.26423606276512146,
                0.3123723268508911,
                0.26423606276512146,
                0.14604157209396362,
                0.019542962312698364,
                -0.0568188801407814,
                -0.061971265822649,
                -0.01920594647526741,
                0.02606804110109806,
                0.03961782529950142,
                0.01865200139582157,
                -0.013138349168002605,
                -0.028463421389460564,
                -0.01789254881441593,
                0.005827637854963541,
                0.021318137645721436,
                0, // padding to make size multiple of 8
                0,
                0,
        };
    }

    // Root raised cosine filter for pulse shaping
    // Notice that the root raised cosine filter works on half the sample rate, i.e., 31.25 MHz
    // Computed in gnuradio using RRC filter taps with Sample rate 31.25 MHz, Symbol rate 16.2 MHz, Excess BW 0.1.
    static constexpr std::array<float, 16> initializeRrcFilter() {
        return std::array<float, 16>{
                0.048089, 0.028415, -0.092251, -0.029997, 0.296034, 0.499418, 0.296034, -0.029997, -0.092251, 0.028415,
                0.048089, 0, 0, 0, 0, 0 // padding to make size multiple of 8
        };
    }

    // Lowpass filter for EFM
    // Computed in octave: fir1(7, 1.8e6 / 31.25e6)
    static constexpr std::array<float, 16> initializeEfmLowpassFilter() {
        return std::array<float, 16>{
            0.007487, 0.012178, 0.025290, 0.045819,
            0.070755, 0.095684, 0.115788, 0.127000,
            0.127000, 0.115788, 0.095684, 0.070755,
            0.045819, 0.025290, 0.012178, 0.007487
        };
    }

    // Equalization filter for EFM.  Works on 1/4 of the original sample frequency
    // Computed in octave using the script make_efm_filter.m, and then numerically optimized.
    static constexpr std::array<float, 32> initializeEfmEqualizationFilter() {
        auto coefficients = std::array<float, 32>{
//                0.030164, 0.065937, 0.101987, 0.129844,
//                0.141895, 0.133502, 0.104427, 0.059129,
//                0.005797, -0.045620, -0.085850, -0.108520,
//                -0.111545, -0.097331, -0.071798, -0.042551
                -0.397856, 0.094957, -0.285097, 0.002505,
                -0.056853, -0.263420, 0.394114, 0.005008,
                0.740662, 0.616144, 0.817413, 0.888591,
                0.858024, 0.612603, 0.227419, -0.712161,
                -0.817860, -0.999957, -0.996745, -0.902156,
                -0.252263, 0.132426, -0.169630, 0.821285,
                -0.095913, 0.166130, 0.376055, -0.558495,
                0.204774, -0.273269, 0.229580, 0.054656
        };
        auto reversed = std::array<float, 32>{};
        for (int i = 0; i < coefficients.size(); i++)
            reversed[coefficients.size() - 1 - i] = coefficients[i];
        return reversed;
    }

}

using namespace RfDemodulatorHelper;

struct DemodulatedBlock {
    static constexpr int c_block_size = 4096 / 2;
    static constexpr int c_efm_block_size = 4096 / 4;
    long rf_location; // offset in the rf input data -- for debugging
    float data[c_block_size];
    float efm_data[c_efm_block_size];
};

class RfDemodulator {
public:
    RfDemodulator(Logger &log, std::string filename);
    RfDemodulator(const RfDemodulator&) = delete;
    void operator=(const RfDemodulator&) = delete;

    bool initialize();
    std::shared_ptr<DemodulatedBlock> getNextDemodulatedBlock();
    void returnBlock(std::shared_ptr<DemodulatedBlock> &buffer);

    void seek(double seconds);
    void cleanup();

private:
    // The filters assume an input sampling rate of 62.5 MHz.  In order to allow that to be set in runtime
    // we could link against the gnuradio libraries.
    static constexpr float c_sample_frequency = 62.5e6f;
    static constexpr int c_decimation_rate = 2;
    static constexpr int c_dropout_decimation_rate = 4;
    static constexpr int c_efm_decimation_rate = 4;
    static constexpr int c_sample_block_size = 4096;
    // enough buffers for one frame
    static constexpr int c_number_of_block_buffers = (int)(MUSE_TOTAL_HEIGHT * MUSE_TOTAL_WIDTH * c_sample_frequency / c_decimation_rate / 16.2e6 / DemodulatedBlock::c_block_size);
    static constexpr float c_center_frequency = 12.5e6f;
    static constexpr float c_frequency_deviation = 1.9e6f;
    static constexpr float c_2pi = 2 * (float)M_PI;
    static constexpr int c_AVX_floats_per_chunk = 8;
    static constexpr int c_NEON_floats_per_chunk = 4;

    static constexpr std::pair<std::array<float, 32>, std::array<float, 32>> c_bandpass_filter = initializeBandpassFilter();
    static constexpr int c_bandpass_filter_size = c_bandpass_filter.first.size();

    static constexpr std::array<float, 32> c_lowpass_filter = initializeLowpassFilter();
    static constexpr int c_lowpass_filter_size = c_lowpass_filter.size();

    static constexpr std::array<float, 16> c_rrc_filter = initializeRrcFilter();
    static constexpr int c_rrc_filter_size = c_rrc_filter.size();

    static constexpr std::array<float, 16> c_efm_lowpass_filter = initializeEfmLowpassFilter();
    static constexpr int c_efm_lowpass_filter_size = c_efm_lowpass_filter.size();

    static constexpr std::array<float, 32> c_efm_equalization_filter = initializeEfmEqualizationFilter();
    static constexpr int c_efm_equalization_filter_size = c_efm_equalization_filter.size();

    static constexpr int c_input_buffer_size = c_sample_block_size + c_bandpass_filter_size - 1;
    static constexpr int c_lowpass_in_buffer_size = c_sample_block_size + c_lowpass_filter_size - 1;
    static constexpr int c_rrc_in_buffer_size = c_sample_block_size / c_decimation_rate + c_rrc_filter_size - 1;
    static constexpr int c_output_buffer_size = c_sample_block_size / c_decimation_rate;

    static constexpr int c_efm_equalization_in_buffer_size = c_sample_block_size / c_efm_decimation_rate + c_efm_equalization_filter_size - 1;
    static constexpr int c_efm_out_buffer_size = c_sample_block_size / c_efm_decimation_rate;

    void demodulate();

    bool readFloats(float *out, size_t n);

    template<size_t filter_size> void firFilter(
                const float *input,   // input signal of length at least output_size + filter_length - 1
                size_t output_size,   // number of output values to compute
                const std::array<float, filter_size> &filter,  // reversed filter coefficients, needs to be aligned at 32 byte multiple (c_AVX_floats_per_chunk * sizeof(float))
                int decimation_rate,
                float *output);

    Logger &m_log;
    const std::string m_filename;
    int m_input_fd;
    bool m_input_is_fifo;
    std::thread *m_demodulator_thread;
    std::deque<std::shared_ptr<DemodulatedBlock>> m_vacant_blocks;
    std::deque<std::shared_ptr<DemodulatedBlock>> m_filled_blocks;
    std::mutex m_mutex;
    std::condition_variable m_cv_filled;
    std::condition_variable m_cv_vacant;
    std::atomic<bool> m_stop_request;
    std::atomic<bool> m_reader_thread_finished;
};

#endif //MUSECPP_RFDEMODULATOR_H
