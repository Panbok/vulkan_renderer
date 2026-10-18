#include "platform/vkr_crash.h"

#if defined(_WIN32)

#include "core/vkr_atomic.h"
#include "platform/vkr_crash_internal.h"

/* windows.h comes through defines.h (vkr_pch.h); dbghelp.h only declares the
   minidump types. MiniDumpWriteDump is resolved at install time, so the
   library gains no dbghelp link dependency. */
#include <dbghelp.h>
#include <signal.h>
#include <stdlib.h>

/* The stack the filter keeps on the installing thread after a stack
   overflow: exception dispatch, the report and MiniDumpWriteDump's own work.
   Like the POSIX alternate stack, only the installing thread gets it. */
#define CRASH_STACK_GUARANTEE (64u * 1024u)
/* RtlCaptureStackBackTrace needs skip + count below 63 on older Windows. */
#define CRASH_BACKTRACE_FRAMES 62u
#define CRASH_MODULE_NAME_CAPACITY 1024u
/* Codes winnt.h does not name: MSVC's C++ throw ("msc") and the heap's
   corruption check (STATUS_HEAP_CORRUPTION, from ntstatus.h). */
#define CRASH_CPP_EXCEPTION_CODE 0xe06d7363u
#define CRASH_HEAP_CORRUPTION_CODE 0xc0000374u

typedef BOOL(WINAPI *CrashMiniDumpWriteDump)(
    HANDLE process, DWORD process_id, HANDLE file, MINIDUMP_TYPE dump_type,
    PMINIDUMP_EXCEPTION_INFORMATION exception,
    PMINIDUMP_USER_STREAM_INFORMATION user_streams,
    PMINIDUMP_CALLBACK_INFORMATION callback);

typedef struct CrashExceptionName {
  DWORD code;
  const char *name;
} CrashExceptionName;

vkr_global const CrashExceptionName CRASH_EXCEPTION_NAMES[] = {
    {EXCEPTION_ACCESS_VIOLATION, "EXCEPTION_ACCESS_VIOLATION"},
    {EXCEPTION_ARRAY_BOUNDS_EXCEEDED, "EXCEPTION_ARRAY_BOUNDS_EXCEEDED"},
    {EXCEPTION_BREAKPOINT, "EXCEPTION_BREAKPOINT"},
    {EXCEPTION_DATATYPE_MISALIGNMENT, "EXCEPTION_DATATYPE_MISALIGNMENT"},
    {EXCEPTION_FLT_DENORMAL_OPERAND, "EXCEPTION_FLT_DENORMAL_OPERAND"},
    {EXCEPTION_FLT_DIVIDE_BY_ZERO, "EXCEPTION_FLT_DIVIDE_BY_ZERO"},
    {EXCEPTION_FLT_INEXACT_RESULT, "EXCEPTION_FLT_INEXACT_RESULT"},
    {EXCEPTION_FLT_INVALID_OPERATION, "EXCEPTION_FLT_INVALID_OPERATION"},
    {EXCEPTION_FLT_OVERFLOW, "EXCEPTION_FLT_OVERFLOW"},
    {EXCEPTION_FLT_STACK_CHECK, "EXCEPTION_FLT_STACK_CHECK"},
    {EXCEPTION_FLT_UNDERFLOW, "EXCEPTION_FLT_UNDERFLOW"},
    {EXCEPTION_ILLEGAL_INSTRUCTION, "EXCEPTION_ILLEGAL_INSTRUCTION"},
    {EXCEPTION_IN_PAGE_ERROR, "EXCEPTION_IN_PAGE_ERROR"},
    {EXCEPTION_INT_DIVIDE_BY_ZERO, "EXCEPTION_INT_DIVIDE_BY_ZERO"},
    {EXCEPTION_INT_OVERFLOW, "EXCEPTION_INT_OVERFLOW"},
    {EXCEPTION_INVALID_DISPOSITION, "EXCEPTION_INVALID_DISPOSITION"},
    {EXCEPTION_NONCONTINUABLE_EXCEPTION, "EXCEPTION_NONCONTINUABLE_EXCEPTION"},
    {EXCEPTION_PRIV_INSTRUCTION, "EXCEPTION_PRIV_INSTRUCTION"},
    {EXCEPTION_SINGLE_STEP, "EXCEPTION_SINGLE_STEP"},
    {EXCEPTION_STACK_OVERFLOW, "EXCEPTION_STACK_OVERFLOW"},
    {CRASH_HEAP_CORRUPTION_CODE, "STATUS_HEAP_CORRUPTION"},
    {CRASH_CPP_EXCEPTION_CODE, "unhandled C++ exception"},
};

