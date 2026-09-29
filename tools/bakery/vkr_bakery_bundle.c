/* `vkr_bakery bundle` (ADR-077): packs a scene's content closure and a
 * recipe's runtime resources into one `.vkpak` archive, copies the runtime
 * executable and shader catalog beside it, and writes `bundle.json`:
 *
 *   <out>/<executable>
 *   <out>/shaders/<backend>/...
 *   <out>/content/<name>.vkpak
 *   <out>/bundle.json
 *   <out>/scripts/lib<module>.a, lib<module>.dylib   (C script modules)
 *
 * The runtime mounts that layout from its own directory (filesystem/vkr_vfs.h).
 * The closure follows what the loaders read: scene documents name meshes,
 * materials, textures and cubemap faces; a mesh names its materials; a
 * material or font configuration names its textures and atlases; an image is
 * read through its `.vkt` sibling. */

#include "vkr_bakery_bundle.h"

#include "core/vkr_hash.h"
#include "filesystem/filesystem.h"
#include "filesystem/vkr_vfs.h"
#include "vkr_bakery_json.h"
#include "vkr_bakery_os.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VKR_BUNDLE_COPY_BLOCK (4u * 1024u * 1024u)

// =============================================================================
// Identity set
// =============================================================================

vkr_internal uint64_t vkr_bundle_set_hash(const char *text) {
  uint64_t hash = 14695981039346656037ull;
  for (const char *c = text; *c; ++c) {
    hash = (hash ^ (uint8_t)*c) * 1099511628211ull;
  }
  return hash;
}

vkr_internal bool8_t vkr_bundle_set_grow(VkrBundleSet *set) {
  const uint32_t slot_capacity =
      set->slot_capacity ? set->slot_capacity * 2u : 1024u;
  uint32_t *slots = calloc(slot_capacity, sizeof(*slots));
  char **items = realloc(set->items, (slot_capacity / 2u) * sizeof(*items));
  if (!slots || !items) {
    free(slots);
    if (items) {
      set->items = items;
    }
    return false_v;
  }
  set->items = items;
  set->capacity = slot_capacity / 2u;
  for (uint32_t i = 0u; i < set->count; ++i) {
    uint64_t slot = vkr_bundle_set_hash(items[i]) & (slot_capacity - 1u);
    while (slots[slot]) {
      slot = (slot + 1u) & (slot_capacity - 1u);
    }
    slots[slot] = i + 1u;
  }
  free(set->slots);
  set->slots = slots;
  set->slot_capacity = slot_capacity;
  return true_v;
}

/* Returns 1 when added, 0 when present, -1 on allocation failure. */
vkr_internal int32_t vkr_bundle_set_add(VkrBundleSet *set,
                                        const char *identity) {
  if (set->count == set->capacity && !vkr_bundle_set_grow(set)) {
    return -1;
  }
  uint64_t slot = vkr_bundle_set_hash(identity) & (set->slot_capacity - 1u);
  while (set->slots[slot]) {
    if (strcmp(set->items[set->slots[slot] - 1u], identity) == 0) {
      return 0;
    }
    slot = (slot + 1u) & (set->slot_capacity - 1u);
  }
  char *copy = strdup(identity);
  if (!copy) {
    return -1;
  }
  set->items[set->count] = copy;
  set->slots[slot] = ++set->count;
  return 1;
}

vkr_internal void vkr_bundle_set_free(VkrBundleSet *set) {
  for (uint32_t i = 0u; i < set->count; ++i) {
    free(set->items[i]);
  }
  free(set->items);
  free(set->slots);
  *set = (VkrBundleSet){0};
}

// =============================================================================
// Closure
// =============================================================================

/* Host path below the first mount matching `identity` that holds a file,
   or a directory when `directory` is set. */
vkr_internal bool8_t vkr_bundle_resolve(const VkrBundle *bundle,
                                        const char *identity, char *out,
                                        uint32_t capacity, bool8_t directory) {
  for (uint32_t i = 0u; i < bundle->mount_count; ++i) {
    const VkrBundleMount *mount = &bundle->mounts[i];
    const uint64_t length = strlen(mount->prefix);
    if (strncmp(identity, mount->prefix, length) != 0 ||
        !vkr_bakery_path_join(out, capacity, mount->directory,
                              identity + length)) {
      continue;
    }
    if (directory ? vkr_bakery_is_directory(out) : vkr_bakery_is_file(out)) {
      return true_v;
    }
  }
  return false_v;
}

bool8_t vkr_bundle_source(const VkrBundle *bundle, const char *identity,
                          char *out, uint32_t capacity) {
  return vkr_bundle_resolve(bundle, identity, out, capacity, false_v);
}

vkr_internal bool8_t vkr_bundle_has_extension(const char *path,
                                              const char *extension) {
  const uint64_t length = strlen(path);
  const uint64_t suffix = strlen(extension);
  if (length < suffix) {
    return false_v;
  }
  for (uint64_t i = 0u; i < suffix; ++i) {
    char c = path[length - suffix + i];
    if (c >= 'A' && c <= 'Z') {
      c = (char)(c - 'A' + 'a');
    }
    if (c != extension[i]) {
      return false_v;
    }
  }
  return true_v;
}

