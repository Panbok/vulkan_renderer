#pragma once

#include "arena.h"
#include "core.h"
#include "string.h"

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

void _log_message(Arena *arena, LogLevel level, const char *file, int line,
                  const char *fmt, ...);

#define log_fatal(arena, fmt, ...)                                             \
  _log_message(arena, LOG_LEVEL_FATAL, __FILE__, __LINE__, fmt, __VA_ARGS__);

#if LOG_LEVEL >= 1
#define log_error(arena, fmt, ...)                                             \
  _log_message(arena, LOG_LEVEL_ERROR, __FILE__, __LINE__, fmt, __VA_ARGS__);
#else
#define log_error(arena, fmt, ...)
#endif

#if LOG_LEVEL >= 2
#define log_warn(arena, fmt, ...)                                              \
  _log_message(arena, LOG_LEVEL_WARN, __FILE__, __LINE__, fmt, __VA_ARGS__);
#else
#define log_warn(arena, fmt, ...)
#endif

#if LOG_LEVEL >= 3
#define log_info(arena, fmt, ...)                                              \
  _log_message(arena, LOG_LEVEL_INFO, __FILE__, __LINE__, fmt, __VA_ARGS__);
#else
#define log_info(arena, fmt, ...)
#endif

#if LOG_LEVEL >= 4
#define log_debug(arena, fmt, ...)                                             \
  _log_message(arena, LOG_LEVEL_DEBUG, __FILE__, __LINE__, fmt, __VA_ARGS__);
#else
#define log_debug(arena, fmt, ...)
#endif

#if LOG_LEVEL >= 5
#define log_trace(arena, fmt, ...)                                             \
  _log_message(arena, LOG_LEVEL_TRACE, __FILE__, __LINE__, fmt, __VA_ARGS__);
#else
#define log_trace(arena, fmt, ...)
#endif
