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
#include "TimingRecovery.h"

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
    std::vector<float> makeRfInputFilter(double input_sample_frequency) const;
    static double interpolate(int n, const double x[], const double y[], double p);

    Logger &m_log;
    double m_input_sample_frequency;
    int m_input_block_size;
    int m_output_fd;
    bool m_rf_input;
    FirFilterStage *m_input_filter;
    std::vector<float> m_filtered_input;
    EfmPll m_efm_pll;
    TimingRecovery m_timing_recovery;
    EfmDecoder m_efm_decoder;

    float m_prev_x; // for minimal IIR filter to kill DC
    float m_prev_y;

    int m_max_reclocked_size;
    bool *m_reclocked_data;
    int m_max_output_samples;
    std::vector<TwoChannelSample> m_output_samples;
};

#endif //AC3RF_EFM_DECODE_EFMDEMODULATOR_H
