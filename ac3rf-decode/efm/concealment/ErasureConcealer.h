//
// Created by staffanu on 12/30/25.
//

#ifndef AC3RF_DECODE_ERASURECONCEALER_H
#define AC3RF_DECODE_ERASURECONCEALER_H

#include <map>
#include <memory>
#include <vector>
#include <string>
#include <optional>
#include "../../Logger.h"
#include "../TwoChannelSample.h"

class ErasureConcealer {
public:
    enum ConcealmentImplementation {
        None,
        RepeatingSample,
        Interpolating,
        AutoregressiveModel
    };

    static std::unique_ptr<ErasureConcealer> create(ConcealmentImplementation impl, Logger &log, std::optional<std::string> debug_filename);

    explicit ErasureConcealer(Logger &log, std::optional<std::string> debug_filename);

    std::vector<TwoChannelSample> processSamples(
        const std::vector<TwoChannelSampleWithErasureFlags> &samples_with_erasures);

    [[nodiscard]] std::string erasureStatistics() const;

    virtual ~ErasureConcealer();

protected:
    // Implementation-specific erasure concealer
    // Some samples might be erased in the returned vector, e.g., if gaps are too long.
    virtual std::vector<TwoChannelSampleWithErasureFlags> processSamplesImpl(const std::vector<TwoChannelSampleWithErasureFlags> &samples_with_erasures) = 0;

private:
    Logger &m_log;
    int m_consecutive_erased_samples[2];
    int m_debug_fd;
    std::map<int, int> m_erased_stats{};
    std::vector<TwoChannelSampleWithErasureFlags> m_saved_input;
};

class NullErasureConcealer : public ErasureConcealer {
public:
    NullErasureConcealer(Logger &log, const std::optional<std::string> &debug_filename)
        : ErasureConcealer(log, debug_filename) {
    }

    std::vector<TwoChannelSampleWithErasureFlags> processSamplesImpl(const std::vector<TwoChannelSampleWithErasureFlags> &samples_with_erasures) override {
        return samples_with_erasures;
    }
};

#endif //AC3RF_DECODE_ERASURECONCEALER_H
