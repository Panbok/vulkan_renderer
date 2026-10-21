#pragma once

#include "filesystem/vkr_vfs.h"
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
 * Part of a file can arrive without the rest: vkr_depot_remote_read
 * downloads only the chunks that hold the bytes it asks for, and a later
 * fetch of the whole file downloads only the other chunks. A texture's
 * smallest mips lie at the start of its file (KTX2), so a loader can draw
 * them before the file is whole (ADR-123).
 *
 * One worker thread owns the connection. It serves every waiting read in
 * one transfer, then every waiting fetch in one transfer, then prefetches
 * in transfers of at most 16 MiB of listed files, so a read or fetch never
 * waits behind a long background download. A file already under the
 * directory with the listed size and its blob in the store counts as
 * present, so a second run fetches nothing it already has.
 *
 * Opened on a sparse working copy (`workspace`), the remote lists the
 * copy's unfetched files at its base and the cooked files it does not
 * track, keeps their objects in the copy's store, never writes over a file
 * on disk and records each file it writes in the copy's fetched log, so a
 * snapshot tells a fetched file that was deleted from one never fetched
 * (vkr_depot_workspace.h). */

typedef struct VkrDepotRemoteConfig {
  /* A working copy's root, or NULL. When set, the working copy names the
     directory, server, key, branch and store, and those fields go unused. */
  const char *workspace;
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
  /* Reads of part of a file that went to the server, and the stored bytes
     their transfers brought; `stored_bytes` counts whole-file transfers. */
  uint32_t reads;
  uint64_t read_bytes;
  /* Files vkr_depot_remote_prefetch queued. */
  uint32_t prefetched;
  /* Microseconds callers spent waiting in vkr_depot_remote_fetch and
     vkr_depot_remote_read, and from the end of opening to the newest file
     written and to the newest read served from the server. */
  uint64_t wait_us;
  uint64_t last_write_us;
  uint64_t last_read_us;
  VkrDepotId commit;
  VkrDepotId cooked_commit;
} VkrDepotRemoteStats;

typedef struct VkrDepotRemote VkrDepotRemote;

/* Connects, fetches the trees and starts the worker. NULL with a message.
   `allocator` makes the worker, its lock and signals on the calling thread;
   the connection the worker pumps allocates from the remote's own memory. */
VkrDepotRemote *vkr_depot_remote_open(VkrAllocator *allocator,
                                      const VkrDepotRemoteConfig *config,
                                      char *error, uint32_t capacity);

/* A VkrVfsRemoteCalls fetch: makes `relative` ('/'-separated, below the
   directory) exist and returns true, or false at once when no commit lists
   it, or when its download failed. Any thread. */
bool8_t vkr_depot_remote_fetch(void *remote, const char *relative);

/* A VkrVfsRemoteCalls read: copies `size` bytes at `offset` of
   `relative` into `destination`. A present file reads from the disk;
   otherwise only the chunks holding those bytes download, ahead of every
   fetch. Blocks; any thread. False when no commit lists the path, the
   range passes its end, or the download failed. */
bool8_t vkr_depot_remote_read(void *remote, const char *relative,
                              uint64_t offset, uint64_t size,
                              void *destination);

/* A VkrVfsRemoteCalls pending: whether a commit lists `relative` and it
   is not yet whole under the directory (and has not failed); its listed
   size in `out_size`. Never blocks on the network. */
bool8_t vkr_depot_remote_pending(void *remote, const char *relative,
                                 uint64_t *out_size);

/* Queues `relative`'s download behind every read and fetch and returns at
   once; nothing for an unlisted file or one already queued or present. */
void vkr_depot_remote_prefetch(void *remote, const char *relative);

/* Files written whole under the directory since opening; it only grows.
   Lock-free. */
uint64_t vkr_depot_remote_arrivals(void *remote);

/* The calls above as vkr_vfs_set_remote takes them, with the remote as
   their context. */
const VkrVfsRemoteCalls *vkr_depot_remote_vfs_calls(void);

void vkr_depot_remote_stats(VkrDepotRemote *remote,
                            VkrDepotRemoteStats *out_stats);

/* The directory files land below: a working copy's root when the remote
   opened on one. */
const char *vkr_depot_remote_directory(const VkrDepotRemote *remote);

/* Stops the worker after its transfer and disconnects. Clear the VFS remote
   first: a read that still asks would use freed memory. */
void vkr_depot_remote_close(VkrDepotRemote *remote);
