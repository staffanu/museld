//
// Created by staffanu on 12/30/25.
//

#include <vector>
#include <fcntl.h>
#include <stdexcept>
#include <cstring>
#include <format>
#include <unistd.h>
#include <optional>
#include <sstream>
#include "ErasureConcealer.h"

#include "ArErasureConcealer.h"
#include "InterpolatingErasureConcealer.h"
#include "RepeatingSampleErasureConcealer.h"
#include "../TwoChannelSample.h"

std::unique_ptr<ErasureConcealer> ErasureConcealer::create(
    ConcealmentImplementation impl, Logger &log, std::optional<std::string> debug_filename) {
    switch (impl) {
        case None:
            return std::make_unique<NullErasureConcealer>(log, debug_filename);
        case RepeatingSample:
            return std::make_unique<RepeatingSampleErasureConcealer>(log, debug_filename);
        case Interpolating:
            return std::make_unique<InterpolatingErasureConcealer>(log, debug_filename);
        case AutoregressiveModel:
            return std::make_unique<ArErasureConcealer>(log, 400, 2000, debug_filename);
        default:
            throw std::runtime_error(std::format("Unknown concealment implementation: {}", static_cast<int>(impl)));
    }
}

ErasureConcealer::ErasureConcealer(Logger &log, std::optional<std::string> debug_filename) :
    m_log(log),
    m_debug_fd(-1),
    m_consecutive_erased_samples{0, 0} {

    if (debug_filename.has_value()) {
        m_debug_fd = open(debug_filename.value().c_str(), O_WRONLY | O_TRUNC | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        if (m_debug_fd == -1)
            throw std::runtime_error(std::format("Unable to open output erasure debug file: {}", strerror(errno)));
    }
}

ErasureConcealer::~ErasureConcealer() {
    if (m_debug_fd != -1)
        close(m_debug_fd);
}

std::vector<TwoChannelSample> ErasureConcealer::processSamples(
    const std::vector<TwoChannelSampleWithErasureFlags> &samples_with_erasures) {

    // Erasure statistics
    for (int ch = 0; ch < 2; ch++)
        for (auto const &output_with_erasure : samples_with_erasures) {
            if (output_with_erasure.erased[ch])
                m_consecutive_erased_samples[ch]++;
            else {
                m_erased_stats[m_consecutive_erased_samples[ch]]++;
                m_consecutive_erased_samples[ch] = 0;
            }
        }

    m_saved_input.insert(m_saved_input.end(), samples_with_erasures.begin(), samples_with_erasures.end());
    auto concealed = processSamplesImpl(samples_with_erasures);

    std::vector<TwoChannelSampleWithErasureFlags> input;
    input.assign(m_saved_input.begin(), m_saved_input.begin() + concealed.size());
    m_saved_input.erase(m_saved_input.begin(), m_saved_input.begin() + concealed.size());

    if (m_debug_fd != -1) {
        int16_t debug_data[concealed.size() * 6];
        for (int i = 0; i < concealed.size(); i++) {
            debug_data[i * 6] = input[i].samples[0];
            debug_data[i * 6 + 1] = input[i].samples[1];
            debug_data[i * 6 + 2] = concealed[i].samples[0];
            debug_data[i * 6 + 3] = concealed[i].samples[1];
            debug_data[i * 6 + 4] = input[i].erased[0];
            debug_data[i * 6 + 5] = input[i].erased[1];
        }
        if (write(m_debug_fd, debug_data, concealed.size() * sizeof(int16_t) * 6) == -1)
            throw std::runtime_error(std::format("Error writing to debug file: {}", strerror(errno)));
    }

    std::vector<TwoChannelSample> output(concealed.size());
    for (int i = 0; i < concealed.size(); i++)
        for (int ch = 0; ch < 2; ch++)
            output[i].samples[ch] = concealed[i].samples[ch];

    return output;
}

std::string ErasureConcealer::erasureStatistics() const {
    std::ostringstream oss;
    oss << "Output erasure run length statistics: ";
    for (auto p: m_erased_stats)
        oss << std::format("{}: {}, ", p.first, p.second);
    return oss.str();
}

