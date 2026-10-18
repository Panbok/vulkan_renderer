#include "platform/vkr_crash.h"
#include "platform/vkr_platform.h"
#include "test_temp_dir.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

bool32_t run_crash_tests(void);

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>

/* A non-signal report lands in the install directory with the reason, the
   process and every breadcrumb; a long breadcrumb is cut to its slot. No
   signal is raised: the tester must survive. */
vkr_internal void test_crash_write_report(void) {
  printf("  Running test_crash_write_report...\n");
  char root[] = "/tmp/vkr-crash-XXXXXX";
  assert(vkr_test_temp_dir_create(root));
  assert(vkr_crash_install(root, "crash-test"));

  char long_pass[VKR_CRASH_TEXT_CAPACITY + 64u];
  memset(long_pass, 'p', sizeof(long_pass) - 1u);
  long_pass[sizeof(long_pass) - 1u] = '\0';
  vkr_crash_set_frame(4242u);
  vkr_crash_set_text(VKR_CRASH_TEXT_SCENE, "assets/scenes/bistro.scene");
  vkr_crash_set_text(VKR_CRASH_TEXT_STAGE, "frame");
  vkr_crash_set_text(VKR_CRASH_TEXT_PASS, long_pass);
  vkr_crash_set_text(VKR_CRASH_TEXT_OPERATION, "level.lint");

  const int64_t before = vkr_platform_get_unix_time();
  assert(vkr_crash_write_report("gpu completion timeout (test wait=7)"));
  const int64_t after = vkr_platform_get_unix_time();

  /* The file name carries the second the report was written in. */
  const uint32_t pid = vkr_platform_get_process_id();
  char path[1024];
  int fd = -1;
  for (int64_t second = before; second <= after && fd < 0; ++second) {
    const int written =
        snprintf(path, sizeof(path), "%s/crash-crash-test-%u-%lld.txt", root,
                 pid, (long long)second);
    assert(written > 0 && (size_t)written < sizeof(path));
    fd = open(path, O_RDONLY);
  }
  assert(fd >= 0);
  static char text[65536];
  const ssize_t length = read(fd, text, sizeof(text) - 1u);
  assert(length > 0);
  text[length] = '\0';
  assert(close(fd) == 0);

  assert(strncmp(text,
                 "VKR crash report\n"
                 "reason: gpu completion timeout (test wait=7)\n"
                 "program: crash-test\n",
                 strlen("VKR crash report\n"
                        "reason: gpu completion timeout (test wait=7)\n"
                        "program: crash-test\n")) == 0);
  char expected[128];
  snprintf(expected, sizeof(expected), "\npid: %u\n", pid);
  assert(strstr(text, expected) != NULL);
  assert(strstr(text, "\nframe: 4242\n") != NULL);
  assert(strstr(text, "\nscene: assets/scenes/bistro.scene\n") != NULL);
  assert(strstr(text, "\nstage: frame\n") != NULL);
  assert(strstr(text, "\noperation: level.lint\n") != NULL);
  const char *pass = strstr(text, "\npass: ");
  assert(pass != NULL);
  pass += strlen("\npass: ");
  assert(strspn(pass, "p") == VKR_CRASH_TEXT_CAPACITY - 1u);
  assert(pass[VKR_CRASH_TEXT_CAPACITY - 1u] == '\n');
  assert(strstr(text, "\nlog (newest records, oldest first):\n") != NULL);
  /* The backtrace follows its heading with at least one frame. */
  const char *backtrace = strstr(text, "\nbacktrace:\n");
  assert(backtrace != NULL);
  assert(backtrace[strlen("\nbacktrace:\n")] != '\0');

  vkr_crash_uninstall();
  vkr_crash_set_frame(0u);
  for (uint32_t i = 0u; i < VKR_CRASH_TEXT_COUNT; ++i) {
    vkr_crash_set_text((VkrCrashText)i, NULL);
  }
  printf("  test_crash_write_report PASSED\n");
}
#endif

bool32_t run_crash_tests(void) {
  printf("--- Starting Crash Report Tests ---\n");
#if !defined(_WIN32)
  test_crash_write_report();
#endif
  printf("--- Crash Report Tests Completed ---\n");
  return true;
}
