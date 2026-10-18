#include "platform/vkr_crash.h"

#if !defined(_WIN32)

#include "core/vkr_atomic.h"
#include "platform/vkr_crash_internal.h"

#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

/* The frames a backtrace prints, and the alternate stack the handler runs
   on. The stack holds the report writer, the path and backtrace's work. */
#define CRASH_BACKTRACE_FRAMES 64
#define CRASH_ALT_STACK_SIZE (128u * 1024u)

vkr_global const int CRASH_SIGNALS[] = {SIGSEGV, SIGBUS,  SIGILL,
                                        SIGFPE,  SIGABRT, SIGTRAP};
vkr_global const char *const CRASH_SIGNAL_NAMES[] = {
    "SIGSEGV", "SIGBUS", "SIGILL", "SIGFPE", "SIGABRT", "SIGTRAP"};

/* The actions install replaced, restored before a signal is handed back. */
vkr_global struct sigaction g_crash_previous[ArrayCount(CRASH_SIGNALS)];
vkr_global bool8_t g_crash_installed[ArrayCount(CRASH_SIGNALS)];
/* Set by the first report; a fault during it, or on another thread at the
   same time, goes straight to the previous action. */
vkr_global VkrAtomicUint32 g_crash_reporting;
vkr_global uint8_t g_crash_alt_stack[CRASH_ALT_STACK_SIZE];
vkr_global bool8_t g_crash_alt_stack_ready = false_v;

vkr_internal void crash_fd_sink(void *context, const char *bytes,
                                uint64_t length) {
  const int fd = *(const int *)context;
  while (length > 0u) {
    const ssize_t written = write(fd, bytes, (size_t)length);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      return;
    }
    bytes += written;
    length -= (uint64_t)written;
  }
}

/* The report file for this second, or stderr when there is none. Appends,
   so a second report in the same second keeps the first. */
vkr_internal int crash_report_open(char *path, uint64_t capacity) {
  if (vkr_crash_report_path(".txt", path, capacity)) {
    const int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd >= 0) {
      return fd;
    }
  }
  path[0] = '\0';
  return STDERR_FILENO;
}

/* Writes one report; true when it went to a file. `reason` is the text of a
   non-signal exit; without it, the reason line names the signal from
   `signal_index` and `info`. */
vkr_internal bool8_t crash_report_write(const char *reason,
                                        uint32_t signal_index,
                                        const siginfo_t *info) {
  char path[VKR_CRASH_PATH_CAPACITY];
  int fd = crash_report_open(path, sizeof(path));
  VkrCrashWriter writer = {.sink = crash_fd_sink, .context = &fd};
  vkr_crash_report_begin(&writer);
  if (reason) {
    vkr_crash_writer_text(&writer, reason);
  } else {
    vkr_crash_writer_text(&writer, CRASH_SIGNAL_NAMES[signal_index]);
    if (info) {
      vkr_crash_writer_text(&writer, " (code ");
      vkr_crash_writer_i64(&writer, info->si_code);
      vkr_crash_writer_text(&writer, ", address ");
      vkr_crash_writer_hex(&writer, (uint64_t)(uintptr_t)info->si_addr);
      vkr_crash_writer_text(&writer, ")");
    }
  }
  vkr_crash_writer_text(&writer, "\n");
  vkr_crash_report_context(&writer);

  /* backtrace_symbols_fd writes straight to the descriptor and does not
     allocate, but neither call is on the POSIX async-signal-safe list; the
     report is complete up to here if they fail. */
  vkr_crash_writer_text(&writer, "\nbacktrace:\n");
  vkr_crash_writer_flush(&writer);
  void *frames[CRASH_BACKTRACE_FRAMES];
  const int frame_count = backtrace(frames, CRASH_BACKTRACE_FRAMES);
  backtrace_symbols_fd(frames, frame_count, fd);

  if (fd == STDERR_FILENO) {
    return false_v;
  }
  (void)close(fd);
  /* One line on stderr says where the report went. */
  fd = STDERR_FILENO;
  vkr_crash_writer_text(&writer, "VKR crash report: ");
  vkr_crash_writer_text(&writer, path);
  vkr_crash_writer_text(&writer, "\n");
  vkr_crash_writer_flush(&writer);
  return true_v;
}

