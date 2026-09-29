#pragma once

#include "containers/str.h"
#include "defines.h"

/* Read-only content mounts (ADR-077). Relative asset paths resolve against one
 * content root: the repository by default, or a bundle's `content/` directory.
 * `.vkpak` archives mounted over that root serve an asset identity (its path
 * below the root) before the directory does; the first mounted archive that
 * holds an identity wins, so a patch archive mounted first overrides a base
 * archive. Every read-only open, existence check and stat of a path below the
 * root consults the mounts, so loaders keep opening paths and keep their
 * format validation.
 *
 * Mounting happens once at startup, before any loader runs; lookups afterwards
 * only read the mount table and are safe from any thread. Archive bytes stay
 * mapped until vkr_vfs_unmount_all(). */

/* ---- `.vkpak` version 2: little-endian, written by `vkr_bakery bundle` ----
 * Version 1 archives, which store every chunk as its bytes, still read. */

#define VKR_PACK_MAGIC "VKPK"
#define VKR_PACK_VERSION 2u
#define VKR_PACK_VERSION_MIN 1u
/** VkrPackChunk.flags (version 2): the stored bytes are one zstd frame. */
#define VKR_PACK_CHUNK_ZSTD 1u
/** Upper bound of a compressed chunk's decoded size. */
#define VKR_PACK_MAX_DECODED_SIZE (1ull << 32)
#define VKR_PACK_HEADER_SIZE 128u
/* Chunks start on this boundary; mappable resources (meshes, textures and
 * volumes) on VKR_PACK_MAPPABLE_ALIGNMENT. */
#define VKR_PACK_ALIGNMENT 64u
#define VKR_PACK_MAPPABLE_ALIGNMENT 4096u
#define VKR_PACK_IDENTITY_MAX 1024u

/** File header at offset zero. `index_sha256` hashes the catalog bytes
 * followed by the chunk table bytes. */
typedef struct VkrPackHeader {
  uint8_t magic[4];
  uint32_t version;
  uint32_t flags;
  uint32_t reserved0;
  uint64_t catalog_offset;
  uint64_t catalog_size;
  uint64_t chunk_table_offset;
  uint64_t chunk_table_size;
  uint64_t total_size;
  uint8_t index_sha256[32];
  uint8_t reserved[40];
} VkrPackHeader;

/** One stored product, sorted by `sha256`, which hashes its decoded bytes;
 * bytes are stored once per hash. `size` counts the stored bytes. A version 2
 * chunk with VKR_PACK_CHUNK_ZSTD holds one zstd frame that decodes to
 * `decoded_size` bytes; otherwise `decoded_size` is zero or `size`. Version 1
 * chunks have no flags. */
typedef struct VkrPackChunk {
  uint8_t sha256[32];
  uint64_t offset;
  uint64_t size;
  uint32_t alignment;
  uint32_t flags;
  uint64_t decoded_size;
} VkrPackChunk;

/** How the runtime reads an entry; informational for tools. */
typedef enum VkrPackLoader {
  VKR_PACK_LOADER_RAW = 0,
  VKR_PACK_LOADER_SCENE,
  VKR_PACK_LOADER_MESH,
  VKR_PACK_LOADER_TEXTURE,
  VKR_PACK_LOADER_MATERIAL,
  VKR_PACK_LOADER_FONT,
  VKR_PACK_LOADER_ANIMATION,
  VKR_PACK_LOADER_COLLISION,
  VKR_PACK_LOADER_VOLUME,
  VKR_PACK_LOADER_JSON,
  VKR_PACK_LOADER_COUNT
} VkrPackLoader;

/** Catalog: a VkrPackCatalogHeader, `entry_count` entries sorted by identity
 * bytes, then the UTF-8 identity strings they point into. Identities are
 * content-root-relative, `/`-separated and never absolute or `..`. */
typedef struct VkrPackCatalogHeader {
  uint32_t entry_count;
  uint32_t reserved;
} VkrPackCatalogHeader;

typedef struct VkrPackEntry {
  uint32_t identity_offset; /**< From the start of the string area. */
  uint32_t identity_length;
  uint32_t chunk;
  uint32_t loader; /**< VkrPackLoader. */
  uint32_t loader_version;
  uint32_t reserved;
} VkrPackEntry;

_Static_assert(sizeof(VkrPackHeader) == VKR_PACK_HEADER_SIZE,
               "VkrPackHeader is a stored layout");
_Static_assert(sizeof(VkrPackChunk) == 64u, "VkrPackChunk is a stored layout");
_Static_assert(sizeof(VkrPackEntry) == 24u, "VkrPackEntry is a stored layout");

/** Validates an archive image: header, bounds, sorted tables, identities and
 * the index hash. Chunk hashes are not recomputed here. */
bool8_t vkr_pack_validate(const uint8_t *data, uint64_t size, char *error,
                          uint64_t error_capacity);

/* ---- Mounts ---- */

/** Decodes one zstd frame of `source_size` bytes into exactly
 * `destination_size` bytes; any thread. */
typedef bool8_t (*VkrVfsDecompressor)(const uint8_t *source,
                                      uint64_t source_size,
                                      uint8_t *destination,
                                      uint64_t destination_size);
/** Installs the codec compressed chunks decode through, before content
 * reads. The library links no codec: the runtime and tools install the zstd
 * decoder they already link. Without one, a compressed entry does not open. */
void vkr_vfs_set_decompressor(VkrVfsDecompressor decompressor);

/** Borrowed bytes of one mounted identity; valid until unmount. A compressed
 * entry decodes once, on its first open, into storage kept until unmount. */
typedef struct VkrVfsView {
  const uint8_t *data;
  uint64_t size;
  uint64_t last_modified; /**< The archive file's modification time. */
} VkrVfsView;

/** Absolute content root with a trailing separator. */
const char *vkr_content_root(void);
/** Replaces the content root; `directory` must be absolute and exist. */
bool8_t vkr_vfs_set_content_root(const char *directory);
/** Maps and validates one archive and appends it to the mount order. */
bool8_t vkr_vfs_mount_pack(const char *path);
/**
 * Mounts content for this process once, before loaders run:
 * - `$VKR_CONTENT` or a `bundle.json` beside the executable (or, for an
 *   executable in a macOS bundle's Contents/MacOS, in Contents/Resources)
 *   names a bundle:
 *   its `content/` directory becomes the root and its archives mount.
 * - `$VKR_CONTENT_PACKS` (paths separated by the platform list separator)
 *   mounts archives over the current root.
 * `$VKR_VFS_RECORD` appends every content read to a file for bundle recipes.
 * Returns false only when a named bundle or archive cannot be mounted.
 */
bool8_t vkr_vfs_mount_startup(void);
/** Scene identity a version 1 bundle opens by default, or NULL. */
const char *vkr_vfs_bundle_scene(void);
/** The mounted bundle's `bundle.json` bytes, valid until unmount; empty when
 * no bundle is mounted. A version 2 description carries a `game` object
 * (docs/proposals/project-packaging.md) its player reads. */
String8 vkr_vfs_bundle_description(void);
uint32_t vkr_vfs_pack_count(void);
/** Resolves a path below the content root (absolute or root-relative) to
 * mounted archive bytes. */
bool8_t vkr_vfs_find(String8 path, VkrVfsView *out_view);
void vkr_vfs_unmount_all(void);
