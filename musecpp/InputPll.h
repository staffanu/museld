//
// Created by staffanu on 6/25/23.
//

#ifndef MUSECPP_INPUTPLL_H
#define MUSECPP_INPUTPLL_H

#include <cstdint>
#include <cmath>
#include <deque>

class InputPll {
public:
    explicit InputPll(int sample_rate);

    // returns true if output contains a full output frame
    bool process(uint16_t sample, uint16_t *output);
    double getInputSamplesPerSample();

private:
    static double c_omega; // undamped frequency
    static double c_zeta; // damping factor

    double m_input_samples_per_sample_ref;
    double m_input_samples_per_sample;
    double m_Ts; // We think of one m_line as one sample here since we detect the phase once per m_line
    double m_G1;
    double m_G2;
    double m_GpdGvco;
    double m_g1;
    double m_g2;

    int m_pixel;
    int m_line;
    int m_line1_frame_pulse_sum;
    int m_line2_frame_pulse_sum;
    int m_consecutive_good_syncs;
    double m_avg_sample_value;
    int m_missed_line_pulses;
    uint32_t m_shift_reg[5];
    int m_shift_reg_last_written_ix;

    enum State {
        eSearching, eLocked, eLockedHoriz
    };
    State m_state;
    State m_prev_state;
    int m_error_sum;
};

#endif //MUSECPP_INPUTPLL_H
