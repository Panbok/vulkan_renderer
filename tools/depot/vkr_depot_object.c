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

// =============================================================================
// Names
// =============================================================================

/* Decodes the code point at `*offset` of well-formed UTF-8; a byte that
   starts no valid sequence decodes as itself. */
static uint32_t depot_utf8_next(const char *text, uint32_t size,
                                uint32_t *offset) {
  const uint8_t c = (uint8_t)text[*offset];
  uint32_t extra = c >= 0xf0u ? 3u : c >= 0xe0u ? 2u : c >= 0xc0u ? 1u : 0u;
  if (*offset + extra >= size) {
    extra = 0u;
  }
  if (extra == 0u) {
    *offset += 1u;
    return c;
  }
  uint32_t point = c & (0x3fu >> extra);
  for (uint32_t k = 1u; k <= extra; ++k) {
    point = (point << 6) | ((uint8_t)text[*offset + k] & 0x3fu);
  }
  *offset += extra + 1u;
  return point;
}

/* The case folding macOS and Windows apply, for the scripts a project
   names files in: ASCII, Latin-1, Latin Extended-A, Greek and Cyrillic.
   Other letters compare exactly. */
static uint32_t depot_fold(uint32_t c) {
  if ((c >= 'A' && c <= 'Z') || (c >= 0xc0u && c <= 0xdeu && c != 0xd7u) ||
      (c >= 0x391u && c <= 0x3abu && c != 0x3a2u) ||
      (c >= 0x410u && c <= 0x42fu)) {
    return c + 0x20u;
  }
  if (c >= 0x400u && c <= 0x40fu) {
    return c + 0x50u;
  }
  if (c == 0x178u) {
    return 0xffu;
  }
  /* Latin Extended-A pairs an upper and a lower case letter; the pairs
     start on even code points except in 0x139-0x148 and 0x179-0x17e. */
  const bool8_t odd_pairs =
      (c >= 0x139u && c <= 0x148u) || (c >= 0x179u && c <= 0x17eu);
  const bool8_t even_pairs = (c >= 0x100u && c <= 0x12fu) ||
                             (c >= 0x132u && c <= 0x137u) ||
                             (c >= 0x14au && c <= 0x177u);
  if ((even_pairs && (c & 1u) == 0u) || (odd_pairs && (c & 1u) == 1u)) {
    return c + 1u;
  }
  return c;
}

int vkr_depot_name_fold_compare(const char *a, uint32_t a_size, const char *b,
                                uint32_t b_size) {
  uint32_t i = 0u;
  uint32_t j = 0u;
  while (i < a_size && j < b_size) {
    const uint32_t x = depot_fold(depot_utf8_next(a, a_size, &i));
    const uint32_t y = depot_fold(depot_utf8_next(b, b_size, &j));
    if (x != y) {
      return x < y ? -1 : 1;
    }
  }
  const bool8_t a_more = i < a_size;
  const bool8_t b_more = j < b_size;
  return a_more == b_more ? 0 : a_more ? 1 : -1;
}

/* Whether `name` equals `word` without case. */
static bool8_t depot_name_is(const char *name, uint32_t size,
                             const char *word) {
  return vkr_depot_name_fold_compare(name, size, word,
                                     (uint32_t)strlen(word)) == 0;
}

/* Windows opens a device for these, with any extension and case. */
static bool8_t depot_name_reserved(const char *name, uint32_t size) {
  uint32_t base = 0u;
  while (base < size && name[base] != '.') {
    ++base;
  }
  while (base > 0u && name[base - 1u] == ' ') {
    --base;
  }
  static const char *const devices[] = {"CON", "PRN", "AUX", "NUL"};
  for (uint32_t i = 0u; i < ArrayCount(devices); ++i) {
    if (depot_name_is(name, base, devices[i])) {
      return true_v;
    }
  }
  if (base == 4u && name[3] >= '1' && name[3] <= '9') {
    return depot_name_is(name, 3u, "COM") || depot_name_is(name, 3u, "LPT");
  }
  return false_v;
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
  /* A tree that carried the depot's or git's own state would overwrite it
     in every working copy that checks it out. */
  if (depot_name_is(name, size, VKR_DEPOT_RESERVED_META) ||
      depot_name_is(name, size, ".git") || depot_name_reserved(name, size)) {
    return false_v;
  }
  for (uint32_t i = 0u; i < size; ++i) {
    const uint8_t c = (uint8_t)name[i];
    if (c < 0x20u || c == 0x7fu || strchr("/\\:<>\"|?*", c)) {
      return false_v;
    }
  }
  /* UTF-8 well-formedness: lead bytes, continuations, no overlong forms,
     surrogates or code points past U+10FFFF. */
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
    const uint8_t next = (uint8_t)name[i + 1u];
    if ((c == 0xe0u && next < 0xa0u) || (c == 0xedu && next >= 0xa0u) ||
        (c == 0xf0u && next < 0x90u) || (c == 0xf4u && next >= 0x90u)) {
      return false_v;
    }
    uint32_t offset = i;
    const uint32_t point = depot_utf8_next(name, size, &offset);
    /* Combining marks: a decomposed name (NFD) and its composed form (NFC)
       are one file on macOS, so only composed letters pass. */
    if ((point >= 0x300u && point <= 0x36fu) ||
        (point >= 0x1ab0u && point <= 0x1affu) ||
        (point >= 0x1dc0u && point <= 0x1dffu) ||
        (point >= 0x20d0u && point <= 0x20ffu) ||
        (point >= 0xfe20u && point <= 0xfe2fu)) {
      return false_v;
    }
    i += extra + 1u;
  }
  return true_v;
}

