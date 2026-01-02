//
// Created by staffanu on 12/30/25.
//

#include <cstdint>
#include "../TwoChannelSample.h"
#include "LinearInterpolationErasureConcealer.h"

LinearInterpolationErasureConcealer::LinearInterpolationErasureConcealer(Logger &log, std::optional<std::string> debug_filename) :
    ErasureConcealer(log, debug_filename) {
    m_input.insert(m_input.end(), c_overlap_size, TwoChannelSampleWithErasureFlags{});
}

// Conceals erasures by performing linear interpolation over missing samples.
std::vector<TwoChannelSampleWithErasureFlags> LinearInterpolationErasureConcealer::processSamplesImpl(const std::vector<TwoChannelSampleWithErasureFlags> &samples_with_erasures) {
    m_input.insert(m_input.end(), samples_with_erasures.begin(), samples_with_erasures.end());

    std::vector<TwoChannelSampleWithErasureFlags> output;
    if (m_input.size() > 2 * c_overlap_size) {
        int output_size = m_input.size() - 2 * c_overlap_size;
        output.insert(output.end(), m_input.begin() + c_overlap_size, m_input.begin() + c_overlap_size + output_size);

        for (int ch = 0; ch < 2; ch++) {
            // Interpolate erasures
            for (int i = 1; i < m_input.size() - 1; i++) {
                if (i != 1 && m_input[i - 1].erased[ch])
                    continue;
                int j;
                for (j = i; j < m_input.size() - 1 && m_input[j].erased[ch]; j++)
                    ;
                int n_erased = j - i;
                if (n_erased == 0)
                    continue; // no erased data
                // data from i until j (exclusive) is erased (or, j is also erased if end of buffer)

                // If we didn't find any non-erased data, just use 0
                int prev = m_input[i - 1].erased[ch] ? 0 : m_input[i - 1].samples[ch];
                int next = m_input[j].erased[ch] ? 0 : m_input[j].samples[ch];
                for (int k = 0; k < n_erased; k++) {
                    if (i + k - c_overlap_size >= 0 && i + k - c_overlap_size < output.size()) {
                        int16_t concealed = (int16_t)(prev + (k + 1) * (next - prev) / (n_erased + 1));
                        output[i + k - c_overlap_size].samples[ch] = concealed;
                        output[i + k - c_overlap_size].erased[ch] = false;
                    }
                }
                i = j;
            }
        }
       m_input.erase(m_input.begin(), m_input.begin() + output_size);
    }
    return output;
}

