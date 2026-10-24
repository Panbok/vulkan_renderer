#pragma once

#include "vkr_depot_object.h"
#include "vkr_depot_protocol.h"

/* A depot repository is a store directory (vkr_depot_store.h) with its
 * branches (docs/proposals/depot-git-model.md, "Repository layout"):
 *
 *   <store>/refs/<branch>              the 32-byte commit ID of each branch
 *   <store>/repository                 the repository's 16-byte ID
 *   <store>/remotes                    a line per remote: name, address,
 *                                      service key, repository ID, interval
 *   <store>/tracking/<remote>/<branch> each remote branch as last fetched or
 *                                      pushed
 *
 * A branch moves only by compare-and-swap under the store's named process
 * lock, so the server that serves a store and a working copy that commits
 * into it can share it. Tracking branches and the remote list are written
 * under the same lock. */

/* Branches one listing returns at most. */
#define VKR_DEPOT_REPOSITORY_BRANCHES_MAX 256u
/* A remote's name with its terminator: 1 to 32 of a-z, 0-9, '-' and '_'. */
#define VKR_DEPOT_REMOTE_NAME 33u
#define VKR_DEPOT_REMOTE_ADDRESS 256u
#define VKR_DEPOT_REMOTES_MAX 16u
/* The remote a clone records, and the one commands use by default. */
#define VKR_DEPOT_ORIGIN "origin"

typedef struct VkrDepotRemoteEntry {
  char name[VKR_DEPOT_REMOTE_NAME];
  /* A numeric address or host name with an optional port. */
  char address[VKR_DEPOT_REMOTE_ADDRESS];
  /* The service's public key. */
  uint8_t key[32];
  /* The repository the service serves under this remote; zero for its
     default repository. */
  uint8_t repository[VKR_DEPOT_REPOSITORY_ID_SIZE];
  /* Seconds between automatic fetch and push; zero for none. */
  uint32_t interval_seconds;
} VkrDepotRemoteEntry;

typedef enum VkrDepotRefSwap {
  VKR_DEPOT_REF_SWAP_OK = 0,
  /* The branch is not where the caller expected; `out_current` holds where
     it is (zero when it does not exist). */
  VKR_DEPOT_REF_SWAP_MOVED,
  /* Another process held the store's lock for the whole wait. */
  VKR_DEPOT_REF_SWAP_BUSY,
  /* An invalid branch name, or the write failed. */
  VKR_DEPOT_REF_SWAP_FAILED,
} VkrDepotRefSwap;

/* Reads a branch; false when it does not exist. */
bool8_t vkr_depot_ref_read(const char *store_root, const char *branch,
                           VkrDepotId *out_commit);

/* Moves `branch` from `expected` (zero: the branch must not exist) to
   `commit` (zero deletes the branch) under the store's lock, waiting for it
   up to about two seconds.
   `out_current` (may be NULL) receives where the branch is after the call. */
VkrDepotRefSwap vkr_depot_ref_swap(const char *store_root, const char *branch,
                                   const VkrDepotId *expected,
                                   const VkrDepotId *commit,
                                   VkrDepotId *out_current);

/* Calls `visit` with each valid branch name, in directory order, at most
   VKR_DEPOT_REPOSITORY_BRANCHES_MAX; returning false stops the walk. A store
   without branches visits none and returns true. */
typedef bool8_t (*VkrDepotBranchVisitor)(void *context, const char *branch);
bool8_t vkr_depot_ref_list(const char *store_root, VkrDepotBranchVisitor visit,
                           void *context);

/* The repository's ID, made from random bytes when missing and `create`. */
bool8_t vkr_depot_repository_id(const char *store_root,
                                uint8_t out[VKR_DEPOT_REPOSITORY_ID_SIZE],
                                bool8_t create);

/* The public half of `<store>/server.key` when the store has one (it is, or
   was, served); never creates the key. */
bool8_t vkr_depot_repository_served_key(const char *store_root,
                                        uint8_t out_public[32]);

bool8_t vkr_depot_remote_name_valid(const char *name);

/* The store's remotes in file order; a store without the file has none. */
bool8_t vkr_depot_remotes_load(const char *store_root,
                               VkrDepotRemoteEntry *out_remotes,
                               uint32_t capacity, uint32_t *out_count);
/* Finds a remote by name; NULL `name` finds `origin`, else the only one. */
bool8_t vkr_depot_remote_find(const char *store_root, const char *name,
                              VkrDepotRemoteEntry *out_remote);
/* Adds a remote or replaces the one with its name. */
bool8_t vkr_depot_remote_set(const char *store_root,
                             const VkrDepotRemoteEntry *remote);
/* Removes a remote and its tracking branches; false when there is none. */
bool8_t vkr_depot_remote_remove(const char *store_root, const char *name);

/* Where `remote`'s `branch` was at the last fetch or push; false when
   unknown. */
bool8_t vkr_depot_tracking_read(const char *store_root, const char *remote,
                                const char *branch, VkrDepotId *out_commit);
/* Records `commit` for `remote`'s `branch`; zero removes the record. */
bool8_t vkr_depot_tracking_set(const char *store_root, const char *remote,
                               const char *branch, const VkrDepotId *commit);
/* Calls `visit` with each tracked branch of `remote`. */
bool8_t vkr_depot_tracking_list(const char *store_root, const char *remote,
                                VkrDepotBranchVisitor visit, void *context);
