#pragma once

#include "defines.h"

/* Crash reports (docs/proposals/profiling-and-diagnostics.md, P3).
 *
 * After vkr_crash_install, a fatal signal (macOS: SIGSEGV, SIGBUS, SIGILL,
 * SIGFPE, SIGABRT, SIGTRAP) writes a text report before the process ends as
 * it would have without the handler, so the system crash report still
 * follows. The report holds the reason, program, pid, the breadcrumbs below,
 * the newest log records and a backtrace. It lands at
 *
 *   <directory>/crash-<program>-<pid>-<unix seconds>.txt
 *
 * where <directory> is the install directory or, without one, the directory
 * of the open session log (log_file_open). With neither, the report goes to
 * stderr. A second report in the same second appends to the same file.
 *
 * Breadcrumbs are cheap enough for every frame: a relaxed atomic store for
 * the frame and a bounded byte copy for a text slot. Each slot has one
 * writer thread; a crash can read a slot mid-copy, so its text can come out
 * torn, never longer than VKR_CRASH_TEXT_CAPACITY.
 *
 * Windows has no implementation yet: install returns false and reports go
 * nowhere (vkr_crash_windows.c describes the expected behaviour). */

#define VKR_CRASH_TEXT_CAPACITY 256u

typedef enum VkrCrashText {
  /** The open scene or level path. */
  VKR_CRASH_TEXT_SCENE = 0,
  /** The main thread's frame stage, set by the application host. */
  VKR_CRASH_TEXT_STAGE,
  /** The render thread's current pass; set per pass or per render stage,
      never per draw. */
  VKR_CRASH_TEXT_PASS,
  /** The editor or agent operation being run. */
  VKR_CRASH_TEXT_OPERATION,
  VKR_CRASH_TEXT_COUNT,
} VkrCrashText;

/**
 * Installs the fatal-signal handlers on an alternate signal stack, so a
 * main-thread stack overflow still reports. `report_directory` may be NULL to
 * follow the session log; `program_name` names the report file (NULL keeps
 * "vkr"). Both are copied. A second call replaces the directory and program
 * and keeps the handlers. A signal that already has a handler, such as a
 * sanitizer's, keeps it. False when no handler could be installed.
 */
bool8_t vkr_crash_install(const char *report_directory,
                          const char *program_name);

/** Restores the signal actions vkr_crash_install replaced. */
void vkr_crash_uninstall(void);

/** Records the frame the application host is running. */
void vkr_crash_set_frame(uint64_t frame);

/** Copies `text` (NULL clears) into a breadcrumb slot, truncated to fit. */
void vkr_crash_set_text(VkrCrashText slot, const char *text);

/** Copies `length` bytes of unterminated `text`, such as a String8, into a
 * breadcrumb slot, truncated to fit. */
void vkr_crash_set_text_bytes(VkrCrashText slot, const char *text,
                              uint64_t length);

/**
 * Writes a report for a fatal exit that is not a signal, such as a GPU
 * completion timeout, with `reason` as its reason line. Works without
 * vkr_crash_install. True when the report went to a file; false when it went
 * to stderr, another report was running, or the platform has none.
 */
bool8_t vkr_crash_write_report(const char *reason);
