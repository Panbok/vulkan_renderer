#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace vkr_tools {

inline uint64_t atomic_process_id(void) {
#if defined(_WIN32)
  return static_cast<uint64_t>(_getpid());
#else
  return static_cast<uint64_t>(getpid());
#endif
}

inline std::filesystem::path
atomic_temporary_path(const std::filesystem::path &destination) {
  static std::atomic<uint32_t> sequence = 0u;
  return std::filesystem::path(
      destination.string() + ".tmp." + std::to_string(atomic_process_id()) +
      "." +
      std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()) +
      "." + std::to_string(sequence.fetch_add(1u, std::memory_order_relaxed)));
}

inline bool write_file_atomic(const char *path, const std::string &contents) {
  if (!path || !path[0])
    return false;
  const std::filesystem::path destination(path);
  const std::filesystem::path temporary = atomic_temporary_path(destination);
  std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
  if (!file)
    return false;
  file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  file.close();
  if (file.fail()) {
    std::error_code error;
    std::filesystem::remove(temporary, error);
    return false;
  }
  std::error_code error;
#if defined(_WIN32)
  if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    error = std::error_code((int)GetLastError(), std::system_category());
#else
  std::filesystem::rename(temporary, destination, error);
#endif
  if (error) {
    std::filesystem::remove(temporary, error);
    return false;
  }
  return true;
}

} // namespace vkr_tools
