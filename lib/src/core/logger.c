#include "logger.h"

#include "core/vkr_threads.h"
#include "memory/vkr_arena_allocator.h"

vkr_global Arena *g_log_arena = NULL;
vkr_global VkrAllocator g_log_allocator = {0};
vkr_global VkrMutex g_log_mutex = NULL;

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

bool8_t log_init(Arena *arena) {
  if (!arena) {
    vkr_platform_stderr_write("Log arena is NULL\n");
    return false_v;
  }
  g_log_allocator = (VkrAllocator){.ctx = arena};
  if (!vkr_allocator_arena(&g_log_allocator) ||
      !vkr_mutex_create(&g_log_allocator, &g_log_mutex)) {
    vkr_allocator_release_global_accounting(&g_log_allocator);
    g_log_allocator = (VkrAllocator){0};
    return false_v;
  }
  g_log_arena = arena;
  return true_v;
}

void log_shutdown(void) {
  if (g_log_mutex)
    vkr_mutex_destroy(&g_log_allocator, &g_log_mutex);
  vkr_allocator_release_global_accounting(&g_log_allocator);
  g_log_allocator = (VkrAllocator){0};
  g_log_arena = NULL;
}

void _log_message(LogLevel level, const char *file, uint32_t line,
                  const char *fmt, ...) {
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

  String8 formatted_message = string8_create_formatted(
      &g_log_allocator, "%s(%s:%d) %.*s\n", LOG_LEVELS[level], file, line,
      message.length, message.str);

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
