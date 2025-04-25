#include "logger.h"

void log_init(Arena *arena) {
  assert(arena != NULL && "Log arena is not initialized.");
  _log_arena = arena;
}

void _log_message(LogLevel level, const char *file, int line, const char *fmt,
                  ...) {
  assert(_log_arena != NULL && "Log arena is not initialized.");

  Scratch scratch = scratch_create(_log_arena);
  va_list args;
  va_start(args, fmt);
  String8 message = string8_create_formatted_v(_log_arena, fmt, args);
  va_end(args);

  assert(message.str != NULL && "Error formatting log message.");

  String8 formatted_message = string8_create_formatted(
      _log_arena, "%s%s(%s:%d) %.*s\033[0m", LOG_LEVEL_COLOURS[level],
      LOG_LEVELS[level], file, line, (int)message.length, message.str);

  assert(formatted_message.str != NULL &&
         "Error formatting final log message.");

  fprintf(stdout, "%.*s\n", (int)formatted_message.length,
          formatted_message.str);

  if (level == LOG_LEVEL_FATAL) {
    debug_break();
  }

  scratch_destroy(scratch);
}
