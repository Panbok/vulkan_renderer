#include "logger.h"

vkr_global Arena *_log_arena = NULL;
vkr_global VkrAllocator _log_allocator = {0};

vkr_global const char *LOG_LEVELS[6] = {
    "[FATAL]: ", "[ERROR]: ", "[WARN]: ", "[INFO]: ", "[DEBUG]: ", "[TRACE]: "};

void log_init(Arena *arena) {
  assert(arena != NULL && "Log arena is not initialized.");
  _log_arena = arena;
  _log_allocator.ctx = arena;
  vkr_allocator_arena(&_log_allocator);
}

void _log_message(LogLevel level, const char *file, uint32_t line,
                  const char *fmt, ...) {
  assert(_log_arena != NULL && "Log arena is not initialized.");

  VkrAllocatorScope scope =
      vkr_allocator_begin_scope(&_log_allocator); // scoped temp strings
  if (!vkr_allocator_scope_is_valid(&scope)) {
    return;
  }
  va_list args;
  va_start(args, fmt);
  String8 message = string8_create_formatted_v(&_log_allocator, fmt, args);
  va_end(args);

  assert(message.str != NULL && "Error formatting log message.");

  String8 formatted_message = string8_create_formatted(
      &_log_allocator, "%s(%s:%d) %.*s\n", LOG_LEVELS[level], file, line,
      message.length, message.str);

  assert(formatted_message.str != NULL &&
         "Error formatting final log message.");

  vkr_platform_console_write(string8_cstr(&formatted_message), level);

  if (level == LOG_LEVEL_FATAL) {
    debug_break();
  }

  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
}
