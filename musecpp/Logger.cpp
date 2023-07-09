//
// Created by staffanu on 7/2/23.
//

#include <chrono>
#include <fmt/format.h>
#include <fmt/chrono.h>
#include "Logger.h"

using namespace std;

const map<int, string> Logger::c_priority_names = {
        { eDebug, "DEBUG" },
        { eInfo, "INFO " },
        { eWarn, "WARN " },
        { eError, "ERROR" },
};

const map<int, string> Logger::c_category_names = {
        { eApplication, "app" },
        { ePerformance, "performance" },
        { eAudio, "audio" },
        { eVideo, "video" },
        { eDecoder, "decoder" },
        { eInput, "input" }
};

void Logger::log(LogPriority priority, LogCategoryFlags categorization, const std::string &message) {
    int flags = categorization;
    if (priority >= m_minimum_priority && categorization & m_enabled_categories) {
        auto tp = chrono::time_point_cast<chrono::microseconds>(chrono::system_clock::now());
        auto us = tp.time_since_epoch().count() % micro::den;

        std::unique_lock<std::mutex> lock(m_mutex);
        cerr << fmt::format("{:%Y-%m-%d %H:%M:%S}.{:06d} [{}] (",
                   tp, us, c_priority_names.at(priority));
        bool first = true;
        for (int bit = 1; flags != 0; bit <<= 1, flags >>= 1) {
            if (flags & 1) {
                if (!first)
                    std::cerr << " ";
                auto cat_str = c_category_names.find(bit);
                if (cat_str == c_category_names.cend())
                    std::cerr << "unknown category " << bit;
                else
                    std::cerr << cat_str->second;
                first = false;
            }
        }
        std::cerr << "): " << message << std::endl;
    }
}
