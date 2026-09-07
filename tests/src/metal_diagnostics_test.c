#include "metal_diagnostics_test.h"

#include "metal/vkr_metal_diagnostics.h"

#include <assert.h>
#include <stdio.h>

#if defined(__APPLE__)
#include <unistd.h>

vkr_internal void metal_diagnostics_test_path(char *path, size_t path_size,
                                              const char *directory,
                                              uint32_t segment) {
  const int written = snprintf(path, path_size, "%s/metal-diagnostics.%u.jsonl",
                               directory, segment);
  assert(written > 0 && (size_t)written < path_size);
}

vkr_internal size_t metal_diagnostics_test_read(const char *path, char *text,
                                                size_t text_size) {
  const int fd = open(path, O_RDONLY);
  assert(fd >= 0);
  struct stat stats = {0};
  assert(fstat(fd, &stats) == 0);
  const off_t start = stats.st_size > (off_t)(text_size - 1u)
                          ? stats.st_size - text_size + 1
                          : 0;
  assert(lseek(fd, start, SEEK_SET) >= 0);
  const ssize_t read_size = read(fd, text, text_size - 1u);
  assert(read_size >= 0);
  text[read_size] = '\0';
  assert(close(fd) == 0);
  return (size_t)read_size;
}

vkr_internal bool8_t metal_diagnostics_test_contains(const char *path,
                                                     const char *needle) {
  const int fd = open(path, O_RDONLY);
  assert(fd >= 0);
  char text[8192];
  const size_t needle_size = strlen(needle);
  size_t carry = 0u;
  bool8_t found = false_v;
  while (!found) {
    const ssize_t read_size = read(fd, text + carry, sizeof(text) - carry - 1u);
    assert(read_size >= 0);
    const size_t total_size = carry + (size_t)read_size;
    text[total_size] = '\0';
    found = strstr(text, needle) != NULL;
    if (read_size == 0) {
      break;
    }
    carry = Min(needle_size - 1u, total_size);
    MemCopy(text, text + total_size - carry, carry);
  }
  assert(close(fd) == 0);
  return found;
}