/* The filter install replaced; the report hands the exception on to it. */
vkr_global LPTOP_LEVEL_EXCEPTION_FILTER g_crash_previous_filter;
vkr_global bool8_t g_crash_installed = false_v;
/* abort() raises SIGABRT and never reaches the exception filter. The
   handler is installed only over the default action, like the POSIX
   handlers, so the previous action is always SIG_DFL. */
vkr_global bool8_t g_crash_abort_installed = false_v;
/* Set by the first report; an exception during it, or on another thread at
   the same time, goes straight to the previous filter. */
vkr_global VkrAtomicUint32 g_crash_reporting;
/* Resolved at install: loading a library inside the filter would take the
   loader lock, which the crashing thread may hold. NULL skips the dump. */
vkr_global CrashMiniDumpWriteDump g_crash_minidump_write;

/* Report buffers. g_crash_reporting admits one report at a time, and after a
   stack overflow the filter has only CRASH_STACK_GUARANTEE of stack, so the
   large buffers live here instead of on it. */
typedef struct CrashScratch {
  char base_path[VKR_CRASH_PATH_CAPACITY];
  wchar_t wide_path[VKR_CRASH_PATH_CAPACITY];
  wchar_t module_path[CRASH_MODULE_NAME_CAPACITY];
  char module_name[CRASH_MODULE_NAME_CAPACITY * 3u];
  void *frames[CRASH_BACKTRACE_FRAMES];
} CrashScratch;

vkr_global CrashScratch g_crash_scratch;

vkr_internal void crash_handle_sink(void *context, const char *bytes,
                                    uint64_t length) {
  const HANDLE handle = *(const HANDLE *)context;
  while (length > 0u) {
    const DWORD chunk = length > 0x40000000u ? 0x40000000u : (DWORD)length;
    DWORD written = 0u;
    if (!WriteFile(handle, bytes, chunk, &written, NULL) || written == 0u) {
      return;
    }
    bytes += written;
    length -= written;
  }
}

/* `base` (UTF-8, from vkr_crash_report_path) plus `extension` as a UTF-16
   path for CreateFileW. */
vkr_internal bool8_t crash_wide_path(const char *base, const wchar_t *extension,
                                     wchar_t *out, uint32_t capacity) {
  uint32_t extension_length = 0u;
  while (extension[extension_length]) {
    ++extension_length;
  }
  const int32_t length =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, base, -1, out,
                          (int32_t)(capacity - extension_length));
  if (length <= 0) {
    return false_v;
  }
  /* The count includes the terminator, which the extension overwrites. */
  MemCopy(out + length - 1, extension,
          (extension_length + 1u) * sizeof(wchar_t));
  return true_v;
}

/* Writes the minidump; a failed dump leaves no partial file behind. Without
   `exception` (abort) the dump carries the threads and no exception
   stream. */
vkr_internal bool8_t crash_write_minidump(const wchar_t *path,
                                          EXCEPTION_POINTERS *exception) {
  const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0u, NULL, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE) {
    return false_v;
  }
  /* The pointers are this process's own, so ClientPointers stays FALSE. */
  MINIDUMP_EXCEPTION_INFORMATION exception_information = {
      .ThreadId = GetCurrentThreadId(),
      .ExceptionPointers = exception,
      .ClientPointers = FALSE,
  };
  const bool8_t written =
      g_crash_minidump_write(GetCurrentProcess(), GetCurrentProcessId(), file,
                             MiniDumpWithThreadInfo,
                             exception ? &exception_information : NULL, NULL,
                             NULL) != FALSE;
  (void)CloseHandle(file);
  if (!written) {
    (void)DeleteFileW(path);
  }
  return written;
}

