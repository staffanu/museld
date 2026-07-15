// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include "TimestampStatistics.h"

using namespace std;

namespace musevk
{
    void TimestampStatistics::add_timestamps(vector<pair<string, int>> timestamps) {
        auto labels = vector<string>();
        for (auto &timestamp: timestamps)
            labels.push_back(timestamp.first);

        auto it = m_data.find(labels);
        if (it == m_data.end()) {
            auto new_stats = vector<TimestampStats>();
            for (int i = 0; i < timestamps.size(); i++) {
                int t = timestamps[i].second;
                int time_since_previous = i == 0 ? 0 : t - timestamps[i - 1].second;
                auto stats = TimestampStats();
                stats.merge(t, time_since_previous);
                new_stats.push_back(stats);
            }
            m_data[labels] = new_stats;
        } else
            for (int i = 0; i < timestamps.size(); i++) {
                int t = timestamps[i].second;
                int timeSincePrevious = i == 0 ? 0 : t - timestamps[i - 1].second;
                it->second[i].merge(t, timeSincePrevious);
            }
    }

    void TimestampStatistics::print_stats(int min_samples_threshold) {
        for (auto timestamp_set: m_data) {
            if (timestamp_set.second[0].m_total.m_n >= min_samples_threshold) {
                for (int i = 0; i < timestamp_set.first.size(); i++) {
                    std::cout << std::setw(30) << timestamp_set.first[i] << ": ";
                    timestamp_set.second[i].print_stats();
                }
                std::cout << std::endl;
            }
        }
    }
}