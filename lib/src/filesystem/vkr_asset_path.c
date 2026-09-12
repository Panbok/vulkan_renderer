#include "filesystem/vkr_asset_path.h"

static bool8_t asset_path_absolute(String8 path) {
  return path.length > 0 && (path.str[0] == '/' || path.str[0] == '\\' ||
                             (path.length > 2 && path.str[1] == ':' &&
                              (path.str[2] == '/' || path.str[2] == '\\')));
}

String8 vkr_asset_path_resolve(VkrAllocator *allocator, String8 owner,
                               String8 reference) {
  if (!allocator || !reference.str || !reference.length ||
      reference.length > 32767 || owner.length > 32767 ||
      (owner.length && !owner.str) ||
      memchr(reference.str, 0, reference.length) ||
      (owner.length && memchr(owner.str, 0, owner.length))) {
    return (String8){0};
  }
  const bool8_t relative = (reference.length >= 2 && reference.str[0] == '.' &&
                            reference.str[1] == '/') ||
                           (reference.length >= 3 && reference.str[0] == '.' &&
                            reference.str[1] == '.' && reference.str[2] == '/');
  uint64_t prefix = 0;
  if (relative && owner.length) {
    for (uint64_t i = 0; i < owner.length && owner.str[i] != '?'; ++i) {
      if (owner.str[i] == '/' || owner.str[i] == '\\') {
        prefix = i + 1;
      }
    }
  }
  if (prefix + reference.length > 32767) {
    return (String8){0};
  }
  uint8_t *buffer =
      vkr_allocator_alloc(allocator, prefix + reference.length + 1,
                          VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!buffer) {
    return (String8){0};
  }
  if (prefix) {
    MemCopy(buffer, owner.str, prefix);
  }
  MemCopy(buffer + prefix, reference.str, reference.length);
  const uint64_t length = prefix + reference.length;
  // Collapse dot components in place, leaving the query opaque. Preserve UNC
  // roots and drive prefixes. '..' is retained when no ordinary segment exists.
  uint64_t read = 0;
  uint64_t write = 0;
  if (length && (buffer[0] == '/' || buffer[0] == '\\')) {
    buffer[write++] = '/';
    read = 1;
    if (read < length && (buffer[read] == '/' || buffer[read] == '\\')) {
      buffer[write++] = '/';
      ++read;
    }
  } else if (length >= 3 && buffer[1] == ':' &&
             (buffer[2] == '/' || buffer[2] == '\\')) {
    buffer[write++] = buffer[0];
    buffer[write++] = ':';
    buffer[write++] = '/';
    read = 3;
  }
  const uint64_t root = write;
  while (read < length && buffer[read] != '?') {
    if (buffer[read] == '/' || buffer[read] == '\\') {
      ++read;
      continue;
    }
    uint64_t start = read;
    while (read < length && buffer[read] != '/' && buffer[read] != '\\' &&
           buffer[read] != '?') {
      ++read;
    }
    uint64_t count = read - start;
    if (count == 1 && buffer[start] == '.') {
      continue;
    }
    if (count == 2 && buffer[start] == '.' && buffer[start + 1] == '.') {
      uint64_t last = write;
      while (last > root && buffer[last - 1] != '/') {
        --last;
      }
      if (write > root && !(write - last == 2 && buffer[last] == '.' &&
                            buffer[last + 1] == '.')) {
        write = last > root ? last - 1 : root;
        continue;
      }
      if (root) {
        continue;
      }
    }
    if (write > root) {
      buffer[write++] = '/';
    }
    MemCopy(buffer + write, buffer + start, count);
    write += count;
  }
  if (read > 0 && (buffer[read - 1] == '/' || buffer[read - 1] == '\\') &&
      write > 0 && buffer[write - 1] != '/') {
    buffer[write++] = '/';
  }
  MemCopy(buffer + write, buffer + read, length - read);
  write += length - read;
  buffer[write] = 0;
  // Exact size is required by independently freeable allocator accounting.
  String8 view = {.str = buffer, .length = write};
  String8 result = string8_duplicate(allocator, &view);
  vkr_allocator_free(allocator, buffer, length + 1,
                     VKR_ALLOCATOR_MEMORY_TAG_STRING);
  return result;
}

FilePath vkr_asset_path_file(VkrAllocator *allocator, String8 path) {
  String8 resolved = vkr_asset_path_resolve(allocator, (String8){0}, path);
  if (!resolved.str) {
    return (FilePath){0};
  }
  if (asset_path_absolute(resolved)) {
    return (FilePath){.path = resolved, .type = FILE_PATH_TYPE_ABSOLUTE};
  }
  String8 root = string8_create_from_cstr((const uint8_t *)PROJECT_SOURCE_DIR,
                                          strlen(PROJECT_SOURCE_DIR));
  if (root.length + resolved.length > 32767) {
    vkr_allocator_free(allocator, resolved.str, resolved.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
    return (FilePath){0};
  }
  uint64_t size = root.length + resolved.length + 1;
  uint8_t *absolute =
      vkr_allocator_alloc(allocator, size, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (absolute) {
    MemCopy(absolute, root.str, root.length);
    MemCopy(absolute + root.length, resolved.str, resolved.length + 1);
  }
  vkr_allocator_free(allocator, resolved.str, resolved.length + 1,
                     VKR_ALLOCATOR_MEMORY_TAG_STRING);
  return (FilePath){
      .path = {.str = absolute, .length = absolute ? size - 1 : 0},
      .type = FILE_PATH_TYPE_ABSOLUTE};
}
