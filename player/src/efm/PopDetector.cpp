// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include "PopDetector.h"
#include <algorithm>
#include <format>
#include <sstream>

PopDetector::PopDetector(Logger &log) :
    m_log(log),
    m_processed_samples(0) {
    m_saved_samples.resize(c_samples_per_side);
}

std::vector<TwoChannelSampleWithErasureFlags> PopDetector::processSamples(
    const std::vector<TwoChannelSampleWithErasureFlags> &samples) {

    m_saved_samples.insert(m_saved_samples.end(), samples.cbegin(), samples.cend());

    std::vector<TwoChannelSampleWithErasureFlags> output;
    if (m_saved_samples.size() >= c_samples_per_side * 2 + 1) {
        output.resize(m_saved_samples.size() - c_samples_per_side * 2);

        for (int i = c_samples_per_side; i < m_saved_samples.size() - c_samples_per_side; i++) {
            TwoChannelSampleWithErasureFlags out = m_saved_samples[i];
            for (int ch = 0; ch < 2; ch++) {
                if (!out.erased[ch]) {
                    // For each sample that is not erased, we consider a small neighborhood.
                    // We consider the sample "bad" and erase it if the sample is much higher or
                    // lower than the surrounding samples.
                    //
                    // We determine the max and min of the unerased samples in the small neighborhood.
                    // We also determine the second highest/lowest, when considering all samples, i.e.,
                    // also erased samples.  We prefer to use the max/min of unerased samples, but if too
                    // many samples were erased, we fall back to using the second largest/smallest instead.
                    int unerased_count = 0;
                    int16_t unerased_max = INT16_MIN;
                    int16_t unerased_min = INT16_MAX;
                    int16_t total_max = INT16_MIN;
                    int16_t total_min = INT16_MAX;
                    int16_t total_max_runner_up = INT16_MIN;
                    int16_t total_min_runner_up = INT16_MAX;
                    for (int k = -c_samples_per_side; k <= c_samples_per_side; k++) {
                        if (k != 0) {
                            int16_t s = m_saved_samples[i + k].samples[ch];
                            if (!m_saved_samples[i + k].erased[ch]) {
                                unerased_count++;
                                unerased_max = std::max(s, unerased_max);
                                unerased_min = std::min(s, unerased_min);
                            }
                            if (s > total_max) {
                                total_max_runner_up = total_max;
                                total_max = s;
                            } else if (s > total_max_runner_up)
                                total_max_runner_up = s;

                            if (s < total_min) {
                                total_min_runner_up = total_min;
                                total_min = s;
                            } else if (s < total_min_runner_up)
                                total_min_runner_up = s;
                        }
                    }
                    // notice int to avoid overflow
                    const int max = unerased_count >= 4 ? unerased_max : total_max_runner_up;
                    const int min = unerased_count >= 4 ? unerased_min : total_min_runner_up;
                    const int max_deviation = 4 * (max - min) + 500;
                    const int sample = m_saved_samples[i].samples[ch];

                    if (sample - max > max_deviation || min - sample > max_deviation) {
                        std::ostringstream ss;
                        ss << std::format("Erased suspected pop: {}:{}  [ ", ch ? "R" : "L", m_processed_samples + i - c_samples_per_side);
                        for (int k = -c_samples_per_side; k <= c_samples_per_side; k++)
                            ss << (k == 0 ? "(" : "") << (m_saved_samples[i + k].erased[ch] ? "*" : "") << m_saved_samples[i + k].samples[ch] << (k == 0 ? ") " : " ");
                        ss << "]";
                        m_log.debug(eAudio, ss.str());
                        out.erased[ch] = true;
                    }
                }
            }
            output[i - c_samples_per_side] = out;
        }

        m_processed_samples += m_saved_samples.size() - c_samples_per_side * 2;
        m_saved_samples.erase(m_saved_samples.cbegin(), m_saved_samples.cend() - c_samples_per_side * 2);
    }
    return output;
}
