#include "filesystem/vkr_vfs.h"

#include "core/vkr_hash.h"
#include "core/vkr_json.h"
#include "filesystem/filesystem.h"
#include "platform/vkr_filesystem_internal.h"
#include "platform/vkr_platform.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define VKR_VFS_MAX_PACKS 16u
#define VKR_VFS_PATH_MAX 4096u

typedef struct VkrVfsPack {
  FileMapping mapping;
  const VkrPackEntry *entries;
  const VkrPackChunk *chunks;
  const uint8_t *strings;
  uint32_t entry_count;
} VkrVfsPack;

/* Written only while mounting at startup; read-only afterwards. */
typedef struct VkrVfsState {
  char root[VKR_VFS_PATH_MAX];
  uint64_t root_length;
  VkrVfsPack packs[VKR_VFS_MAX_PACKS];
  uint32_t pack_count;
  char bundle_scene[VKR_PACK_IDENTITY_MAX];
  /* The mounted bundle.json, malloc-owned until vkr_vfs_unmount_all. */
  uint8_t *bundle_description;
  uint64_t bundle_description_size;
  /* One fwrite per line; stdio locks the stream for concurrent loaders. */
  FILE *record;
} VkrVfsState;

vkr_global VkrVfsState vkr_vfs = {0};

/* Mounting runs before the logger exists, so failures go to stderr. */
vkr_internal void vkr_vfs_report(const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  fputs("[vkr_vfs] ", stderr);
  vfprintf(stderr, format, arguments);
  fputc('\n', stderr);
  va_end(arguments);
}

// =============================================================================
// Archive format
// =============================================================================

vkr_internal bool8_t vkr_pack_fail(char *error, uint64_t capacity,
                                   const char *message) {
  if (error && capacity) {
    const uint64_t length = Min(strlen(message), capacity - 1u);
    MemCopy(error, message, length);
    error[length] = 0;
  }
  return false_v;
}

vkr_internal bool8_t vkr_pack_range(uint64_t offset, uint64_t size,
                                    uint64_t total) {
  return offset <= total && size <= total - offset;
}

/* Identities are relative, `/`-separated and free of empty, `.` and `..`
 * segments, so one identity has one spelling. */
vkr_internal bool8_t vkr_pack_identity_valid(const uint8_t *text,
                                             uint32_t length) {
  if (!length || length >= VKR_PACK_IDENTITY_MAX || text[0] == '/') {
    return false_v;
  }
  uint32_t segment = 0u;
  for (uint32_t i = 0u; i <= length; ++i) {
    if (i < length && text[i] == 0) {
      return false_v;
    }
    if (i < length && text[i] == '\\') {
      return false_v;
    }
    if (i == length || text[i] == '/') {
      const uint32_t size = i - segment;
      if (!size || (size == 1u && text[segment] == '.') ||
          (size == 2u && text[segment] == '.' && text[segment + 1u] == '.')) {
        return false_v;
      }
      segment = i + 1u;
    }
  }
  return true_v;
}

vkr_internal int32_t vkr_pack_compare(const uint8_t *lhs, uint32_t lhs_length,
                                      const uint8_t *rhs, uint32_t rhs_length) {
  const int32_t order = MemCompare(lhs, rhs, Min(lhs_length, rhs_length));
  if (order) {
    return order;
  }
  return lhs_length < rhs_length ? -1 : lhs_length > rhs_length ? 1 : 0;
}

