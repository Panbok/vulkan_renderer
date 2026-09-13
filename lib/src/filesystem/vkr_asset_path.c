#include "filesystem/vkr_asset_path.h"

static bool8_t asset_path_separator(uint8_t value) {
#if defined(PLATFORM_WINDOWS)
  return value == '/' || value == '\\';
#else
  return value == '/';
#endif
}

// Return the protected root extent before considering resource query metadata.
// Extended DOS and UNC spellings are supported; device namespaces and ambiguous
// drive-relative paths have no deterministic asset identity.
static bool8_t asset_path_root(String8 path, uint64_t *root) {
  *root = 0;
  uint64_t start = 0;
#if defined(PLATFORM_WINDOWS)
  if (path.length >= 4 && asset_path_separator(path.str[0]) &&
      asset_path_separator(path.str[1]) &&
      (path.str[2] == '?' || path.str[2] == '.') &&
      asset_path_separator(path.str[3])) {
    if (path.str[2] == '.') {
      return false_v;
    }
    start = 4;
    if (path.length >= 8 && (path.str[4] == 'U' || path.str[4] == 'u') &&
        (path.str[5] == 'N' || path.str[5] == 'n') &&
        (path.str[6] == 'C' || path.str[6] == 'c') &&
        asset_path_separator(path.str[7])) {
      start = 8;
    } else if (!(path.length >= 7 && path.str[5] == ':' &&
                 asset_path_separator(path.str[6]))) {
      return false_v;
    }
  }
  if (path.length >= start + 2 && path.str[start + 1] == ':') {
    uint8_t drive = path.str[start];
    if (!((drive >= 'A' && drive <= 'Z') || (drive >= 'a' && drive <= 'z')) ||
        path.length < start + 3 || !asset_path_separator(path.str[start + 2])) {
      return false_v;
    }
    *root = start + 3;
    return true_v;
  }
#endif
  if (start == 8 || (path.length >= 2 && asset_path_separator(path.str[0]) &&
                     asset_path_separator(path.str[1]))) {
    uint64_t at = start == 8 ? 8 : 2;
    for (uint32_t part = 0; part < 2; ++part) {
      uint64_t begin = at;
      while (at < path.length && !asset_path_separator(path.str[at]) &&
             path.str[at] != '?') {
        ++at;
      }
      uint64_t count = at - begin;
      if (!count || (count == 1 && path.str[begin] == '.') ||
          (count == 2 && path.str[begin] == '.' &&
           path.str[begin + 1] == '.')) {
        return false_v;
      }
      if (part == 0) {
        if (at == path.length || !asset_path_separator(path.str[at])) {
          return false_v;
        }
        ++at;
      }
    }
    *root = at;
  } else if (path.length && asset_path_separator(path.str[0])) {
    *root = 1;
  }
  return true_v;
}

bool8_t vkr_asset_path_managed_valid(String8 reference) {
  if (!reference.str || !reference.length) {
    return false_v;
  }
  uint64_t segment = 0;
  for (uint64_t i = 0; i <= reference.length; ++i) {
    if (i < reference.length &&
        (reference.str[i] == '\\' || reference.str[i] == ':' ||
         reference.str[i] == 0)) {
      return false_v;
    }
    if (i == reference.length || reference.str[i] == '/') {
      uint64_t count = i - segment;
      if (!count || (count == 1 && reference.str[segment] == '.') ||
          (count == 2 && reference.str[segment] == '.' &&
           reference.str[segment + 1] == '.')) {
        return false_v;
      }
      segment = i + 1;
    }
  }
  return true_v;
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
  uint64_t reference_root = 0;
  uint64_t owner_root = 0;
  if (!asset_path_root(reference, &reference_root) ||
      !asset_path_root(owner, &owner_root)) {
    return (String8){0};
  }
  const bool8_t relative =
      (reference.length >= 2 && reference.str[0] == '.' &&
       asset_path_separator(reference.str[1])) ||
      (reference.length >= 3 && reference.str[0] == '.' &&
       reference.str[1] == '.' && asset_path_separator(reference.str[2]));
  uint64_t prefix = 0;
  if (relative && owner.length) {
    prefix = owner_root;
    for (uint64_t i = owner_root; i < owner.length && owner.str[i] != '?';
         ++i) {
      if (asset_path_separator(owner.str[i])) {
        prefix = i + 1;
      }
    }
  }
  if (prefix + reference.length > 32767) {
    return (String8){0};
  }
  const uint64_t length = prefix + reference.length;
  uint8_t *buffer = vkr_allocator_alloc(allocator, length + 1,
                                        VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!buffer) {
    return (String8){0};
  }
  if (prefix) {
    MemCopy(buffer, owner.str, prefix);
  }
  MemCopy(buffer + prefix, reference.str, reference.length);
  const uint64_t root = prefix ? owner_root : reference_root;
  uint64_t write = root;
  uint64_t read = root;
  for (uint64_t i = 0; i < root; ++i) {
    if (asset_path_separator(buffer[i])) {
      buffer[i] = '/';
    }
  }
  while (read < length && buffer[read] != '?') {
    if (asset_path_separator(buffer[read])) {
      ++read;
      continue;
    }
    uint64_t start = read;
    while (read < length && !asset_path_separator(buffer[read]) &&
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
    if (write && buffer[write - 1] != '/') {
      buffer[write++] = '/';
    }
    MemCopy(buffer + write, buffer + start, count);
    write += count;
  }
  if (read > 0 && asset_path_separator(buffer[read - 1]) && write > 0 &&
      buffer[write - 1] != '/') {
    buffer[write++] = '/';
  }
  MemCopy(buffer + write, buffer + read, length - read);
  write += length - read;
  buffer[write] = 0;
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
  uint64_t native_root = 0;
  if (asset_path_root(resolved, &native_root) && native_root) {
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
