#include "logger.h"

#include "core/vkr_atomic.h"
#include "core/vkr_threads.h"
#include "filesystem/filesystem.h"
#include "memory/vkr_arena_allocator.h"
#include <limits.h>

/* Release compiles INFO and above in; a process that does not request
   capture prints errors only, as Release printed before. */
#if defined(VKR_EDITOR_LOGGING) && VKR_EDITOR_LOGGING
#define VKR_DEFAULT_CAPTURE_LEVEL LOG_LEVEL_INFO
#elif defined(VKR_LOG_DEFAULT_LEVEL)
#define VKR_DEFAULT_CAPTURE_LEVEL ((LogLevel)VKR_LOG_DEFAULT_LEVEL)
#else
#define VKR_DEFAULT_CAPTURE_LEVEL LOG_LEVEL_TRACE
#endif
vkr_global VkrAtomicUint32 g_log_max_level = VKR_DEFAULT_CAPTURE_LEVEL;
/* Set before log_init by a process that shows its logs, such as the editor. */
vkr_global bool8_t g_log_capture_requested = false_v;
vkr_global LogLevel g_log_capture_level = LOG_LEVEL_INFO;

void log_max_level_set(LogLevel level) {
  if ((uint32_t)level > LOG_LEVEL_TRACE)
    return;
  vkr_atomic_uint32_store(&g_log_max_level, (uint32_t)level,
                          VKR_MEMORY_ORDER_RELAXED);
}

LogLevel log_max_level_get(void) {
  return (LogLevel)vkr_atomic_uint32_load(&g_log_max_level,
                                          VKR_MEMORY_ORDER_RELAXED);
}

bool8_t log_level_enabled(LogLevel level) {
  return level <= log_max_level_get();
}

vkr_global Arena *g_log_arena = NULL;
vkr_global VkrAllocator g_log_allocator = {0};
vkr_global VkrMutex g_log_mutex = NULL;
vkr_global VkrLogRecord *g_log_history = NULL;
vkr_global uint64_t g_log_sequence = 0u;
vkr_global uint32_t g_log_history_count = 0u;
/* The session log file: every record is appended and flushed as it is
   logged, so the file survives a crash. */
vkr_global FILE *g_log_file = NULL;
/* The session log's path for crash reports: written under the lock, read
   without it (log_file_directory). */
#define VKR_LOG_FILE_PATH_CAPACITY 2048u
vkr_global char g_log_file_path[VKR_LOG_FILE_PATH_CAPACITY];

vkr_global VkrAtomicUint64 g_log_counts[LOG_LEVEL_TRACE + 1];

vkr_global const char *LOG_LEVELS[6] = {
    "[FATAL]: ", "[ERROR]: ", "[WARN]: ", "[INFO]: ", "[DEBUG]: ", "[TRACE]: "};

vkr_internal INLINE void log_lock(void) {
  if (g_log_mutex) {
    vkr_mutex_lock(g_log_mutex);
  }
}

vkr_internal INLINE void log_unlock(void) {
  if (g_log_mutex) {
    vkr_mutex_unlock(g_log_mutex);
  }
}

void log_capture_request(LogLevel threshold) {
  g_log_capture_requested = true_v;
  g_log_capture_level = threshold;
}

bool8_t log_init(Arena *arena) {
  if (!arena) {
    vkr_platform_stderr_write("Log arena is NULL\n");
    return false_v;
  }
  log_max_level_set(g_log_capture_requested ? g_log_capture_level
                                            : VKR_DEFAULT_CAPTURE_LEVEL);
  g_log_allocator = (VkrAllocator){.ctx = arena};
  if (!vkr_allocator_arena(&g_log_allocator) ||
      !vkr_mutex_create(&g_log_allocator, &g_log_mutex)) {
    vkr_allocator_release_global_accounting(&g_log_allocator);
    g_log_allocator = (VkrAllocator){0};
    return false_v;
  }
#if defined(VKR_EDITOR_LOGGING) && VKR_EDITOR_LOGGING
  g_log_capture_requested = true_v;
#endif
  if (g_log_capture_requested) {
    g_log_history = vkr_allocator_alloc(&g_log_allocator,
                                        (uint64_t)VKR_LOG_HISTORY_CAPACITY *
                                            sizeof(*g_log_history),
                                        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!g_log_history) {
      vkr_mutex_destroy(&g_log_allocator, &g_log_mutex);
      vkr_allocator_release_global_accounting(&g_log_allocator);
      g_log_allocator = (VkrAllocator){0};
      return false_v;
    }
  }
  g_log_sequence = 0u;
  g_log_history_count = 0u;
  g_log_arena = arena;
  return true_v;
}

