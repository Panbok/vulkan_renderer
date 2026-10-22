#include "filesystem/filesystem.h"
#include "platform/vkr_platform.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#if defined(PLATFORM_WINDOWS)
#include <windows.h>
#endif

bool32_t run_process_path_tests(void);
int32_t process_path_test_child(void);
int32_t process_stream_test_child(void);

/* Bytes the stream child writes, 6 MB: more than any pipe buffer holds (4 MB on
   Windows), so the parent must read while the child still writes. */
#define PROCESS_STREAM_TEST_LINES 6144u
#define PROCESS_STREAM_TEST_LINE 1024u

/* Writes PROCESS_STREAM_TEST_LINES lines whose bytes follow from their
   position, each ending in a line break. */
int32_t process_stream_test_child(void) {
  char line[PROCESS_STREAM_TEST_LINE + 1u];
  for (uint32_t l = 0u; l < PROCESS_STREAM_TEST_LINES; ++l) {
    for (uint32_t i = 0u; i + 1u < PROCESS_STREAM_TEST_LINE; ++i) {
      line[i] = (char)('a' + (l * 7u + i) % 26u);
    }
    line[PROCESS_STREAM_TEST_LINE - 1u] = '\n';
    line[PROCESS_STREAM_TEST_LINE] = '\0';
    vkr_platform_stdout_write(line);
  }
  return 7;
}

typedef struct ProcessStreamRead {
  uint64_t bytes;
  uint32_t calls;
  bool8_t ordered;
  /* Calls after which the callback refuses more; zero never refuses. */
  uint32_t refuse_after;
} ProcessStreamRead;

static bool8_t process_stream_receive(void *context, const uint8_t *bytes,
                                      uint64_t size) {
  ProcessStreamRead *read = context;
  for (uint64_t i = 0u; i < size; ++i) {
    const uint64_t at = read->bytes + i;
    const uint32_t l = (uint32_t)(at / PROCESS_STREAM_TEST_LINE);
    const uint32_t c = (uint32_t)(at % PROCESS_STREAM_TEST_LINE);
    const uint8_t expected = c + 1u == PROCESS_STREAM_TEST_LINE
                                 ? (uint8_t)'\n'
                                 : (uint8_t)('a' + (l * 7u + c) % 26u);
    read->ordered = read->ordered && bytes[i] == expected;
  }
  read->bytes += size;
  ++read->calls;
  return read->refuse_after == 0u || read->calls < read->refuse_after;
}

/* Detects: streamed stdout that loses, reorders or truncates bytes past a
   pipe's buffer, waits for the child to end before reading (which deadlocks
   a child that fills the pipe), keeps reading after the callback refuses,
   or accepts both a stream and an output file. */
static void process_stream_test(void) {
  printf("  Running process_stream_test...\n");
  char executable[4096];
  assert(vkr_platform_executable_path(executable, sizeof(executable)));
  const char *arguments[] = {"--process-stream-test-child"};
  ProcessStreamRead read = {.ordered = true_v};
  VkrPlatformProcessConfig config = {
      .executable = executable,
      .arguments = arguments,
      .argument_count = ArrayCount(arguments),
      .timeout_ms = 60000u,
      .termination_grace_ms = 2000u,
      .hidden = true_v,
      .on_stdout = process_stream_receive,
      .stdout_context = &read,
  };
  int32_t exit_code = -1;
  bool8_t timed_out = false_v;
  assert(vkr_platform_process_run(&config, &exit_code, &timed_out));
  assert(!timed_out && exit_code == 7);
  assert(read.ordered && read.bytes == (uint64_t)PROCESS_STREAM_TEST_LINES *
                                           PROCESS_STREAM_TEST_LINE);

  /* A refusal stops the child before it writes everything. */
  read = (ProcessStreamRead){.ordered = true_v, .refuse_after = 1u};
  assert(vkr_platform_process_run(&config, &exit_code, &timed_out));
  assert(!timed_out && exit_code != 7 && read.calls == 1u && read.ordered);

  config.stdout_path = "unused.txt";
  assert(!vkr_platform_process_run(&config, &exit_code, &timed_out));
  printf("  process_stream_test PASSED\n");
}

/* A non-ASCII value the launcher sets for the child, as the Bakery hands its
   tools a usage-report path inside a cache below a non-ASCII profile. */
