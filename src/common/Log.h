#pragma once

#include <sstream>
#include <string>

namespace usp {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

void set_log_level(LogLevel level);
LogLevel log_level();
LogLevel parse_log_level(const std::string& s);
void log_write(LogLevel level, const std::string& msg);
std::string utc_now_iso8601();

}  // namespace usp

#define USP_LOG(level, expr)                                   \
  do {                                                         \
    if (static_cast<int>(level) >= static_cast<int>(::usp::log_level())) { \
      std::ostringstream usp_log_os_;                          \
      usp_log_os_ << expr;                                     \
      ::usp::log_write(level, usp_log_os_.str());              \
    }                                                          \
  } while (0)

#define LOG_DEBUG(expr) USP_LOG(::usp::LogLevel::Debug, expr)
#define LOG_INFO(expr) USP_LOG(::usp::LogLevel::Info, expr)
#define LOG_WARN(expr) USP_LOG(::usp::LogLevel::Warn, expr)
#define LOG_ERROR(expr) USP_LOG(::usp::LogLevel::Error, expr)