void log_shutdown(void) {
  log_file_close();
  if (g_log_mutex)
    vkr_mutex_destroy(&g_log_allocator, &g_log_mutex);
  vkr_allocator_release_global_accounting(&g_log_allocator);
  g_log_allocator = (VkrAllocator){0};
  g_log_arena = NULL;
  g_log_history = NULL;
  g_log_history_count = 0u;
  g_log_sequence = 0u;
}

VkrLogHistorySnapshot log_history_snapshot(uint64_t after_sequence,
                                           VkrLogRecord *records,
                                           uint32_t capacity) {
  VkrLogHistorySnapshot snapshot = {0};
  if (!g_log_history || (capacity != 0u && !records))
    return snapshot;
  log_lock();
  snapshot.newest_sequence = g_log_sequence;
  snapshot.first_available_sequence =
      g_log_history_count ? g_log_sequence - g_log_history_count + 1u : 0u;
  snapshot.overwritten_count = g_log_sequence - g_log_history_count;
  const uint64_t first =
      Max(after_sequence == UINT64_MAX ? UINT64_MAX : after_sequence + 1u,
          snapshot.first_available_sequence);
  if (first != 0u && first <= g_log_sequence) {
    snapshot.count =
        (uint32_t)Min((uint64_t)capacity, g_log_sequence - first + 1u);
    for (uint32_t i = 0u; i < snapshot.count; ++i)
      records[i] = g_log_history[(first + i - 1u) % VKR_LOG_HISTORY_CAPACITY];
  }
  log_unlock();
  return snapshot;
}

vkr_internal void log_history_append(LogLevel level, const char *file,
                                     uint32_t line, String8 message) {
  if (!g_log_history)
    return;
  VkrLogRecord *record =
      &g_log_history[g_log_sequence % VKR_LOG_HISTORY_CAPACITY];
  record->sequence = ++g_log_sequence;
  record->level = level;
  record->line = line;
  VkrTime time = {0};
  const bool8_t has_time = vkr_platform_get_utc_time(&time);
  record->utc_date = has_time ? (uint32_t)((time.year + 1900) * 10000 +
                                           (time.month + 1) * 100 + time.day)
                              : 0u;
  record->utc_time_ms =
      has_time
          ? (uint32_t)(((time.hours * 60 + time.minutes) * 60 + time.seconds) *
                           1000 +
                       time.milliseconds)
          : 0u;
  uint32_t length =
      (uint32_t)Min(message.length, VKR_LOG_MESSAGE_CAPACITY - 1u);
  while (length < message.length && length > 0u &&
         (message.str[length] & 0xc0u) == 0x80u)
    --length;
  record->message_length = (uint16_t)length;
  record->truncated = length != message.length;
  MemCopy(record->message, message.str, length);
  record->message[length] = 0u;
  const uint64_t source_length = string_length(file);
  const uint64_t source_start =
      source_length >= VKR_LOG_SOURCE_CAPACITY
          ? source_length - (VKR_LOG_SOURCE_CAPACITY - 1u)
          : 0u;
  uint64_t start = source_start;
  while (start < source_length && ((uint8_t)file[start] & 0xc0u) == 0x80u)
    ++start;
  record->source_length = (uint16_t)(source_length - start);
  MemCopy(record->source, file + start, record->source_length);
  record->source[record->source_length] = 0u;
  record->truncated |= start != 0u;
  g_log_history_count = Min(g_log_history_count + 1u, VKR_LOG_HISTORY_CAPACITY);
}

