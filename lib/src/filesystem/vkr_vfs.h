#pragma once

#include "containers/str.h"
#include "defines.h"
#include "filesystem/filesystem.h"

/* Read-only content mounts (ADR-077). Relative asset paths resolve against one
 * content root: the repository by default, an installed program's `content/`
 * directory (vkr_content_root()), or a bundle's `content/` directory.
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
  /* Baked lightmap sets (.vklm, ADR-087); appended so earlier values keep
     their meaning in existing archives. */
  VKR_PACK_LOADER_LIGHTMAP,
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

/** Absolute content root with a trailing separator: the repository the
 * program was built from, or the `content/` directory an installed program
 * (an editor distribution or a package) carries beside its executable, or in
 * its macOS application bundle's Contents/Resources. */
const char *vkr_content_root(void);
/** True while the content root is the repository. An installed program keeps
 * its settings and caches in per-user directories instead, and has no
 * shader sources. */
bool8_t vkr_content_root_is_repository(void);
/** Replaces the content root; `directory` must be absolute and exist. */
bool8_t vkr_vfs_set_content_root(const char *directory);
/**
 * Sets the directory whose `assets/` tree serves reads before the content
 * root's: a read-only open, map, existence check or stat of a path below the
 * root whose identity starts with `assets/` uses `<directory>/<identity>`
 * when that file exists. Mounted archives still win, and writes go to
 * the root unless the writer asks vkr_vfs_overlay_write_path. The editor sets
 * its open workspace, so a depot working copy carries a project's own meshes,
 * materials and textures.
 *
 * NULL, an empty string, or the content root itself clears it. Call from one
 * thread; loaders may run meanwhile. Earlier directories stay readable until
 * vkr_vfs_unmount_all(). Returns false when `directory` is too long.
 */
bool8_t vkr_vfs_set_overlay_root(const char *directory);
/** The environment variable naming an overlay directory for a process the
 * editor starts, such as a project bake's tools (ADR-077). */
#define VKR_VFS_ASSET_OVERLAY_VARIABLE "VKR_ASSET_OVERLAY"
/**
 * Sets the overlay from VKR_VFS_ASSET_OVERLAY_VARIABLE when it names one;
 * an unset variable keeps the current overlay. vkr_vfs_mount_startup calls
 * it; a tool that does not mount reads it itself. False when the value does
 * not fit.
 */
bool8_t vkr_vfs_overlay_from_environment(void);
/**
 * The overlay directory with a trailing separator, or NULL when none is set.
 * The string stays valid until vkr_vfs_unmount_all().
 */
const char *vkr_vfs_overlay_root(void);
/**
 * Where a write of `path` lands so the open workspace carries it: with an
 * overlay set and an `assets/` identity below the content root,
 * `<overlay>/<identity>` in `buffer`, whether or not that file exists yet.
 * False when the write goes to `path` itself.
 */
bool8_t vkr_vfs_overlay_write_path(const FilePath *path, FilePath *out_path,
                                   char *buffer, uint64_t capacity);
/**
 * The path a read of `path` should open, for a tool that opens files without
 * the VFS, such as the bake tools' streams: the overlay's copy of an
 * `assets/` identity below the content root (a relative `path` counts from
 * the root) when that file exists, written to `buffer`; otherwise `path`
 * itself. Writes keep `path`.
 */
const char *vkr_vfs_read_path(const char *path, char *buffer,
                              uint64_t capacity);
/**
 * A remote content directory's calls (asset streaming, ADR-123). Each takes
 * a path '/'-separated and relative to the directory; any thread may call
 * each of them at once.
 */
typedef struct VkrVfsRemoteCalls {
  /* Makes `relative` exist on disk and returns true, or returns false at
     once when the remote holds no such file. Blocks while it downloads. */
  bool8_t (*fetch)(void *context, const char *relative);
  /* Whether the remote holds `relative` and has not yet written it whole,
     with its size in `out_size`. Never blocks on the network. */
  bool8_t (*pending)(void *context, const char *relative, uint64_t *out_size);
  /* Copies `size` bytes at `offset` of `relative` into `destination`
     without downloading the rest of the file; blocks while they download. */
  bool8_t (*read)(void *context, const char *relative, uint64_t offset,
                  uint64_t size, void *destination);
  /* Queues the whole file's download behind every read and fetch and
     returns at once. */
  void (*prefetch)(void *context, const char *relative);
  /* Files the remote has written whole so far; it only grows. */
  uint64_t (*arrivals)(void *context);
} VkrVfsRemoteCalls;
/**
 * Sets the directory a remote fills on demand: before a read-only open, map,
 * existence check or stat of a path below `directory` reaches the disk, and
 * before the workspace overlay looks for its copy there, `calls->fetch` gets
 * the path's chance to arrive. `directory` must be absolute. `calls` is
 * copied; `fetch` is required and the other calls may be NULL. NULL `calls`
 * clears it. Call from one thread; `context` must outlive every read until
 * the remote is cleared and no read can still use it. Returns false when
 * `directory` is too long.
 */
bool8_t vkr_vfs_set_remote(const char *directory,
                           const VkrVfsRemoteCalls *calls, void *context);
/**
 * Whether a read of `path` would wait for the remote: the remote holds the
 * file a read-only open of `path` resolves to (the workspace overlay's copy
 * or `path` itself) and has not yet written it whole. Its size goes to
 * `out_size` (may be NULL). Never blocks on the network and never fetches;
 * false without a remote.
 */
bool8_t vkr_vfs_remote_pending(const FilePath *path, uint64_t *out_size);
/**
 * Copies `size` bytes at `offset` of the pending file `path` resolves to
 * into `destination`, downloading only the bytes asked for. Blocks while
 * they download. False when no remote file is pending for `path`, or the
 * read fails.
 */
bool8_t vkr_vfs_remote_read(const FilePath *path, uint64_t offset,
                            uint64_t size, void *destination);
/** Queues the whole pending file `path` resolves to; returns at once. */
void vkr_vfs_remote_prefetch(const FilePath *path);
/**
 * Files the remote has written whole so far, zero without one. A caller
 * that waits for a pending file rechecks it only after this changes.
 */
uint64_t vkr_vfs_remote_arrivals(void);
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
