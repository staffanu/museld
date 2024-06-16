//
// Created by staffanu on 2/7/24.
//

#ifndef MUSECPP_EFMPLL_H
#define MUSECPP_EFMPLL_H

class Logger;

class EfmPll {
public:
    EfmPll(Logger &log, double sample_frequency);

    int reclock(const float *input, int input_size, bool *output, int max_output_size);

private:
    static constexpr double c_counter_max = 65536.0;
    static constexpr double c_nominal_frequency = 4321800;
    static constexpr double c_max_error_sum = 0x7fffff;
    static constexpr double c_min_error_sum = -0x800000;

    const double c_sample_frequency;
    const double c_nominal_add;

    const double c_omega; // un-dampened frequency
    const double c_zeta; // damping factor
    const double c_Ts;
    const double c_G1;
    const double c_G2;
    const double c_GpdGvcoG1; // P
    const double c_GpdGvcoG2; // I
    const double c_g1;
    const double c_g2;

    Logger &m_log;
    double m_clk_counter;
    double m_prev_in;
    double m_error_sum;
    double m_filter_out;
    int m_toggle_count;
    double m_toggle_pos;
};

#endif //MUSECPP_EFMPLL_H
