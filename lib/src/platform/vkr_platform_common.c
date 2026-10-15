#include "platform/vkr_platform.h"

#include "containers/str.h"

/* Platform services that build on the per-OS primitives. */

void vkr_platform_stdout_writef(const char *format, ...) {
  char message[4096];
  va_list args;
  va_start(args, format);
  (void)string_format_v(message, sizeof(message), format, args);
  va_end(args);
  vkr_platform_stdout_write(message);
}

void vkr_platform_stderr_writef(const char *format, ...) {
  char message[4096];
  va_list args;
  va_start(args, format);
  (void)string_format_v(message, sizeof(message), format, args);
  va_end(args);
  vkr_platform_stderr_write(message);
}

int64_t vkr_platform_get_unix_time(void) { return (int64_t)time(NULL); }
