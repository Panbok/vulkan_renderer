#include "logger.h"

vkr_global Arena *_log_arena = NULL;

vkr_global const char *LOG_LEVELS[6] = {
    "[FATAL]: ", "[ERROR]: ", "[WARN]: ", "[INFO]: ", "[DEBUG]: ", "[TRACE]: "};

void log_init(Arena *arena) {
  assert(arena != NULL && "Log arena is not initialized.");
  _log_arena = arena;
}

void _log_message(LogLevel level, const char *file, uint32_t line,
                  const char *fmt, ...) {
  assert(_log_arena != NULL && "Log arena is not initialized.");

  Scratch scratch = scratch_create(_log_arena);
  va_list args;
  va_start(args, fmt);
  String8 message = string8_create_formatted_v(_log_arena, fmt, args);
  va_end(args);

  assert(message.str != NULL && "Error formatting log message.");

  String8 formatted_message = string8_create_formatted(
      scratch.arena, "%s(%s:%d) %.*s\n", LOG_LEVELS[level], file, line,
      message.length, message.str);

  assert(formatted_message.str != NULL &&
         "Error formatting final log message.");

  vkr_platform_console_write(string8_cstr(&formatted_message), level);

  if (level == LOG_LEVEL_FATAL) {
    debug_break();
  }

  scratch_destroy(scratch, ARENA_MEMORY_TAG_STRING);
}
