#pragma once

#include "vkr_bakery_commands.h"

/* Content closure and `.vkpak` writer of `vkr_bakery bundle` (ADR-077),
 * shared by recipe bundles (vkr_bakery_bundle.c) and project packages
 * (vkr_bakery_package.c). Identities are content-root-relative, '/'-separated
 * and never absolute or `..`. A mount table maps identity prefixes to host
 * directories; the first mount holding an identity's file serves it. */

#define VKR_BUNDLE_MAX_MOUNTS 8u

/* Insertion-ordered set of identities; the order is the closure's discovery
 * order and doubles as its work list. */
typedef struct VkrBundleSet {
  char **items;
  uint32_t count;
  uint32_t capacity;
  uint32_t *slots; /* Item index + 1; zero is empty. */
  uint32_t slot_capacity;
} VkrBundleSet;

typedef struct VkrBundleMount {
  /* Empty, or an identity prefix ending in '/' that `directory` replaces. */
  const char *prefix;
  const char *directory; /* Absolute host directory. */
} VkrBundleMount;

typedef struct VkrBundle {
  VkrBakeryCli *cli;
  Arena *arena;
  VkrBundleMount mounts[VKR_BUNDLE_MAX_MOUNTS];
  uint32_t mount_count;
  VkrBundleSet files;
  uint32_t missing;
} VkrBundle;

typedef struct VkrBundleItem {
  const char *identity;
  uint8_t sha256[32];
  uint64_t size;
  uint32_t chunk;
} VkrBundleItem;

/** Host path of an identity's file through the mounts; false when no mount
 * holds one. */
bool8_t vkr_bundle_source(const VkrBundle *bundle, const char *identity,
                          char *out, uint32_t capacity);
/** Adds an existing file named by a root-relative reference. A required
 * missing file is reported and counted in `missing`. */
bool8_t vkr_bundle_add(VkrBundle *bundle, const char *reference,
                       uint64_t length, bool8_t required);
/** Every file below a directory identity, in byte order of names. */
void vkr_bundle_add_directory(VkrBundle *bundle, const char *identity);
/** Expands the set until no document names a new file. */
void vkr_bundle_close(VkrBundle *bundle);
/** Hashes every file of the set; `*out_items` is malloc storage the caller
 * frees. Reports the first unreadable file. */
bool8_t vkr_bundle_hash(VkrBundle *bundle, VkrBundleItem **out_items,
                        uint64_t *out_bytes);
/** Writes an archive of `items` to `path` through a staging file, sorting
 * `items` by identity. */
bool8_t vkr_bundle_write_pack(VkrBundle *bundle, const char *path,
                              VkrBundleItem *items, uint32_t item_count,
                              uint32_t *out_chunks, uint64_t *out_bytes);
/** Reads an archive back through the runtime's validator and rehashes every
 * chunk. */
bool8_t vkr_bundle_verify(const char *path);
/** VkrPackLoader of an identity by its extension. */
uint32_t vkr_bundle_loader(const char *identity);
/** Copies a directory tree, cloning files where the volume can. */
bool8_t vkr_bundle_copy_tree(const char *source, const char *destination);
void vkr_bundle_free(VkrBundle *bundle);

/** `vkr_bakery bundle <project directory>`: packages a managed project
 * (docs/proposals/project-packaging.md). */
int vkr_bakery_bundle_project(VkrBakeryCli *cli, const char *project);
