// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#ifndef MUSELD_CATEGORYLOGGER_H
#define MUSELD_CATEGORYLOGGER_H

#include <map>
#include <mutex>
#include <string>
#include "Logger.h"

class CategoryLogger : public Logger {
public:
    static const std::map<LogCategoryFlags, LogPriority> c_log_all;
    static const std::map<LogCategoryFlags, LogPriority> c_log_info;
    static const std::map<LogCategoryFlags, LogPriority> c_log_warn;

    explicit CategoryLogger(std::map<LogCategoryFlags, LogPriority> log_priority_per_category);

    void log(LogPriority priority, LogCategoryFlags categorization, const std::string &message) override;
    [[nodiscard]] bool isEnabled(LogPriority priority, LogCategoryFlags categorization) const override;
    void sync() override;

private:
    static const std::map<int, std::string> c_priority_names;
    static const std::map<int, std::string> c_category_names;

    std::map<LogCategoryFlags, LogPriority> m_log_priority_per_category;
    LogPriority m_minimum_priority;
    std::mutex m_mutex;
};

#endif //MUSELD_CATEGORYLOGGER_H
