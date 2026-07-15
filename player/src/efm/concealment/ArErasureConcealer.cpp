// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <algorithm>
#include <cassert>
#include <cmath>
#include "ArErasureConcealer.h"

ArErasureConcealer::ArErasureConcealer(
    Logger &log, int order, double mu, int max_li_erasures, int crossfade_length, std::optional<std::string> debug_filename) :
    ErasureConcealer(log, debug_filename),
    m_linear_interpolator(log, max_li_erasures, std::nullopt),
    m_log(log),
    m_order(order),
    m_mu(mu),
    m_error_replay_size(239),
    m_crossfade_length(crossfade_length),
    m_index(0),
    m_last_erased_index{0, 0},
    m_last_valid_index{0, 0} {
    m_a[0].resize(order);
    m_a[1].resize(order);
}

std::vector<TwoChannelSampleWithErasureFlags> ArErasureConcealer::processSamplesImpl(
    const std::vector<TwoChannelSampleWithErasureFlags> &samples_with_erasures) {

    auto post_linear_interpolation = m_linear_interpolator.processSamplesImpl(samples_with_erasures);

    std::vector<TwoChannelSampleWithErasureFlags> output;

    for (const auto &sample : post_linear_interpolation) {
        auto output_sample = sample;
        for (int ch = 0; ch < 2; ch++) {
            // LMS update if current sample not erased and history is valid so we can predict the current sample and compute the error
            if (!sample.erased[ch] && m_input_history.size() == m_order
                && std::all_of(m_input_history.cbegin(), m_input_history.cend(), [ch](const auto &s) { return !s.erased[ch]; })) {
                double prediction = 0;
                for (int i = 0; i < m_order; i++)
                    prediction += m_input_history[i].samples[ch] * m_a[ch][i];
                const double error = sample.samples[ch] - prediction;

                double x_2_sum = 0;
                for (int i = 0; i < m_order; i++)
                    x_2_sum += m_input_history[i].samples[ch] * m_input_history[i].samples[ch];

                const double c = m_mu / (x_2_sum + 10.0);
                for (int i = 0; i < m_order; i++)
                    m_a[ch][i] = m_a[ch][i] * (1 - 1e-5) + c * error * m_input_history[i].samples[ch];

                m_error_history[ch].push_back(error);
                if (m_error_history[ch].size() > m_error_replay_size)
                    m_error_history[ch].pop_front();
            }

            uint64_t d = m_index - m_last_erased_index[ch];
            bool crossfade = d < m_crossfade_length;
            if (m_output_history.size() == m_order && (output_sample.erased[ch] || crossfade)) {
                double predicted = 0;
                for (int i = 0; i < m_order; i++)
                    predicted += m_output_history[i].samples[ch] * m_a[ch][i];
                if (!m_error_history[ch].empty())
                    predicted += m_error_history[ch][(m_index - m_last_valid_index[ch]) % m_error_history[ch].size()];
                predicted *= 0.9;

                if (output_sample.erased[ch]) {
                    output_sample.samples[ch] = (int16_t)std::clamp(std::round(predicted), (double)INT16_MIN, (double)INT16_MAX);
                    output_sample.erased[ch] = false;
                } else {
                    // crossfade
                    double p = (double)d / m_crossfade_length;
                    output_sample.samples[ch] = (int16_t)std::clamp(std::round((1 - p) * predicted + p * sample.samples[ch]), (double)INT16_MIN, (double)INT16_MAX);
                }
            }

            // if (m_index - m_last_valid_index[ch] > 500 && !sample.erased[ch])
            //      printf("%ld %ld\n", m_index, m_index - m_last_valid_index[ch]);

            if (sample.erased[ch])
                m_last_erased_index[ch] = m_index;
            else
                m_last_valid_index[ch] = m_index;
        }
        output.push_back(output_sample);

        m_input_history.push_front(sample);
        m_output_history.push_front(output_sample);
        if (m_input_history.size() > m_order) {
            m_input_history.pop_back();
            m_output_history.pop_back();
        }
        assert(m_input_history.size() == m_output_history.size());
        m_index++;
    }

    return output;
}
