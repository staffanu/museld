//
// Created by Staffan Ulfberg on 11/4/25.
//

#ifndef AC3RF_EFM_DECODE_EFMDEMODULATOR_H
#define AC3RF_EFM_DECODE_EFMDEMODULATOR_H

#include <string>

#include "../Logger.h"
#include "../FirFilterStage.h"
#include "TwoChannelSample.h"
#include "EfmPll.h"
#include "EfmDecoder.h"
#include "GardnerTimingRecovery.h"

class EfmDemodulator {
public:
    EfmDemodulator(Logger &log, double input_sample_frequency, int input_block_size, int output_fd, bool rf_input, bool use_simd);
    ~EfmDemodulator();

    EfmDemodulator(const EfmDemodulator &) = delete;
    EfmDemodulator &operator=(const EfmDemodulator &) = delete;
    EfmDemodulator(EfmDemodulator &&) = delete;
    EfmDemodulator &operator=(EfmDemodulator &&) = delete;

    std::vector<TwoChannelSample> demodulate(float *input_buffer);
    [[nodiscard]] std::string reedSolomonStatistics() const;

private:
    Logger &m_log;
    double m_input_sample_frequency;
    int m_input_block_size;
    int m_output_fd;
    bool m_rf_input;
    bool m_use_simd;
    std::vector<float> m_input_filter;
    EfmPll m_efm_pll;
    GardnerTimingRecovery m_gardner_timing_recovery;
    EfmDecoder m_efm_decoder;

    float *m_input_filter_buffer; // size is input block size + input filter size - 1
    float *m_filtered_input;
    int m_max_reclocked_size;
    bool *m_reclocked_data;
    int m_max_output_samples;
    std::vector<TwoChannelSample> m_output_samples;
};

#endif //AC3RF_EFM_DECODE_EFMDEMODULATOR_H
