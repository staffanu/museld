//
// Created by staffanu on 7/2/23.
//

#ifndef MUSECPP_LOGGER_H
#define MUSECPP_LOGGER_H

#include <string>
#include <map>
#include <iostream>
#include <mutex>

enum LogPriority {
    eDebug = 1,
    eInfo = 2,
    eWarn = 3,
    eError = 4
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
    Logger(LogPriority minimum_priority, LogCategoryFlags enabled_categories)
    : m_minimum_priority(minimum_priority),
      m_enabled_categories(enabled_categories)
    {}

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

private:
    static const std::map<int, std::string> c_priority_names;
    static const std::map<int, std::string> c_category_names;

    void log(LogPriority priority, LogCategoryFlags categorization, const std::string &message);

    LogPriority m_minimum_priority;
    LogCategoryFlags m_enabled_categories;
    std::mutex m_mutex;
};

#endif //MUSECPP_LOGGER_H