#define PROCESS_PATH_TEST_VARIABLE "VKR_PROCESS_PATH_TEST_VALUE"
#define PROCESS_PATH_TEST_VALUE "C:/Users/Пользователь/кэш"

int32_t process_path_test_child(void) {
#if defined(PLATFORM_WINDOWS)
  // Relative native creation independently proves the working directory the
  // child actually received, even if a launcher silently substitutes another.
  HANDLE marker = CreateFileW(L"child-marker.txt", GENERIC_WRITE, 0, NULL,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
  if (marker == INVALID_HANDLE_VALUE) {
    return 21;
  }
  const char bytes[] = "child working directory confirmed";
  DWORD written = 0;
  BOOL success = WriteFile(marker, bytes, sizeof(bytes) - 1, &written, NULL);
  CloseHandle(marker);
  if (!success || written != sizeof(bytes) - 1) {
    return 22;
  }
  // The parent compares these bytes with the UTF-8 value it set.
  char value[128];
  if (!vkr_platform_environment_variable(PROCESS_PATH_TEST_VARIABLE, value,
                                         sizeof(value))) {
    return 25;
  }
  vkr_platform_stdout_write("process stdout confirmed ");
  vkr_platform_stdout_write(value);
  vkr_platform_stdout_write("\n");
  vkr_platform_stderr_write("process stderr confirmed\n");
  return 23;
#else
  return 24;
#endif
}

#if defined(PLATFORM_WINDOWS)
static FilePath process_path_test_file(const char *path) {
  return (FilePath){.path = {.str = (uint8_t *)path, .length = strlen(path)},
                    .type = FILE_PATH_TYPE_ABSOLUTE};
}

static void process_path_test_contents(const char *path, const char *expected) {
  FilePath file = process_path_test_file(path);
  FileHandle handle = {0};
  assert(file_open(&file, (FileMode){.set = FILE_MODE_READ | FILE_MODE_BINARY},
                   &handle) == FILE_ERROR_NONE);
  uint8_t bytes[128] = {0};
  uint64_t read = 0;
  assert(file_read_into(&handle, bytes, sizeof(bytes), &read) ==
         FILE_ERROR_NONE);
  assert(read == strlen(expected));
  assert(MemCompare(bytes, expected, read) == 0);
  file_close(&handle);
  assert(file_remove(&file) == FILE_ERROR_NONE);
}
#endif

/* A child that cannot be started is a false return that names the failed
   step and its OS error, never an exit code. Catches a launcher that reports
   a failed exec as the child's exit status 127, which a caller cannot tell
   from the tool's own result or retry safely. */
static void process_start_failure_test(void) {
  printf("  Running process_start_failure_test...\n");
  VkrPlatformProcessError error = {.step = "unset", .code = -1};
  const VkrPlatformProcessConfig config = {
      .executable = PROJECT_SOURCE_DIR "tests/tmp/no_such_tool/vkr_missing",
      .termination_grace_ms = 1000u,
      .terminate_process_tree = true_v,
      .hidden = true_v,
      .out_error = &error,
  };
  int32_t exit_code = 0;
  bool8_t timed_out = false_v;
  assert(!vkr_platform_process_run(&config, &exit_code, &timed_out));
  assert(error.step && error.code != 0 && !error.started && !timed_out);
#if !defined(PLATFORM_WINDOWS)
  assert(strcmp(error.step, "exec") == 0 && error.code == ENOENT);
#endif
  printf("  process_start_failure_test PASSED\n");
}

bool32_t run_process_path_tests(void) {
  process_stream_test();
  process_start_failure_test();
#if defined(PLATFORM_WINDOWS)
  printf("--- Starting Windows Process Path Tests ---\n");
  char directories[7][1024];
  int32_t count = snprintf(directories[0], sizeof(directories[0]),
                           PROJECT_SOURCE_DIR "tests/tmp/процесс_%u",
                           vkr_platform_get_process_id());
  assert(count > 0 && (uint32_t)count < sizeof(directories[0]));
  FilePath root = process_path_test_file(directories[0]);
  assert(file_create_directory_exclusive(&root) == FILE_ERROR_NONE);
  for (uint32_t i = 1; i < ArrayCount(directories); ++i) {
    count =
        snprintf(directories[i], sizeof(directories[i]),
                 "%s/длинный_каталог_abcdefghijklmnopqrstuvwxyz_0123456789_%u",
                 directories[i - 1], i);
    assert(count > 0 && (uint32_t)count < sizeof(directories[i]));
    FilePath directory = process_path_test_file(directories[i]);
    assert(file_create_directory_exclusive(&directory) == FILE_ERROR_NONE);
  }
  const char *working = directories[ArrayCount(directories) - 1];
  wchar_t native[32768];
  FilePath directory = process_path_test_file(working);
  assert(file_windows_native_path(&directory, native));
  assert(wcslen(native) > 300);
  char stdout_path[1024];
  char stderr_path[1024];
  char marker_path[1024];
  count = snprintf(stdout_path, sizeof(stdout_path), "%s/вывод.log", working);
  assert(count > 0 && (uint32_t)count < sizeof(stdout_path));
  count = snprintf(stderr_path, sizeof(stderr_path), "%s/ошибки.log", working);
  assert(count > 0 && (uint32_t)count < sizeof(stderr_path));
  count = snprintf(marker_path, sizeof(marker_path), "%s/child-marker.txt",
                   working);
  assert(count > 0 && (uint32_t)count < sizeof(marker_path));
  char executable[32768];
  assert(vkr_platform_executable_path(executable, sizeof(executable)));
  const char *arguments[] = {"--process-path-test-child"};
  const VkrPlatformEnvironmentVariable environment[] = {
      {.name = PROCESS_PATH_TEST_VARIABLE, .value = PROCESS_PATH_TEST_VALUE},
  };
  VkrPlatformProcessConfig config = {
      .executable = executable,
      .arguments = arguments,
      .argument_count = ArrayCount(arguments),
      .working_directory = working,
      .stdout_path = stdout_path,
      .stderr_path = stderr_path,
      .environment = environment,
      .environment_count = ArrayCount(environment),
      .timeout_ms = 10000,
      .hidden = true_v,
  };
  int32_t exit_code = 0;
  bool8_t timed_out = false_v;
  bool8_t launched = vkr_platform_process_run(&config, &exit_code, &timed_out);
  if (!launched) {
    // CreateProcess's working-directory field retains MAX_PATH restrictions.
    // Volumes without short aliases must reject this before attempting launch.
    assert(GetLastError() == ERROR_FILENAME_EXCED_RANGE);
    wchar_t short_directory[32768];
    DWORD short_length =
        GetShortPathNameW(native, short_directory, ArrayCount(short_directory));
    if (short_length && short_length < ArrayCount(short_directory)) {
      size_t prefix = !_wcsnicmp(short_directory, L"\\\\?\\UNC\\", 8) ? 6u : 4u;
      assert(short_length >= MAX_PATH + prefix);
    }
    printf("  Long working-directory activation unavailable: volume has no "
           "usable short alias; explicit rejection verified.\n");
    config.working_directory = directories[0];
    count = snprintf(marker_path, sizeof(marker_path), "%s/child-marker.txt",
                     directories[0]);
    assert(count > 0 && (uint32_t)count < sizeof(marker_path));
    assert(vkr_platform_process_run(&config, &exit_code, &timed_out));
  }
  assert(!timed_out && exit_code == 23);
  process_path_test_contents(marker_path, "child working directory confirmed");
  process_path_test_contents(
      stdout_path, "process stdout confirmed " PROCESS_PATH_TEST_VALUE "\n");
  // The launcher restores its own environment after the launch.
  char value[128];
  assert(vkr_platform_environment_variable(PROCESS_PATH_TEST_VARIABLE, value,
                                           sizeof(value)));
  assert(value[0] == '\0');
  process_path_test_contents(stderr_path, "process stderr confirmed\n");
  // Each directory was exclusively acquired above; remove only those now-empty
  // directories in reverse order, never a recursively computed subtree.
  for (uint32_t i = ArrayCount(directories); i > 0; --i) {
    FilePath owned = process_path_test_file(directories[i - 1]);
    assert(file_windows_native_path(&owned, native));
    assert(RemoveDirectoryW(native));
  }
  printf("--- Windows Process Path Tests Completed ---\n");
#endif
  return true_v;
}
