/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#include "common/log.h"

#include <cstdarg>
#include <cstdlib>
#include <ctime>

/// 进程内所有缓冲池共享的日志级别，由 CLI 的 --verbose 调整
static LogLevel g_log_level = LogLevel::WARN;

void log_set_level(LogLevel level) { g_log_level = level; }

static const char *level_name(LogLevel level)
{
  switch (level) {
    case LogLevel::ERR: return "ERROR";
    case LogLevel::WARN: return "WARN";
    case LogLevel::INFO: return "INFO";
    case LogLevel::DBG: return "DEBUG";
    default: return "OFF";
  }
}

void log_print(LogLevel level, const char *file, int line, const char *fmt, ...)
{
  if (level > g_log_level || g_log_level == LogLevel::OFF) {
    return;
  }

  time_t now      = time(nullptr);
  char   when[32] = {0};
  strftime(when, sizeof(when), "%H:%M:%S", localtime(&now));

  const char *base = file;
  for (const char *p = file; *p != '\0'; ++p) {
    if (*p == '/' || *p == '\\') {
      base = p + 1;
    }
  }

  fprintf(stderr, "[%s][%s] %s:%d - ", when, level_name(level), base, line);

  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);

  fprintf(stderr, "\n");
  fflush(stderr);
}
