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
  log_unlock();
}

void _log_message(LogLevel level, const char *file, uint32_t line,
                  const char *fmt, ...) {
  if (!log_level_enabled(level))
    return;
  assert(g_log_arena != NULL && "Log arena is not initialized.");

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
