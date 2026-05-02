// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#include <algorithm>
#include <chrono>
#include <iostream>
#include <format>
#include <sstream>
#include "CategoryLogger.h"

using namespace std;

CategoryLogger::CategoryLogger(std::map<LogCategoryFlags, LogPriority> log_priority_per_category)
    : m_log_priority_per_category(std::move(log_priority_per_category)),
      m_minimum_priority(eDebug)
{
    m_minimum_priority = std::min_element(
            m_log_priority_per_category.cbegin(), m_log_priority_per_category.cend(),
            [](const std::pair<LogCategoryFlags, LogPriority> a, const std::pair<LogCategoryFlags, LogPriority> b) {
                return a.second < b.second;
            })->second;
}

const map<LogCategoryFlags, LogPriority> CategoryLogger::c_log_all = {
        { eApplication, eDebug },
        { ePerformance, eDebug },
        { eAudio, eDebug },
        { eVideo, eDebug },
        { eDecoder, eDebug },
        { eInput, eDebug },
        { eOutput, eDebug },
};

const map<LogCategoryFlags, LogPriority> CategoryLogger::c_log_info = {
        { eApplication, eInfo },
        { ePerformance, eInfo },
        { eAudio, eInfo },
        { eVideo, eInfo },
        { eDecoder, eInfo },
        { eInput, eInfo },
        { eOutput, eInfo },
};

const map<LogCategoryFlags, LogPriority> CategoryLogger::c_log_warn = {
        { eApplication, eWarn },
        { ePerformance, eWarn },
        { eAudio, eWarn },
        { eVideo, eWarn },
        { eDecoder, eWarn },
        { eInput, eWarn },
        { eOutput, eWarn },
};

const map<int, string> CategoryLogger::c_priority_names = {
        { eDebug, "DEBUG" },
        { eInfo,  "INFO " },
        { eWarn,  "WARN " },
        { eError, "ERROR" },
        { eOff,   "OFF [NEVER-SHOWN]" },
};

const map<int, string> CategoryLogger::c_category_names = {
        { eApplication, "app" },
        { ePerformance, "performance" },
        { eAudio,       "audio" },
        { eVideo,       "video" },
        { eDecoder,     "decoder" },
        { eInput,       "input" },
        { eOutput,      "output" },
};

void CategoryLogger::log(LogPriority priority, LogCategoryFlags categorization, const std::string &message) {
    int flags = categorization;
    if (priority >= m_minimum_priority) {
        auto tp = chrono::time_point_cast<chrono::milliseconds>(chrono::system_clock::now());
        auto ms = tp.time_since_epoch().count() % milli::den;
        ostringstream ss;
        ss << std::format("{:%Y-%m-%d %H:%M:%S}.{:03d} [{}] (",
                          tp, ms, c_priority_names.at(priority));
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
                if ((cat_level == m_log_priority_per_category.cend() && priority == eError) ||
                    (cat_level != m_log_priority_per_category.cend() && priority >= cat_level->second))
                    do_log = true;
            }
        }
        if (do_log) {
            std::unique_lock<std::mutex> lock(m_mutex);
            cerr << ss.str() << "): " << message << endl;
        }
    }
}

bool CategoryLogger::isEnabled(LogPriority priority, LogCategoryFlags category) const {
    auto it = m_log_priority_per_category.find(category);
    return it != m_log_priority_per_category.cend() && it->second <= priority;
}

void CategoryLogger::sync() {
    std::unique_lock<std::mutex> lock(m_mutex);
    std::cerr.flush();
}
