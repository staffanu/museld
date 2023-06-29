//
// Created by staffanu on 6/25/23.
//

#include <iostream>
#include <optional>
#include "InputPll.h"
#include "MuseTypes.h"

using namespace std;

double InputPll::c_omega = 2 * M_PI * 5000 / 54e6;
double InputPll::c_zeta = 0.75;

InputPll::InputPll(int sample_rate)
: m_input_samples_per_sample_ref(sample_rate / 16.2e6),
  m_input_samples_per_sample(m_input_samples_per_sample_ref),
  m_Ts(m_input_samples_per_sample_ref * 480),
  m_G1(1 - exp(-2 * c_zeta * c_omega * m_Ts)),
  m_G2(1 + exp(-2 * c_omega * c_zeta * m_Ts) -
    2 * exp(-c_omega * c_zeta * m_Ts) * cos(c_omega * m_Ts * sqrt(1 - c_zeta * c_zeta))),
  m_GpdGvco(64 * (1 / m_input_samples_per_sample_ref) * 480),
  m_g1(m_G1 / m_GpdGvco),
  m_g2(m_G2 / m_GpdGvco),
  m_pixel(1),
  m_line(1),
  m_line1_frame_pulse_sum(0),
  m_line2_frame_pulse_sum(0),
  m_consecutive_good_syncs(0),
  m_avg_sample_value(128),
  m_missed_line_pulses(0),
  m_shift_reg{},
  m_shift_reg_last_written_ix(0),
  m_state(eSearching),
  m_prev_state(eSearching),
  m_error_sum(0) {
    cout << "m_g1=" << m_g1 << " m_g2=" << m_g2 << endl;
}

bool InputPll::process(uint16_t sample, uint16_t *output) {
    m_avg_sample_value = m_avg_sample_value * (1 - 1e-7) + 1e-7 * sample;

    if (m_state == eLockedHoriz)
        output[m_pixel - 1] = sample; // ensure first line is correct when lock is established
    else if (m_state == eLocked)
        output[MUSE_TOTAL_WIDTH * (m_line - 1) + m_pixel - 1] = sample;

    m_shift_reg_last_written_ix++;
    if (m_shift_reg_last_written_ix == 5)
        m_shift_reg_last_written_ix = 0;
    m_shift_reg[m_shift_reg_last_written_ix] = sample;

    if (m_state == eLockedHoriz || m_state == eLocked && m_line <= 2) {
        if (m_pixel == 316) {
            m_line1_frame_pulse_sum = 0;
            m_line2_frame_pulse_sum = 0;
        } else if (m_pixel >= 317 && m_pixel < 480) { // notice we ignore m_pixel 480
            int framePulsePixel = m_pixel - 317;
            int line1FramePulseValue =
                    ((framePulsePixel < 140 && (framePulsePixel / 4) % 2 == 1) ||
                     (framePulsePixel >= 140 && framePulsePixel < 156)) ? 1 : -1;
            int line2FramePulseValue = -line1FramePulseValue;
            m_line1_frame_pulse_sum += line1FramePulseValue * (sample - (int) m_avg_sample_value);
            m_line2_frame_pulse_sum += line2FramePulseValue * (sample - (int) m_avg_sample_value);
        } else if (m_pixel == 480) {
            if (m_state == eLockedHoriz && m_line1_frame_pulse_sum > 3000) { // TODO: maybe adjust or make threshold dynamic
                m_line = 1;
                m_state = eLocked;
            }
            if (m_state == eLocked && (m_line == 1 || m_line == 2)) {
                if ((m_line == 1 && m_line1_frame_pulse_sum > 3000) || (m_line == 2 && m_line2_frame_pulse_sum > 3000))
                    m_missed_line_pulses = 0;
                else {
                    if (m_missed_line_pulses < 3)
                        m_missed_line_pulses += 1;
                    else
                        m_state = eSearching;
                }
            }
        }
    }

    if (m_pixel == 8) {
        // When locked: m_line 1: positive, m_line 2: negative, m_line 3: negative, m_line 4: positive, then alternating up to 1125
        bool sync_should_be_positive = m_line == 1 || (m_line > 3 && m_line % 2 == 0);

        int sample0 = (int)m_shift_reg[(m_shift_reg_last_written_ix + 1) % 5];
        int sample2 = (int)m_shift_reg[(m_shift_reg_last_written_ix + 3) % 5];
        int sample4 = (int)m_shift_reg[m_shift_reg_last_written_ix];

        bool m_sync_is_good = (sync_should_be_positive && sample0 < sample2 && sample2 < sample4) ||
                         (!sync_should_be_positive && sample0 > sample2 && sample2 > sample4);

        if (m_sync_is_good) {
            int avgLevel = (sample0 + sample4) / 2;
            int new_error = sync_should_be_positive ? sample2 - avgLevel : avgLevel - sample2; // negative means we sampled too early
            m_error_sum += new_error;
            m_input_samples_per_sample =
                    m_input_samples_per_sample_ref - new_error * m_g1 - m_error_sum * m_g2;
        }

        if (m_state == eSearching) {
            m_consecutive_good_syncs = m_sync_is_good ? m_consecutive_good_syncs + 1 :
                                       m_consecutive_good_syncs >= 2 ? m_consecutive_good_syncs - 2 : 0;
            if (m_consecutive_good_syncs >= 50)
                m_state = eLockedHoriz;
        }
    }

    if (m_pixel < 480)
        m_pixel++;
    else if ((m_state == eSearching && ((m_consecutive_good_syncs < 5 && m_line == 50) || m_line == 100)) ||
             (m_state == eLockedHoriz && m_line == 1125)) {
        m_consecutive_good_syncs = 0;
        m_error_sum = 0;
        m_input_samples_per_sample = m_input_samples_per_sample_ref;

        m_state = eSearching; // TODO: add to VHDL
        m_line = 3;
        m_pixel = 263; // "random", start search from new position
    } else {
        m_pixel = 1;
        m_line = m_line == 1125 ? 1 : m_line + 1;
    }

    if (m_state != m_prev_state) {
        cout << "New m_state " << m_state << " at m_line " << m_line;
        m_prev_state = m_state;
    }

    return m_line == 1 && m_pixel == 1;
}

double InputPll::getInputSamplesPerSample() {
    return m_input_samples_per_sample;
}
