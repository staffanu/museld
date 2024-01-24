//
// Created by staffanu on 7/2/23.
//

#ifndef MUSECPP_LOGGER_H
#define MUSECPP_LOGGER_H

#include <string>
#include <algorithm>
#include <map>
#include <mutex>

enum LogPriority {
    eDebug = 1,
    eInfo = 2,
    eWarn = 3,
    eError = 4,
    eOff = 100,
};

enum LogCategoryFlags {
    eApplication = 1,
    ePerformance = 2,
    eAudio = 4,
    eVideo = 8,
    eDecoder = 16,
    eInput = 32,
};

inline LogCategoryFlags operator|(LogCategoryFlags a, LogCategoryFlags b)
{
    return static_cast<LogCategoryFlags>(static_cast<int>(a) | static_cast<int>(b));
}

class Logger {
public:
    static const std::map<LogCategoryFlags, LogPriority> c_log_all;
    static const std::map<LogCategoryFlags, LogPriority> c_log_info;
    static const std::map<LogCategoryFlags, LogPriority> c_log_warn;

    // The log_priority_per_category map contains the minimum priority for each category
    // to produce a log message.  eError messages are always logged, so passing an empty
    // map logs all eError logs.
    explicit Logger(std::map<LogCategoryFlags, LogPriority> log_priority_per_category)
    : m_log_priority_per_category(std::move(log_priority_per_category))
    {
        m_minimum_priority = std::min_element(
                m_log_priority_per_category.cbegin(), m_log_priority_per_category.cend(),
                [](const std::pair<LogCategoryFlags, LogPriority> a, const std::pair<LogCategoryFlags, LogPriority> b) -> bool {
                    return a.second < b.second;
                })->second;
    }

    void log(LogPriority priority, LogCategoryFlags categorization, const std::string &message);

    void error(LogCategoryFlags categorization, const std::string &message) {
        log(eError, categorization, message);
    }
    void warn(LogCategoryFlags categorization, const std::string &message) {
        log(eWarn, categorization, message);
    }
    void info(LogCategoryFlags categorization, const std::string &message) {
        log(eInfo, categorization, message);
    }
    void debug(LogCategoryFlags categorization, const std::string &message) {
        log(eDebug, categorization, message);
    }
    bool isEnabled(LogPriority priority, LogCategoryFlags categorization) const;

private:
    static const std::map<int, std::string> c_priority_names;
    static const std::map<int, std::string> c_category_names;

    std::map<LogCategoryFlags, LogPriority> m_log_priority_per_category;
    LogPriority m_minimum_priority;
    std::mutex m_mutex;
};

#endif //MUSECPP_LOGGER_H