vkr_internal bool8_t vkr_bundle_is_image(const char *path) {
  static const char *const images[] = {".png", ".jpg",  ".jpeg", ".tga",
                                       ".bmp", ".hdr",  ".exr",  ".ktx2",
                                       ".dds", ".webp", ".psd"};
  for (uint32_t i = 0u; i < ArrayCount(images); ++i) {
    if (vkr_bundle_has_extension(path, images[i])) {
      return true_v;
    }
  }
  return false_v;
}

/* Normalizes a reference into a root-relative identity: drops a `?query`,
 * resolves `.` and `..`, and rejects references outside the root. */
vkr_internal bool8_t vkr_bundle_identity(const char *reference, uint64_t length,
                                         char *out, uint32_t capacity) {
  uint32_t written = 0u;
  uint64_t start = 0u;
  for (uint64_t i = 0u; i < length; ++i) {
    if (reference[i] == '?') {
      length = i;
      break;
    }
  }
  if (!length || reference[0] == '/' || reference[0] == '\\' ||
      (length > 1u && reference[1] == ':')) {
    return false_v;
  }
  for (uint64_t i = 0u; i <= length; ++i) {
    if (i < length && reference[i] != '/' && reference[i] != '\\') {
      continue;
    }
    const uint64_t size = i - start;
    const char *segment = reference + start;
    start = i + 1u;
    if (!size || (size == 1u && segment[0] == '.')) {
      continue;
    }
    if (size == 2u && segment[0] == '.' && segment[1] == '.') {
      if (!written) {
        return false_v;
      }
      while (written && out[written - 1u] != '/') {
        --written;
      }
      if (written) {
        --written;
      }
      continue;
    }
    if (written + size + 2u > capacity) {
      return false_v;
    }
    if (written) {
      out[written++] = '/';
    }
    MemCopy(out + written, segment, size);
    written += (uint32_t)size;
  }
  out[written] = 0;
  return written > 0u;
}

/* Adds an existing root-relative file. A material or font texture resolves
 * to its `.vkt` sibling when one exists, as the texture system reads it;
 * cubemap faces load their source images (`as_named`). */
vkr_internal bool8_t vkr_bundle_add_file(VkrBundle *bundle,
                                         const char *reference, uint64_t length,
                                         bool8_t required, bool8_t as_named) {
  char identity[VKR_PACK_IDENTITY_MAX];
  if (!vkr_bundle_identity(reference, length, identity,
                           sizeof(identity) - 8u)) {
    return false_v;
  }
  char path[VKR_BAKERY_PATH_CAPACITY];
  if (!as_named && vkr_bundle_is_image(identity)) {
    char cooked[VKR_PACK_IDENTITY_MAX];
    snprintf(cooked, sizeof(cooked), "%s.vkt", identity);
    if (vkr_bundle_source(bundle, cooked, path, sizeof(path))) {
      MemCopy(identity, cooked, strlen(cooked) + 1u);
    }
  }
  if (!vkr_bundle_source(bundle, identity, path, sizeof(path))) {
    if (required) {
      vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_IDX_MISSING_SOURCE, identity,
                            0u, 0u, "bundle input not found", NULL);
      ++bundle->missing;
    }
    return false_v;
  }
  return vkr_bundle_set_add(&bundle->files, identity) >= 0;
}

bool8_t vkr_bundle_add(VkrBundle *bundle, const char *reference,
                       uint64_t length, bool8_t required) {
  return vkr_bundle_add_file(bundle, reference, length, required, false_v);
}

/* A string that names an existing file relative to the root or to the
 * document that holds it. As the runtime resolves them, `./` and `../`
 * references are relative to the owner only. A required reference that
 * names no file is reported against its owner. */
vkr_internal bool8_t vkr_bundle_reference(VkrBundle *bundle, const char *owner,
                                          const char *text, uint64_t length,
                                          bool8_t required) {
  if (!length || length >= VKR_PACK_IDENTITY_MAX ||
      memchr(text, '\n', length)) {
    return false_v;
  }
  const bool8_t owner_relative =
      (length > 1u && text[0] == '.' && text[1] == '/') ||
      (length > 2u && text[0] == '.' && text[1] == '.' && text[2] == '/');
  if (!owner_relative && vkr_bundle_add(bundle, text, length, false_v)) {
    return true_v;
  }
  char relative[VKR_PACK_IDENTITY_MAX];
  char directory[VKR_PACK_IDENTITY_MAX];
  vkr_bakery_path_parent(directory, sizeof(directory), owner);
  if (directory[0] &&
      (uint32_t)snprintf(relative, sizeof(relative), "%s/%.*s", directory,
                         (int)length, text) < sizeof(relative) &&
      vkr_bundle_add(bundle, relative, strlen(relative), false_v)) {
    return true_v;
  }
  if (required) {
    char message[VKR_PACK_IDENTITY_MAX + 32u];
    snprintf(message, sizeof(message), "names a missing file: %.*s",
             (int)length, text);
    vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_IDX_MISSING_SOURCE, owner, 0u, 0u,
                          message, NULL);
    ++bundle->missing;
  }
  return false_v;
}

