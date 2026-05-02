// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include "StreamLogger.h"

using namespace std;

const map<LogCategoryFlags, LogPriority> StreamLogger::c_log_all = {
        { eApplication, eDebug },
        { ePerformance, eDebug },
        { eAudio, eDebug },
        { eVideo, eDebug },
        { eDecoder, eDebug },
        { eInput, eDebug },
        { eOutput, eDebug },
};

const map<LogCategoryFlags, LogPriority> StreamLogger::c_log_info = {
        { eApplication, eInfo },
        { ePerformance, eInfo },
        { eAudio, eInfo },
        { eVideo, eInfo },
        { eDecoder, eInfo },
        { eInput, eInfo },
        { eOutput, eInfo },
};

const map<LogCategoryFlags, LogPriority> StreamLogger::c_log_warn = {
    { eApplication, eWarn },
    { ePerformance, eWarn },
    { eAudio, eWarn },
    { eVideo, eWarn },
    { eDecoder, eWarn },
    { eInput, eWarn },
    { eOutput, eWarn },
};

const map<LogCategoryFlags, LogPriority> StreamLogger::c_log_error = {
        { eApplication, eError },
        { ePerformance, eError },
        { eAudio, eError },
        { eVideo, eError },
        { eDecoder, eError },
        { eInput, eError },
        { eOutput, eError },
};

const map<LogCategoryFlags, LogPriority> StreamLogger::c_log_off = {
    { eApplication, eOff },
    { ePerformance, eOff },
    { eAudio, eOff },
    { eVideo, eOff },
    { eDecoder, eOff },
    { eInput, eOff },
    { eOutput, eOff },
};

const map<int, string> StreamLogger::c_priority_names = {
        { eDebug, "DEBUG" },
        { eInfo,  "INFO " },
        { eWarn,  "WARN " },
        { eError, "ERROR" },
        { eOff,   "OFF [NEVER-SHOWN]" },
};

const map<int, string> StreamLogger::c_category_names = {
        { eApplication, "app" },
        { ePerformance, "performance" },
        { eAudio,       "audio" },
        { eVideo,       "video" },
        { eDecoder,     "decoder" },
        { eInput,       "input" },
        { eOutput,      "output" },
};

StreamLogger::StreamLogger(std::map<LogCategoryFlags, LogPriority> log_priority_per_category,
                           std::ostream &log_stream,
                           bool log_category)
    : m_log_priority_per_category(std::move(log_priority_per_category))
    , m_log_stream(log_stream)
    , m_log_category(log_category)
    , m_minimum_priority(eDebug)
{
    if (!m_log_priority_per_category.empty())
        m_minimum_priority = std::min_element(
            m_log_priority_per_category.cbegin(), m_log_priority_per_category.cend(),
            [](const pair<LogCategoryFlags, LogPriority> &a, const pair<LogCategoryFlags, LogPriority> &b) {
                return a.second < b.second;
            })->second;
}

void StreamLogger::log(LogPriority priority, LogCategoryFlags categorization, const std::string &message) {
    int flags = categorization;
    if (priority >= m_minimum_priority) {
        auto tp = chrono::time_point_cast<chrono::milliseconds>(chrono::system_clock::now());
        auto now_t = chrono::system_clock::to_time_t(tp);
        struct tm tm_info{};
        localtime_r(&now_t, &tm_info);
        ostringstream ss;
        ss << std::put_time(&tm_info, "%Y-%m-%d %H:%M:%S")
           << " [" << c_priority_names.at(priority) << "] ";
        if (m_log_category)
            ss << "(";
        bool first = true;
        bool do_log = false;
        for (int bit = 1; flags != 0; bit <<= 1, flags >>= 1) {
            if (flags & 1) {
                if (m_log_category) {
                    if (!first)
                        ss << " ";
                    auto cat_str = c_category_names.find(bit);
                    if (cat_str == c_category_names.cend())
                        ss << "unknown category " << bit;
                    else
                        ss << cat_str->second;
                    first = false;
                }
                auto cat_level = m_log_priority_per_category.find((LogCategoryFlags)bit);
                if ((cat_level == m_log_priority_per_category.cend() && priority == eError) ||
                    (cat_level != m_log_priority_per_category.cend() && priority >= cat_level->second))
                    do_log = true;
            }
        }

        if (do_log) {
            if (m_log_category)
                ss << "): ";
            std::unique_lock<std::mutex> lock(m_mutex);
            m_log_stream << ss.str() << message << endl;
        }
    }
}

bool StreamLogger::isEnabled(LogPriority priority, LogCategoryFlags category) const {
    auto it = m_log_priority_per_category.find(category);
    return it != m_log_priority_per_category.cend() && it->second <= priority;
}

void StreamLogger::sync() {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_log_stream.flush();
}