/* One line per record: UTC date and time, level, source and message. */
vkr_internal void log_file_write(const VkrLogRecord *record) {
  const uint32_t ms = record->utc_time_ms;
  fprintf(g_log_file, "%04u-%02u-%02u %02u:%02u:%02u.%03u %s%.*s:%u %.*s%s\n",
          record->utc_date / 10000u, record->utc_date / 100u % 100u,
          record->utc_date % 100u, ms / 3600000u, ms / 60000u % 60u,
          ms / 1000u % 60u, ms % 1000u, LOG_LEVELS[record->level],
          (int)record->source_length, (const char *)record->source,
          record->line, (int)record->message_length,
          (const char *)record->message,
          record->truncated ? " [truncated]" : "");
  fflush(g_log_file);
}

bool8_t log_file_open(const char *path) {
  if (!path || !path[0] || !g_log_history) {
    return false_v;
  }
  FILE *stream = file_fopen(path, "ab");
  if (!stream) {
    return false_v;
  }
  log_lock();
  if (g_log_file) {
    fclose(g_log_file);
  }
  g_log_file = stream;
  const uint64_t path_length = string_length(path);
  if (path_length < sizeof(g_log_file_path)) {
    MemCopy(g_log_file_path, path, path_length + 1u);
  } else {
    g_log_file_path[0] = '\0';
  }
  /* The records kept so far start the file, oldest first. */
  const uint64_t first = g_log_sequence - g_log_history_count + 1u;
  for (uint64_t sequence = first; sequence <= g_log_sequence; ++sequence) {
    log_file_write(&g_log_history[(sequence - 1u) % VKR_LOG_HISTORY_CAPACITY]);
  }
  log_unlock();
  return true_v;
}

void log_file_close(void) {
  log_lock();
  if (g_log_file) {
    fclose(g_log_file);
    g_log_file = NULL;
  }
  g_log_file_path[0] = '\0';
  log_unlock();
}

bool8_t log_file_directory(char *out, uint64_t capacity) {
  if (!out || capacity == 0u) {
    return false_v;
  }
  out[0] = '\0';
  const uint64_t length =
      string_length_bounded(g_log_file_path, sizeof(g_log_file_path));
  if (length == 0u || length == sizeof(g_log_file_path)) {
    return false_v;
  }
  uint64_t separator = length;
  while (separator > 0u && g_log_file_path[separator - 1u] != '/' &&
         g_log_file_path[separator - 1u] != '\\') {
    --separator;
  }
  /* A bare file name lives in the working directory; "/name" in the root. */
  const char *directory = separator ? g_log_file_path : ".";
  const uint64_t directory_length = separator <= 1u ? 1u : separator - 1u;
  if (directory_length >= capacity) {
    return false_v;
  }
  MemCopy(out, directory, directory_length);
  out[directory_length] = '\0';
  return true_v;
}

/* Writes `value` as exactly `width` decimal digits, zero-padded. */
vkr_internal void log_emergency_digits(char *out, uint32_t value,
                                       uint32_t width) {
  for (uint32_t i = width; i > 0u; --i) {
    out[i - 1u] = (char)('0' + value % 10u);
    value /= 10u;
  }
}

/* log_file_write's line, built without stdio for a signal handler. */
vkr_internal void log_emergency_record(const VkrLogRecord *record,
                                       VkrLogEmergencyWrite write,
                                       void *context) {
  const uint32_t date = record->utc_date;
  const uint32_t ms = record->utc_time_ms;
  char stamp[] = "0000-00-00 00:00:00.000 ";
  log_emergency_digits(stamp, date / 10000u, 4u);
  log_emergency_digits(stamp + 5, date / 100u % 100u, 2u);
  log_emergency_digits(stamp + 8, date % 100u, 2u);
  log_emergency_digits(stamp + 11, ms / 3600000u % 100u, 2u);
  log_emergency_digits(stamp + 14, ms / 60000u % 60u, 2u);
  log_emergency_digits(stamp + 17, ms / 1000u % 60u, 2u);
  log_emergency_digits(stamp + 20, ms % 1000u, 3u);
  write(context, stamp, sizeof(stamp) - 1u);

  const uint32_t level =
      Min((uint32_t)record->level, (uint32_t)LOG_LEVEL_TRACE);
  write(context, LOG_LEVELS[level], string_length(LOG_LEVELS[level]));

  /* A torn record can hold any length; stay inside its arrays. */
  const uint64_t source_length =
      Min((uint64_t)record->source_length, VKR_LOG_SOURCE_CAPACITY - 1u);
  write(context, (const char *)record->source, source_length);

  char line[12] = ":";
  uint32_t digits = 1u;
  uint32_t value = record->line;
  do {
    ++digits;
    value /= 10u;
  } while (value > 0u && digits < 11u);
  log_emergency_digits(line + 1, record->line, digits - 1u);
  line[digits++] = ' ';
  write(context, line, digits);

  const uint64_t message_length =
      Min((uint64_t)record->message_length, VKR_LOG_MESSAGE_CAPACITY - 1u);
  write(context, (const char *)record->message, message_length);
  if (record->truncated) {
    write(context, " [truncated]", 12u);
  }
  write(context, "\n", 1u);
}

