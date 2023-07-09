//
// Created by staffanu on 7/2/23.
//

#include <chrono>
#include <fmt/format.h>
#include <fmt/chrono.h>
#include <sstream>
#include "Logger.h"

using namespace std;

const map<LogCategoryFlags, LogPriority>  Logger::c_log_all = {
        { eApplication, eDebug },
        { ePerformance, eDebug },
        { eAudio, eDebug },
        { eVideo, eDebug },
        { eDecoder, eDebug },
        { eInput, eDebug },
};

const map<LogCategoryFlags, LogPriority>  Logger::c_log_info = {
        { eApplication, eInfo },
        { ePerformance, eInfo },
        { eAudio, eInfo },
        { eVideo, eInfo },
        { eDecoder, eInfo },
        { eInput, eInfo },
};

const map<LogCategoryFlags, LogPriority>  Logger::c_log_warn = {
        { eApplication, eWarn },
        { ePerformance, eWarn },
        { eAudio, eWarn },
        { eVideo, eWarn },
        { eDecoder, eWarn },
        { eInput, eWarn },
};

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
    if (priority >= m_minimum_priority) {
        auto tp = chrono::time_point_cast<chrono::microseconds>(chrono::system_clock::now());
        auto us = tp.time_since_epoch().count() % micro::den;
        ostringstream ss;
        ss << fmt::format("{:%Y-%m-%d %H:%M:%S}.{:06d} [{}] (",
                   tp, us, c_priority_names.at(priority));
        bool first = true;
        bool do_log = false;
        for (int bit = 1; flags != 0; bit <<= 1, flags >>= 1) {
            if (flags & 1) {
                if (!first)
                    ss << " ";
                auto cat_str = c_category_names.find(bit);
                if (cat_str == c_category_names.cend())
                    ss << "unknown category " << bit;
                else
                    ss << cat_str->second;
                first = false;
                auto cat_level = m_log_priority_per_category.find((LogCategoryFlags)bit);
                if (priority == eError || cat_level != m_log_priority_per_category.cend() && priority >= cat_level->second)
                    do_log = true;
            }
        }

        if (do_log) {
            std::unique_lock<std::mutex> lock(m_mutex);
            cerr << ss.str() << "): " << message << endl;
        }
    }
}
