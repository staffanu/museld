//
// Created by staffanu on 1/24/26.
//

#include "PopDetector.h"
#include <algorithm>
#include <cstdio>

PopDetector::PopDetector() :
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
                    int16_t neigh_max = INT16_MIN;
                    int16_t neigh_min = INT16_MAX;
                    for (int k = 1; k <= c_samples_per_side; k++) {
                        int16_t sl = m_saved_samples[i - k].samples[ch];
                        int16_t sr = m_saved_samples[i + k].samples[ch];
                        neigh_max = std::max(neigh_max, std::max(sl, sr));
                        neigh_min = std::min(neigh_min, std::min(sl, sr));
                    }
                    int max_deviation = 4 * (neigh_max - neigh_min + 120); // notice int to avoid overflow
                    int sample = m_saved_samples[i].samples[ch];

                    if (sample - neigh_max > max_deviation || neigh_min - sample > max_deviation) {
                        // printf("pop: %d:%ld   [", ch, m_processed_samples + i - 2);
                        // for (int k = -c_samples_per_side; k <= c_samples_per_side; k++)
                        //     printf(k == 0 ? "(%d) " : "%d ", m_saved_samples[i + k].samples[ch]);
                        // printf("]\n");
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