vkr_internal void vkr_bundle_walk_json(VkrBundle *bundle, const char *owner,
                                       const VkrBakeryJson *value) {
  if (!value) {
    return;
  }
  if (value->type == VKR_BAKERY_JSON_STRING) {
    (void)vkr_bundle_reference(bundle, owner, (const char *)value->string.str,
                               value->string.length, false_v);
    return;
  }
  if (value->type == VKR_BAKERY_JSON_OBJECT) {
    /* Cubemap faces are named by a base path and an extension. */
    String8 base = {0};
    String8 extension = {0};
    if (vkr_bakery_json_get_string(value, "base_path", &base) &&
        vkr_bakery_json_get_string(value, "extension", &extension)) {
      static const char *const faces[] = {"_r", "_l", "_u", "_d", "_f", "_b"};
      for (uint32_t i = 0u; i < ArrayCount(faces); ++i) {
        char face[VKR_PACK_IDENTITY_MAX];
        snprintf(face, sizeof(face), "%.*s%s.%.*s", (int)base.length, base.str,
                 faces[i], (int)extension.length, extension.str);
        (void)vkr_bundle_add_file(bundle, face, strlen(face), true_v, true_v);
      }
    }
  }
  for (const VkrBakeryJson *child = value->first; child; child = child->next) {
    vkr_bundle_walk_json(bundle, owner, child);
  }
}

/* `key=value` documents (materials, font configurations): every value that
 * names a file. */
vkr_internal void vkr_bundle_walk_pairs(VkrBundle *bundle, const char *owner,
                                        const uint8_t *data, uint64_t length) {
  uint64_t line = 0u;
  while (line < length) {
    uint64_t end = line;
    while (end < length && data[end] != '\n') {
      ++end;
    }
    const uint8_t *equals = memchr(data + line, '=', end - line);
    if (data[line] != '#' && equals) {
      const uint8_t *value = equals + 1;
      uint64_t size = (uint64_t)(data + end - value);
      while (size && (value[size - 1u] == '\r' || value[size - 1u] == ' ')) {
        --size;
      }
      (void)vkr_bundle_reference(bundle, owner, (const char *)value, size,
                                 false_v);
    }
    line = end + 1u;
  }
}

/* A mesh names its materials, which the mesh cooker reports, relative to the
 * root or, as managed bundles name them, to the mesh; an adjacent
 * `.remap.json` maps them (vkr_mesh_cooked.h). */
vkr_internal void vkr_bundle_walk_mesh(VkrBundle *bundle, const char *owner,
                                       const char *path) {
  char remap[VKR_PACK_IDENTITY_MAX];
  if ((uint32_t)snprintf(remap, sizeof(remap), "%s.remap.json", owner) <
      sizeof(remap)) {
    (void)vkr_bundle_add(bundle, remap, strlen(remap), false_v);
  }
  char report[VKR_BAKERY_PATH_CAPACITY];
  vkr_bakery_temp_path(report, sizeof(report), bundle->cli->config.cache_dir,
                       "bundle-mesh.json");
  (void)vkr_bakery_make_directories(bundle->cli->config.cache_dir);
  const char *arguments[] = {"--inspect", "--input", path, "--output", report};
  if (vkr_bakery_run_tool_inline("mesh", arguments, ArrayCount(arguments))) {
    vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_BUNDLE_FAILED, owner, 0u, 0u,
                          "cannot read the mesh's materials", NULL);
    ++bundle->missing;
    return;
  }
  uint8_t *data = NULL;
  uint64_t length = 0u;
  if (vkr_bakery_read_file(report, MB(64), &data, &length)) {
    const VkrBakeryJson *value =
        vkr_bakery_json_parse(bundle->arena, data, length, 16u, NULL);
    const VkrBakeryJson *materials = vkr_bakery_json_get(value, "materials");
    for (const VkrBakeryJson *material = materials ? materials->first : NULL;
         material; material = material->next) {
      if (material->type == VKR_BAKERY_JSON_STRING) {
        (void)vkr_bundle_reference(bundle, owner,
                                   (const char *)material->string.str,
                                   material->string.length, true_v);
      }
    }
    free(data);
  }
  (void)vkr_bakery_remove_file(report);
}

vkr_internal void vkr_bundle_expand(VkrBundle *bundle, const char *identity) {
  char path[VKR_BAKERY_PATH_CAPACITY];
  if (!vkr_bundle_source(bundle, identity, path, sizeof(path))) {
    return;
  }
  if (vkr_bundle_has_extension(identity, ".vkb")) {
    vkr_bundle_walk_mesh(bundle, identity, path);
    return;
  }
  const bool8_t json = vkr_bundle_has_extension(identity, ".json");
  const bool8_t pairs = vkr_bundle_has_extension(identity, ".mt") ||
                        vkr_bundle_has_extension(identity, ".fontcfg");
  if (!json && !pairs) {
    return;
  }
  uint8_t *data = NULL;
  uint64_t length = 0u;
  if (!vkr_bakery_read_file(path, MB(256), &data, &length)) {
    return;
  }
  if (json) {
    vkr_bundle_walk_json(
        bundle, identity,
        vkr_bakery_json_parse(bundle->arena, data, length, 256u, NULL));
  } else {
    vkr_bundle_walk_pairs(bundle, identity, data, length);
  }
  free(data);
}

typedef struct VkrBundleDirectory {
  VkrBundle *bundle;
  const char *identity;
} VkrBundleDirectory;

vkr_internal bool8_t vkr_bundle_visit(void *context, const char *name,
                                      bool8_t is_directory);

