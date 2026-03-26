//
// Created by Staffan Ulfberg on 11/4/25.
//

#ifndef AC3RF_EFM_DECODE_EFMDEMODULATOR_H
#define AC3RF_EFM_DECODE_EFMDEMODULATOR_H

#include <string>

#include "../Logger.h"
#include "../filter/FirFilterStage.h"
#include "../filter/IirFilter.h"
#include "TimingRecovery.h"

class EfmDemodulator {
public:
    EfmDemodulator(Logger &log, double input_sample_frequency, int input_block_size, bool use_simd,
        bool rf_input, int log2_decimation, int adaptive_filter_size, std::optional<std::string> retiming_debug_filename);
    ~EfmDemodulator();

    EfmDemodulator(const EfmDemodulator &) = delete;
    EfmDemodulator &operator=(const EfmDemodulator &) = delete;
    EfmDemodulator(EfmDemodulator &&) = delete;
    EfmDemodulator &operator=(EfmDemodulator &&) = delete;

    // Filters the input samples and performs clock recovery to extract the channel bit stream of 4.3218 M symbols/s
    void demodulate(const float *input_buffer, std::vector<float> &reclocked_data);

private:
    static IirFilter<5> *makeEllipticLowpassFilter(double Fs);

    Logger &m_log;
    double m_input_sample_frequency;
    int m_input_block_size;
    bool m_rf_input;
    int m_log2_decimation;
    int m_decimation_factor;
    std::vector<FirFilterStage *> m_decimation_filter_stages;
    IirFilter<2> m_remove_dc_filter;
    IirFilter<5> *m_low_pass_filter;
    IirFilter<3> *m_phase_adjust_filter;
    std::vector<float> m_filtered_input;

    TimingRecovery m_timing_recovery;
};

#endif //AC3RF_EFM_DECODE_EFMDEMODULATOR_H
