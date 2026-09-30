#include "common/Log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace usp {

namespace {
std::atomic<int> g_level{static_cast<int>(LogLevel::Info)};
std::mutex g_mutex;

const char* level_name(LogLevel l) {
  switch (l) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO ";
    case LogLevel::Warn: return "WARN ";
    case LogLevel::Error: return "ERROR";
  }
  return "?";
}
}  // namespace

void set_log_level(LogLevel level) { g_level = static_cast<int>(level); }
LogLevel log_level() { return static_cast<LogLevel>(g_level.load()); }

LogLevel parse_log_level(const std::string& s) {
  if (s == "debug") return LogLevel::Debug;
  if (s == "warn" || s == "warning") return LogLevel::Warn;
  if (s == "error") return LogLevel::Error;
  return LogLevel::Info;
}

std::string utc_now_iso8601() {
  using namespace std::chrono;
  auto now = system_clock::now();
  std::time_t t = system_clock::to_time_t(now);
  auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900,
                tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms));
  return buf;
}

void log_write(LogLevel level, const std::string& msg) {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::fprintf(stderr, "%s %s %s\n", utc_now_iso8601().c_str(), level_name(level), msg.c_str());
  std::fflush(stderr);
}

}  // namespace usp