bool8_t vkr_pack_validate(const uint8_t *data, uint64_t size, char *error,
                          uint64_t error_capacity) {
  if (!data || size < sizeof(VkrPackHeader)) {
    return vkr_pack_fail(error, error_capacity, "archive is truncated");
  }
  VkrPackHeader header;
  MemCopy(&header, data, sizeof(header));
  if (MemCompare(header.magic, VKR_PACK_MAGIC, 4u) != 0) {
    return vkr_pack_fail(error, error_capacity, "not a .vkpak archive");
  }
  if (header.version != VKR_PACK_VERSION || header.flags != 0u) {
    return vkr_pack_fail(error, error_capacity,
                         "unsupported .vkpak version or flags");
  }
  if (header.total_size != size ||
      !vkr_pack_range(header.catalog_offset, header.catalog_size, size) ||
      !vkr_pack_range(header.chunk_table_offset, header.chunk_table_size,
                      size) ||
      header.catalog_size < sizeof(VkrPackCatalogHeader) ||
      header.chunk_table_size % sizeof(VkrPackChunk) != 0u ||
      header.catalog_offset % 8u != 0u ||
      header.chunk_table_offset % 8u != 0u) {
    return vkr_pack_fail(error, error_capacity,
                         "archive tables are out of bounds");
  }

  VkrSha256 hash;
  vkr_sha256_init(&hash);
  vkr_sha256_update(&hash, data + header.catalog_offset, header.catalog_size);
  vkr_sha256_update(&hash, data + header.chunk_table_offset,
                    header.chunk_table_size);
  uint8_t digest[VKR_SHA256_DIGEST_SIZE];
  vkr_sha256_final(&hash, digest);
  if (MemCompare(digest, header.index_sha256, sizeof(digest)) != 0) {
    return vkr_pack_fail(error, error_capacity, "archive index hash mismatch");
  }

  const uint64_t chunk_count = header.chunk_table_size / sizeof(VkrPackChunk);
  const VkrPackChunk *chunks =
      (const VkrPackChunk *)(data + header.chunk_table_offset);
  for (uint64_t i = 0u; i < chunk_count; ++i) {
    const VkrPackChunk *chunk = &chunks[i];
    if (!vkr_pack_range(chunk->offset, chunk->size, size) ||
        !chunk->alignment || (chunk->alignment & (chunk->alignment - 1u)) ||
        chunk->offset % chunk->alignment != 0u ||
        (i && MemCompare(chunks[i - 1u].sha256, chunk->sha256, 32u) >= 0)) {
      return vkr_pack_fail(error, error_capacity,
                           "archive chunk table is invalid");
    }
  }

  const uint8_t *catalog = data + header.catalog_offset;
  VkrPackCatalogHeader catalog_header;
  MemCopy(&catalog_header, catalog, sizeof(catalog_header));
  const uint64_t entries_size =
      (uint64_t)catalog_header.entry_count * sizeof(VkrPackEntry);
  if (entries_size > header.catalog_size - sizeof(catalog_header)) {
    return vkr_pack_fail(error, error_capacity, "archive catalog is truncated");
  }
  const VkrPackEntry *entries =
      (const VkrPackEntry *)(catalog + sizeof(catalog_header));
  const uint8_t *strings = catalog + sizeof(catalog_header) + entries_size;
  const uint64_t strings_size =
      header.catalog_size - sizeof(catalog_header) - entries_size;
  for (uint32_t i = 0u; i < catalog_header.entry_count; ++i) {
    const VkrPackEntry *entry = &entries[i];
    if (!vkr_pack_range(entry->identity_offset, entry->identity_length,
                        strings_size) ||
        entry->chunk >= chunk_count || entry->loader >= VKR_PACK_LOADER_COUNT ||
        !vkr_pack_identity_valid(strings + entry->identity_offset,
                                 entry->identity_length)) {
      return vkr_pack_fail(error, error_capacity,
                           "archive catalog entry is invalid");
    }
    if (i && vkr_pack_compare(strings + entries[i - 1u].identity_offset,
                              entries[i - 1u].identity_length,
                              strings + entry->identity_offset,
                              entry->identity_length) >= 0) {
      return vkr_pack_fail(error, error_capacity,
                           "archive catalog is not sorted");
    }
  }
  return true_v;
}

// =============================================================================
// Content root and identities
// =============================================================================

vkr_internal void vkr_vfs_default_root(void) {
  if (!vkr_vfs.root_length) {
    (void)vkr_string_copy_bounded(vkr_vfs.root, sizeof(vkr_vfs.root),
                                  PROJECT_SOURCE_DIR);
    vkr_vfs.root_length = strlen(vkr_vfs.root);
  }
}

const char *vkr_content_root(void) {
  vkr_vfs_default_root();
  return vkr_vfs.root;
}

bool8_t vkr_vfs_set_content_root(const char *directory) {
  const uint64_t length = directory ? strlen(directory) : 0u;
  if (!length || length + 2u > sizeof(vkr_vfs.root)) {
    return false_v;
  }
  MemCopy(vkr_vfs.root, directory, length);
  if (directory[length - 1u] != '/' && directory[length - 1u] != '\\') {
    vkr_vfs.root[length] = '/';
    vkr_vfs.root[length + 1u] = 0;
    vkr_vfs.root_length = length + 1u;
  } else {
    vkr_vfs.root[length] = 0;
    vkr_vfs.root_length = length;
  }
  return true_v;
}

