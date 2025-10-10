#include <chrono>
#include <iostream>
#include <format>
#include "Logger.h"

const std::map<int, std::string> Logger::c_priority_names = {
    { eDebug, "DEBUG" },
    { eInfo, "INFO " },
    { eWarn, "WARN " },
    { eError, "ERROR" },
    { eOff, "OFF [NEVER-SHOWN]" },
};

Logger::Logger(LogPriority log_priority)
: m_log_priority(log_priority),
  m_mutex()
  {}

void Logger::log(LogPriority priority, const std::string &message) {
    if (priority >= m_log_priority) {
        auto tp = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
        auto ms = tp.time_since_epoch().count() % std::milli::den;
        std::ostringstream ss;
        ss << std::format("{:%Y-%m-%d %H:%M:%S}.{:03d} [{}] (",
                          tp, ms, c_priority_names.at(priority));
        std::unique_lock<std::mutex> lock(m_mutex);
        std::cerr << ss.str() << "): " << message << std::endl;
    }
}

bool Logger::isEnabled(LogPriority priority) const {
    return priority >= m_log_priority;
}