vkr_internal void test_metal_diagnostics_json_and_rotation(void) {
  printf("  Running test_metal_diagnostics_json_and_rotation...\n");
  char root[] = "/tmp/vkr-metal-diagnostics-XXXXXX";
  assert(mkdtemp(root));
  char directory[1024];
  const int directory_length =
      snprintf(directory, sizeof(directory), "%s/log", root);
  assert(directory_length > 0 && (size_t)directory_length < sizeof(directory));

  VkrMetalDiagnostics diagnostics = {0};
  assert(vkr_metal_diagnostics_open(&diagnostics, directory));
  vkr_metal_diagnostics_record(&diagnostics, "event\"\\\n", 7u, 6u,
                               "quote\" slash\\ newline\n control%c", 1);
  char path[1024];
  metal_diagnostics_test_path(path, sizeof(path), directory, 0u);
  char text[8192];
  metal_diagnostics_test_read(path, text, sizeof(text));
  assert(strcmp(text,
                "{\"seq\":0,\"event\":\"event\\\"\\\\\\n\",\"submitted\":7,"
                "\"completed\":6,\"detail\":\"quote\\\" slash\\\\ newline\\n "
                "control\\u0001\"}\n") == 0);
  vkr_metal_diagnostics_record(&diagnostics, "utf8", 7u, 6u,
                               "caf\xc3\xa9 \xe2\x98\x83");
  metal_diagnostics_test_read(path, text, sizeof(text));
  assert(strstr(text, "\"detail\":\"caf\xc3\xa9 \xe2\x98\x83\"") != NULL);
  vkr_metal_diagnostics_flush(&diagnostics);
  assert(diagnostics.enabled);

  char long_detail[700];
  MemSet(long_detail, 'x', sizeof(long_detail) - 1u);
  long_detail[sizeof(long_detail) - 1u] = '\0';
  vkr_metal_diagnostics_record(&diagnostics, "long", 8u, 7u, "%s", long_detail);
  metal_diagnostics_test_read(path, text, sizeof(text));
  assert(strstr(text, "\"truncated\":true") != NULL);

  char clipped_utf8[520];
  MemSet(clipped_utf8, 'x', 509u);
  MemCopy(clipped_utf8 + 509u, "\xe2\x98\x83", 4u);
  vkr_metal_diagnostics_record(&diagnostics, "clipped_utf8", 8u, 7u, "%s",
                               clipped_utf8);
  metal_diagnostics_test_read(path, text, sizeof(text));
  assert(strstr(text, "\\u00e2") == NULL);
  assert(strstr(text, "\\u0098") == NULL);

  for (uint32_t i = 0u; i < 24000u; ++i) {
    vkr_metal_diagnostics_record(&diagnostics, "rotate", i, i, "%s",
                                 long_detail);
  }
  vkr_metal_diagnostics_record(&diagnostics, "tail", 24001u, 24001u,
                               "tail-final");
  vkr_metal_diagnostics_flush(&diagnostics);
  assert(diagnostics.enabled);
  const uint32_t active_segment = diagnostics.active_segment;
  assert(diagnostics.active_bytes > 0u);
  vkr_metal_diagnostics_close(&diagnostics);

  for (uint32_t segment = 0u; segment < 2u; ++segment) {
    metal_diagnostics_test_path(path, sizeof(path), directory, segment);
    struct stat stats = {0};
    assert(stat(path, &stats) == 0);
    assert((uint64_t)stats.st_size <= VKR_METAL_DIAGNOSTIC_SEGMENT_BYTES);
    assert(!metal_diagnostics_test_contains(path, "\"seq\":0,"));
  }
  metal_diagnostics_test_path(path, sizeof(path), directory, active_segment);
  metal_diagnostics_test_read(path, text, sizeof(text));
  assert(strstr(text, "tail-final") != NULL);

  VkrMetalDiagnostics refused = {0};
  assert(!vkr_metal_diagnostics_open(&refused, directory));
  assert(!refused.enabled);

  char failure_directory[1024];
  const int failure_directory_length = snprintf(
      failure_directory, sizeof(failure_directory), "%s/failure", root);
  assert(failure_directory_length > 0 &&
         (size_t)failure_directory_length < sizeof(failure_directory));
  VkrMetalDiagnostics failed = {0};
  assert(vkr_metal_diagnostics_open(&failed, failure_directory));
  const uint32_t failed_active = failed.active_segment;
  const int remaining_fd = failed.segment_fds[1u - failed_active];
  assert(close(failed.segment_fds[failed_active]) == 0);
  vkr_metal_diagnostics_record(&failed, "failure", 0u, 0u, "forced write");
  assert(!failed.enabled);
  assert(fcntl(remaining_fd, F_GETFD) == -1 && errno == EBADF);
  for (uint32_t segment = 0u; segment < 2u; ++segment) {
    metal_diagnostics_test_path(path, sizeof(path), failure_directory, segment);
    assert(unlink(path) == 0);
  }
  assert(rmdir(failure_directory) == 0);

  for (uint32_t segment = 0u; segment < 2u; ++segment) {
    metal_diagnostics_test_path(path, sizeof(path), directory, segment);
    assert(unlink(path) == 0);
  }
  assert(rmdir(directory) == 0);
  assert(rmdir(root) == 0);
  printf("  test_metal_diagnostics_json_and_rotation PASSED\n");
}

#endif

bool32_t run_metal_diagnostics_tests(void) {
#if defined(__APPLE__)
  printf("Running Metal diagnostics tests...\n");
  test_metal_diagnostics_json_and_rotation();
  printf("Metal diagnostics tests PASSED\n");
#endif
  return true_v;
}
