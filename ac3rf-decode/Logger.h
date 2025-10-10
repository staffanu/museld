#ifndef AC3RRF_DECODE_LOGGER_H
#define AC3RRF_DECODE_LOGGER_H

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

class Logger {
public:
    explicit Logger(LogPriority log_priority);

    void log(LogPriority priority, const std::string &message);

    void error(const std::string &message) {
        log(eError, message);
    }
    void warn(const std::string &message) {
        log(eWarn, message);
    }
    void info(const std::string &message) {
        log(eInfo, message);
    }
    void debug(const std::string &message) {
        log(eDebug, message);
    }
    bool isEnabled(LogPriority priority) const;

private:
    static const std::map<int, std::string> c_priority_names;

    LogPriority m_log_priority;
    std::mutex m_mutex;
};

#endif //AC3RRF_DECODE_LOGGER_H
