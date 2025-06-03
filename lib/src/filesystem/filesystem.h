#pragma once

#include "arena.h"
#include "bitset.h"
#include "defines.h"
#include "pch.h"
#include "platform.h"
#include "str.h"

/**
 * @file filesystem.h
 * @brief Synchronous filesystem interface with arena-based memory management.
 *
 * This module provides a cross-platform synchronous filesystem API built on top
 * of standard C file operations. It integrates seamlessly with the arena
 * allocator system for efficient memory management and uses a flexible
 * bitset-based file mode system for fine-grained control over file operations.
 *
 * Key Features:
 * - **Arena Integration:** All file operations that allocate memory use arena
 * allocators, eliminating the need for manual memory management and reducing
 * fragmentation.
 * - **Bitset File Modes:** File open modes are specified using bitsets,
 * allowing arbitrary combinations of read/write/append/binary/truncate flags.
 * - **UTF-8 Assumption:** All text operations assume valid UTF-8 encoding.
 * - **Single-Threaded:** This implementation is designed for single-threaded
 * use only.
 * - **Error Handling:** Comprehensive error reporting with detailed error
 * codes.
 * - **Large File Support:** Uses `ftello`/`fseeko` for files larger than 2GB.
 *
 * File Mode System:
 * Instead of simple enumerated modes, this API uses a bitset-based system that
 * allows combining multiple flags to achieve the desired file behavior:
 *
 * Basic Flags:
 * - `FILE_MODE_READ`: Enable reading from the file
 * - `FILE_MODE_WRITE`: Enable writing to the file
 * - `FILE_MODE_APPEND`: Open in append mode (writes go to end)
 * - `FILE_MODE_BINARY`: Open in binary mode (no text transformations)
 * - `FILE_MODE_TRUNCATE`: Truncate file to zero length if it exists
 * - `FILE_MODE_CREATE`: Create file if it doesn't exist (implicit with
 * write/append)
 *
 * Predefined Combinations:
 * - `FILE_MODE_READ_WRITE`: Read and write access
 * - `FILE_MODE_READ_APPEND`: Read access with append writes
 * - `FILE_MODE_WRITE_CREATE`: Write with file creation
 * - `FILE_MODE_WRITE_TRUNCATE`: Write with truncation
 * - `FILE_MODE_READ_WRITE_CREATE`: Read/write with creation
 * - `FILE_MODE_READ_WRITE_APPEND`: Read/write with append
 * - `FILE_MODE_READ_WRITE_TRUNCATE`: Read/write with truncation
 *
 * Standard C Mode Mapping:
 * The bitset flags are mapped to standard C `fopen` mode strings:
 * - READ only → "r" or "rb"
 * - WRITE only → "w" or "wb"
 * - WRITE + TRUNCATE → "w" or "wb"
 * - APPEND → "a" or "ab"
 * - READ + WRITE → "r+" or "r+b"
 * - READ + WRITE + TRUNCATE → "w+" or "w+b"
 * - READ + APPEND → "a+" or "a+b"
 *
 * Memory Management:
 * All functions that return allocated data use the provided `Arena*` parameter
 * for allocation. The caller is responsible for managing the arena's lifetime.
 * Data allocated from operations remains valid until the arena is reset or
 * destroyed.
 *
 * Error Handling:
 * Functions return `FileError` codes for detailed error reporting. Boolean
 * functions return `false` on failure. Error logging is done through the
 * logging system using `log_error` for non-fatal errors.
 *
 * @example Basic Usage:
 * ```c
 * Arena *arena = arena_create();
 *
 * // Create file path
 * FilePath path = file_path_create("data.txt", FILE_PATH_TYPE_RELATIVE);
 *
 * // Set up read mode
 * FileMode mode = bitset8_create();
 * bitset8_set(&mode, FILE_MODE_READ);
 *
 * FileHandle handle;
 * if (file_open(&path, mode, &handle)) {
 *     String8 content;
 *     if (file_read_string(&handle, arena, &content) == FILE_ERROR_NONE) {
 *         // Use content.str and content.length
 *     }
 *     file_close(&handle);
 * }
 *
 * arena_destroy(arena);
 * ```
 *
 * @example Write with Multiple Flags:
 * ```c
 * FileMode mode = bitset8_create();
 * bitset8_set(&mode, FILE_MODE_WRITE);
 * bitset8_set(&mode, FILE_MODE_BINARY);
 * bitset8_set(&mode, FILE_MODE_TRUNCATE);
 *
 * FileHandle handle;
 * if (file_open(&path, mode, &handle)) {
 *     uint8_t data[] = {0x00, 0x01, 0x02, 0x03};
 *     uint64_t written;
 *     file_write(&handle, sizeof(data), data, &written);
 *     file_close(&handle);
 * }
 * ```
 */