vkr_internal bool8_t vkr_vfs_is_separator(uint8_t c) {
#if defined(PLATFORM_WINDOWS)
  return c == '/' || c == '\\';
#else
  return c == '/';
#endif
}

vkr_internal bool8_t vkr_vfs_same_byte(uint8_t lhs, uint8_t rhs) {
  if (vkr_vfs_is_separator(lhs) && vkr_vfs_is_separator(rhs)) {
    return true_v;
  }
#if defined(PLATFORM_WINDOWS)
  if (lhs >= 'A' && lhs <= 'Z') {
    lhs = (uint8_t)(lhs - 'A' + 'a');
  }
  if (rhs >= 'A' && rhs <= 'Z') {
    rhs = (uint8_t)(rhs - 'A' + 'a');
  }
#endif
  return lhs == rhs;
}

/* Writes the root-relative identity of `path` into `out`; false when the path
 * lies outside the content root or climbs above it. */
vkr_internal bool8_t vkr_vfs_identity(String8 path, char *out,
                                      uint32_t *out_length) {
  vkr_vfs_default_root();
  if (!path.str || path.length <= vkr_vfs.root_length) {
    return false_v;
  }
  for (uint64_t i = 0u; i < vkr_vfs.root_length; ++i) {
    if (!vkr_vfs_same_byte(path.str[i], (uint8_t)vkr_vfs.root[i])) {
      return false_v;
    }
  }
  uint32_t length = 0u;
  uint64_t start = vkr_vfs.root_length;
  for (uint64_t i = start; i <= path.length; ++i) {
    if (i < path.length && !vkr_vfs_is_separator(path.str[i])) {
      continue;
    }
    const uint64_t size = i - start;
    const uint8_t *segment = path.str + start;
    start = i + 1u;
    if (!size || (size == 1u && segment[0] == '.')) {
      continue;
    }
    if (size == 2u && segment[0] == '.' && segment[1] == '.') {
      if (!length) {
        return false_v;
      }
      while (length && out[length - 1u] != '/') {
        --length;
      }
      if (length) {
        --length;
      }
      continue;
    }
    if (length + size + 2u > VKR_PACK_IDENTITY_MAX) {
      return false_v;
    }
    if (length) {
      out[length++] = '/';
    }
    MemCopy(out + length, segment, size);
    length += (uint32_t)size;
  }
  out[length] = 0;
  *out_length = length;
  return length > 0u;
}

vkr_internal bool8_t vkr_vfs_lookup(const char *identity, uint32_t length,
                                    VkrVfsView *out_view) {
  for (uint32_t p = 0u; p < vkr_vfs.pack_count; ++p) {
    const VkrVfsPack *pack = &vkr_vfs.packs[p];
    uint32_t low = 0u;
    uint32_t high = pack->entry_count;
    while (low < high) {
      const uint32_t middle = low + (high - low) / 2u;
      const VkrPackEntry *entry = &pack->entries[middle];
      const int32_t order = vkr_pack_compare(
          pack->strings + entry->identity_offset, entry->identity_length,
          (const uint8_t *)identity, length);
      if (order == 0) {
        const VkrPackChunk *chunk = &pack->chunks[entry->chunk];
        *out_view = (VkrVfsView){
            .data = pack->mapping.data + chunk->offset,
            .size = chunk->size,
            .last_modified = pack->mapping.last_modified,
        };
        return true_v;
      }
      if (order < 0) {
        low = middle + 1u;
      } else {
        high = middle;
      }
    }
  }
  return false_v;
}

bool8_t vkr_vfs_find(String8 path, VkrVfsView *out_view) {
  if (!out_view || (!vkr_vfs.pack_count && !vkr_vfs.record)) {
    return false_v;
  }
  char identity[VKR_PACK_IDENTITY_MAX];
  uint32_t length = 0u;
  if (!vkr_vfs_identity(path, identity, &length)) {
    return false_v;
  }
  if (vkr_vfs.record) {
    identity[length] = '\n';
    (void)fwrite(identity, 1u, length + 1u, vkr_vfs.record);
    identity[length] = 0;
  }
  return vkr_vfs_lookup(identity, length, out_view);
}

uint32_t vkr_vfs_pack_count(void) { return vkr_vfs.pack_count; }

const char *vkr_vfs_bundle_scene(void) {
  return vkr_vfs.bundle_scene[0] ? vkr_vfs.bundle_scene : NULL;
}

