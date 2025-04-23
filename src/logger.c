#include "logger.h"
#include "string.h"

void _log_message(Arena *arena, LogLevel level, const char *file, int line,
                  const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  String8 message = string8_create_formatted_v(arena, fmt, args);
  va_end(args);

  if (message.str == NULL) {
    assert(0 && "Error formatting log message.");
  }

  String8 formatted_message = string8_create_formatted(
      arena, "%s%s(%s):%d %.*s\033[0m", LOG_LEVEL_COLOURS[level],
      LOG_LEVELS[level], file, line, (int)message.length, message.str);

  if (formatted_message.str == NULL) {
    assert(0 && "Error formatting final log message.");
  }

  fprintf(stdout, "%.*s\n", (int)formatted_message.length,
          formatted_message.str);

  if (level == LOG_LEVEL_FATAL) {
    debug_break();
  }
}
