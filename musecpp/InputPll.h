//
// Created by staffanu on 6/25/23.
//

#ifndef MUSECPP_INPUTPLL_H
#define MUSECPP_INPUTPLL_H

#include <cstdint>
#include <cmath>
#include <deque>
#include "util/PercentileFilter.h"

class Logger;

class InputPll {
public:
    explicit InputPll(Logger &log);
    void initialize(double sample_rate);

    // returns true if output contains a full output frame
    struct PllResult {
        bool frame_done;
        int samples_to_read;
        double input_samples_per_sample;
        bool locked;
        long frame_input_offset;
    };
    PllResult process(int sample_count, long input_offset,
                      const float samples[], const uint8_t dropouts[],
                      float *output, uint8_t *dropout_output);
    [[nodiscard]] double getInputSamplesPerSample() const;
    void setUnlocked();

private:
    static double c_omega; // undamped frequency
    static double c_zeta; // damping factor

    Logger &m_log;
    double m_input_samples_per_sample_ref;
    double m_input_samples_per_sample;
    double m_Ts; // We think of one line as one sample here since we detect the phase once per line
    double m_G1;
    double m_G2;
    double m_GpdGvco;
    double m_g1;
    double m_g2;

    int m_pixel;
    int m_line;
    float m_line1_frame_pulse_sum;
    float m_line2_frame_pulse_sum;
    PercentileFilter m_upper_percentile_filter;
    PercentileFilter m_lower_percentile_filter;
    float m_max_frame_pulse_sum_difference;
    int m_consecutive_good_syncs;
    int m_missed_line_pulses;
    long m_frame_start_offset;

    enum State {
        eSearching, eLocked, eLockedHoriz
    };
    State m_state;
    double m_error_sum;
};

#endif //MUSECPP_INPUTPLL_H