/**
 * @brief Specifies whether a file path is relative or absolute.
 */
typedef enum FilePathType : uint32_t {
  FILE_PATH_TYPE_RELATIVE, /**< Path is relative to current working directory */
  FILE_PATH_TYPE_ABSOLUTE, /**< Path is absolute from filesystem root */
} FilePathType;

/**
 * @brief Individual file mode flags that can be combined using bitsets.
 *
 * These flags control how a file is opened and what operations are permitted.
 * Multiple flags can be combined to achieve the desired behavior.
 */
typedef enum FileModeFlags : uint32_t {
  FILE_MODE_FLAGS_NONE = 0,    /**< No flags set */
  FILE_MODE_READ = 1 << 0,     /**< Enable reading from file */
  FILE_MODE_WRITE = 1 << 1,    /**< Enable writing to file */
  FILE_MODE_APPEND = 1 << 2,   /**< Open in append mode (writes go to end) */
  FILE_MODE_CREATE = 1 << 3,   /**< Create file if it doesn't exist */
  FILE_MODE_TRUNCATE = 1 << 4, /**< Truncate file to zero length */
  FILE_MODE_BINARY =
      1 << 5, /**< Open in binary mode (no text transformations) */
  FILE_MODE_TEXT =
      1 << 6, /**< Open in text mode (platform-specific line ending handling) */

  /* Convenience combinations */
  FILE_MODE_READ_WRITE =
      FILE_MODE_READ | FILE_MODE_WRITE, /**< Read and write access */
  FILE_MODE_READ_APPEND =
      FILE_MODE_READ | FILE_MODE_APPEND, /**< Read with append writes */
  FILE_MODE_WRITE_CREATE =
      FILE_MODE_WRITE | FILE_MODE_CREATE, /**< Write with file creation */
  FILE_MODE_WRITE_TRUNCATE =
      FILE_MODE_WRITE | FILE_MODE_TRUNCATE, /**< Write with truncation */
  FILE_MODE_READ_WRITE_CREATE =
      FILE_MODE_READ | FILE_MODE_WRITE |
      FILE_MODE_CREATE, /**< Read/write with creation */
  FILE_MODE_READ_WRITE_APPEND = FILE_MODE_READ | FILE_MODE_WRITE |
                                FILE_MODE_APPEND, /**< Read/write with append */
  FILE_MODE_READ_WRITE_TRUNCATE =
      FILE_MODE_READ | FILE_MODE_WRITE |
      FILE_MODE_TRUNCATE, /**< Read/write with truncation */

  FILE_MODE_FLAGS_COUNT, /**< Total number of flag combinations */
} FileModeFlags;

/**
 * @brief File operation error codes.
 *
 * These codes provide detailed information about what went wrong during
 * file operations, allowing for appropriate error handling and user feedback.
 */
typedef enum FileError : uint32_t {
  FILE_ERROR_NONE,          /**< Operation completed successfully */
  FILE_ERROR_NOT_FOUND,     /**< File or directory does not exist */
  FILE_ERROR_ACCESS_DENIED, /**< Insufficient permissions to access file */
  FILE_ERROR_IO_ERROR, /**< General I/O error (disk full, network error, etc.)
                        */
  FILE_ERROR_INVALID_HANDLE, /**< File handle is invalid or file is not open */
  FILE_ERROR_COUNT,          /**< Total number of error types */
} FileError;

/**
 * @brief A bitset representing the file opening mode.
 *
 * Use `bitset8_set()` to enable specific flags from `FileModeFlags`.
 * Multiple flags can be combined to achieve complex opening behaviors.
 */
typedef Bitset8 FileMode;

/**
 * @brief Represents a file path with its type information.
 *
 * Contains both the path string and whether it's relative or absolute.
 * The path string is stored as a `String8` for UTF-8 compatibility.
 */