vkr_internal uint32_t crash_signal_index(int signal_number) {
  for (uint32_t i = 0u; i < ArrayCount(CRASH_SIGNALS); ++i) {
    if (CRASH_SIGNALS[i] == signal_number) {
      return i;
    }
  }
  return 0u;
}

/* A hardware fault repeats when the handler returns: the faulting
   instruction runs again under the restored action, so the system crash
   report sees the real fault. A trap (int3 resumes past itself), abort or a
   sent signal does not repeat and is raised again instead. */
vkr_internal bool8_t crash_fault_repeats(int signal_number,
                                         const siginfo_t *info) {
  if (!info || signal_number == SIGABRT || signal_number == SIGTRAP) {
    return false_v;
  }
#if defined(SI_USER) && SI_USER > 0
  /* macOS numbers sent-signal codes from 0x10001. */
  return info->si_code > 0 && info->si_code < SI_USER;
#else
  return info->si_code > 0;
#endif
}

vkr_internal void crash_signal_handler(int signal_number, siginfo_t *info,
                                       void *user_context) {
  (void)user_context;
  const uint32_t index = crash_signal_index(signal_number);
  if (vkr_atomic_uint32_exchange(&g_crash_reporting, 1u,
                                 VKR_MEMORY_ORDER_ACQ_REL) == 0u) {
    (void)crash_report_write(NULL, index, info);
  }
  (void)sigaction(signal_number, &g_crash_previous[index], NULL);
  if (!crash_fault_repeats(signal_number, info)) {
    (void)raise(signal_number);
  }
}

bool8_t vkr_crash_install(const char *report_directory,
                          const char *program_name) {
  vkr_crash_configure(report_directory, program_name);
  if (!g_crash_alt_stack_ready) {
    /* Only the installing thread runs the handler on this stack; a stack
       overflow on another thread ends without a report. */
    const stack_t stack = {.ss_sp = g_crash_alt_stack,
                           .ss_size = sizeof(g_crash_alt_stack)};
    g_crash_alt_stack_ready = sigaltstack(&stack, NULL) == 0;
    /* The first backtrace call loads the unwinder; do it here, not in a
       signal handler. */
    void *frame = NULL;
    (void)backtrace(&frame, 1);
  }

  bool8_t any_installed = false_v;
  for (uint32_t i = 0u; i < ArrayCount(CRASH_SIGNALS); ++i) {
    if (g_crash_installed[i]) {
      any_installed = true_v;
      continue;
    }
    struct sigaction previous = {0};
    if (sigaction(CRASH_SIGNALS[i], NULL, &previous) != 0) {
      continue;
    }
    /* A handler someone else installed, such as a sanitizer's, stays. */
    const bool8_t is_default =
        !(previous.sa_flags & SA_SIGINFO) && previous.sa_handler == SIG_DFL;
    if (!is_default) {
      continue;
    }
    struct sigaction action = {0};
    action.sa_sigaction = crash_signal_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    (void)sigemptyset(&action.sa_mask);
    if (sigaction(CRASH_SIGNALS[i], &action, NULL) == 0) {
      g_crash_previous[i] = previous;
      g_crash_installed[i] = true_v;
      any_installed = true_v;
    }
  }
  return any_installed;
}

void vkr_crash_uninstall(void) {
  for (uint32_t i = 0u; i < ArrayCount(CRASH_SIGNALS); ++i) {
    if (g_crash_installed[i]) {
      (void)sigaction(CRASH_SIGNALS[i], &g_crash_previous[i], NULL);
      g_crash_installed[i] = false_v;
    }
  }
}

bool8_t vkr_crash_write_report(const char *reason) {
  /* A report already running, from a signal or another thread, owns the
     output. */
  if (vkr_atomic_uint32_exchange(&g_crash_reporting, 1u,
                                 VKR_MEMORY_ORDER_ACQ_REL) != 0u) {
    return false_v;
  }
  const bool8_t written =
      crash_report_write(reason && reason[0] ? reason : "fatal exit", 0u, NULL);
  vkr_atomic_uint32_store(&g_crash_reporting, 0u, VKR_MEMORY_ORDER_RELEASE);
  return written;
}

#endif
