#include "filesystem.h"

FilePath file_path_create(char *path, FilePathType type) {
  assert_log(path != NULL, "path is NULL");
  assert_log(strlen(path) > 0, "path is empty");
  assert_log(type == FILE_PATH_TYPE_RELATIVE || type == FILE_PATH_TYPE_ABSOLUTE,
             "invalid file path type");

  FilePath result = {0};
  result.path = string8_create((uint8_t *)path, strlen(path));
  result.type = type;
  return result;
}