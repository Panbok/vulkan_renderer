#pragma once

#include "containers/str.h"
#include "defines.h"
#include "memory/arena.h"
#include "platform/vkr_platform.h"

typedef enum LogLevel {
  LOG_LEVEL_FATAL = 0,
  LOG_LEVEL_ERROR = 1,
  LOG_LEVEL_WARN = 2,
  LOG_LEVEL_INFO = 3,
  LOG_LEVEL_DEBUG = 4,
  LOG_LEVEL_TRACE = 5,
} LogLevel;

#define VKR_LOG_HISTORY_CAPACITY 2048u
#define VKR_LOG_MESSAGE_CAPACITY 2048u
#define VKR_LOG_SOURCE_CAPACITY 192u

/** Copied logger data; no pointer borrows survive the logger lock. */
typedef struct VkrLogRecord {
  uint64_t sequence;
  uint32_t utc_date;    /**< YYYYMMDD; zero if UTC is unavailable. */
  uint32_t utc_time_ms; /**< Milliseconds since UTC midnight. */
  uint32_t line;
  uint16_t message_length;
  uint16_t source_length;
  LogLevel level;
  bool8_t truncated;
  uint8_t source[VKR_LOG_SOURCE_CAPACITY];
  uint8_t message[VKR_LOG_MESSAGE_CAPACITY];
} VkrLogRecord;

typedef struct VkrLogHistorySnapshot {
  uint64_t newest_sequence;
  uint64_t first_available_sequence;
  uint64_t overwritten_count;
  uint32_t count;
} VkrLogHistorySnapshot;

/** Copy oldest available records newer than after_sequence. Capacity zero is a
 * metadata query. Storage is allocated once at log_init in editor builds only.
 */
VkrLogHistorySnapshot log_history_snapshot(uint64_t after_sequence,
                                           VkrLogRecord *records,
                                           uint32_t capacity);

/** Runtime producer threshold; defaults to INFO in editor builds. Display
 * filters cannot recover records omitted at this boundary. */
void log_max_level_set(LogLevel level);
LogLevel log_max_level_get(void);
bool8_t log_level_enabled(LogLevel level);

VKR_MUST_USE bool8_t log_init(Arena *arena);
// Call after logging threads join and before releasing the borrowed arena.
void log_shutdown(void);
void _log_message(LogLevel level, const char *file, uint32_t line,
                  const char *fmt, ...);

#define log_fatal(fmt, ...)                                                    \
  do {                                                                         \
    _log_message(LOG_LEVEL_FATAL, __FILE__, __LINE__, fmt, ##__VA_ARGS__);     \
  } while (0)

// A compiled-out log statement still type-checks its arguments behind a
// short-circuited operand. Values that only feed diagnostics therefore stay
// "used" in every configuration without being evaluated. The expression form
// avoids constant-condition warnings on compilers that flag `if (0)`.
#define VKR_LOG_DISCARD(fmt, ...)                                              \
  ((void)(0 && (_log_message(LOG_LEVEL_TRACE, __FILE__, __LINE__, fmt,         \
                             ##__VA_ARGS__),                                   \
                0)))

#if defined(__clang_analyzer__)
// Static analysis treats an assertion as the documented precondition it is.
#define assert_log(expr, message)                                              \
  do {                                                                         \
    if (!(expr)) {                                                             \
      __builtin_unreachable();                                                 \
    }                                                                          \
  } while (0)
#elif ASSERT_LOG
#define assert_log(expr, message)                                              \
  do {                                                                         \
    if (!(expr)) {                                                             \
      _log_message(LOG_LEVEL_FATAL, __FILE__, __LINE__,                        \
                   "Assertion Failure: %s, message: '%s'", #expr, message);    \
    }                                                                          \
  } while (0)
#else
// The unevaluated operand keeps assertion-only values referenced.
#define assert_log(expr, message) ((void)sizeof((expr) ? 1 : 0))
#endif

#if LOG_LEVEL >= 1
#define log_error(fmt, ...)                                                    \
  do {                                                                         \
    _log_message(LOG_LEVEL_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__);     \
  } while (0)
#else
#define log_error(fmt, ...) VKR_LOG_DISCARD(fmt, ##__VA_ARGS__)
#endif

#if LOG_LEVEL >= 2
#define log_warn(fmt, ...)                                                     \
  do {                                                                         \
    _log_message(LOG_LEVEL_WARN, __FILE__, __LINE__, fmt, ##__VA_ARGS__);      \
  } while (0)
#else
#define log_warn(fmt, ...) VKR_LOG_DISCARD(fmt, ##__VA_ARGS__)
#endif

#if LOG_LEVEL >= 3
#define log_info(fmt, ...)                                                     \
  do {                                                                         \
    _log_message(LOG_LEVEL_INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__);      \
  } while (0)
#else
#define log_info(fmt, ...) VKR_LOG_DISCARD(fmt, ##__VA_ARGS__)
#endif

#if LOG_LEVEL >= 4
#define log_debug(fmt, ...)                                                    \
  do {                                                                         \
    if (log_level_enabled(LOG_LEVEL_DEBUG)) {                                  \
      _log_message(LOG_LEVEL_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__);   \
    }                                                                          \
  } while (0)
#else
#define log_debug(fmt, ...) VKR_LOG_DISCARD(fmt, ##__VA_ARGS__)
#endif

#if LOG_LEVEL >= 5
#define log_trace(fmt, ...)                                                    \
  do {                                                                         \
    if (log_level_enabled(LOG_LEVEL_TRACE)) {                                  \
      _log_message(LOG_LEVEL_TRACE, __FILE__, __LINE__, fmt, ##__VA_ARGS__);   \
    }                                                                          \
  } while (0)
#else
#define log_trace(fmt, ...) VKR_LOG_DISCARD(fmt, ##__VA_ARGS__)
#endif
