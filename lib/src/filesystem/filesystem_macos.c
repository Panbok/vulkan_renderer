#include "filesystem.h"

FilePath file_path_create(const char *path, FilePathType type) {
  FilePath result = {0};
  result.path = string8_create((uint8_t *)path, strlen(path));
  result.type = type;
  return result;
}