/* The reason line after "reason: ": the exception's name, code and address
   and, for an access or in-page fault, what the access did and where. */
vkr_internal void crash_write_exception_reason(VkrCrashWriter *writer,
                                               const EXCEPTION_RECORD *record) {
  const char *name = "unknown exception";
  for (uint32_t i = 0u; i < ArrayCount(CRASH_EXCEPTION_NAMES); ++i) {
    if (CRASH_EXCEPTION_NAMES[i].code == record->ExceptionCode) {
      name = CRASH_EXCEPTION_NAMES[i].name;
      break;
    }
  }
  vkr_crash_writer_text(writer, name);
  vkr_crash_writer_text(writer, " (code ");
  vkr_crash_writer_hex(writer, record->ExceptionCode);
  vkr_crash_writer_text(writer, ", address ");
  vkr_crash_writer_hex(writer, (uint64_t)(uintptr_t)record->ExceptionAddress);

  const bool8_t is_access_fault =
      record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
      record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR;
  if (is_access_fault && record->NumberParameters >= 2u) {
    /* ExceptionInformation[0]: 0 read, 1 write, 8 execute (DEP). */
    const ULONG_PTR access = record->ExceptionInformation[0];
    const char *access_name = "access";
    if (access == 0u) {
      access_name = "read";
    } else if (access == 1u) {
      access_name = "write";
    } else if (access == 8u) {
      access_name = "execute";
    }
    vkr_crash_writer_text(writer, ", ");
    vkr_crash_writer_text(writer, access_name);
    vkr_crash_writer_text(writer, " ");
    vkr_crash_writer_hex(writer, (uint64_t)record->ExceptionInformation[1]);
  }
  vkr_crash_writer_text(writer, ")");
}

/* Writes the module that holds `address` as "<base name>+<offset>". Only the
   loader is asked; symbols come from the minidump or an offline lookup. */
vkr_internal void crash_write_module_offset(VkrCrashWriter *writer,
                                            const void *address) {
  HMODULE module = NULL;
  const DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
  if (!GetModuleHandleExW(flags, (LPCWSTR)address, &module) || !module) {
    vkr_crash_writer_text(writer, "?");
    return;
  }

  wchar_t *module_path = g_crash_scratch.module_path;
  const DWORD length = GetModuleFileNameW(
      module, module_path, ArrayCount(g_crash_scratch.module_path));
  uint32_t name_start = 0u;
  for (uint32_t i = 0u; i < length; ++i) {
    if (module_path[i] == L'\\' || module_path[i] == L'/') {
      name_start = i + 1u;
    }
  }
  int32_t name_length = 0;
  if (length > name_start) {
    name_length = WideCharToMultiByte(
        CP_UTF8, 0u, module_path + name_start, (int32_t)(length - name_start),
        g_crash_scratch.module_name,
        (int32_t)sizeof(g_crash_scratch.module_name), NULL, NULL);
  }
  if (name_length > 0) {
    vkr_crash_writer_bytes(writer, g_crash_scratch.module_name,
                           (uint64_t)name_length);
  } else {
    vkr_crash_writer_text(writer, "?");
  }
  vkr_crash_writer_text(writer, "+");
  vkr_crash_writer_hex(writer,
                       (uint64_t)((uintptr_t)address - (uintptr_t)module));
}

/* Writes this thread's frames as "#<n> <address> <module>+<offset>". From an
   exception filter the walk passes the dispatcher into the faulting frames;
   when `fault_address` is among them, the handler frames above it are
   dropped so the first line is the faulting instruction. */
