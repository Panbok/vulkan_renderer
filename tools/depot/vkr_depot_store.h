#pragma once

#include "vkr_depot_object.h"

/* A content-addressed store of depot chunks and objects on disk
 * (`<root>/objects/<2 hex>/<62 hex>`). A stored file is a 5-byte header,
 * the representation (0 raw, 1 zstd) and the decoded size as u32, then the
 * payload; that representation is also what travels between stores. Every
 * write checks the SHA-256 of the decoded bytes against the ID, writes a
 * sibling temporary and renames it, so a store never holds a wrong or
 * partial object and concurrent writers of one ID agree. */

#define VKR_DEPOT_PATH_CAPACITY 4096u
#define VKR_DEPOT_STORED_HEADER 5u
/* FastCDC bounds: chunks are 64 KiB to 1 MiB, 256 KiB on average. */
#define VKR_DEPOT_CHUNK_MIN (64u * 1024u)
#define VKR_DEPOT_CHUNK_AVERAGE (256u * 1024u)
#define VKR_DEPOT_CHUNK_MAX (1024u * 1024u)
/* Largest decoded object a store accepts, and its largest stored form:
   a compressed form is kept only when smaller. */
#define VKR_DEPOT_OBJECT_MAX (32u * 1024u * 1024u)
#define VKR_DEPOT_STORED_MAX (VKR_DEPOT_STORED_HEADER + VKR_DEPOT_OBJECT_MAX)

/* The compressor a process installs (zstd); NULL stores raw only. */
typedef struct VkrDepotCodec {
  uint64_t (*bound)(uint64_t size);
  /* Returns the compressed size, or zero on failure. */
  uint64_t (*compress)(void *dst, uint64_t capacity, const void *src,
                       uint64_t size, int32_t level);
  /* Returns the decoded size, or UINT64_MAX on failure. */
  uint64_t (*decompress)(void *dst, uint64_t capacity, const void *src,
                         uint64_t size);
} VkrDepotCodec;

typedef struct VkrDepotStore {
  char root[VKR_DEPOT_PATH_CAPACITY];
  const VkrDepotCodec *codec;
} VkrDepotStore;

/* Opens or creates a store at `root`. `codec` may be NULL. */
bool8_t vkr_depot_store_open(VkrDepotStore *store, const char *root,
                             const VkrDepotCodec *codec);

void vkr_depot_store_path(const VkrDepotStore *store, const VkrDepotId *id,
                          char *out, uint32_t capacity);

bool8_t vkr_depot_store_has(const VkrDepotStore *store, const VkrDepotId *id);

/* Stores decoded `bytes` named `id`, compressed when that saves at least a
   tenth. False when the bytes do not hash to `id`. */
bool8_t vkr_depot_store_put(const VkrDepotStore *store, const VkrDepotId *id,
                            const uint8_t *bytes, uint64_t size);

/* Stores a received representation after decoding and checking it. */
bool8_t vkr_depot_store_put_stored(const VkrDepotStore *store,
                                   const VkrDepotId *id, const uint8_t *stored,
                                   uint64_t size);

/* Decoded bytes in malloc storage the caller frees. */
bool8_t vkr_depot_store_get(const VkrDepotStore *store, const VkrDepotId *id,
                            uint8_t **out_bytes, uint64_t *out_size);

/* The stored representation in malloc storage, as sent to a peer. */
bool8_t vkr_depot_store_get_stored(const VkrDepotStore *store,
                                   const VkrDepotId *id, uint8_t **out_bytes,
                                   uint64_t *out_size);

/* Decodes a representation into malloc storage. */
bool8_t vkr_depot_stored_decode(const VkrDepotCodec *codec,
                                const uint8_t *stored, uint64_t size,
                                uint8_t **out_bytes, uint64_t *out_size);

/* Content-defined chunking (FastCDC with normalized chunking): the length
   of the chunk that starts `data`. Deterministic on every host. */
uint32_t vkr_depot_chunk_length(const uint8_t *data, uint64_t size);
