#pragma once

#include "defines.h"

/* Depot objects (docs/proposals/network-protocol.md, "Depot"). Every object
 * and every chunk is named by the SHA-256 of its decoded bytes, the identity
 * `.vkpak` chunks and Bakery outputs use. Objects are canonical binary
 * records, little-endian, behind an 8-byte header: "VKD1", the kind and
 * three zero bytes.
 *
 *   BLOB    size u64, chunk count u32, then per chunk: id[32], size u32
 *   TREE    entry count u32, then per entry in byte order of names:
 *           name length u16, name, kind u8, size u64, id[32]
 *   COMMIT  tree id[32], parent count u8, parents[32], author key[32],
 *           time u64 (Unix microseconds), message length u32, message
 *
 * A parser accepts only the canonical form: sorted unique names, valid
 * names, sizes that add up and no trailing bytes. */

#define VKR_DEPOT_ID_SIZE 32u
#define VKR_DEPOT_ID_HEX 65u
#define VKR_DEPOT_NAME_MAX 255u
#define VKR_DEPOT_PARENTS_MAX 4u
#define VKR_DEPOT_MESSAGE_MAX 4096u
#define VKR_DEPOT_HEADER_SIZE 8u
/* A branch name with its terminator. */
#define VKR_DEPOT_BRANCH_NAME 64u

typedef struct VkrDepotId {
  uint8_t bytes[VKR_DEPOT_ID_SIZE];
} VkrDepotId;

typedef enum VkrDepotKind {
  VKR_DEPOT_KIND_CHUNK = 0,
  VKR_DEPOT_KIND_BLOB = 1,
  VKR_DEPOT_KIND_TREE = 2,
  VKR_DEPOT_KIND_COMMIT = 3,
} VkrDepotKind;

typedef enum VkrDepotEntryKind {
  VKR_DEPOT_ENTRY_FILE = 1,
  VKR_DEPOT_ENTRY_TREE = 2,
} VkrDepotEntryKind;

typedef struct VkrDepotChunkRef {
  VkrDepotId id;
  uint32_t size;
} VkrDepotChunkRef;

typedef struct VkrDepotTreeEntry {
  /* Borrowed from the parsed bytes or owned by the builder. */
  const char *name;
  uint16_t name_size;
  uint8_t kind; /**< VkrDepotEntryKind. */
  /* Files: the content size. Trees: zero. */
  uint64_t size;
  VkrDepotId id;
} VkrDepotTreeEntry;

typedef struct VkrDepotCommit {
  VkrDepotId tree;
  VkrDepotId parents[VKR_DEPOT_PARENTS_MAX];
  uint32_t parent_count;
  uint8_t author[32];
  uint64_t time_us;
  /* Borrowed from the parsed bytes. */
  const char *message;
  uint32_t message_size;
} VkrDepotCommit;

bool8_t vkr_depot_id_equal(const VkrDepotId *a, const VkrDepotId *b);
bool8_t vkr_depot_id_zero(const VkrDepotId *id);
void vkr_depot_id_of(const void *bytes, uint64_t size, VkrDepotId *out_id);
void vkr_depot_id_hex(const VkrDepotId *id, char out[VKR_DEPOT_ID_HEX]);
bool8_t vkr_depot_id_parse(const char *hex, VkrDepotId *out_id);

/* The kind in an object's header, or UINT32_MAX when `bytes` is not an
   object. Chunks have no header. */
uint32_t vkr_depot_object_kind(const uint8_t *bytes, uint64_t size);

/* True for a name a tree may hold: UTF-8, 1 to 255 bytes, no '/', '\\',
   NUL or control bytes, not "." or "..", no trailing space or dot. */
bool8_t vkr_depot_name_valid(const char *name, uint32_t size);

/* Encoders write into malloc storage the caller frees. */
bool8_t vkr_depot_blob_encode(const VkrDepotChunkRef *chunks, uint32_t count,
                              uint8_t **out_bytes, uint64_t *out_size);
/* Sorts `entries` by name and refuses duplicates, including names equal
   when compared without case (macOS and Windows fold case). */
bool8_t vkr_depot_tree_encode(VkrDepotTreeEntry *entries, uint32_t count,
                              uint8_t **out_bytes, uint64_t *out_size);
bool8_t vkr_depot_commit_encode(const VkrDepotCommit *commit,
                                uint8_t **out_bytes, uint64_t *out_size);

/* Parsers validate the whole object, then call `visit` per element in
   order; `visit` returning false stops with false. */
typedef bool8_t (*VkrDepotChunkVisitor)(void *context,
                                        const VkrDepotChunkRef *chunk);
bool8_t vkr_depot_blob_parse(const uint8_t *bytes, uint64_t size,
                             uint64_t *out_file_size,
                             VkrDepotChunkVisitor visit, void *context);

typedef bool8_t (*VkrDepotEntryVisitor)(void *context,
                                        const VkrDepotTreeEntry *entry);
bool8_t vkr_depot_tree_parse(const uint8_t *bytes, uint64_t size,
                             VkrDepotEntryVisitor visit, void *context);

bool8_t vkr_depot_commit_parse(const uint8_t *bytes, uint64_t size,
                               VkrDepotCommit *out_commit);
