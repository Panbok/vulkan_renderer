#pragma once

#include "vkr_depot_store.h"

/* A depot working copy: a directory of files plus `.vkrdepot/`, which holds
 * the state file (remote, server key, branch, base commit and the index of
 * every tracked file with its size, modification time and blob) and, unless
 * another store is named, the local object store.
 *
 * A snapshot hashes only files whose size or modification time differ from
 * the index, chunks them with FastCDC into the store and builds the trees;
 * unchanged subtrees keep their IDs. A checkout writes each file that
 * differs through a temporary and a rename and removes tracked files the
 * target lacks. */

#define VKR_DEPOT_META_DIR ".vkrdepot"
#define VKR_DEPOT_REMOTE_MAX 256u

typedef struct VkrDepotIndexEntry {
  /* Portable relative path with '/' separators; malloc storage. */
  char *path;
  uint64_t size;
  int64_t mtime_ns;
  VkrDepotId blob;
} VkrDepotIndexEntry;

typedef struct VkrDepotFileList {
  VkrDepotIndexEntry *entries;
  uint32_t count;
  uint32_t capacity;
} VkrDepotFileList;

typedef struct VkrDepotWorkspace {
  char root[VKR_DEPOT_PATH_CAPACITY];
  VkrDepotStore store;
  char remote[VKR_DEPOT_REMOTE_MAX];
  uint8_t server_key[32];
  char branch[VKR_DEPOT_BRANCH_NAME];
  VkrDepotId base;
  /* Tracked files at `base`, sorted by path. */
  VkrDepotFileList index;
} VkrDepotWorkspace;

typedef enum VkrDepotChange {
  VKR_DEPOT_CHANGE_ADDED = 1,
  VKR_DEPOT_CHANGE_MODIFIED,
  VKR_DEPOT_CHANGE_DELETED,
} VkrDepotChange;

typedef bool8_t (*VkrDepotChangeVisitor)(void *context, const char *path,
                                         VkrDepotChange change);

void vkr_depot_file_list_free(VkrDepotFileList *list);
/* Adds a copy of `path`. */
bool8_t vkr_depot_file_list_add(VkrDepotFileList *list, const char *path,
                                uint64_t size, int64_t mtime_ns,
                                const VkrDepotId *blob);
void vkr_depot_file_list_sort(VkrDepotFileList *list);
const VkrDepotIndexEntry *vkr_depot_file_list_find(const VkrDepotFileList *list,
                                                   const char *path);

/* Creates `<root>/.vkrdepot` with an empty index. `store_root` NULL keeps
   the store inside it. */
bool8_t vkr_depot_workspace_create(VkrDepotWorkspace *workspace,
                                   const char *root, const char *store_root,
                                   const VkrDepotCodec *codec);
/* Opens the working copy at `root` or one of its parents. */
bool8_t vkr_depot_workspace_open(VkrDepotWorkspace *workspace, const char *root,
                                 const VkrDepotCodec *codec);
bool8_t vkr_depot_workspace_save(const VkrDepotWorkspace *workspace);
void vkr_depot_workspace_close(VkrDepotWorkspace *workspace);

/* Hashes the working directory into the store. Fills `out_files` with every
   file and its blob (the index after a commit of this snapshot) and returns
   the root tree. */
bool8_t vkr_depot_workspace_snapshot(VkrDepotWorkspace *workspace,
                                     VkrDepotId *out_tree,
                                     VkrDepotFileList *out_files);

/* Every file of `tree`, whose objects must be in the store. */
bool8_t vkr_depot_tree_files(const VkrDepotStore *store, const VkrDepotId *tree,
                             VkrDepotFileList *out_files);

/* Calls `visit` for each path whose blob differs between `from` and `to`. */
bool8_t vkr_depot_file_list_diff(const VkrDepotFileList *from,
                                 const VkrDepotFileList *to,
                                 VkrDepotChangeVisitor visit, void *context);

/* Writes `tree` into the working directory and makes it the index. Files the
   caller marks in `keep` (local changes) stay untouched. */
bool8_t vkr_depot_workspace_checkout(VkrDepotWorkspace *workspace,
                                     const VkrDepotId *tree,
                                     const VkrDepotFileList *keep);

/* Writes one file of the store to `path` through a temporary. */
bool8_t vkr_depot_write_blob(const VkrDepotStore *store, const VkrDepotId *blob,
                             const char *path);
