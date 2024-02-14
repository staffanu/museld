//
// Created by staffanu on 2/7/24.
//

#ifndef MUSECPP_EFMPLL_H
#define MUSECPP_EFMPLL_H

class Logger;

class EfmPll {
public:
    EfmPll(Logger &log);

    int reclock(const float *input, int input_size, bool *output, int max_output_size);

private:
    static constexpr double c_sample_frequency = 62.5e6 / 4;
    static constexpr double c_counter_max = 65536.0;
    static constexpr double c_nominal_frequency = 4321800;
    static constexpr double c_nominal_add = c_counter_max * c_nominal_frequency / c_sample_frequency;
    static constexpr double c_max_error_sum = 0x7fffff;
    static constexpr double c_min_error_sum = -0x800000;

    static constexpr double c_omega = 2 * M_PI * 10000; // un-dampened frequency
    static constexpr double c_zeta = 0.71; // damping factor
    static constexpr double c_Ts = 1 / c_sample_frequency;
    static const double c_G1;
    static const double c_G2;
    static const double c_GpdGvcoG1;
    static const double c_GpdGvcoG2;
    static const double c_g1;
    static const double c_g2;

    double m_clk_counter;
    double m_prev_in;
    double m_error_sum;
    double m_filter_out;
    int m_toggle_count;
    double m_toggle_pos;
};

#endif //MUSECPP_EFMPLL_H
