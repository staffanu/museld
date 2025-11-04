//
// Created by Staffan Ulfberg on 11/4/25.
//

#ifndef AC3RF_EFM_DECODE_EFMDEMODULATOR_H
#define AC3RF_EFM_DECODE_EFMDEMODULATOR_H

#include <string>

#include "../Logger.h"
#include "../FirFilterStage.h"
#include "TwoChannelSample.h"

class EfmDemodulator {
public:
    EfmDemodulator(Logger &log, double input_sample_frequency, int input_block_size, int output_fd, bool use_simd);
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
};

#endif //AC3RF_EFM_DECODE_EFMDEMODULATOR_H
