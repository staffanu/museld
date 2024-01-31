//
// Created by staffanu on 6/25/23.
//

#ifndef MUSECPP_INPUTPLL_H
#define MUSECPP_INPUTPLL_H

#include <cstdint>
#include <cmath>
#include <deque>

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
    };
    PllResult process(int sample_count, const float samples[], float *output);
    double getInputSamplesPerSample();

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
    double m_line1_frame_pulse_sum;
    double m_line2_frame_pulse_sum;
    int m_consecutive_good_syncs;
    double m_avg_sample_value;
    int m_missed_line_pulses;

    enum State {
        eSearching, eLocked, eLockedHoriz
    };
    State m_state;
    double m_error_sum;
};

#endif //MUSECPP_INPUTPLL_H