String8 vkr_vfs_bundle_description(void) {
  return (String8){.str = vkr_vfs.bundle_description,
                   .length = vkr_vfs.bundle_description_size};
}

// =============================================================================
// Mounting
// =============================================================================

bool8_t vkr_vfs_mount_pack(const char *path) {
  if (!path || vkr_vfs.pack_count == VKR_VFS_MAX_PACKS) {
    vkr_vfs_report("Cannot mount %s: at most %u archives mount",
                   path ? path : "", VKR_VFS_MAX_PACKS);
    return false_v;
  }
  const FilePath file = {
      .path = string8_create_from_cstr((const uint8_t *)path, strlen(path)),
      .type = FILE_PATH_TYPE_ABSOLUTE};
  VkrVfsPack pack = {0};
  const FileError mapped = file_map_readonly(&file, &pack.mapping);
  if (mapped != FILE_ERROR_NONE) {
    const String8 reason = file_get_error_string(mapped);
    vkr_vfs_report("Cannot map content archive %s: %.*s", path,
                   (int)reason.length, reason.str);
    return false_v;
  }
  char error[128];
  if (!vkr_pack_validate(pack.mapping.data, pack.mapping.size, error,
                         sizeof(error))) {
    vkr_vfs_report("Content archive %s is invalid: %s", path, error);
    file_unmap(&pack.mapping);
    return false_v;
  }
  VkrPackHeader header;
  MemCopy(&header, pack.mapping.data, sizeof(header));
  const uint8_t *catalog = pack.mapping.data + header.catalog_offset;
  VkrPackCatalogHeader catalog_header;
  MemCopy(&catalog_header, catalog, sizeof(catalog_header));
  pack.entry_count = catalog_header.entry_count;
  pack.entries = (const VkrPackEntry *)(catalog + sizeof(catalog_header));
  pack.strings = (const uint8_t *)(pack.entries + pack.entry_count);
  pack.chunks =
      (const VkrPackChunk *)(pack.mapping.data + header.chunk_table_offset);
  vkr_vfs.packs[vkr_vfs.pack_count++] = pack;
  return true_v;
}

void vkr_vfs_unmount_all(void) {
  for (uint32_t i = 0u; i < vkr_vfs.pack_count; ++i) {
    file_unmap(&vkr_vfs.packs[i].mapping);
  }
  vkr_vfs.pack_count = 0u;
  free(vkr_vfs.bundle_description);
  vkr_vfs.bundle_description = NULL;
  vkr_vfs.bundle_description_size = 0u;
  if (vkr_vfs.record) {
    fclose(vkr_vfs.record);
    vkr_vfs.record = NULL;
  }
}

/* Reads a whole bundle description into malloc storage the caller frees. */
vkr_internal uint8_t *vkr_vfs_read_description(const char *path,
                                               uint64_t *out_size) {
  FILE *file = file_fopen(path, "rb");
  if (!file) {
    return NULL;
  }
  uint8_t *bytes = NULL;
  long size = -1;
  if (fseek(file, 0, SEEK_END) == 0 && (size = ftell(file)) > 0 &&
      size <= (long)MB(64) && fseek(file, 0, SEEK_SET) == 0) {
    bytes = malloc((size_t)size);
    if (bytes && fread(bytes, 1u, (size_t)size, file) != (size_t)size) {
      free(bytes);
      bytes = NULL;
    }
  }
  fclose(file);
  *out_size = bytes ? (uint64_t)size : 0u;
  return bytes;
}

/* `bundle.json` in `directory`: its scene becomes the default scene, its
 * `content/` directory the content root, and its archives mount in order.
 * Only root members count; version 2 nests scene documents in `game`. */