typedef struct FilePath {
  String8 path;      /**< The file path as a UTF-8 string */
  FilePathType type; /**< Whether the path is relative or absolute */
} FilePath;

/**
 * @brief Handle to an open file with associated metadata.
 *
 * Encapsulates the platform-specific file handle along with the path
 * and mode information for debugging and error reporting.
 */
typedef struct FileHandle {
  void *handle;  /**< Platform-specific file handle (FILE* on POSIX) */
  FilePath path; /**< Copy of the file path used to open this file */
  FileMode mode; /**< Copy of the mode flags used to open this file */
} FileHandle;

/**
 * @brief File metadata information.
 *
 * Contains basic file statistics that can be retrieved without opening the
 * file.
 */
typedef struct FileStats {
  uint64_t size;          /**< File size in bytes */
  uint64_t last_modified; /**< Last modification time as Unix timestamp */
} FileStats;

/**
 * @brief Creates a new file path structure.
 *
 * Initializes a `FilePath` with the given path string and type.
 * The path string is copied into a `String8` structure.
 *
 * @param path The file path as a null-terminated string. Must not be NULL or
 * empty.
 * @param type Whether the path is relative or absolute.
 * @return A new `FilePath` structure containing the path information.
 */
FilePath file_path_create(const char *path, FilePathType type);

/**
 * @brief Opens a file with the specified mode flags.
 *
 * Opens the file at the given path using the mode specified by the bitset.
 * The mode bitset can contain any combination of `FileModeFlags` to achieve
 * the desired file access behavior.
 *
 * @param path Pointer to the file path to open. Must not be NULL.
 * @param mode Bitset containing the desired file mode flags.
 * @param out_handle Pointer to store the opened file handle. Must not be NULL.
 * @return `true` if the file was opened successfully, `false` otherwise.
 *
 * @example
 * ```c
 * FileMode mode = bitset8_create();
 * bitset8_set(&mode, FILE_MODE_READ);
 * bitset8_set(&mode, FILE_MODE_BINARY);
 *
 * FileHandle handle;
 * if (file_open(&path, mode, &handle)) {
 *     // File is now open for binary reading
 *     file_close(&handle);
 * }
 * ```
 */
bool8_t file_open(FilePath *path, FileMode mode, FileHandle *out_handle);

/**
 * @brief Closes an open file handle.
 *
 * Closes the file associated with the handle and releases any system resources.
 * After calling this function, the handle should not be used for further
 * operations. It is safe to call this function multiple times on the same
 * handle.
 *
 * @param handle Pointer to the file handle to close. Must not be NULL.
 */
void file_close(FileHandle *handle);

/**
 * @brief Checks if a file or directory exists.
 *
 * Tests whether the specified path exists in the filesystem without opening it.
 * This function works for both files and directories.
 *
 * @param path The file path to check as a null-terminated string. Must not be
 * NULL or empty.
 * @return `true` if the path exists, `false` otherwise.
 */
bool8_t file_exists(const char *path);

/**
 * @brief Retrieves file statistics without opening the file.
 *
 * Gathers basic information about the file such as size and modification time.
 * This is more efficient than opening the file just to get metadata.
 *
 * @param path Pointer to the file path. Must not be NULL.
 * @param out_stats Pointer to store the file statistics. Must not be NULL.
 * @return `FILE_ERROR_NONE` on success, `FILE_ERROR_NOT_FOUND` if the file
 * doesn't exist, or another error code on failure.
 */
FileError file_stats(FilePath *path, FileStats *out_stats);

/**
 * @brief Reads a single line from a text file.
 *
 * Reads one line from the current position in the file, including the newline
 * character if present. The line is allocated from the provided arena and
 * stored as a `String8`. The file position advances to the start of the next
 * line.
 *
 * @param handle Pointer to an open file handle. Must not be NULL.
 * @param arena Arena to allocate the line string from. Must not be NULL.
 * @param out_line Pointer to store the line as a `String8`. Must not be NULL.
 * @return `FILE_ERROR_NONE` on success, `FILE_ERROR_INVALID_HANDLE` if the file
 * is not open, or another error code on failure.
 *
 * @note The maximum line length is 32,000 characters. Longer lines will be
 * truncated.
 */
FileError file_read_line(FileHandle *handle, Arena *arena, String8 *out_line);

