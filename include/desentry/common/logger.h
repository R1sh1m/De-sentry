#pragma once
// Minimal thread-safe leveled logger. Not a dependency -- production
// deployments would swap this for spdlog/glog, but the MVP should not
// require pulling a logging framework off the network to build.

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <sstream>
#include <string>

namespace desentry {

enum class LogLevel { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3 };

class Logger {
 public:
  static Logger& Instance() {
    static Logger instance;
    return instance;
  }

  void SetLevel(LogLevel level) { level_ = level; }

  void Log(LogLevel level, const std::string& component, const std::string& msg) {
    if (level < level_) return;
    std::lock_guard<std::mutex> lock(mu_);
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    std::fprintf(stderr, "%s.%03d [%s] %-5s %s\n", buf, static_cast<int>(ms.count()),
                 component.c_str(), LevelName(level), msg.c_str());
  }

 private:
  static const char* LevelName(LogLevel level) {
    switch (level) {
      case LogLevel::kDebug: return "DEBUG";
      case LogLevel::kInfo: return "INFO";
      case LogLevel::kWarn: return "WARN";
      case LogLevel::kError: return "ERROR";
    }
    return "?";
  }

  LogLevel level_ = LogLevel::kInfo;
  std::mutex mu_;
};

}  // namespace desentry

#define DSN_LOG(level, component, msg_stream)                              \
  do {                                                                     \
    std::ostringstream _dsn_oss;                                          \
    _dsn_oss << msg_stream;                                               \
    ::desentry::Logger::Instance().Log(level, component, _dsn_oss.str()); \
  } while (0)

#define DSN_LOG_DEBUG(component, msg) DSN_LOG(::desentry::LogLevel::kDebug, component, msg)
#define DSN_LOG_INFO(component, msg) DSN_LOG(::desentry::LogLevel::kInfo, component, msg)
#define DSN_LOG_WARN(component, msg) DSN_LOG(::desentry::LogLevel::kWarn, component, msg)
#define DSN_LOG_ERROR(component, msg) DSN_LOG(::desentry::LogLevel::kError, component, msg)