uint32_t log_history_emergency_write(uint32_t max_records,
                                     VkrLogEmergencyWrite write,
                                     void *context) {
  if (!g_log_history || !write) {
    return 0u;
  }
  /* No lock: the crash may have happened while this thread held it. */
  const uint64_t newest = g_log_sequence;
  const uint64_t kept =
      Min((uint64_t)g_log_history_count, (uint64_t)VKR_LOG_HISTORY_CAPACITY);
  const uint64_t count = Min(Min(kept, newest), (uint64_t)max_records);
  for (uint64_t sequence = newest - count + 1u; sequence <= newest;
       ++sequence) {
    log_emergency_record(
        &g_log_history[(sequence - 1u) % VKR_LOG_HISTORY_CAPACITY], write,
        context);
  }
  return (uint32_t)count;
}

VkrLogCounts log_counts_get(void) {
  VkrLogCounts counts = {0};
  for (uint32_t i = 0u; i < ArrayCount(counts.by_level); ++i) {
    counts.by_level[i] =
        vkr_atomic_uint64_load(&g_log_counts[i], VKR_MEMORY_ORDER_RELAXED);
  }
  return counts;
}

void _log_message(LogLevel level, const char *file, uint32_t line,
                  const char *fmt, ...) {
  if (!log_level_enabled(level))
    return;
  assert(g_log_arena != NULL && "Log arena is not initialized.");
  vkr_atomic_uint64_fetch_add(&g_log_counts[level], 1u,
                              VKR_MEMORY_ORDER_RELAXED);

  log_lock();

  VkrAllocatorScope scope =
      vkr_allocator_begin_scope(&g_log_allocator); // scoped temp strings
  if (!vkr_allocator_scope_is_valid(&scope)) {
    log_unlock();
    return;
  }
  va_list args;
  va_start(args, fmt);
  String8 message = string8_create_formatted_v(&g_log_allocator, fmt, args);
  va_end(args);

  if (!message.str)
    goto allocation_failed;

  log_history_append(level, file, line, message);
  if (g_log_file && g_log_history) {
    log_file_write(
        &g_log_history[(g_log_sequence - 1u) % VKR_LOG_HISTORY_CAPACITY]);
  }

  String8 formatted_message = string8_create_formatted(
      &g_log_allocator, "%s(%s:%d) %.*s\n", LOG_LEVELS[level], file, line,
      (int)Min(message.length, (uint64_t)INT_MAX), message.str);

  if (!formatted_message.str)
    goto allocation_failed;

  vkr_platform_console_write(string8_cstr(&formatted_message), level);

  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);

  log_unlock();

  if (level == LOG_LEVEL_FATAL) {
    debug_break();
  }
  return;

allocation_failed:
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  log_unlock();
  vkr_platform_stderr_write("Log formatting allocation failed\n");
}

void vkr_assert_fail(const char *expression, const char *file, uint32_t line) {
  /* Fixed storage: an assertion can fail inside the allocator or before the
     logger exists. */
  char message[1024];
  (void)snprintf(message, sizeof(message),
                 "[FATAL]: Assertion failed: %s (%s:%u)\n", expression, file,
                 line);
  vkr_platform_stderr_write(message);
  debug_break();
  abort();
}
