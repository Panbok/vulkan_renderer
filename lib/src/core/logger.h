#pragma once

#include "containers/str.h"
#include "defines.h"
#include "memory/arena.h"
#include "platform/platform.h"

static Arena *_log_arena = NULL;

static const char *LOG_LEVELS[6] = {
    "[FATAL]: ", "[ERROR]: ", "[WARN]: ", "[INFO]: ", "[DEBUG]: ", "[TRACE]: "};
// FATAL, ERROR, WARN, INFO, DEBUG, TRACE
static const char *LOG_LEVEL_COLOURS[6] = {"\033[41m", "\033[31m", "\033[33m",
                                           "\033[32m", "\033[35m", "\033[30m"};

typedef enum LogLevel {
  LOG_LEVEL_FATAL = 0,
  LOG_LEVEL_ERROR = 1,
  LOG_LEVEL_WARN = 2,
  LOG_LEVEL_INFO = 3,
  LOG_LEVEL_DEBUG = 4,
  LOG_LEVEL_TRACE = 5,
} LogLevel;

void log_init(Arena *arena);
void _log_message(LogLevel level, const char *file, uint32_t line,
                  const char *fmt, ...);

#define log_fatal(fmt, ...)                                                    \
  _log_message(LOG_LEVEL_FATAL, __FILE__, __LINE__, fmt, ##__VA_ARGS__);

#if ASSERT_LOG
#define assert_log(expr, message)                                              \
  if (!(expr)) {                                                               \
    _log_message(LOG_LEVEL_FATAL, __FILE__, __LINE__,                          \
                 "Assertion Failure: %s, message: '%s'", #expr, message);      \
  }
#else
#define assert_log(expr, message)
#endif

#if LOG_LEVEL >= 1
#define log_error(fmt, ...)                                                    \
  _log_message(LOG_LEVEL_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__);
#else
#define log_error(fmt, ...)
#endif

#if LOG_LEVEL >= 2
#define log_warn(fmt, ...)                                                     \
  _log_message(LOG_LEVEL_WARN, __FILE__, __LINE__, fmt, ##__VA_ARGS__);
#else
#define log_warn(fmt, ...)
#endif

#if LOG_LEVEL >= 3
#define log_info(fmt, ...)                                                     \
  _log_message(LOG_LEVEL_INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__);
#else
#define log_info(fmt, ...)
#endif

#if LOG_LEVEL >= 4
#define log_debug(fmt, ...)                                                    \
  _log_message(LOG_LEVEL_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__);
#else
#define log_debug(fmt, ...)
#endif

#if LOG_LEVEL >= 5
#define log_trace(fmt, ...)                                                    \
  _log_message(LOG_LEVEL_TRACE, __FILE__, __LINE__, fmt, ##__VA_ARGS__);
#else
#define log_trace(fmt, ...)
#endif
