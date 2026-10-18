#include "platform/vkr_crash.h"

#if defined(_WIN32)

#include "platform/vkr_crash_internal.h"

/* TODO(windows): implement crash reports. Breadcrumbs, the report path and
 * the report text are portable (vkr_crash_common.c); the Windows backend
 * owns only the handler, the file writes and the backtrace:
 *
 * - vkr_crash_install: call vkr_crash_configure, then
 *   SetUnhandledExceptionFilter. Reserve stack for the filter with
 *   SetThreadStackGuarantee so EXCEPTION_STACK_OVERFLOW still reports, and
 *   preload dbghelp.dll (LoadLibraryW, GetProcAddress of MiniDumpWriteDump)
 *   here, not in the filter. Return true once the filter is set.
 * - The filter guards against re-entry like vkr_crash_posix.c
 *   (g_crash_reporting), then:
 *   1. builds vkr_crash_report_path(".dmp", ...) and writes a minidump with
 *      MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
 *      MiniDumpWithThreadInfo, &exception_information, NULL, NULL), where
 *      exception_information carries the EXCEPTION_POINTERS and
 *      ClientPointers = FALSE;
 *   2. builds vkr_crash_report_path(".txt", ...) and writes the same text
 *      report as POSIX through a VkrCrashWriter whose sink calls WriteFile
 *      (CreateFileW on the UTF-16 path, OPEN_ALWAYS, FILE_APPEND_DATA):
 *      vkr_crash_report_begin, a reason line naming the exception code
 *      (EXCEPTION_ACCESS_VIOLATION and so on), ExceptionAddress and, for an
 *      access violation, the read/write flag and address, then
 *      vkr_crash_report_context, then "\nbacktrace:\n" and
 *      RtlCaptureStackBackTrace frames as module+offset (no symbol lookup
 *      in the filter; the minidump carries the rest); with no directory,
 *      write to GetStdHandle(STD_ERROR_HANDLE);
 *   3. returns EXCEPTION_CONTINUE_SEARCH so Windows Error Reporting still
 *      runs and the process ends as before.
 * - vkr_crash_uninstall restores the previous filter.
 * - vkr_crash_write_report writes the text report without a minidump
 *   (step 2 with the given reason and a RtlCaptureStackBackTrace of the
 *   caller) and returns true when it went to a file.
 * The filter takes no lock and allocates nothing; log records come from
 * log_history_emergency_write. */

bool8_t vkr_crash_install(const char *report_directory,
                          const char *program_name) {
  (void)report_directory;
  (void)program_name;
  return false_v;
}

void vkr_crash_uninstall(void) {}

bool8_t vkr_crash_write_report(const char *reason) {
  (void)reason;
  return false_v;
}

#endif