void vkr_bundle_close(VkrBundle *bundle) {
  for (uint32_t i = 0u; i < bundle->files.count; ++i) {
    char identity[VKR_PACK_IDENTITY_MAX];
    snprintf(identity, sizeof(identity), "%s", bundle->files.items[i]);
    vkr_bundle_expand(bundle, identity);
  }
}

vkr_internal bool8_t vkr_bundle_hex_digest(const char *hex, uint8_t out[32]) {
  for (uint32_t i = 0u; i < 32u; ++i) {
    unsigned int byte = 0u;
    if (sscanf(hex + i * 2u, "%2x", &byte) != 1) {
      return false_v;
    }
    out[i] = (uint8_t)byte;
  }
  return true_v;
}

bool8_t vkr_bundle_hash(VkrBundle *bundle, VkrBundleItem **out_items,
                        uint64_t *out_bytes) {
  VkrBundleItem *items =
      calloc(bundle->files.count ? bundle->files.count : 1u, sizeof(*items));
  *out_items = items;
  *out_bytes = 0u;
  if (!items) {
    return false_v;
  }
  for (uint32_t i = 0u; i < bundle->files.count; ++i) {
    char path[VKR_BAKERY_PATH_CAPACITY];
    char hex[VKR_BAKERY_SHA256_HEX];
    items[i].identity = bundle->files.items[i];
    if (!vkr_bundle_source(bundle, items[i].identity, path, sizeof(path)) ||
        !vkr_bakery_hash_file(path, hex, &items[i].size) ||
        !vkr_bundle_hex_digest(hex, items[i].sha256)) {
      vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_IDX_UNREADABLE_SOURCE,
                            items[i].identity, 0u, 0u, "cannot read", NULL);
      return false_v;
    }
    *out_bytes += items[i].size;
  }
  return true_v;
}

void vkr_bundle_free(VkrBundle *bundle) { vkr_bundle_set_free(&bundle->files); }

void vkr_bundle_add_directory(VkrBundle *bundle, const char *identity) {
  char path[VKR_BAKERY_PATH_CAPACITY];
  if (!vkr_bundle_resolve(bundle, identity, path, sizeof(path), true_v)) {
    return;
  }
  VkrBundleDirectory directory = {.bundle = bundle, .identity = identity};
  (void)vkr_bakery_list_directory(path, vkr_bundle_visit, &directory);
}

vkr_internal bool8_t vkr_bundle_visit(void *context, const char *name,
                                      bool8_t is_directory) {
  VkrBundleDirectory *directory = context;
  char child[VKR_PACK_IDENTITY_MAX];
  if ((uint32_t)snprintf(child, sizeof(child), "%s/%s", directory->identity,
                         name) >= sizeof(child) ||
      name[0] == '.') {
    return true_v;
  }
  if (is_directory) {
    vkr_bundle_add_directory(directory->bundle, child);
  } else {
    (void)vkr_bundle_set_add(&directory->bundle->files, child);
  }
  return true_v;
}

// =============================================================================
// Archive writer
// =============================================================================

vkr_internal int vkr_bundle_compare_identity(const void *lhs, const void *rhs) {
  const VkrBundleItem *a = lhs;
  const VkrBundleItem *b = rhs;
  const uint64_t a_length = strlen(a->identity);
  const uint64_t b_length = strlen(b->identity);
  const int order = memcmp(a->identity, b->identity, Min(a_length, b_length));
  return order ? order : (a_length < b_length ? -1 : a_length > b_length);
}

vkr_internal int vkr_bundle_compare_chunk(const void *lhs, const void *rhs) {
  return memcmp(((const VkrPackChunk *)lhs)->sha256,
                ((const VkrPackChunk *)rhs)->sha256, 32u);
}

uint32_t vkr_bundle_loader(const char *identity) {
  static const struct {
    const char *extension;
    VkrPackLoader loader;
  } loaders[] = {
      {".scene.json", VKR_PACK_LOADER_SCENE},
      {".vkb", VKR_PACK_LOADER_MESH},
      {".vkt", VKR_PACK_LOADER_TEXTURE},
      {".mt", VKR_PACK_LOADER_MATERIAL},
      {".vkfa", VKR_PACK_LOADER_FONT},
      {".fontcfg", VKR_PACK_LOADER_FONT},
      {".vka", VKR_PACK_LOADER_ANIMATION},
      {".vkc", VKR_PACK_LOADER_COLLISION},
      {".vkdv", VKR_PACK_LOADER_VOLUME},
      {".json", VKR_PACK_LOADER_JSON},
  };
  for (uint32_t i = 0u; i < ArrayCount(loaders); ++i) {
    if (vkr_bundle_has_extension(identity, loaders[i].extension)) {
      return loaders[i].loader;
    }
  }
  return VKR_PACK_LOADER_RAW;
}

vkr_internal uint32_t vkr_bundle_alignment(const char *identity) {
  const uint32_t loader = vkr_bundle_loader(identity);
  return loader == VKR_PACK_LOADER_MESH || loader == VKR_PACK_LOADER_TEXTURE ||
                 loader == VKR_PACK_LOADER_VOLUME
             ? VKR_PACK_MAPPABLE_ALIGNMENT
             : VKR_PACK_ALIGNMENT;
}

