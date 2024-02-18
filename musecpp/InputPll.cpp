//
// Created by staffanu on 6/25/23.
//

#include <cassert>
#include <fmt/format.h>
#include "InputPll.h"
#include "MuseTypes.h"
#include "Logger.h"

using namespace std;

double InputPll::c_omega = 2 * M_PI * 5000 / 54e6;
double InputPll::c_zeta = 0.75;

InputPll::InputPll(Logger &log)
: m_log(log),
  m_pixel(1),
  m_line(1),
  m_line1_frame_pulse_sum(0),
  m_line2_frame_pulse_sum(0),
  m_upper_percentile_filter(0.995f, 239.f),
  m_lower_percentile_filter(0.005f, 16.f),
  m_max_frame_pulse_sum_difference(0),
  m_consecutive_good_syncs(0),
  m_missed_line_pulses(0),
  m_state(eSearching),
  m_error_sum(0) {
}

void InputPll::initialize(double sample_rate) {
    m_input_samples_per_sample_ref = sample_rate / 16.2e6;
    m_input_samples_per_sample = m_input_samples_per_sample_ref;
    m_Ts = m_input_samples_per_sample_ref * 480;
    m_G1 = 1 - exp(-2 * c_zeta * c_omega * m_Ts);
    m_G2 = 1 + exp(-2 * c_omega * c_zeta * m_Ts) -
           2 * exp(-c_omega * c_zeta * m_Ts) * cos(c_omega * m_Ts * sqrt(1 - c_zeta * c_zeta));
    m_GpdGvco = 64 * (1 / m_input_samples_per_sample_ref) * 480;
    m_g1 = m_G1 / m_GpdGvco;
    m_g2 = m_G2 / m_GpdGvco;

    m_log.debug(eInput, fmt::format("m_g1={:.5f} m_g2={:.7f}", m_g1, m_g2));
}

