#include "filesystem/filesystem.h"
#include "platform/vkr_platform.h"
#include <assert.h>
#include <stdio.h>

#if defined(PLATFORM_WINDOWS)
#include <windows.h>
#endif

bool32_t run_process_path_tests(void);
int32_t process_path_test_child(void);

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
  vkr_platform_stdout_write("process stdout confirmed\n");
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

bool32_t run_process_path_tests(void) {
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
  VkrPlatformProcessConfig config = {
      .executable = executable,
      .arguments = arguments,
      .argument_count = ArrayCount(arguments),
      .working_directory = working,
      .stdout_path = stdout_path,
      .stderr_path = stderr_path,
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
  process_path_test_contents(stdout_path, "process stdout confirmed\n");
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
