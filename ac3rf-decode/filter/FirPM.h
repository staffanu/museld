//
// Created by staffanu on 11/18/25.
//

#ifndef AC3RF_DECODE_FIRPM_H
#define AC3RF_DECODE_FIRPM_H

#include <format>
#include <stdexcept>
#include <vector>
#include "firpm/pm.h"

namespace FirPM {
    template<typename R>
    static std::vector<R> firpm(
        std::size_t n,
        const std::vector<double> &bands,
        const std::vector<double> &des,
        const std::vector<double> &weights
    ) {
        auto out = pm::firpm<double>(n, bands, des, weights);
        if (out.status != pm::status_t::STATUS_SUCCESS)
            throw std::runtime_error(std::format("PM failed: {}", (int)out.status));
        std::vector<R> result(out.h.cbegin(), out.h.cend());
        return result;
    }

    template<typename R>
    static std::pair<std::string, std::vector<R>> low_pass(
        std::size_t n,
        double Fs,
        double transition_start,
        double transition_end
        )
    {
        return make_pair(std::format("Low pass Fs {}, trans start {}, end {}", Fs, transition_start, transition_end),
            firpm<R>(n, {0, transition_start / (Fs / 2), transition_end / (Fs / 2), 1}, {1, 1, 0, 0}, {1, 1}));
    }
}

#endif //AC3RF_DECODE_FIRPM_H
