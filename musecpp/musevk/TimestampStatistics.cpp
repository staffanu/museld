//
// Created by staffanu on 6/6/23.
//

#include "TimestampStatistics.h"

using namespace std;

namespace musevk {
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
}