vkr_internal bool8_t vkr_bundle_pad(FILE *file, uint64_t *offset,
                                    uint32_t alignment) {
  static const uint8_t zeros[VKR_PACK_MAPPABLE_ALIGNMENT] = {0};
  const uint64_t padding = (alignment - *offset % alignment) % alignment;
  if (padding && fwrite(zeros, 1u, padding, file) != padding) {
    return false_v;
  }
  *offset += padding;
  return true_v;
}

vkr_internal bool8_t vkr_bundle_copy(FILE *file, const char *path,
                                     uint64_t size, uint8_t *block) {
  FILE *source = file_fopen(path, "rb");
  if (!source) {
    return false_v;
  }
  uint64_t copied = 0u;
  while (copied < size) {
    const size_t count = fread(
        block, 1u, (size_t)Min(size - copied, VKR_BUNDLE_COPY_BLOCK), source);
    if (!count || fwrite(block, 1u, count, file) != count) {
      break;
    }
    copied += count;
  }
  fclose(source);
  return copied == size;
}

/* Writes the archive to `path` through a staging file. Chunks follow in
 * identity order; the catalog and chunk table follow them, and the header,
 * written last, points at both. */
void vkr_bundle_sort(VkrBundleItem *items, uint32_t item_count) {
  qsort(items, item_count, sizeof(*items), vkr_bundle_compare_identity);
}

bool8_t vkr_bundle_write_pack(VkrBundle *bundle, const char *path,
                              VkrBundleItem *items, uint32_t item_count,
                              uint32_t *out_chunks, uint64_t *out_bytes) {
  qsort(items, item_count, sizeof(*items), vkr_bundle_compare_identity);
  VkrPackChunk *chunks = calloc(item_count ? item_count : 1u, sizeof(*chunks));
  uint8_t *block = malloc(VKR_BUNDLE_COPY_BLOCK);
  char staging[VKR_BAKERY_PATH_CAPACITY];
  snprintf(staging, sizeof(staging), "%s.tmp", path);
  FILE *file = file_fopen(staging, "wb");
  bool8_t ok = chunks && block && file;
  uint32_t chunk_count = 0u;
  uint64_t offset = 0u;

  static const uint8_t header_space[VKR_PACK_HEADER_SIZE] = {0};
  ok = ok && fwrite(header_space, 1u, sizeof(header_space), file) ==
                 sizeof(header_space);
  offset = sizeof(header_space);
  for (uint32_t i = 0u; ok && i < item_count; ++i) {
    VkrBundleItem *item = &items[i];
    uint32_t existing = UINT32_MAX;
    for (uint32_t c = 0u; c < chunk_count && existing == UINT32_MAX; ++c) {
      if (MemCompare(chunks[c].sha256, item->sha256, 32u) == 0) {
        existing = c;
      }
    }
    if (existing != UINT32_MAX) {
      item->chunk = existing;
      continue;
    }
    const uint32_t alignment = vkr_bundle_alignment(item->identity);
    char source[VKR_BAKERY_PATH_CAPACITY];
    ok = vkr_bundle_pad(file, &offset, alignment) &&
         vkr_bundle_source(bundle, item->identity, source, sizeof(source)) &&
         vkr_bundle_copy(file, source, item->size, block);
    chunks[chunk_count] = (VkrPackChunk){
        .offset = offset, .size = item->size, .alignment = alignment};
    MemCopy(chunks[chunk_count].sha256, item->sha256, 32u);
    item->chunk = chunk_count++;
    offset += item->size;
  }

  /* The table is sorted by hash; entries follow their chunks' new places. */
  uint32_t *order = malloc((chunk_count ? chunk_count : 1u) * sizeof(*order));
  VkrPackChunk *sorted =
      malloc((chunk_count ? chunk_count : 1u) * sizeof(*sorted));
  ok = ok && order && sorted;
  if (ok) {
    MemCopy(sorted, chunks, chunk_count * sizeof(*sorted));
    qsort(sorted, chunk_count, sizeof(*sorted), vkr_bundle_compare_chunk);
    for (uint32_t c = 0u; c < chunk_count; ++c) {
      for (uint32_t s = 0u; s < chunk_count; ++s) {
        if (MemCompare(sorted[s].sha256, chunks[c].sha256, 32u) == 0) {
          order[c] = s;
          break;
        }
      }
    }
  }

  /* Catalog: header, entries, identity strings. */
  uint64_t strings_size = 0u;
  for (uint32_t i = 0u; i < item_count; ++i) {
    strings_size += strlen(items[i].identity);
  }
  const uint64_t catalog_size = sizeof(VkrPackCatalogHeader) +
                                (uint64_t)item_count * sizeof(VkrPackEntry) +
                                strings_size;
  uint8_t *catalog = malloc(catalog_size);
  ok = ok && catalog && strings_size < UINT32_MAX;
  if (ok) {
    const VkrPackCatalogHeader header = {.entry_count = item_count};
    MemCopy(catalog, &header, sizeof(header));
    VkrPackEntry *entries = (VkrPackEntry *)(catalog + sizeof(header));
    uint8_t *strings = (uint8_t *)(entries + item_count);
    uint32_t cursor = 0u;
    for (uint32_t i = 0u; i < item_count; ++i) {
      const uint32_t length = (uint32_t)strlen(items[i].identity);
      entries[i] = (VkrPackEntry){
          .identity_offset = cursor,
          .identity_length = length,
          .chunk = order[items[i].chunk],
          .loader = vkr_bundle_loader(items[i].identity),
      };
      MemCopy(strings + cursor, items[i].identity, length);
      cursor += length;
    }
  }

  VkrPackHeader header = {.version = VKR_PACK_VERSION};
  MemCopy(header.magic, VKR_PACK_MAGIC, 4u);
  ok = ok && vkr_bundle_pad(file, &offset, 8u);
  header.catalog_offset = offset;
  header.catalog_size = catalog_size;
  ok = ok && fwrite(catalog, 1u, catalog_size, file) == catalog_size;
  offset += catalog_size;
  ok = ok && vkr_bundle_pad(file, &offset, 8u);
  header.chunk_table_offset = offset;
  header.chunk_table_size = (uint64_t)chunk_count * sizeof(VkrPackChunk);
  ok = ok && fwrite(sorted, sizeof(*sorted), chunk_count, file) == chunk_count;
  offset += header.chunk_table_size;
  header.total_size = offset;
  if (ok) {
    VkrSha256 hash;
    vkr_sha256_init(&hash);
    vkr_sha256_update(&hash, catalog, catalog_size);
    vkr_sha256_update(&hash, sorted, header.chunk_table_size);
    vkr_sha256_final(&hash, header.index_sha256);
  }
  ok = ok && fseek(file, 0, SEEK_SET) == 0 &&
       fwrite(&header, sizeof(header), 1u, file) == 1u;
  if (file && fclose(file) != 0) {
    ok = false_v;
  }
  ok = ok && vkr_bakery_rename(staging, path, true_v);
  if (!ok) {
    (void)vkr_bakery_remove_file(staging);
  }
  free(catalog);
  free(sorted);
  free(order);
  free(block);
  free(chunks);
  *out_chunks = chunk_count;
  *out_bytes = offset;
  return ok;
}

