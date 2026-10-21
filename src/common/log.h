/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

#include <cstdio>

enum class LogLevel
{
  OFF = 0,
  ERR,
  WARN,
  INFO,
  DBG,
};

void log_set_level(LogLevel level);
void log_print(LogLevel level, const char *file, int line, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;

#define LOG_ERROR(fmt, ...) log_print(LogLevel::ERR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) log_print(LogLevel::WARN, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) log_print(LogLevel::INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_DBG(fmt, ...) log_print(LogLevel::DBG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

/// 与 miniob 一致：内部不变量被破坏时立刻终止，便于发现设计错误
#define ASSERT(cond, fmt, ...)                                                          \
  do {                                                                                  \
    if (!(cond)) {                                                                      \
      log_print(LogLevel::ERR, __FILE__, __LINE__, "Assertion failed: " fmt, ##__VA_ARGS__); \
      ::abort();                                                                        \
    }                                                                                   \
  } while (0)
