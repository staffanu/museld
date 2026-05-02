//
// Created by staffanu on 6/6/23.
//

#ifndef MUSECPP_TIMESTAMPSTATISTICS_H
#define MUSECPP_TIMESTAMPSTATISTICS_H

#include <cstdint>
#include <vector>
#include <map>
#include <string>
#include <iostream>
#include <valarray>
#include <iomanip>

namespace musevk {
    class TimestampStatistics {
    public:
        TimestampStatistics() :
        m_data() {
        };

        void add_timestamps(std::vector<std::pair<std::string, int>> timestamps);
        void print_stats(int min_samples_threshold);

    private:
        struct SingleStatistic {
            SingleStatistic() : m_sum(0), m_sum2(0), m_n(0), m_min(INT32_MAX), m_max(0) {}
            double m_sum;
            double m_sum2;
            int m_n;
            double m_min;
            double m_max;

            void merge(int v) {
                m_n++;
                if (v > m_max)
                    m_max = v;
                if (v < m_min)
                    m_min = v;
                m_sum += v;
                m_sum2 += (double)v * v;
            }

            [[nodiscard]] std::string to_string() const {
                char s[100];
                snprintf(s, sizeof(s), "n=%-4d mean %6.0f (stddev %5.0f) (%6.0f-%6.0f)",
                         m_n,
                         m_sum / m_n,
                         sqrt(m_sum2 / m_n - (m_sum / m_n) * (m_sum / m_n)),
                         m_min,
                         m_max);
                return {s};
            }
        };

        struct TimestampStats {
            TimestampStats() :
            m_total(),
            m_elapsed_since_prev() {
            }

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
