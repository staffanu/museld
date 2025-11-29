//
// Created by Staffan Ulfberg on 11/4/25.
//

#ifndef AC3RF_EFM_DECODE_EFMDEMODULATOR_H
#define AC3RF_EFM_DECODE_EFMDEMODULATOR_H

#include <string>

#include "../Logger.h"
#include "../filter/FirFilterStage.h"
#include "../filter/IirFilter.h"
#include "TwoChannelSample.h"
#include "EfmPll.h"
#include "EfmDecoder.h"
#include "TimingRecovery.h"

class EfmDemodulator {
public:
    EfmDemodulator(Logger &log, double input_sample_frequency, int input_block_size, int output_fd, bool use_simd,
        bool rf_input, int adaptive_filter_size, std::optional<std::string> retiming_debug_filename);
    ~EfmDemodulator();

    EfmDemodulator(const EfmDemodulator &) = delete;
    EfmDemodulator &operator=(const EfmDemodulator &) = delete;
    EfmDemodulator(EfmDemodulator &&) = delete;
    EfmDemodulator &operator=(EfmDemodulator &&) = delete;

    std::vector<TwoChannelSample> demodulate(const float *input_buffer);
    [[nodiscard]] std::string reedSolomonStatistics();

private:
    static IirFilter<5> *makeEllipticLowpassFilter(double Fs);

    Logger &m_log;
    double m_input_sample_frequency;
    int m_input_block_size;
    int m_output_fd;
    bool m_rf_input;
    int m_log2decimation;
    int m_decimation_factor;
    std::vector<FirFilterStage *> m_decimation_filter_stages;
    IirFilter<2> m_remove_dc_filter;
    IirFilter<5> *m_low_pass_filter;
    IirFilter<3> *m_phase_adjust_filter;
    std::vector<float> m_filtered_input;

    EfmPll m_efm_pll;
    TimingRecovery m_timing_recovery;
    EfmDecoder m_efm_decoder;

    int m_max_reclocked_size;
    bool *m_reclocked_data;
    int m_max_output_samples;
    std::vector<TwoChannelSample> m_output_samples;
};

#endif //AC3RF_EFM_DECODE_EFMDEMODULATOR_H
