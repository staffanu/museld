//
// Created by staffanu on 2/7/24.
//

#include <fmt/format.h>
#include <cassert>
#include "EfmPll.h"
#include "../util/Logger.h"

const double EfmPll::c_G1 = 1 - exp(-2 * c_zeta * c_omega * c_Ts);
const double EfmPll::c_G2 = 1 + exp(-2 * c_omega * c_zeta * c_Ts)
                            - 2 * exp(-c_omega * c_zeta * c_Ts) * cos(c_omega * c_Ts * sqrt(1 - c_zeta * c_zeta));
const double EfmPll::c_GpdGvcoG1 = 1 / 5.0; // P
const double EfmPll::c_GpdGvcoG2 = 1; // I
const double EfmPll::c_g1 = c_G1 / c_GpdGvcoG1;
const double EfmPll::c_g2 = c_G2 / c_GpdGvcoG2;

EfmPll::EfmPll(Logger &log)
        : m_log(log),
          m_clk_counter(0),
          m_prev_in(false),
          m_error_sum(0),
          m_filter_out(0),
          m_toggle_count(0),
          m_toggle_pos(0) {
    log.debug(eInput | eAudio, fmt::format("c_g1={:.5f} c_g2={:.7f}", c_g1, c_g2));
}

int EfmPll::reclock(const float *input, int input_size, bool *output, int max_output_size) {
    int output_size = 0;
    for (int i = 0; i < input_size; i++) {
        double data_in = input[i];

        if (data_in * m_prev_in <= 0) {
            m_toggle_count += 1;
            double frac = fabs(m_prev_in - data_in) < 0.01 ? 0.5 : m_prev_in / (m_prev_in - data_in);
            m_toggle_pos = m_clk_counter - (1 - frac) * (c_nominal_add + m_filter_out);
            if (m_toggle_pos < 0)
                m_toggle_pos += c_counter_max;
            m_prev_in = data_in;
        }

        double new_counter = m_clk_counter + c_nominal_add + m_filter_out;
        if (new_counter >= c_counter_max)
            new_counter -= c_counter_max;
        m_filter_out = 0;
        if (new_counter < m_clk_counter) {
            if (output_size >= max_output_size) {
                m_log.warn(eAudio, "EFM reclocked data buffer overflow");
                return output_size;
            }
            output[output_size++] = m_toggle_count % 2 == 1;

            m_filter_out = 0;
            if (m_toggle_count == 1) {
                double error = -(m_toggle_pos - c_counter_max / 2);
                if (error > 0 && m_error_sum + error > c_max_error_sum)
                    m_error_sum = c_max_error_sum;
                else if (error < 0 && m_error_sum + error < c_min_error_sum)
                    m_error_sum = c_min_error_sum;
                else
                    m_error_sum += error;
                m_filter_out = error * c_g1;
            }
            m_filter_out += m_error_sum * c_g2;

            m_toggle_count = 0;
        }
        m_clk_counter = new_counter;
    }
    return output_size;
}
