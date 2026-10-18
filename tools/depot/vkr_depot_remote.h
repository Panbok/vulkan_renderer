#pragma once

#include "memory/vkr_allocator.h"
#include "vkr_depot_store.h"
#include "vkr_depot_workspace.h"

/* Remote content: a local directory a depot server fills on demand (asset
 * streaming, docs/proposals/network-protocol.md, "Asset streaming").
 *
 * Opening connects, resolves a branch to its newest commit and fetches only
 * the trees of that commit, which list every file with its size and blob.
 * When the branch has a cooked side branch for this computer's GPU family
 * (`<branch>.cooked.bc` or `.astc`, which holds the platform files the
 * depot ignores, such as `.vkt` textures), its files join the list. A file
 * the list holds arrives when someone asks for it: vkr_depot_remote_fetch
 * blocks until its blob is in the local store and written under the
 * directory. vkr_vfs_set_remote installs it, so every loader read below the
 * directory asks first.
 *
 * One worker thread owns the connection. It takes every waiting request at
 * once and downloads their blobs in one pipelined transfer. A file already
 * under the directory with the listed size and its blob in the store counts
 * as present, so a second run fetches nothing it already has. */

typedef struct VkrDepotRemoteConfig {
  /* Absolute; files land below it and it is created when missing. */
  const char *directory;
  /* A numeric address or host name with an optional port. */
  const char *address;
  /* The server's public key, 32 bytes. */
  const uint8_t *server_key;
  /* NULL for "main". */
  const char *branch;
  /* NULL for `<directory>/.vkrdepot-stream`, where fetched objects stay. */
  const char *store_root;
  const VkrCodec *codec;
  /* Skips the cooked side branch even when the server has one. */
  bool8_t no_cooked;
} VkrDepotRemoteConfig;

typedef struct VkrDepotRemoteStats {
  /* The files the commits list. */
  uint32_t listed;
  /* Files written under the directory, and those found present at open. */
  uint32_t fetched;
  uint32_t present;
  /* Requests the remote could not serve. */
  uint32_t failed;
  uint64_t stored_bytes;
  /* Microseconds callers spent waiting in vkr_depot_remote_fetch, and from
     the end of opening to the newest file written. */
  uint64_t wait_us;
  uint64_t last_write_us;
  VkrDepotId commit;
  VkrDepotId cooked_commit;
} VkrDepotRemoteStats;

typedef struct VkrDepotRemote VkrDepotRemote;

/* Connects, fetches the trees and starts the worker. NULL with a message. */
VkrDepotRemote *vkr_depot_remote_open(VkrAllocator *allocator,
                                      const VkrDepotRemoteConfig *config,
                                      char *error, uint32_t capacity);

/* A VkrVfsRemoteFetch: makes `relative` ('/'-separated, below the
   directory) exist and returns true, or false at once when no commit lists
   it, or when its download failed. Any thread. */
bool8_t vkr_depot_remote_fetch(void *remote, const char *relative);

void vkr_depot_remote_stats(VkrDepotRemote *remote,
                            VkrDepotRemoteStats *out_stats);

/* Stops the worker after its transfer and disconnects. Clear the VFS remote
   first: a read that still asks would use freed memory. */
void vkr_depot_remote_close(VkrDepotRemote *remote);
