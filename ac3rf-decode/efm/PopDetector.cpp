//
// Created by staffanu on 1/24/26.
//

#include "PopDetector.h"
#include <algorithm>

PopDetector::PopDetector() {
    m_saved_samples.resize(2);
}

std::vector<TwoChannelSampleWithErasureFlags> PopDetector::processSamples(
    const std::vector<TwoChannelSampleWithErasureFlags> &samples) {

    m_saved_samples.insert(m_saved_samples.end(), samples.cbegin(), samples.cend());

    std::vector<TwoChannelSampleWithErasureFlags> output;
    if (m_saved_samples.size() >= 5) {
        output.resize(m_saved_samples.size() - 4);

        for (int i = 2; i < m_saved_samples.size() - 2; i++) {
            TwoChannelSampleWithErasureFlags out = m_saved_samples[i];
            for (int ch = 0; ch < 2; ch++) {
                int16_t v[4] = {
                    m_saved_samples[i - 2].samples[ch],
                    m_saved_samples[i - 1].samples[ch],
                    m_saved_samples[i + 1].samples[ch],
                    m_saved_samples[i + 2].samples[ch]
                };
                int16_t neigh_max = *std::max_element(v, v + 4);
                int16_t neigh_min = *std::min_element(v, v + 4);
                int d = neigh_max - neigh_min + 5;
                int sample = m_saved_samples[i].samples[ch];

                if (sample - neigh_max > 20 * d || neigh_min - sample > 20 * d) {
                    // if (!out.erased[ch])
                    //     printf("%d:%d   [%d %d (%d) %d %d]\n", ch, i, v[0], v[1], m_saved_samples[i].samples[ch],v[2], v[3]);
                    out.erased[ch] = true;
                }
            }
            output[i - 2] = out;
        }

        m_saved_samples.erase(m_saved_samples.cbegin(), m_saved_samples.cend() - 4);
    }
    return output;
}