InputPll::PllResult InputPll::process(int sample_count, const float samples[], float *output) {
    for (int sample_ix = 0; sample_ix < sample_count; sample_ix++) {
        // if we are at the first pixel we should also not be in the middle of the loop
        assert (!(m_state == eLocked && m_line == 1 && m_pixel == 1) || sample_ix == 0);

        float sample = samples[sample_ix];
        m_upper_percentile_filter.update(sample);
        m_lower_percentile_filter.update(sample);

        int output_index = MUSE_TOTAL_WIDTH * (m_line - 1) + m_pixel - 1;
        output[output_index] = sample;

        if (m_state == eLockedHoriz || m_state == eLocked && m_line <= 2) {
            if (m_pixel == 312) {
                m_line1_frame_pulse_sum = m_line2_frame_pulse_sum;
                m_line2_frame_pulse_sum = 0;
            } else if (m_pixel >= 313 && m_pixel <= 478) { // skip the two last pixels: especially line 2 often have bad values
                int pulsePixel = m_pixel - 313;
                float pulseValue = (pulsePixel < 144 ? pulsePixel / 4 % 2 == 0 : pulsePixel < 160) ? 1.f : -1.f;
                m_line2_frame_pulse_sum += pulseValue * sample;
            }
            // We calculate the sums over 164 pixels, so if the signal levels of the high and low parts of the signal
            // differ by d, a perfect fit with the frame sync signals should have a difference between line 1 and line 2
            // of 166 * d.  If the input is scaled according to the MUSE specification, d = 239 - 16 = 223,
            // so the difference should be 37018.  In practice, for a correctly scaled input signal we get around 27000,
            // or 73 % of the theoretical value.  The percentile filters estimate the upper/lower signal values, but there
            // is some variation due to the content of the rest of the signal, so we set the thresholds lower.
            if (m_pixel == 480) {
                if (m_state == eLockedHoriz) {
                    float threshold = 223.0f * 0.25f * (m_upper_percentile_filter.getEstimate() - m_lower_percentile_filter.getEstimate());
                    m_max_frame_pulse_sum_difference = max(m_max_frame_pulse_sum_difference, m_line1_frame_pulse_sum - m_line2_frame_pulse_sum);
                    if (m_line1_frame_pulse_sum - m_line2_frame_pulse_sum > threshold) {

                        m_log.info(eInput, fmt::format("New state eLocked at line {}, diff={}, threshold={}",
                                                       m_line, m_line1_frame_pulse_sum - m_line2_frame_pulse_sum, threshold));
                        // copy the two last lines to lines 1 and 2 so that the first frame is complete
                        for (int i = 0; i < MUSE_TOTAL_WIDTH; i++) {
                            output[i] = output[(m_line - 2) * MUSE_TOTAL_WIDTH + i];
                            output[i + MUSE_TOTAL_WIDTH] = output[(m_line - 1) * MUSE_TOTAL_WIDTH + i];
                        }
                        m_line = 2;
                        m_state = eLocked;
                    }
                }
                if (m_state == eLocked && m_line == 2) {
                    float threshold = 223.0f * 0.25f * (m_upper_percentile_filter.getEstimate() - m_lower_percentile_filter.getEstimate());
                    if (m_line1_frame_pulse_sum - m_line2_frame_pulse_sum > threshold)
                        m_missed_line_pulses = 0;
                    else {
                        if (m_missed_line_pulses < 3)
                            m_missed_line_pulses += 1;
                        else {
                            m_log.warn(eInput, fmt::format("Missed line pulses (diff={}, threshold={}): New state eSearching at line {}",
                                                           m_line1_frame_pulse_sum - m_line2_frame_pulse_sum, threshold, m_line));
                            m_state = eSearching;
                        }
                    }
                }
            }
        }

        if (m_pixel == 8) {
            // When locked: line 1: positive, line 2: negative, line 3: negative, m_line 4: positive, then alternating up to 1125
            bool sync_should_be_positive = m_line == 1 || (m_line > 3 && m_line % 2 == 0);

            float sample0 = output[output_index - 4]; // (int)m_shift_reg[(m_shift_reg_last_written_ix + 1) % 5];
            float sample2 = output[output_index - 2]; // (int)m_shift_reg[(m_shift_reg_last_written_ix + 3) % 5];
            float sample4 = output[output_index]; // (int)m_shift_reg[m_shift_reg_last_written_ix];

            bool m_sync_is_good = (sync_should_be_positive && sample0 < sample2 && sample2 < sample4) ||
                                  (!sync_should_be_positive && sample0 > sample2 && sample2 > sample4);

            if (m_sync_is_good) {
                double avgLevel = (sample0 + sample4) / 2;
                double new_error = sync_should_be_positive ? sample2 - avgLevel : avgLevel - sample2; // negative means we sampled too early
                m_error_sum += new_error;
                m_input_samples_per_sample =
                        m_input_samples_per_sample_ref - new_error * m_g1 - m_error_sum * m_g2;
            }
            if (m_state == eSearching) {
                m_consecutive_good_syncs = m_sync_is_good ? m_consecutive_good_syncs + 1 :
                                           m_consecutive_good_syncs >= 2 ? m_consecutive_good_syncs - 2 : 0;
                if (m_consecutive_good_syncs >= 50) {
                    m_log.info(eInput, fmt::format("New state eLockedHoriz at line {}", m_line));
                    m_state = eLockedHoriz;
                    m_line2_frame_pulse_sum = 0;
                    m_max_frame_pulse_sum_difference = 0;
                }
            }
            if ((m_state == eSearching && ((m_consecutive_good_syncs < 5 && m_line == 50) || m_line == 100)) ||
                (m_state == eLockedHoriz && m_line == 1125)) {
                m_consecutive_good_syncs = 0;
                m_error_sum = 0;
                m_input_samples_per_sample = m_input_samples_per_sample_ref;

                if (m_state == eLockedHoriz) {
                    float threshold = 223.0f * 0.25f * (m_upper_percentile_filter.getEstimate() - m_lower_percentile_filter.getEstimate());
                    m_log.info(eInput, fmt::format(
                            "Locked horizontally, but frame pulses not found (max sum={}, threshold={}): New state eSearching at line {}",
                            m_max_frame_pulse_sum_difference, threshold, m_line));
                }
                m_state = eSearching; // TODO: add to VHDL
                m_line = 3;
                m_pixel = 263; // "random", start search from new position
            }
        }

        if (m_pixel < 480)
            m_pixel++;
        else {
            m_pixel = 1;
            m_line = m_line == 1125 ? 1 : m_line + 1;
        }
    }
    bool frame_done = m_state == eLocked && m_line == 1 && m_pixel == 1;

    int samples_to_read = m_line == 1125 ? 481 - m_pixel : m_pixel < 9 ? 9 - m_pixel : 480 - m_pixel + 9;
    return {frame_done, samples_to_read, m_input_samples_per_sample, m_state == eLocked};
}

double InputPll::getInputSamplesPerSample() const {
    return m_input_samples_per_sample;
}

void InputPll::setUnlocked() {
    m_state = eSearching;
    m_line = 333; // make sure we do not recognize old data and immediately re-lock
    m_log.info(eInput, "state externally set to eSearching");
}