vkr_internal void crash_write_backtrace(VkrCrashWriter *writer,
                                        const void *fault_address) {
  /* No frames are skipped: with inlining, a fixed skip count could drop the
     caller instead of this function. */
  void **frames = g_crash_scratch.frames;
  const uint32_t frame_count =
      RtlCaptureStackBackTrace(0u, CRASH_BACKTRACE_FRAMES, frames, NULL);
  uint32_t first = 0u;
  if (fault_address) {
    for (uint32_t i = 0u; i < frame_count; ++i) {
      if (frames[i] == fault_address) {
        first = i;
        break;
      }
    }
  }
  for (uint32_t i = first; i < frame_count; ++i) {
    vkr_crash_writer_text(writer, "#");
    vkr_crash_writer_u64(writer, i - first);
    vkr_crash_writer_text(writer, " ");
    vkr_crash_writer_hex(writer, (uint64_t)(uintptr_t)frames[i]);
    vkr_crash_writer_text(writer, " ");
    crash_write_module_offset(writer, frames[i]);
    vkr_crash_writer_text(writer, "\n");
  }
}

/* Writes one report; true when the text went to a file. `reason` is the text
   of a non-exception exit; without it, the reason line comes from
   `exception`. A minidump comes first when `with_dump` is set. Both files of
   one crash share one base path, so they pair up even when the second ticks
   between them. */
