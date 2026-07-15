// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef AC3RF_STREAMLOGGER_H
#define AC3RF_STREAMLOGGER_H

#include <iostream>
#include <map>
#include <mutex>
#include "Logger.h"

// Concrete Logger that writes formatted, timestamped messages to an ostream.
// Use the static c_log_* presets to configure the per-category minimum priority.
class StreamLogger : public Logger {
public:
    static const std::map<LogCategoryFlags, LogPriority> c_log_all;
    static const std::map<LogCategoryFlags, LogPriority> c_log_info;
    static const std::map<LogCategoryFlags, LogPriority> c_log_warn;
    static const std::map<LogCategoryFlags, LogPriority> c_log_error;
    static const std::map<LogCategoryFlags, LogPriority> c_log_off;

    // log_priority_per_category: minimum priority per category to emit output.
    // eError messages are always emitted; an empty map logs only errors.
    explicit StreamLogger(std::map<LogCategoryFlags, LogPriority> log_priority_per_category,
                          std::ostream &log_stream,
                          bool log_category);

    void log(LogPriority priority, LogCategoryFlags categorization, const std::string &message) override;
    [[nodiscard]] bool isEnabled(LogPriority priority, LogCategoryFlags categorization) const override;
    void sync() override;

private:
    static const std::map<int, std::string> c_priority_names;
    static const std::map<int, std::string> c_category_names;

    const std::map<LogCategoryFlags, LogPriority> m_log_priority_per_category;
    std::ostream &m_log_stream;
    const bool m_log_category;
    LogPriority m_minimum_priority;
    std::mutex m_mutex;
};

#endif //AC3RF_STREAMLOGGER_H
