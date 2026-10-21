#pragma once

#include "core/vkr_codec.h"
#include "vkr_depot_object.h"

/* A content-addressed store of depot chunks and objects on disk
 * (`<root>/objects/<2 hex>/<62 hex>`). A stored file is a 5-byte header,
 * the representation (0 raw, 1 zstd) and the decoded size as u32, then the
 * payload; that representation is also what travels between stores. Every
 * write checks the SHA-256 of the decoded bytes against the ID, writes a
 * sibling temporary and renames it, so concurrent writers of one ID agree.
 * Writes are not flushed (owner decision, 2026-10-11): a flush per object
 * cost a seek each on a hard disk, minutes for one scene. Every read checks
 * the hash instead and removes an object a crash left torn, so it is
 * missing and fetched again rather than served wrong. */

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

typedef struct VkrDepotStore {
  char root[VKR_DEPOT_PATH_CAPACITY];
  /* The compressor a process installs (zstd); NULL stores raw only. */
  const VkrCodec *codec;
} VkrDepotStore;

/* Opens or creates a store at `root`. `codec` may be NULL. */
bool8_t vkr_depot_store_open(VkrDepotStore *store, const char *root,
                             const VkrCodec *codec);

void vkr_depot_store_path(const VkrDepotStore *store, const VkrDepotId *id,
                          char *out, uint32_t capacity);

bool8_t vkr_depot_store_has(const VkrDepotStore *store, const VkrDepotId *id);

/* Stores decoded `bytes` named `id`, compressed when that saves at least a
   tenth. False when the bytes do not hash to `id`. */
bool8_t vkr_depot_store_put(const VkrDepotStore *store, const VkrDepotId *id,
                            const uint8_t *bytes, uint64_t size);

/* Stores a received representation after decoding and checking it.
   `out_bytes`, when given, receives the decoded bytes (the caller frees
   them), so a caller that reads them decodes once. */
bool8_t vkr_depot_store_put_stored(const VkrDepotStore *store,
                                   const VkrDepotId *id, const uint8_t *stored,
                                   uint64_t size, uint8_t **out_bytes,
                                   uint64_t *out_size);

/* Decoded bytes in malloc storage the caller frees; false, with the file
   removed, for an object whose bytes no longer hash to `id`. */
bool8_t vkr_depot_store_get(const VkrDepotStore *store, const VkrDepotId *id,
                            uint8_t **out_bytes, uint64_t *out_size);

/* The stored representation in malloc storage, as sent to a peer, checked
   as vkr_depot_store_get checks it. */
bool8_t vkr_depot_store_get_stored(const VkrDepotStore *store,
                                   const VkrDepotId *id, uint8_t **out_bytes,
                                   uint64_t *out_size);

/* Decodes a representation into malloc storage. */
bool8_t vkr_depot_stored_decode(const VkrCodec *codec, const uint8_t *stored,
                                uint64_t size, uint8_t **out_bytes,
                                uint64_t *out_size);

/* Content-defined chunking (FastCDC with normalized chunking): the length
   of the chunk that starts `data`. Deterministic on every host. */
uint32_t vkr_depot_chunk_length(const uint8_t *data, uint64_t size);
