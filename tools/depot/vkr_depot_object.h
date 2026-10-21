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
/* A working copy's own state folder, a name no tree may hold. */
#define VKR_DEPOT_RESERVED_META ".vkrdepot"

/* Limits every reader enforces, so a hostile tree cannot exhaust a stack,
   memory or a disk: */
/* The largest file; snapshots read a file whole, and a blob manifest that
   repeats one chunk cannot claim more. */
#define VKR_DEPOT_FILE_MAX (4ull * 1024ull * 1024ull * 1024ull)
/* Folders below a tree's root. */
#define VKR_DEPOT_TREE_DEPTH_MAX 64u
/* Files of one tree, counting each path a shared subtree appears at. */
#define VKR_DEPOT_TREE_FILES_MAX 1000000u
/* The sum of a tree's file sizes. */
#define VKR_DEPOT_TREE_BYTES_MAX (512ull * 1024ull * 1024ull * 1024ull)
/* A relative path with its terminator. */
#define VKR_DEPOT_PATH_MAX 4096u

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

/* True for a name a tree may hold on every host: well-formed UTF-8 (no
   overlong forms or surrogates) of 1 to 255 bytes; no control bytes or any
   of / \\ : < > " | ? *; not "." or ".."; no trailing space or dot; not
   `.vkrdepot` or `.git` or a Windows device name (CON, PRN, AUX, NUL,
   COM1-9, LPT1-9, with any extension), in any case; and no combining marks
   (U+0300-U+036F and the other combining blocks), so names are composed
   (NFC) and cannot alias each other through normalization. */
bool8_t vkr_depot_name_valid(const char *name, uint32_t size);

/* True for a relative path of valid names joined by '/', in at most
   VKR_DEPOT_TREE_DEPTH_MAX folders and shorter than VKR_DEPOT_PATH_MAX. */
bool8_t vkr_depot_path_valid(const char *path);

/* Orders names with letters folded to lower case (ASCII, Latin-1, Latin
   Extended-A, Greek and Cyrillic; other letters compare exactly); zero for
   names macOS and Windows take for one file. Trees, checkouts and locks
   all compare paths by this one key. */
int vkr_depot_name_fold_compare(const char *a, uint32_t a_size, const char *b,
                                uint32_t b_size);

/* Encoders write into malloc storage the caller frees. */
bool8_t vkr_depot_blob_encode(const VkrDepotChunkRef *chunks, uint32_t count,
                              uint8_t **out_bytes, uint64_t *out_size);
/* Sorts `entries` by name and refuses duplicates, including names equal
   when compared without case (macOS and Windows fold case); the parser
   refuses them as well. */
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

/* Calls `visit` for each chunk of a blob that holds bytes of the file's
   range [offset, offset + size), in order, with the file offset the chunk
   starts at. False when the blob is invalid, the range is empty or passes
   the file's end, or `visit` returns false. */
typedef bool8_t (*VkrDepotRangeVisitor)(void *context,
                                        const VkrDepotChunkRef *chunk,
                                        uint64_t chunk_offset);
bool8_t vkr_depot_blob_range(const uint8_t *bytes, uint64_t blob_size,
                             uint64_t offset, uint64_t size,
                             VkrDepotRangeVisitor visit, void *context);

typedef bool8_t (*VkrDepotEntryVisitor)(void *context,
                                        const VkrDepotTreeEntry *entry);
bool8_t vkr_depot_tree_parse(const uint8_t *bytes, uint64_t size,
                             VkrDepotEntryVisitor visit, void *context);

bool8_t vkr_depot_commit_parse(const uint8_t *bytes, uint64_t size,
                               VkrDepotCommit *out_commit);
