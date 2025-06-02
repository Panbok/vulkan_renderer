#pragma once

#include "defines.h"
#include "pch.h"
#include "platform.h"
#include "str.h"

typedef enum FilePathType : uint32_t {
  FILE_PATH_TYPE_RELATIVE,
  FILE_PATH_TYPE_ABSOLUTE,
} FilePathType;

typedef enum FileMode : uint32_t {
  FILE_MODE_READ,
  FILE_MODE_WRITE,
  FILE_MODE_APPEND,
} FileMode;

typedef struct FilePath {
  String8 path;
  FilePathType type;
} FilePath;

typedef struct FileHandle {
  void *handle;
  FilePath path;
  FileMode mode;
} FileHandle;

FilePath file_path_create(char *path, FilePathType type);

bool8_t file_open(FilePath *path, FileMode mode, FileHandle *out_handle);

void file_close(FileHandle *handle);

bool8_t file_exists(FilePath *path);

bool8_t file_read(FileHandle *handle, void *out_data, uint64_t size);

bool8_t file_write(FileHandle *handle, const void *data, uint64_t size);

bool8_t file_read_line(FileHandle *handle, String8 *out_line);

bool8_t file_read_all(FileHandle *handle, String8 *out_data);

bool8_t file_write_all(FileHandle *handle, const String8 *data);
