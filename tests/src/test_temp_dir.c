#include "test_temp_dir.h"

#if !defined(_WIN32)
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define VKR_TEST_TEMP_DIR_MAX 32u
#define VKR_TEST_TEMP_PATH_MAX 1024u

static char s_temp_dirs[VKR_TEST_TEMP_DIR_MAX][VKR_TEST_TEMP_PATH_MAX];
static uint32_t s_temp_dir_count = 0u;
static bool8_t s_cleanup_installed = false_v;

bool8_t vkr_test_remove_tree(const char *path) {
  struct stat status;
  if (lstat(path, &status) != 0) {
    return true_v;
  }
  if (!S_ISDIR(status.st_mode)) {
    return unlink(path) == 0;
  }
  DIR *directory = opendir(path);
  if (!directory) {
    return false_v;
  }
  bool8_t ok = true_v;
  struct dirent *entry = NULL;
  while (ok && (entry = readdir(directory)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    char child[VKR_TEST_TEMP_PATH_MAX];
    const int length =
        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
    ok = length > 0 && (size_t)length < sizeof(child) &&
         vkr_test_remove_tree(child);
  }
  closedir(directory);
  return ok && rmdir(path) == 0;
}

static void vkr_test_temp_dirs_remove(void) {
  for (uint32_t i = 0u; i < s_temp_dir_count; ++i) {
    (void)vkr_test_remove_tree(s_temp_dirs[i]);
  }
  s_temp_dir_count = 0u;
}

/* A failed assertion aborts. The tester is single-threaded at that point, so
 * removing the fixtures here is acceptable test-only behavior; the default
 * action then still reports the abort. */
static void vkr_test_temp_dirs_on_abort(int signal_number) {
  vkr_test_temp_dirs_remove();
  signal(signal_number, SIG_DFL);
  raise(signal_number);
}

bool8_t vkr_test_temp_dir_create(char *template_path) {
  if (!template_path || s_temp_dir_count >= VKR_TEST_TEMP_DIR_MAX ||
      strlen(template_path) >= VKR_TEST_TEMP_PATH_MAX ||
      !mkdtemp(template_path)) {
    return false_v;
  }
  if (!s_cleanup_installed) {
    s_cleanup_installed = true_v;
    atexit(vkr_test_temp_dirs_remove);
    signal(SIGABRT, vkr_test_temp_dirs_on_abort);
  }
  memcpy(s_temp_dirs[s_temp_dir_count++], template_path,
         strlen(template_path) + 1u);
  return true_v;
}
#endif
