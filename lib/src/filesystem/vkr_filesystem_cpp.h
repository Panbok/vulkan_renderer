#pragma once

#include <filesystem>
#include <string>
#include <system_error>

extern "C" {
#include "filesystem/filesystem.h"
}

/* Use at a filesystem operation, after resolving any document-relative paths.
 * Windows STL operations need the extended native form for paths over MAX_PATH.
 * This result is a host path, never a serialized managed document reference. */
inline std::filesystem::path
vkr_filesystem_native_path(const std::filesystem::path &path) {
#if defined(_WIN32)
  const std::string utf8 = path.u8string();
  FilePath value = {};
  value.path.str = (uint8_t *)utf8.data();
  value.path.length = utf8.size();
  wchar_t native[32768];
  if (!file_windows_native_path(&value, native)) {
    throw std::filesystem::filesystem_error(
        "invalid UTF-8 host path", path,
        std::make_error_code(std::errc::invalid_argument));
  }
  return std::filesystem::path(native);
#else
  return path;
#endif
}

inline std::filesystem::path
vkr_filesystem_native_utf8_path(const std::string &utf8) {
  return vkr_filesystem_native_path(std::filesystem::u8path(utf8));
}
