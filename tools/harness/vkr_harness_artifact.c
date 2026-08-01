#include "vkr_harness.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <direct.h>
#include <process.h>
#define VKR_HARNESS_MKDIR(path) _mkdir(path)
#define VKR_HARNESS_PID() _getpid()
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define VKR_HARNESS_MKDIR(path) mkdir((path), 0755)
#define VKR_HARNESS_PID() getpid()
#endif

bool8_t vkr_harness_make_directories(const char *path,
                                     VkrHarnessError *out_error) {
  if (!path || path[0] == '\0' || strlen(path) >= VKR_HARNESS_PATH_MAX) {
    vkr_harness_error_set(out_error, "artifact.path", "$",
                          "Artifact directory path is invalid");
    return false_v;
  }
  char current[VKR_HARNESS_PATH_MAX];
  snprintf(current, sizeof(current), "%s", path);
  char *first_component = current + 1;
#if defined(_WIN32)
  if (current[0] && current[1] == ':') {
    first_component = current + 3;
  }
#endif
  for (char *cursor = first_component; *cursor; ++cursor) {
    if (*cursor != '/' && *cursor != '\\') {
      continue;
    }
    const char saved = *cursor;
    *cursor = '\0';
    if (VKR_HARNESS_MKDIR(current) != 0 && errno != EEXIST) {
      vkr_harness_error_set(out_error, "artifact.mkdir", "$",
                            "Unable to create directory '%s'", current);
      return false_v;
    }
    *cursor = saved;
  }
  if (VKR_HARNESS_MKDIR(current) != 0 && errno != EEXIST) {
    vkr_harness_error_set(out_error, "artifact.mkdir", "$",
                          "Unable to create directory '%s'", current);
    return false_v;
  }
  return true_v;
}

bool8_t vkr_harness_atomic_write(const char *path, const void *data,
                                 uint64_t length, VkrHarnessError *out_error) {
  if (!path || (!data && length > 0u)) {
    return false_v;
  }
  char temp[VKR_HARNESS_PATH_MAX];
  const int written = snprintf(temp, sizeof(temp), "%s.tmp.%u", path,
                               (unsigned)VKR_HARNESS_PID());
  if (written < 0 || (uint32_t)written >= sizeof(temp)) {
    vkr_harness_error_set(out_error, "artifact.path", "$",
                          "Artifact path is too long");
    return false_v;
  }
  FILE *file = fopen(temp, "wb");
  if (!file ||
      (length > 0u &&
       fwrite(data, 1u, (size_t)length, file) != (size_t)length) ||
      fflush(file) != 0) {
    if (file) {
      fclose(file);
    }
    remove(temp);
    vkr_harness_error_set(out_error, "artifact.write", "$",
                          "Unable to write '%s'", path);
    return false_v;
  }
#if !defined(_WIN32)
  if (fsync(fileno(file)) != 0) {
    fclose(file);
    remove(temp);
    vkr_harness_error_set(out_error, "artifact.sync", "$",
                          "Unable to sync '%s'", path);
    return false_v;
  }
#endif
  if (fclose(file) != 0 || rename(temp, path) != 0) {
    remove(temp);
    vkr_harness_error_set(out_error, "artifact.rename", "$",
                          "Unable to atomically publish '%s'", path);
    return false_v;
  }
  return true_v;
}

bool8_t vkr_harness_generate_run_id(char out_run_id[64]) {
  if (!out_run_id) {
    return false_v;
  }
  struct timespec now = {0};
  if (timespec_get(&now, TIME_UTC) != TIME_UTC) {
    return false_v;
  }
  struct tm utc = {0};
#if defined(_WIN32)
  if (gmtime_s(&utc, &now.tv_sec) != 0) {
    return false_v;
  }
#else
  if (!gmtime_r(&now.tv_sec, &utc)) {
    return false_v;
  }
#endif
  const uint32_t nonce =
      ((uint32_t)now.tv_nsec ^ (uint32_t)VKR_HARNESS_PID()) & 0xffffffu;
  return snprintf(out_run_id, 64, "%04d%02d%02dT%02d%02d%02d.%03ldZ-%06x",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
                  utc.tm_min, utc.tm_sec, now.tv_nsec / 1000000L, nonce) > 0;
}

bool8_t vkr_harness_timestamp_utc(char out_timestamp[40]) {
  if (!out_timestamp) {
    return false_v;
  }
  struct timespec now = {0};
  if (timespec_get(&now, TIME_UTC) != TIME_UTC) {
    return false_v;
  }
  struct tm utc = {0};
#if defined(_WIN32)
  if (gmtime_s(&utc, &now.tv_sec) != 0) {
    return false_v;
  }
#else
  if (!gmtime_r(&now.tv_sec, &utc)) {
    return false_v;
  }
#endif
  return snprintf(out_timestamp, 40, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
                  utc.tm_min, utc.tm_sec, now.tv_nsec / 1000000L) > 0;
}
