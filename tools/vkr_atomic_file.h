#pragma once

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
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

/* Every caller turns a false into a bare `return 1`, so a failure that says
   nothing reaches the Bakery as an exit code with an empty log. Name the step
   and the OS error instead. */
inline bool write_file_atomic(const char *path, const std::string &contents) {
  if (!path || !path[0]) {
    std::cerr << "write_file_atomic: empty destination path\n";
    return false;
  }
  const std::filesystem::path destination(path);
  const std::filesystem::path temporary = atomic_temporary_path(destination);
  std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
  if (!file) {
    std::cerr << "write_file_atomic: cannot open " << temporary.string()
              << " for writing: "
              << std::error_code(errno, std::generic_category()).message()
              << "\n";
    return false;
  }
  file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  file.close();
  if (file.fail()) {
    std::cerr << "write_file_atomic: failed writing " << contents.size()
              << " bytes to " << temporary.string() << ": "
              << std::error_code(errno, std::generic_category()).message()
              << "\n";
    std::error_code remove_error;
    std::filesystem::remove(temporary, remove_error);
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
    std::cerr << "write_file_atomic: cannot replace " << destination.string()
              << " with " << temporary.string() << ": " << error.message()
              << "\n";
    std::error_code remove_error;
    std::filesystem::remove(temporary, remove_error);
    return false;
  }
  return true;
}

} // namespace vkr_tools
