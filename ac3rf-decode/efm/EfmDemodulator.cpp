//
// Created by Staffan Ulfberg on 11/4/25.
//

#include "EfmDemodulator.h"

EfmDemodulator::EfmDemodulator(Logger &log, double input_sample_frequency, int input_block_size, int output_fd, bool use_simd)
: m_log(log),
  m_input_sample_frequency(input_sample_frequency),
  m_input_block_size(input_block_size),
  m_output_fd(output_fd) {



}

EfmDemodulator::~EfmDemodulator() {

}

std::vector<TwoChannelSample> EfmDemodulator::demodulate(float *input_buffer) {
    return std::vector<TwoChannelSample>();
}

std::string EfmDemodulator::reedSolomonStatistics() const {
    return "";
}