vkr_internal bool8_t vkr_vfs_mount_bundle(const char *directory) {
  char path[VKR_VFS_PATH_MAX];
  snprintf(path, sizeof(path), "%s/bundle.json", directory);
  uint64_t size = 0u;
  uint8_t *bytes = vkr_vfs_read_description(path, &size);
  if (!bytes) {
    vkr_vfs_report("Cannot read bundle description %s", path);
    return false_v;
  }
  char root[VKR_VFS_PATH_MAX];
  snprintf(root, sizeof(root), "%s/content/", directory);
  bool8_t ok = vkr_vfs_set_content_root(root);
  VkrJsonReader reader = vkr_json_reader_create(bytes, size);
  String8 scene = {0};
  if (ok && vkr_json_find_root_field(&reader, "scene") &&
      vkr_json_parse_string(&reader, &scene) &&
      scene.length < sizeof(vkr_vfs.bundle_scene)) {
    MemCopy(vkr_vfs.bundle_scene, scene.str, scene.length);
    vkr_vfs.bundle_scene[scene.length] = 0;
  }
  vkr_json_reader_reset(&reader);
  if (ok && (!vkr_json_find_root_field(&reader, "packs") ||
             reader.pos >= reader.length || reader.data[reader.pos] != '[')) {
    vkr_vfs_report("Bundle description %s lists no archives", path);
    ok = false_v;
  }
  if (ok) {
    ++reader.pos; /* Past '['. */
  }
  while (ok) {
    vkr_json_skip_whitespace(&reader);
    String8 pack = {0};
    if (!vkr_json_parse_string(&reader, &pack)) {
      break;
    }
    char pack_path[VKR_VFS_PATH_MAX];
    snprintf(pack_path, sizeof(pack_path), "%s/%.*s", directory,
             (int)pack.length, pack.str);
    ok = vkr_vfs_mount_pack(pack_path);
    vkr_json_skip_whitespace(&reader);
    if (reader.pos >= reader.length || reader.data[reader.pos] != ',') {
      break;
    }
    ++reader.pos;
  }
  if (ok) {
    vkr_vfs.bundle_description = bytes;
    vkr_vfs.bundle_description_size = size;
  } else {
    free(bytes);
  }
  return ok && vkr_vfs.pack_count > 0u;
}

bool8_t vkr_vfs_mount_startup(void) {
  vkr_vfs_default_root();
  const char *record = getenv("VKR_VFS_RECORD");
  if (record && record[0] && !vkr_vfs.record) {
    vkr_vfs.record = file_fopen(record, "ab");
    if (vkr_vfs.record) {
      (void)setvbuf(vkr_vfs.record, NULL, _IOLBF, 0);
    } else {
      vkr_vfs_report("Cannot record content reads to %s", record);
    }
  }
  if (vkr_vfs.pack_count) {
    return true_v;
  }

  const char *bundle = getenv("VKR_CONTENT");
  if (bundle && bundle[0]) {
    return vkr_vfs_mount_bundle(bundle);
  }
  char executable[VKR_VFS_PATH_MAX];
  if (vkr_platform_executable_path(executable, sizeof(executable))) {
    char *separator = NULL;
    for (char *c = executable; *c; ++c) {
      if (vkr_vfs_is_separator((uint8_t)*c)) {
        separator = c;
      }
    }
    if (separator) {
      *separator = 0;
      /* Beside the executable, or in a macOS application bundle's
         Contents/Resources when it runs from Contents/MacOS. */
      char directories[2][VKR_VFS_PATH_MAX];
      uint32_t directory_count = 0u;
      snprintf(directories[directory_count++], VKR_VFS_PATH_MAX, "%s",
               executable);
      const uint64_t length = strlen(executable);
      const char suffix[] = "/Contents/MacOS";
      if (length > sizeof(suffix) - 1u &&
          !strcmp(executable + length - (sizeof(suffix) - 1u), suffix)) {
        snprintf(directories[directory_count++], VKR_VFS_PATH_MAX,
                 "%.*s/Contents/Resources",
                 (int)(length - (sizeof(suffix) - 1u)), executable);
      }
      for (uint32_t i = 0u; i < directory_count; ++i) {
        char description[VKR_VFS_PATH_MAX];
        snprintf(description, sizeof(description), "%s/bundle.json",
                 directories[i]);
        const FilePath file = {
            .path = string8_create_from_cstr((const uint8_t *)description,
                                             strlen(description)),
            .type = FILE_PATH_TYPE_ABSOLUTE};
        if (file_exists(&file)) {
          return vkr_vfs_mount_bundle(directories[i]);
        }
      }
    }
  }

  const char *packs = getenv("VKR_CONTENT_PACKS");
  if (!packs || !packs[0]) {
    return true_v;
  }
#if defined(PLATFORM_WINDOWS)
  const char list_separator = ';';
#else
  const char list_separator = ':';
#endif
  const char *start = packs;
  for (const char *c = packs;; ++c) {
    if (*c != list_separator && *c != 0) {
      continue;
    }
    if (c > start) {
      char pack_path[VKR_VFS_PATH_MAX];
      snprintf(pack_path, sizeof(pack_path), "%.*s", (int)(c - start), start);
      if (!vkr_vfs_mount_pack(pack_path)) {
        return false_v;
      }
    }
    if (*c == 0) {
      break;
    }
    start = c + 1;
  }
  return true_v;
}