/**
 * @brief Writes a line of text to a file.
 *
 * Writes the contents of the string to the file followed by a newline
 * character. The file is automatically flushed after writing to ensure data
 * persistence.
 *
 * @param handle Pointer to an open file handle with write permissions. Must not
 * be NULL.
 * @param text Pointer to the string to write. Must not be NULL.
 * @return `FILE_ERROR_NONE` on success, `FILE_ERROR_INVALID_HANDLE` if the file
 * is not open for writing, `FILE_ERROR_IO_ERROR` on write failure, or another
 * error code.
 */
FileError file_write_line(FileHandle *handle, const String8 *text);

/**
 * @brief Reads a specific number of bytes from a file.
 *
 * Reads up to `size` bytes from the current file position into a buffer
 * allocated from the provided arena. The actual number of bytes read is
 * returned through `bytes_read`, which may be less than requested if EOF is
 * reached.
 *
 * @param handle Pointer to an open file handle with read permissions. Must not
 * be NULL.
 * @param arena Arena to allocate the read buffer from. Must not be NULL.
 * @param size Number of bytes to read. Must be greater than 0.
 * @param bytes_read Pointer to store the actual number of bytes read. Must not
 * be NULL.
 * @param out_buffer Pointer to store the allocated buffer containing the data.
 * Must not be NULL.
 * @return `FILE_ERROR_NONE` on success, `FILE_ERROR_INVALID_HANDLE` if the file
 * is not open, `FILE_ERROR_IO_ERROR` on read failure, or another error code.
 */
FileError file_read(FileHandle *handle, Arena *arena, uint64_t size,
                    uint64_t *bytes_read, uint8_t **out_buffer);

/**
 * @brief Writes raw data to a file.
 *
 * Writes the specified buffer contents to the file at the current position.
 * The file is automatically flushed after writing to ensure data persistence.
 *
 * @param handle Pointer to an open file handle with write permissions. Must not
 * be NULL.
 * @param size Number of bytes to write. Must be greater than 0.
 * @param buffer Pointer to the data to write. Must not be NULL.
 * @param bytes_written Pointer to store the actual number of bytes written.
 * Must not be NULL.
 * @return `FILE_ERROR_NONE` on success, `FILE_ERROR_INVALID_HANDLE` if the file
 * is not open for writing, `FILE_ERROR_IO_ERROR` on write failure, or another
 * error code.
 */
FileError file_write(FileHandle *handle, uint64_t size, const uint8_t *buffer,
                     uint64_t *bytes_written);

/**
 * @brief Reads the entire file as a UTF-8 string.
 *
 * Reads all remaining content from the current file position to the end and
 * returns it as a null-terminated `String8`. This is convenient for loading
 * text files where the entire content is needed at once.
 *
 * @param handle Pointer to an open file handle with read permissions. Must not
 * be NULL.
 * @param arena Arena to allocate the string from. Must not be NULL.
 * @param out_data Pointer to store the file contents as a `String8`. Must not
 * be NULL.
 * @return `FILE_ERROR_NONE` on success, `FILE_ERROR_INVALID_HANDLE` if the file
 * is not open, `FILE_ERROR_IO_ERROR` on read failure, or another error code.
 *
 * @note The returned string includes a null terminator for C compatibility.
 */
FileError file_read_string(FileHandle *handle, Arena *arena, String8 *out_data);

/**
 * @brief Reads all remaining data from a file.
 *
 * Reads from the current file position to the end of the file, allocating
 * a buffer from the provided arena. This preserves the current file position
 * behavior - it reads from wherever the file pointer currently is.
 *
 * @param handle Pointer to an open file handle with read permissions. Must not
 * be NULL.
 * @param arena Arena to allocate the read buffer from. Must not be NULL.
 * @param out_buffer Pointer to store the allocated buffer containing the data.
 * Must not be NULL.
 * @param bytes_read Pointer to store the number of bytes read. Must not be
 * NULL.
 * @return `FILE_ERROR_NONE` on success, `FILE_ERROR_INVALID_HANDLE` if the file
 * is not open, `FILE_ERROR_IO_ERROR` on read failure, or another error code.
 *
 * @note Uses `ftello`/`fseeko` for large file support (>2GB).
 */
FileError file_read_all(FileHandle *handle, Arena *arena, uint8_t **out_buffer,
                        uint64_t *bytes_read);
