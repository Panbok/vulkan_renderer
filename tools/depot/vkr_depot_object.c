#include "vkr_depot_object.h"

#include "core/vkr_byte_io.h"
#include "core/vkr_hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t depot_magic[4] = {'V', 'K', 'D', '1'};

bool8_t vkr_depot_id_equal(const VkrDepotId *a, const VkrDepotId *b) {
  return memcmp(a->bytes, b->bytes, VKR_DEPOT_ID_SIZE) == 0;
}

bool8_t vkr_depot_id_zero(const VkrDepotId *id) {
  for (uint32_t i = 0u; i < VKR_DEPOT_ID_SIZE; ++i) {
    if (id->bytes[i]) {
      return false_v;
    }
  }
  return true_v;
}

void vkr_depot_id_of(const void *bytes, uint64_t size, VkrDepotId *out_id) {
  vkr_sha256(bytes, size, out_id->bytes);
}

void vkr_depot_id_hex(const VkrDepotId *id, char out[VKR_DEPOT_ID_HEX]) {
  vkr_sha256_hex(id->bytes, out);
}

bool8_t vkr_depot_id_parse(const char *hex, VkrDepotId *out_id) {
  if (!hex || strlen(hex) != VKR_DEPOT_ID_SIZE * 2u) {
    return false_v;
  }
  for (uint32_t i = 0u; i < VKR_DEPOT_ID_SIZE; ++i) {
    uint32_t value = 0u;
    for (uint32_t k = 0u; k < 2u; ++k) {
      const char c = hex[2u * i + k];
      uint32_t digit = 0u;
      if (c >= '0' && c <= '9') {
        digit = (uint32_t)(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        digit = (uint32_t)(c - 'a' + 10);
      } else {
        return false_v;
      }
      value = value * 16u + digit;
    }
    out_id->bytes[i] = (uint8_t)value;
  }
  return true_v;
}

uint32_t vkr_depot_object_kind(const uint8_t *bytes, uint64_t size) {
  if (size < VKR_DEPOT_HEADER_SIZE || memcmp(bytes, depot_magic, 4u) != 0 ||
      bytes[5] != 0u || bytes[6] != 0u || bytes[7] != 0u) {
    return UINT32_MAX;
  }
  const uint32_t kind = bytes[4];
  return kind >= VKR_DEPOT_KIND_BLOB && kind <= VKR_DEPOT_KIND_COMMIT
             ? kind
             : UINT32_MAX;
}

static void depot_header(uint8_t *out, uint32_t kind) {
  memcpy(out, depot_magic, 4u);
  out[4] = (uint8_t)kind;
  out[5] = out[6] = out[7] = 0u;
}

bool8_t vkr_depot_name_valid(const char *name, uint32_t size) {
  if (size == 0u || size > VKR_DEPOT_NAME_MAX) {
    return false_v;
  }
  if ((size == 1u && name[0] == '.') ||
      (size == 2u && name[0] == '.' && name[1] == '.')) {
    return false_v;
  }
  /* Windows drops a trailing space or dot, so two names would collide. */
  if (name[size - 1u] == ' ' || name[size - 1u] == '.') {
    return false_v;
  }
  for (uint32_t i = 0u; i < size; ++i) {
    const uint8_t c = (uint8_t)name[i];
    if (c < 0x20u || c == '/' || c == '\\' || c == 0x7fu) {
      return false_v;
    }
  }
  /* UTF-8 well-formedness: lead bytes, continuations, no overlong pairs. */
  uint32_t i = 0u;
  while (i < size) {
    const uint8_t c = (uint8_t)name[i];
    if (c < 0x80u) {
      i += 1u;
      continue;
    }
    uint32_t extra = 0u;
    if (c >= 0xc2u && c <= 0xdfu) {
      extra = 1u;
    } else if ((c & 0xf0u) == 0xe0u) {
      extra = 2u;
    } else if (c >= 0xf0u && c <= 0xf4u) {
      extra = 3u;
    } else {
      return false_v;
    }
    if (size - i <= extra) {
      return false_v;
    }
    for (uint32_t k = 1u; k <= extra; ++k) {
      if (((uint8_t)name[i + k] & 0xc0u) != 0x80u) {
        return false_v;
      }
    }
    i += extra + 1u;
  }
  return true_v;
}

// =============================================================================
// Blob
// =============================================================================

bool8_t vkr_depot_blob_encode(const VkrDepotChunkRef *chunks, uint32_t count,
                              uint8_t **out_bytes, uint64_t *out_size) {
  const uint64_t size = VKR_DEPOT_HEADER_SIZE + 8u + 4u +
                        (uint64_t)count * (VKR_DEPOT_ID_SIZE + 4u);
  uint8_t *bytes = malloc(size);
  if (!bytes) {
    return false_v;
  }
  depot_header(bytes, VKR_DEPOT_KIND_BLOB);
  uint64_t total = 0u;
  uint8_t *cursor = bytes + VKR_DEPOT_HEADER_SIZE + 12u;
  for (uint32_t i = 0u; i < count; ++i) {
    memcpy(cursor, chunks[i].id.bytes, VKR_DEPOT_ID_SIZE);
    vkr_store_le_u32(cursor + VKR_DEPOT_ID_SIZE, chunks[i].size);
    cursor += VKR_DEPOT_ID_SIZE + 4u;
    total += chunks[i].size;
  }
  vkr_store_le_u64(bytes + VKR_DEPOT_HEADER_SIZE, total);
  vkr_store_le_u32(bytes + VKR_DEPOT_HEADER_SIZE + 8u, count);
  *out_bytes = bytes;
  *out_size = size;
  return true_v;
}

bool8_t vkr_depot_blob_parse(const uint8_t *bytes, uint64_t size,
                             uint64_t *out_file_size,
                             VkrDepotChunkVisitor visit, void *context) {
  if (vkr_depot_object_kind(bytes, size) != VKR_DEPOT_KIND_BLOB ||
      size < VKR_DEPOT_HEADER_SIZE + 12u) {
    return false_v;
  }
  const uint64_t file_size = vkr_load_le_u64(bytes + VKR_DEPOT_HEADER_SIZE);
  const uint32_t count = vkr_load_le_u32(bytes + VKR_DEPOT_HEADER_SIZE + 8u);
  if (size != VKR_DEPOT_HEADER_SIZE + 12u +
                  (uint64_t)count * (VKR_DEPOT_ID_SIZE + 4u)) {
    return false_v;
  }
  /* An empty file has no chunks; others have nonempty chunks summing up. */
  uint64_t total = 0u;
  const uint8_t *cursor = bytes + VKR_DEPOT_HEADER_SIZE + 12u;
  for (uint32_t i = 0u; i < count; ++i) {
    VkrDepotChunkRef chunk;
    memcpy(chunk.id.bytes, cursor, VKR_DEPOT_ID_SIZE);
    chunk.size = vkr_load_le_u32(cursor + VKR_DEPOT_ID_SIZE);
    if (chunk.size == 0u) {
      return false_v;
    }
    total += chunk.size;
    cursor += VKR_DEPOT_ID_SIZE + 4u;
  }
  if (total != file_size) {
    return false_v;
  }
  if (out_file_size) {
    *out_file_size = file_size;
  }
  if (!visit) {
    return true_v;
  }
  cursor = bytes + VKR_DEPOT_HEADER_SIZE + 12u;
  for (uint32_t i = 0u; i < count; ++i) {
    VkrDepotChunkRef chunk;
    memcpy(chunk.id.bytes, cursor, VKR_DEPOT_ID_SIZE);
    chunk.size = vkr_load_le_u32(cursor + VKR_DEPOT_ID_SIZE);
    if (!visit(context, &chunk)) {
      return false_v;
    }
    cursor += VKR_DEPOT_ID_SIZE + 4u;
  }
  return true_v;
}

// =============================================================================
// Tree
// =============================================================================

static int depot_name_compare(const char *a, uint32_t a_size, const char *b,
                              uint32_t b_size) {
  const int result = memcmp(a, b, Min(a_size, b_size));
  if (result != 0) {
    return result;
  }
  return a_size < b_size ? -1 : a_size > b_size ? 1 : 0;
}

static int depot_entry_compare(const void *lhs, const void *rhs) {
  const VkrDepotTreeEntry *a = lhs;
  const VkrDepotTreeEntry *b = rhs;
  return depot_name_compare(a->name, a->name_size, b->name, b->name_size);
}

static bool8_t depot_names_fold_equal(const VkrDepotTreeEntry *a,
                                      const VkrDepotTreeEntry *b) {
  if (a->name_size != b->name_size) {
    return false_v;
  }
  for (uint32_t i = 0u; i < a->name_size; ++i) {
    char x = a->name[i];
    char y = b->name[i];
    x = (x >= 'A' && x <= 'Z') ? (char)(x - 'A' + 'a') : x;
    y = (y >= 'A' && y <= 'Z') ? (char)(y - 'A' + 'a') : y;
    if (x != y) {
      return false_v;
    }
  }
  return true_v;
}

bool8_t vkr_depot_tree_encode(VkrDepotTreeEntry *entries, uint32_t count,
                              uint8_t **out_bytes, uint64_t *out_size) {
  qsort(entries, count, sizeof(VkrDepotTreeEntry), depot_entry_compare);
  uint64_t size = VKR_DEPOT_HEADER_SIZE + 4u;
  for (uint32_t i = 0u; i < count; ++i) {
    if (!vkr_depot_name_valid(entries[i].name, entries[i].name_size) ||
        (entries[i].kind != VKR_DEPOT_ENTRY_FILE &&
         entries[i].kind != VKR_DEPOT_ENTRY_TREE)) {
      return false_v;
    }
    /* ASCII case folding catches the collisions both hosts make. */
    for (uint32_t j = 0u; j < i; ++j) {
      if (depot_names_fold_equal(&entries[i], &entries[j])) {
        return false_v;
      }
    }
    size += 2u + entries[i].name_size + 1u + 8u + VKR_DEPOT_ID_SIZE;
  }
  uint8_t *bytes = malloc(size);
  if (!bytes) {
    return false_v;
  }
  depot_header(bytes, VKR_DEPOT_KIND_TREE);
  vkr_store_le_u32(bytes + VKR_DEPOT_HEADER_SIZE, count);
  uint8_t *cursor = bytes + VKR_DEPOT_HEADER_SIZE + 4u;
  for (uint32_t i = 0u; i < count; ++i) {
    const VkrDepotTreeEntry *entry = &entries[i];
    cursor[0] = (uint8_t)(entry->name_size & 0xffu);
    cursor[1] = (uint8_t)(entry->name_size >> 8);
    memcpy(cursor + 2u, entry->name, entry->name_size);
    cursor += 2u + entry->name_size;
    *cursor++ = entry->kind;
    vkr_store_le_u64(cursor,
                     entry->kind == VKR_DEPOT_ENTRY_FILE ? entry->size : 0u);
    cursor += 8u;
    memcpy(cursor, entry->id.bytes, VKR_DEPOT_ID_SIZE);
    cursor += VKR_DEPOT_ID_SIZE;
  }
  *out_bytes = bytes;
  *out_size = size;
  return true_v;
}

bool8_t vkr_depot_tree_parse(const uint8_t *bytes, uint64_t size,
                             VkrDepotEntryVisitor visit, void *context) {
  if (vkr_depot_object_kind(bytes, size) != VKR_DEPOT_KIND_TREE ||
      size < VKR_DEPOT_HEADER_SIZE + 4u) {
    return false_v;
  }
  const uint32_t count = vkr_load_le_u32(bytes + VKR_DEPOT_HEADER_SIZE);
  const uint8_t *cursor = bytes + VKR_DEPOT_HEADER_SIZE + 4u;
  const uint8_t *end = bytes + size;
  VkrDepotTreeEntry previous = {0};
  /* Two passes: validate everything, then visit. */
  for (uint32_t pass = 0u; pass < 2u; ++pass) {
    cursor = bytes + VKR_DEPOT_HEADER_SIZE + 4u;
    for (uint32_t i = 0u; i < count; ++i) {
      if ((uint64_t)(end - cursor) < 2u) {
        return false_v;
      }
      VkrDepotTreeEntry entry = {0};
      entry.name_size = (uint16_t)(cursor[0] | (cursor[1] << 8));
      cursor += 2u;
      if ((uint64_t)(end - cursor) <
          (uint64_t)entry.name_size + 1u + 8u + VKR_DEPOT_ID_SIZE) {
        return false_v;
      }
      entry.name = (const char *)cursor;
      cursor += entry.name_size;
      entry.kind = *cursor++;
      entry.size = vkr_load_le_u64(cursor);
      cursor += 8u;
      memcpy(entry.id.bytes, cursor, VKR_DEPOT_ID_SIZE);
      cursor += VKR_DEPOT_ID_SIZE;
      if (pass == 0u) {
        if (!vkr_depot_name_valid(entry.name, entry.name_size) ||
            (entry.kind != VKR_DEPOT_ENTRY_FILE &&
             entry.kind != VKR_DEPOT_ENTRY_TREE) ||
            (entry.kind == VKR_DEPOT_ENTRY_TREE && entry.size != 0u)) {
          return false_v;
        }
        if (i > 0u && depot_name_compare(previous.name, previous.name_size,
                                         entry.name, entry.name_size) >= 0) {
          return false_v;
        }
        previous = entry;
      } else if (visit && !visit(context, &entry)) {
        return false_v;
      }
    }
    if (cursor != end) {
      return false_v;
    }
  }
  return true_v;
}

// =============================================================================
// Commit
// =============================================================================

bool8_t vkr_depot_commit_encode(const VkrDepotCommit *commit,
                                uint8_t **out_bytes, uint64_t *out_size) {
  if (commit->parent_count > VKR_DEPOT_PARENTS_MAX ||
      commit->message_size > VKR_DEPOT_MESSAGE_MAX) {
    return false_v;
  }
  const uint64_t size = VKR_DEPOT_HEADER_SIZE + VKR_DEPOT_ID_SIZE + 1u +
                        (uint64_t)commit->parent_count * VKR_DEPOT_ID_SIZE +
                        32u + 8u + 4u + commit->message_size;
  uint8_t *bytes = malloc(size);
  if (!bytes) {
    return false_v;
  }
  depot_header(bytes, VKR_DEPOT_KIND_COMMIT);
  uint8_t *cursor = bytes + VKR_DEPOT_HEADER_SIZE;
  memcpy(cursor, commit->tree.bytes, VKR_DEPOT_ID_SIZE);
  cursor += VKR_DEPOT_ID_SIZE;
  *cursor++ = (uint8_t)commit->parent_count;
  for (uint32_t i = 0u; i < commit->parent_count; ++i) {
    memcpy(cursor, commit->parents[i].bytes, VKR_DEPOT_ID_SIZE);
    cursor += VKR_DEPOT_ID_SIZE;
  }
  memcpy(cursor, commit->author, 32u);
  cursor += 32u;
  vkr_store_le_u64(cursor, commit->time_us);
  cursor += 8u;
  vkr_store_le_u32(cursor, commit->message_size);
  cursor += 4u;
  if (commit->message_size > 0u) {
    memcpy(cursor, commit->message, commit->message_size);
  }
  *out_bytes = bytes;
  *out_size = size;
  return true_v;
}

bool8_t vkr_depot_commit_parse(const uint8_t *bytes, uint64_t size,
                               VkrDepotCommit *out_commit) {
  if (vkr_depot_object_kind(bytes, size) != VKR_DEPOT_KIND_COMMIT ||
      size < VKR_DEPOT_HEADER_SIZE + VKR_DEPOT_ID_SIZE + 1u) {
    return false_v;
  }
  VkrDepotCommit commit = {0};
  const uint8_t *cursor = bytes + VKR_DEPOT_HEADER_SIZE;
  const uint8_t *end = bytes + size;
  memcpy(commit.tree.bytes, cursor, VKR_DEPOT_ID_SIZE);
  cursor += VKR_DEPOT_ID_SIZE;
  commit.parent_count = *cursor++;
  if (commit.parent_count > VKR_DEPOT_PARENTS_MAX ||
      (uint64_t)(end - cursor) <
          (uint64_t)commit.parent_count * VKR_DEPOT_ID_SIZE + 32u + 8u + 4u) {
    return false_v;
  }
  for (uint32_t i = 0u; i < commit.parent_count; ++i) {
    memcpy(commit.parents[i].bytes, cursor, VKR_DEPOT_ID_SIZE);
    cursor += VKR_DEPOT_ID_SIZE;
  }
  memcpy(commit.author, cursor, 32u);
  cursor += 32u;
  commit.time_us = vkr_load_le_u64(cursor);
  cursor += 8u;
  commit.message_size = vkr_load_le_u32(cursor);
  cursor += 4u;
  if (commit.message_size > VKR_DEPOT_MESSAGE_MAX ||
      (uint64_t)(end - cursor) != commit.message_size) {
    return false_v;
  }
  commit.message = (const char *)cursor;
  *out_commit = commit;
  return true_v;
}