// =============================================================================
// Filesystem seam: archive-backed handles
// =============================================================================

bool8_t fs_vfs_open(const FilePath *path, FileMode mode,
                    FileHandle *out_handle) {
  if (bitset8_is_set(&mode, FILE_MODE_WRITE) ||
      bitset8_is_set(&mode, FILE_MODE_APPEND) ||
      bitset8_is_set(&mode, FILE_MODE_CREATE) ||
      bitset8_is_set(&mode, FILE_MODE_TRUNCATE)) {
    return false_v;
  }
  VkrVfsView view = {0};
  if (!vkr_vfs_find(path->path, &view)) {
    return false_v;
  }
  *out_handle = (FileHandle){
      /* Non-null so every validity check treats the handle as open. */
      .handle = (void *)view.data,
      .path = path,
      .mode = mode,
      .memory = view.data,
      .memory_size = view.size,
      .memory_position = 0u,
  };
  return true_v;
}

bool8_t fs_vfs_stats(const FilePath *path, FileStats *out_stats) {
  VkrVfsView view = {0};
  if (!path || !vkr_vfs_find(path->path, &view)) {
    return false_v;
  }
  if (out_stats) {
    out_stats->size = view.size;
    out_stats->last_modified = view.last_modified;
  }
  return true_v;
}

FILE *fs_vfs_fopen(const char *path, const char *mode, bool8_t *out_served) {
  *out_served = false_v;
  if (!mode || mode[0] != 'r' || strchr(mode, '+')) {
    return NULL;
  }
  VkrVfsView view = {0};
  if (!vkr_vfs_find(
          string8_create_from_cstr((const uint8_t *)path, strlen(path)),
          &view)) {
    return NULL;
  }
  *out_served = true_v;
#if defined(PLATFORM_WINDOWS)
  /* No memory streams: a temporary file carries the bytes. */
  FILE *file = tmpfile();
  if (file && (fwrite(view.data, 1u, view.size, file) != view.size ||
               fseek(file, 0, SEEK_SET) != 0)) {
    fclose(file);
    file = NULL;
  }
  return file;
#else
  /* An empty entry still needs a readable stream. */
  static const uint8_t empty = 0u;
  return fmemopen(view.size ? (void *)view.data : (void *)&empty,
                  view.size ? view.size : 1u, "rb");
#endif
}

FileError fs_memory_read_into(FileHandle *handle, void *buffer, uint64_t size,
                              uint64_t *bytes_read) {
  if ((!buffer && size) || !bytes_read) {
    return FILE_ERROR_INVALID_HANDLE;
  }
  const uint64_t available = handle->memory_size - handle->memory_position;
  const uint64_t count = Min(size, available);
  MemCopy(buffer, handle->memory + handle->memory_position, count);
  handle->memory_position += count;
  *bytes_read = count;
  return FILE_ERROR_NONE;
}

FileError fs_memory_remaining(FileHandle *handle, uint64_t *out_size) {
  *out_size = handle->memory_size - handle->memory_position;
  return FILE_ERROR_NONE;
}

FileError fs_memory_read_line(FileHandle *handle, VkrAllocator *allocator,
                              VkrAllocator *line_allocator,
                              uint64_t max_line_length, String8 *out_line) {
  *out_line = (String8){0};
  if (max_line_length == 0u || max_line_length == UINT64_MAX) {
    return FILE_ERROR_LINE_TOO_LONG;
  }
  const uint64_t available = handle->memory_size - handle->memory_position;
  if (!available) {
    return FILE_ERROR_EOF;
  }
  const uint8_t *start = handle->memory + handle->memory_position;
  const uint8_t *newline = memchr(start, '\n', (size_t)available);
  uint64_t length = newline ? (uint64_t)(newline - start) + 1u : available;
  length = Min(length, max_line_length);
  VkrAllocator *target = line_allocator ? line_allocator : allocator;
  uint8_t *line = vkr_allocator_alloc(target, max_line_length + 1u,
                                      VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!line) {
    return FILE_ERROR_OUT_OF_MEMORY;
  }
  MemCopy(line, start, length);
  line[length] = 0;
  handle->memory_position += length;
  *out_line = (String8){.str = line, .length = length};
  return FILE_ERROR_NONE;
}
