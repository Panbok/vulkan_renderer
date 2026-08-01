#include "vkr_harness.h"

bool8_t vkr_harness_make_directories(const char *path,
                                     VkrHarnessError *out_error) {
  if (!path || path[0] == '\0' || string_length(path) >= VKR_HARNESS_PATH_MAX) {
    vkr_harness_error_set(out_error, "artifact.path", "$",
                          "Artifact directory path is invalid");
    return false_v;
  }
  Arena *arena = arena_create(KB(4), KB(4));
  if (!arena) {
    vkr_harness_error_set(out_error, "artifact.mkdir", "$",
                          "Unable to allocate directory scratch storage");
    return false_v;
  }
  VkrAllocator allocator = {.ctx = arena};
  vkr_allocator_arena(&allocator);
  String8 path_view =
      string8_create_from_cstr((const uint8_t *)path, string_length(path));
  const bool8_t created = file_ensure_directory(&allocator, &path_view);
  vkr_allocator_release_global_accounting(&allocator);
  arena_destroy(arena);
  if (!created) {
    vkr_harness_error_set(out_error, "artifact.mkdir", "$",
                          "Unable to create directory '%s'", path);
  }
  return created;
}

bool8_t vkr_harness_atomic_write(const char *path, const void *data,
                                 uint64_t length, VkrHarnessError *out_error) {
  if (!path || (!data && length > 0u)) {
    return false_v;
  }
  char temp[VKR_HARNESS_PATH_MAX];
  const int32_t written = string_format(temp, sizeof(temp), "%s.tmp.%u", path,
                                        vkr_platform_get_process_id());
  if (written < 0 || (uint32_t)written >= sizeof(temp)) {
    vkr_harness_error_set(out_error, "artifact.path", "$",
                          "Artifact path is too long");
    return false_v;
  }
  FilePath temp_path = vkr_harness_file_path(temp);
  FilePath final_path = vkr_harness_file_path(path);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_WRITE);
  bitset8_set(&mode, FILE_MODE_BINARY);
  bitset8_set(&mode, FILE_MODE_TRUNCATE);
  FileHandle file = {0};
  uint64_t bytes_written = 0u;
  if (file_open(&temp_path, mode, &file) != FILE_ERROR_NONE ||
      (length > 0u &&
       file_write(&file, length, data, &bytes_written) != FILE_ERROR_NONE) ||
      bytes_written != length) {
    file_close(&file);
    (void)file_remove(&temp_path);
    vkr_harness_error_set(out_error, "artifact.write", "$",
                          "Unable to write '%s'", path);
    return false_v;
  }
  if (file_sync(&file) != FILE_ERROR_NONE) {
    file_close(&file);
    (void)file_remove(&temp_path);
    vkr_harness_error_set(out_error, "artifact.sync", "$",
                          "Unable to sync '%s'", path);
    return false_v;
  }
  file_close(&file);
  if (file_rename(&temp_path, &final_path, true_v) != FILE_ERROR_NONE) {
    (void)file_remove(&temp_path);
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
  VkrTime utc = {0};
  if (!vkr_platform_get_utc_time(&utc)) {
    return false_v;
  }
  static uint32_t sequence = 0u;
  const uint32_t nonce =
      (utc.milliseconds ^ vkr_platform_get_process_id() ^ ++sequence) &
      0xffffffu;
  return string_format(out_run_id, 64, "%04d%02d%02dT%02d%02d%02d.%03dZ-%06x",
                       utc.year + 1900, utc.month + 1, utc.day, utc.hours,
                       utc.minutes, utc.seconds, utc.milliseconds, nonce) > 0;
}

bool8_t vkr_harness_timestamp_utc(char out_timestamp[40]) {
  if (!out_timestamp) {
    return false_v;
  }
  VkrTime utc = {0};
  if (!vkr_platform_get_utc_time(&utc)) {
    return false_v;
  }
  return string_format(out_timestamp, 40, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                       utc.year + 1900, utc.month + 1, utc.day, utc.hours,
                       utc.minutes, utc.seconds, utc.milliseconds) > 0;
}