vkr_internal bool8_t crash_report_write(const char *reason,
                                        EXCEPTION_POINTERS *exception,
                                        bool8_t with_dump) {
  char *base_path = g_crash_scratch.base_path;
  wchar_t *wide_path = g_crash_scratch.wide_path;
  const uint32_t wide_capacity = ArrayCount(g_crash_scratch.wide_path);
  const bool8_t has_path =
      vkr_crash_report_path("", base_path, sizeof(g_crash_scratch.base_path));

  bool8_t dumped = false_v;
  if (has_path && with_dump && g_crash_minidump_write &&
      crash_wide_path(base_path, L".dmp", wide_path, wide_capacity)) {
    dumped = crash_write_minidump(wide_path, exception);
  }

  /* Appends, so a second report in the same second keeps the first. */
  HANDLE file = INVALID_HANDLE_VALUE;
  if (has_path &&
      crash_wide_path(base_path, L".txt", wide_path, wide_capacity)) {
    file = CreateFileW(wide_path, FILE_APPEND_DATA,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
  }
  const bool8_t to_file = file != INVALID_HANDLE_VALUE;
  HANDLE output = to_file ? file : GetStdHandle(STD_ERROR_HANDLE);

  VkrCrashWriter writer = {.sink = crash_handle_sink, .context = &output};
  vkr_crash_report_begin(&writer);
  if (reason) {
    vkr_crash_writer_text(&writer, reason);
  } else {
    crash_write_exception_reason(&writer, exception->ExceptionRecord);
  }
  vkr_crash_writer_text(&writer, "\n");
  vkr_crash_report_context(&writer);
  vkr_crash_writer_text(&writer, "\nbacktrace:\n");
  crash_write_backtrace(
      &writer, exception ? exception->ExceptionRecord->ExceptionAddress : NULL);
  vkr_crash_writer_flush(&writer);

  if (!to_file) {
    return false_v;
  }
  (void)CloseHandle(file);
  /* One line on stderr says where the report went, and one where the dump
     went. */
  output = GetStdHandle(STD_ERROR_HANDLE);
  vkr_crash_writer_text(&writer, "VKR crash report: ");
  vkr_crash_writer_text(&writer, base_path);
  vkr_crash_writer_text(&writer, ".txt\n");
  if (dumped) {
    vkr_crash_writer_text(&writer, "VKR crash dump: ");
    vkr_crash_writer_text(&writer, base_path);
    vkr_crash_writer_text(&writer, ".dmp\n");
  }
  vkr_crash_writer_flush(&writer);
  return true_v;
}

/* Runs for an exception no frame handled, on the faulting thread, unless a
   debugger is attached. Takes no lock of its own and allocates nothing;
   MiniDumpWriteDump and the loader queries are the exceptions the dump and
   the module names cost. */
vkr_internal LONG WINAPI crash_exception_filter(EXCEPTION_POINTERS *exception) {
  if (exception && exception->ExceptionRecord &&
      vkr_atomic_uint32_exchange(&g_crash_reporting, 1u,
                                 VKR_MEMORY_ORDER_ACQ_REL) == 0u) {
    (void)crash_report_write(NULL, exception, true_v);
  }
  /* The previous filter, such as the C runtime's, and then Windows Error
     Reporting still run, so the process ends as it would have without the
     report: same exit code, same system crash report. */
  if (g_crash_previous_filter) {
    return g_crash_previous_filter(exception);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

/* Runs for abort() or raise(SIGABRT) on the raising thread. The process
   then ends through abort() under the default action, as abort() would have
   without the handler: the C runtime fast-fails with
   FAST_FAIL_FATAL_APP_EXIT (exit 0xc0000409, Windows Error Reporting runs),
   or exits with 3 when _set_abort_behavior turned the fault report off.
   raise(SIGABRT) under SIG_DFL would instead _exit(3) and skip the system
   report. */
vkr_internal void crash_abort_handler(int signal_number) {
  if (vkr_atomic_uint32_exchange(&g_crash_reporting, 1u,
                                 VKR_MEMORY_ORDER_ACQ_REL) == 0u) {
    (void)crash_report_write("SIGABRT (abort)", NULL, true_v);
  }
  /* The C runtime resets the action before it calls a handler; set it
     anyway so abort() cannot come back here. */
  (void)signal(signal_number, SIG_DFL);
  abort();
}

bool8_t vkr_crash_install(const char *report_directory,
                          const char *program_name) {
  vkr_crash_configure(report_directory, program_name);
  if (g_crash_installed) {
    return true_v;
  }

  if (!g_crash_minidump_write) {
    /* System32's copy only: dbghelp.dll is not a known DLL, so a plain load
       would search the executable's directory first. The library stays
       loaded for the life of the process. */
    const HMODULE dbghelp =
        LoadLibraryExW(L"dbghelp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (dbghelp) {
      /* Through void (*)(void): the cast between function types is then
         explicit rather than from FARPROC's unprototyped signature. */
      const FARPROC address = GetProcAddress(dbghelp, "MiniDumpWriteDump");
      g_crash_minidump_write = (CrashMiniDumpWriteDump)(void (*)(void))address;
    }
  }

  /* Without a guarantee, a stack overflow leaves the filter too little stack
     to write anything. */
  ULONG guarantee = CRASH_STACK_GUARANTEE;
  (void)SetThreadStackGuarantee(&guarantee);

  g_crash_previous_filter = SetUnhandledExceptionFilter(crash_exception_filter);
  g_crash_installed = true_v;

  /* A SIGABRT handler someone else installed stays; ours goes back off. */
  const _crt_signal_t previous_abort = signal(SIGABRT, crash_abort_handler);
  if (previous_abort == SIG_DFL) {
    g_crash_abort_installed = true_v;
  } else if (previous_abort != SIG_ERR) {
    (void)signal(SIGABRT, previous_abort);
  }
  return true_v;
}

void vkr_crash_uninstall(void) {
  if (!g_crash_installed) {
    return;
  }
  (void)SetUnhandledExceptionFilter(g_crash_previous_filter);
  g_crash_previous_filter = NULL;
  g_crash_installed = false_v;
  if (g_crash_abort_installed) {
    (void)signal(SIGABRT, SIG_DFL);
    g_crash_abort_installed = false_v;
  }
}

bool8_t vkr_crash_write_report(const char *reason) {
  /* A report already running, from the filter or another thread, owns the
     output. */
  if (vkr_atomic_uint32_exchange(&g_crash_reporting, 1u,
                                 VKR_MEMORY_ORDER_ACQ_REL) != 0u) {
    return false_v;
  }
  const bool8_t written = crash_report_write(
      reason && reason[0] ? reason : "fatal exit", NULL, false_v);
  vkr_atomic_uint32_store(&g_crash_reporting, 0u, VKR_MEMORY_ORDER_RELEASE);
  return written;
}

#endif