/* Reads the archive back through the runtime's validator and rehashes every
 * chunk, so a writer defect cannot ship. */
bool8_t vkr_bundle_verify(const char *path) {
  const FilePath file = {
      .path = string8_create_from_cstr((const uint8_t *)path, strlen(path)),
      .type = FILE_PATH_TYPE_ABSOLUTE};
  FileMapping mapping = {0};
  if (file_map_readonly(&file, &mapping) != FILE_ERROR_NONE) {
    return false_v;
  }
  char error[128];
  bool8_t ok =
      vkr_pack_validate(mapping.data, mapping.size, error, sizeof(error));
  if (!ok) {
    vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_BUNDLE_FAILED, path, 0u, 0u,
                          error, NULL);
  }
  VkrPackHeader header;
  MemCopy(&header, mapping.data, sizeof(header));
  const VkrPackChunk *chunks =
      (const VkrPackChunk *)(mapping.data + header.chunk_table_offset);
  const uint64_t count = header.chunk_table_size / sizeof(VkrPackChunk);
  for (uint64_t i = 0u; ok && i < count; ++i) {
    uint8_t digest[VKR_SHA256_DIGEST_SIZE];
    vkr_sha256(mapping.data + chunks[i].offset, chunks[i].size, digest);
    ok = MemCompare(digest, chunks[i].sha256, sizeof(digest)) == 0;
  }
  file_unmap(&mapping);
  return ok;
}

// =============================================================================
// Command
// =============================================================================

typedef struct VkrBundleCopy {
  const char *source;
  const char *destination;
  bool8_t ok;
} VkrBundleCopy;

vkr_internal bool8_t vkr_bundle_copy_visit(void *context, const char *name,
                                           bool8_t is_directory) {
  VkrBundleCopy *copy = context;
  char source[VKR_BAKERY_PATH_CAPACITY];
  char destination[VKR_BAKERY_PATH_CAPACITY];
  if (!vkr_bakery_path_join(source, sizeof(source), copy->source, name) ||
      !vkr_bakery_path_join(destination, sizeof(destination), copy->destination,
                            name)) {
    copy->ok = false_v;
    return false_v;
  }
  if (is_directory) {
    VkrBundleCopy child = {.source = source,
                           .destination = destination,
                           .ok = vkr_bakery_make_directories(destination)};
    child.ok = child.ok &&
               vkr_bakery_list_directory(source, vkr_bundle_copy_visit, &child);
    copy->ok = copy->ok && child.ok;
  } else {
    copy->ok = copy->ok && vkr_bakery_clone_or_copy(source, destination);
  }
  return copy->ok;
}

bool8_t vkr_bundle_copy_tree(const char *source, const char *destination) {
  VkrBundleCopy copy = {
      .source = source, .destination = destination, .ok = true_v};
  return vkr_bakery_make_directories(destination) &&
         vkr_bakery_list_directory(source, vkr_bundle_copy_visit, &copy) &&
         copy.ok;
}

