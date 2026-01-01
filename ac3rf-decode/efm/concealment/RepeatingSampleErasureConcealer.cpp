//
// Created by staffanu on 12/30/25.
//

#include "../TwoChannelSample.h"
#include "RepeatingSampleErasureConcealer.h"

RepeatingSampleErasureConcealer::RepeatingSampleErasureConcealer(Logger &log, std::optional<std::string> debug_filename) :
    ErasureConcealer(log, debug_filename),
    m_prev_sample{} {
}

// Conceals erasures by just repeating the previous sample
std::vector<TwoChannelSampleWithErasureFlags> RepeatingSampleErasureConcealer::processSamplesImpl(const std::vector<TwoChannelSampleWithErasureFlags> &samples_with_erasures) {
    std::vector<TwoChannelSampleWithErasureFlags> output;
    output.assign(samples_with_erasures.begin(), samples_with_erasures.end());

    for (int i = 0; i < output.size(); i++) {
        for (int ch = 0; ch < 2; ch++) {
            if (output[i].erased[ch]) {
                output[i].samples[ch] = m_prev_sample.samples[ch];
                output[i].erased[ch] = false;
            }
        }
        m_prev_sample = output[i];
    }
    return output;
}
