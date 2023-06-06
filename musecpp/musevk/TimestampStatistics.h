//
// Created by staffanu on 6/6/23.
//

#ifndef MUSECPP_TIMESTAMPSTATISTICS_H
#define MUSECPP_TIMESTAMPSTATISTICS_H


#include <vector>
#include <map>
#include <string>
#include <iostream>
#include <valarray>
#include <iomanip>

namespace musevk {
    class TimestampStatistics {
    public:
        TimestampStatistics() = default;

        void add_timestamps(std::vector<std::pair<std::string, int>> timestamps);

        void print_stats() {
            for (auto timestamp_set: m_data) {
                for (int i = 0; i < timestamp_set.first.size(); i++) {
                    std::cout << std::setw(30) << timestamp_set.first[i] << ": ";
                    timestamp_set.second[i].print_stats();
                }
                std::cout << std::endl;
            }
        }
    private:
        struct SingleStatistic {
            SingleStatistic() : m_min(INT32_MAX), m_max(0), m_sum(0), m_sum2(0), m_n(0) {}
            int m_sum;
            long m_sum2;
            int m_n;
            int m_min;
            int m_max;

            void merge(int v) {
                m_n++;
                if (v > m_max)
                    m_max = v;
                if (v < m_min)
                    m_min = v;
                m_sum += v;
                m_sum2 += v * v;
            }

            std::string to_string() const {
                char s[100];
                snprintf(s, sizeof(s), "n=%-4d mean %5d (stddev %4d) (%5d-%5d)",
                         m_n,
                         m_sum / m_n,
                         (int)sqrt((double)m_sum2 / m_n - ((double)m_sum / m_n) * ((double)m_sum / m_n)),
                         m_min,
                         m_max);
                return {s};
            }
        };

        struct TimestampStats {
            SingleStatistic m_total;
            SingleStatistic m_elapsed_since_prev;

            void merge(int total_elapsed, int elapsed_since_prev) {
                m_total.merge(total_elapsed);
                m_elapsed_since_prev.merge(elapsed_since_prev);
            }

            void print_stats() const {
                std::cout << "Cum: " << m_total.to_string() <<
                          ", This: " << m_elapsed_since_prev.to_string() << std::endl;
            };
        };

        std::map<std::vector<std::string>, std::vector<TimestampStats>> m_data;
    };
}

#endif //MUSECPP_TIMESTAMPSTATISTICS_H