bool8_t vkr_depot_path_valid(const char *path) {
  const size_t length = strlen(path);
  if (length == 0u || length >= VKR_DEPOT_PATH_MAX) {
    return false_v;
  }
  uint32_t depth = 0u;
  const char *component = path;
  for (;;) {
    const char *slash = strchr(component, '/');
    const uint32_t size =
        (uint32_t)(slash ? (size_t)(slash - component) : strlen(component));
    if (++depth > VKR_DEPOT_TREE_DEPTH_MAX + 1u ||
        !vkr_depot_name_valid(component, size)) {
      return false_v;
    }
    if (!slash) {
      return true_v;
    }
    component = slash + 1;
  }
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
  /* One chunk repeated many times would expand far past its size. */
  if (file_size > VKR_DEPOT_FILE_MAX ||
      size != VKR_DEPOT_HEADER_SIZE + 12u +
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

static int depot_entry_fold_compare(const void *lhs, const void *rhs) {
  const VkrDepotTreeEntry *a = lhs;
  const VkrDepotTreeEntry *b = rhs;
  return vkr_depot_name_fold_compare(a->name, a->name_size, b->name,
                                     b->name_size);
}

/* Whether no two names are equal under ASCII case folding, the collisions
   macOS and Windows both make. Sorts a copy, so it costs n log n. */
static bool8_t depot_names_fold_unique(const VkrDepotTreeEntry *entries,
                                       uint32_t count) {
  if (count < 2u) {
    return true_v;
  }
  VkrDepotTreeEntry *sorted = malloc((size_t)count * sizeof(*sorted));
  if (!sorted) {
    return false_v;
  }
  MemCopy(sorted, entries, (uint64_t)count * sizeof(*sorted));
  qsort(sorted, count, sizeof(*sorted), depot_entry_fold_compare);
  bool8_t unique = true_v;
  for (uint32_t i = 1u; i < count && unique; ++i) {
    unique = depot_entry_fold_compare(&sorted[i - 1u], &sorted[i]) != 0;
  }
  free(sorted);
  return unique;
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
    size += 2u + entries[i].name_size + 1u + 8u + VKR_DEPOT_ID_SIZE;
  }
  if (!depot_names_fold_unique(entries, count)) {
    return false_v;
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
  /* Every entry takes at least 43 bytes; a count past that is invalid. */
  if ((uint64_t)count * (2u + 1u + 8u + VKR_DEPOT_ID_SIZE) >
      size - VKR_DEPOT_HEADER_SIZE - 4u) {
    return false_v;
  }
  VkrDepotTreeEntry *entries =
      count ? malloc((size_t)count * sizeof(VkrDepotTreeEntry)) : NULL;
  if (count && !entries) {
    return false_v;
  }
  bool8_t ok = false_v;
  VkrDepotTreeEntry previous = {0};
  /* Two passes: validate everything, then visit. */
  for (uint32_t pass = 0u; pass < 2u; ++pass) {
    cursor = bytes + VKR_DEPOT_HEADER_SIZE + 4u;
    for (uint32_t i = 0u; i < count; ++i) {
      if ((uint64_t)(end - cursor) < 2u) {
        goto cleanup;
      }
      VkrDepotTreeEntry entry = {0};
      entry.name_size = (uint16_t)(cursor[0] | (cursor[1] << 8));
      cursor += 2u;
      if ((uint64_t)(end - cursor) <
          (uint64_t)entry.name_size + 1u + 8u + VKR_DEPOT_ID_SIZE) {
        goto cleanup;
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
            (entry.kind == VKR_DEPOT_ENTRY_TREE && entry.size != 0u) ||
            entry.size > VKR_DEPOT_FILE_MAX) {
          goto cleanup;
        }
        if (i > 0u && depot_name_compare(previous.name, previous.name_size,
                                         entry.name, entry.name_size) >= 0) {
          goto cleanup;
        }
        previous = entry;
        entries[i] = entry;
      } else if (visit && !visit(context, &entry)) {
        goto cleanup;
      }
    }
    if (cursor != end) {
      goto cleanup;
    }
    /* The encoder refuses names equal without case; so does the parser, or
       a checkout would write two paths that are one file there. */
    if (pass == 0u && !depot_names_fold_unique(entries, count)) {
      goto cleanup;
    }
  }
  ok = true_v;
cleanup:
  free(entries);
  return ok;
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