int vkr_bakery_cmd_bundle(VkrBakeryCli *cli) {
  if (cli->positional_count != 1u) {
    return vkr_bakery_usage(
        "bundle needs one recipe or one managed project directory");
  }
  char manifest[VKR_BAKERY_PATH_CAPACITY];
  if (vkr_bakery_is_directory(cli->positional[0]) &&
      vkr_bakery_path_join(manifest, sizeof(manifest), cli->positional[0],
                           "project.json") &&
      vkr_bakery_is_file(manifest)) {
    return vkr_bakery_bundle_project(cli, cli->positional[0]);
  }
  if (!cli->out) {
    return vkr_bakery_usage("bundle needs one recipe and --out <dir>");
  }
  Arena *arena = (Arena *)cli->allocator.ctx;
  VkrBundle bundle = {.cli = cli,
                      .arena = arena,
                      .mounts = {{.prefix = "", .directory = cli->config.root}},
                      .mount_count = 1u};
  int code = VKR_BAKERY_EXIT_FAILED;
  VkrBundleItem *items = NULL;

  uint8_t *data = NULL;
  uint64_t length = 0u;
  VkrBakeryJsonError error = {0};
  const VkrBakeryJson *recipe = NULL;
  if (!vkr_bakery_read_file(cli->positional[0], MB(4), &data, &length) ||
      !(recipe = vkr_bakery_json_parse(arena, data, length, 32u, &error))) {
    vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_REC_UNREADABLE,
                          cli->positional[0], error.line, error.column,
                          error.message[0] ? error.message
                                           : "cannot read the bundle recipe",
                          NULL);
    free(data);
    return VKR_BAKERY_EXIT_USAGE;
  }
  free(data);
  String8 name = {0};
  String8 scene = {0};
  int64_t version = 0;
  if (!vkr_bakery_json_get_int(recipe, "version", &version) || version != 1 ||
      !vkr_bakery_json_get_string(recipe, "name", &name) || !name.length ||
      !vkr_bakery_json_get_string(recipe, "scene", &scene)) {
    vkr_bakery_event_diag(
        0u, VKR_BAKERY_DIAG_REC_INVALID_VALUE, cli->positional[0], 0u, 0u,
        "a bundle recipe needs version 1, a name and a scene", NULL);
    return VKR_BAKERY_EXIT_USAGE;
  }
  const char *scene_identity =
      vkr_bakery_json_cstr_value(arena, vkr_bakery_json_get(recipe, "scene"));

  /* The closure: the scene, then the recipe's runtime resources, expanded
   * until no document names a new file. */
  if (!vkr_bundle_add(&bundle, scene_identity, strlen(scene_identity),
                      true_v)) {
    goto cleanup;
  }
  const VkrBakeryJson *include = vkr_bakery_json_get(recipe, "include");
  for (const VkrBakeryJson *entry = include ? include->first : NULL; entry;
       entry = entry->next) {
    const char *identity = vkr_bakery_json_cstr_value(arena, entry);
    char path[VKR_BAKERY_PATH_CAPACITY];
    if (!identity ||
        !vkr_bakery_path_join(path, sizeof(path), cli->config.root, identity)) {
      continue;
    }
    if (vkr_bakery_is_directory(path)) {
      vkr_bundle_add_directory(&bundle, identity);
    } else {
      (void)vkr_bundle_add(&bundle, identity, strlen(identity), true_v);
    }
  }
  vkr_bundle_close(&bundle);
  if (bundle.missing) {
    vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_BUNDLE_FAILED, cli->positional[0],
                          0u, 0u, "bundle inputs are missing", NULL);
    goto cleanup;
  }

  /* Hash every file; identical bytes become one chunk. */
  uint64_t input_bytes = 0u;
  if (!vkr_bundle_hash(&bundle, &items, &input_bytes)) {
    goto cleanup;
  }
  vkr_bakery_print("bundle %.*s: %u files, %.1f MiB\n", (int)name.length,
                   name.str, bundle.files.count,
                   (float64_t)input_bytes / (1024.0 * 1024.0));
  if (cli->config.dry_run) {
    for (uint32_t i = 0u; i < bundle.files.count; ++i) {
      vkr_bakery_print("  %s\n", bundle.files.items[i]);
    }
    code = VKR_BAKERY_EXIT_OK;
    goto cleanup;
  }

  char content[VKR_BAKERY_PATH_CAPACITY];
  char pack[VKR_BAKERY_PATH_CAPACITY];
  char pack_name[256];
  snprintf(pack_name, sizeof(pack_name), "%.*s.vkpak", (int)name.length,
           name.str);
  if (!vkr_bakery_path_join(content, sizeof(content), cli->out, "content") ||
      !vkr_bakery_make_directories(content) ||
      !vkr_bakery_path_join(pack, sizeof(pack), content, pack_name)) {
    goto cleanup;
  }
  uint32_t chunk_count = 0u;
  uint64_t pack_bytes = 0u;
  if (!vkr_bundle_write_pack(&bundle, pack, items, bundle.files.count,
                             &chunk_count, &pack_bytes) ||
      !vkr_bundle_verify(pack)) {
    vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_BUNDLE_FAILED, pack, 0u, 0u,
                          "cannot write or verify the archive", NULL);
    goto cleanup;
  }
  vkr_bakery_print("wrote %s: %u entries, %u chunks, %.1f MiB\n", pack,
                   bundle.files.count, chunk_count,
                   (float64_t)pack_bytes / (1024.0 * 1024.0));

  /* Script modules: their static archives ship for the link step the entity
   * behavior runtime will own; the objects come from the action cache. */
  VkrBakeryJson *scripts = vkr_bakery_json_array(arena);
  const VkrBakeryJson *modules = vkr_bakery_json_get(recipe, "scripts");
  if (modules && modules->count) {
    char directory[VKR_BAKERY_PATH_CAPACITY];
    VkrBakeryGraph graph;
    if (!vkr_bakery_path_join(directory, sizeof(directory), cli->out,
                              "scripts") ||
        !vkr_bakery_graph_init(&graph, &cli->config)) {
      goto cleanup;
    }
    for (const VkrBakeryJson *module = modules->first; module;
         module = module->next) {
      const char *identity = vkr_bakery_json_cstr_value(arena, module);
      char path[VKR_BAKERY_PATH_CAPACITY];
      if (!identity || !vkr_bakery_path_join(path, sizeof(path),
                                             cli->config.root, identity)) {
        continue;
      }
      VkrBakeryAction *library =
          vkr_bakery_plan_script(&graph, path, directory);
      if (library) {
        graph.root_count += 1u;
        for (uint32_t i = 0u; i < library->output_count; ++i) {
          if (strcmp(library->outputs[i].role, "archive") == 0) {
            char relative[VKR_BAKERY_PATH_CAPACITY];
            (void)snprintf(relative, sizeof(relative), "scripts/%s",
                           vkr_bakery_path_name(library->outputs[i].path));
            vkr_bakery_json_append(scripts,
                                   vkr_bakery_json_cstr(arena, relative));
          }
        }
      }
    }
    const bool8_t built = !graph.plan_failed &&
                          vkr_bakery_make_directories(directory) &&
                          vkr_bakery_graph_execute(&graph);
    vkr_bakery_graph_shutdown(&graph);
    if (!built) {
      vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_BUNDLE_FAILED,
                            cli->positional[0], 0u, 0u,
                            "a script module did not build", NULL);
      goto cleanup;
    }
  }

  /* The runtime and the shader catalog it resolves beside itself. */
  const char *executable_name = "";
  if (cli->app) {
    executable_name = vkr_bakery_path_name(cli->app);
    char destination[VKR_BAKERY_PATH_CAPACITY];
    if (!vkr_bakery_path_join(destination, sizeof(destination), cli->out,
                              executable_name)) {
      goto cleanup;
    }
    if (!vkr_bakery_clone_or_copy(cli->app, destination)) {
      vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_BUNDLE_MISSING_RUNTIME,
                            cli->app, 0u, 0u, "cannot copy the runtime", NULL);
      goto cleanup;
    }
  }
  if (cli->shaders) {
    char destination[VKR_BAKERY_PATH_CAPACITY];
    if (!vkr_bakery_path_join(destination, sizeof(destination), cli->out,
                              "shaders")) {
      goto cleanup;
    }
    if (!vkr_bundle_copy_tree(cli->shaders, destination)) {
      vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_BUNDLE_MISSING_RUNTIME,
                            cli->shaders, 0u, 0u,
                            "cannot copy the shader catalog", NULL);
      goto cleanup;
    }
  }

  /* bundle.json: what the runtime mounts and what produced it. */
  VkrBakeryJson *description = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, description, "version",
                      vkr_bakery_json_int(arena, 1));
  vkr_bakery_json_set(arena, description, "name",
                      vkr_bakery_json_string(arena, name));
  vkr_bakery_json_set(arena, description, "scene",
                      vkr_bakery_json_cstr(arena, scene_identity));
  vkr_bakery_json_set(arena, description, "platform",
                      vkr_bakery_json_cstr(arena, cli->config.platform));
  vkr_bakery_json_set(arena, description, "config",
                      vkr_bakery_json_cstr(arena, cli->build_config
                                                      ? cli->build_config
                                                      : "release"));
  vkr_bakery_json_set(arena, description, "executable",
                      vkr_bakery_json_cstr(arena, executable_name));
  VkrBakeryJson *packs = vkr_bakery_json_array(arena);
  char pack_reference[300];
  snprintf(pack_reference, sizeof(pack_reference), "content/%s", pack_name);
  vkr_bakery_json_append(packs, vkr_bakery_json_cstr(arena, pack_reference));
  vkr_bakery_json_set(arena, description, "packs", packs);
  VkrBakeryJson *products = vkr_bakery_json_object(arena);
  for (uint32_t i = 0u; i < bundle.files.count; ++i) {
    char hex[VKR_BAKERY_SHA256_HEX];
    for (uint32_t b = 0u; b < 32u; ++b) {
      snprintf(hex + b * 2u, 3u, "%02x", items[i].sha256[b]);
    }
    vkr_bakery_json_set(arena, products, items[i].identity,
                        vkr_bakery_json_cstr(arena, hex));
  }
  vkr_bakery_json_set(arena, description, "products", products);
  vkr_bakery_json_set(arena, description, "scripts", scripts);
  String8 text = {0};
  char description_path[VKR_BAKERY_PATH_CAPACITY];
  if (!vkr_bakery_json_write(arena, description, VKR_BAKERY_JSON_PRETTY,
                             &text) ||
      !vkr_bakery_path_join(description_path, sizeof(description_path),
                            cli->out, "bundle.json") ||
      !vkr_bakery_write_file_atomic(description_path, text.str, text.length)) {
    goto cleanup;
  }
  code = VKR_BAKERY_EXIT_OK;

cleanup:
  free(items);
  vkr_bundle_free(&bundle);
  return code;
}
