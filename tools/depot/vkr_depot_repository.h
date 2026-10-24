#pragma once

#include "vkr_depot_object.h"

/* A depot repository is a store directory (vkr_depot_store.h) with its
 * branches (docs/proposals/depot-git-model.md, "Repository layout"):
 *
 *   <store>/refs/<branch>  the 32-byte commit ID of each branch
 *
 * A branch moves only by compare-and-swap under the store's named process
 * lock, so the server that serves a store and a working copy that commits
 * into it can share it. */

/* Branches one listing returns at most. */
#define VKR_DEPOT_REPOSITORY_BRANCHES_MAX 256u

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
   `commit` under the store's lock, waiting for it up to about two seconds.
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
