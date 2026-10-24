#pragma once

#include "vkr_depot_progress.h"
#include "vkr_depot_store.h"

/* A depot working copy: a directory of files plus `.vkrdepot/`, which holds
 * the state file (branch, base commit and the index of every tracked file
 * with its size, modification time and blob) and, unless another store is
 * named, the local object store, which is the repository: its remote
 * `origin` is the server the working copy came from (ADR-133).
 *
 * A snapshot hashes only files whose size or modification time differ from
 * the index, chunks them with FastCDC into the store and builds the trees;
 * unchanged subtrees keep their IDs. A checkout writes each file that
 * differs through a temporary and a rename and removes tracked files the
 * target lacks.
 *
 * `.vkrdepotignore` at the root lists paths a snapshot leaves out, one
 * pattern per line ('#' starts a comment): '*' and '?' match within a name,
 * '**' across folders, a trailing '/' matches a folder and all it holds,
 * and a pattern without '/' matches a name at any depth. As in git, the
 * rules apply to untracked paths only: a tracked file stays in snapshots
 * whatever a pulled `.vkrdepotignore` says. A checkout never writes or
 * removes an ignored path, so another machine's tree cannot plant files
 * this machine keeps for itself (such as scripts it builds and loads).
 *
 * A sparse working copy tracks every file of its base but holds on disk only
 * those it fetched: an index entry whose `mtime_ns` is
 * VKR_DEPOT_MTIME_UNFETCHED names a file that is not on disk and still holds
 * its blob. A snapshot keeps such an entry while its file is absent, so
 * only a file that was on disk and is gone counts as deleted. A checkout
 * writes only the files on disk and the root `.vkrdepotignore` and makes
 * every folder of the target; every other file of the target stays
 * unfetched with the target's blob. The files
 * arrive when a reader opens them, through a remote content directory on
 * the working copy (vkr_depot_remote.h). */

#define VKR_DEPOT_META_DIR ".vkrdepot"
#define VKR_DEPOT_IGNORE_FILE ".vkrdepotignore"
#define VKR_DEPOT_IGNORE_MAX 128u
#define VKR_DEPOT_REMOTE_MAX 256u
/* Platform files a computer cooks for its own GPU family, which the ignore
   rules keep out of the project's history and a cooked side branch carries
   (vkr_depot_publish_cooked). */
#define VKR_DEPOT_COOKED_SUFFIX ".vkt"
/* VkrDepotIndexEntry.mtime_ns of a file a sparse working copy has not
   fetched; -1 marks a file a checkout wrote that no snapshot has hashed. */
#define VKR_DEPOT_MTIME_UNFETCHED (-2)
/* `.vkrdepot/fetched`: a line `<blob> <path>` for each file a remote wrote
   into a sparse working copy, so a snapshot tells a fetched file that was
   deleted from one never fetched. A commit or checkout clears it. */
#define VKR_DEPOT_FETCHED_LOG "fetched"

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
  char branch[VKR_DEPOT_BRANCH_NAME];
  VkrDepotId base;
  /* Tracked files at `base`, sorted by path. */
  VkrDepotFileList index;
  /* Holds on disk only the files it fetched; saved with the state. */
  bool8_t sparse;
  /* Borrowed; NULL for none. Open and create clear it. Snapshot, checkout
     and the sync steps report through it and stop when it is canceled. */
  VkrDepotProgress *progress;
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
                                   const VkrCodec *codec);
/* Opens the working copy at `root` or one of its parents. */
bool8_t vkr_depot_workspace_open(VkrDepotWorkspace *workspace, const char *root,
                                 const VkrCodec *codec);
bool8_t vkr_depot_workspace_save(const VkrDepotWorkspace *workspace);
void vkr_depot_workspace_close(VkrDepotWorkspace *workspace);

/* Hashes the working directory into the store. Fills `out_files` with every
   file and its blob (the index after a commit of this snapshot) and returns
   the root tree. A cancel stops it with false; objects it stored stay. */
bool8_t vkr_depot_workspace_snapshot(VkrDepotWorkspace *workspace,
                                     VkrDepotId *out_tree,
                                     VkrDepotFileList *out_files);

/* As vkr_depot_workspace_snapshot, but of the untracked platform files only
   (VKR_DEPOT_COOKED_SUFFIX), wherever the ignore rules leave their folder
   walked; the tracked files and the index stay out of it. */
bool8_t vkr_depot_workspace_snapshot_cooked(VkrDepotWorkspace *workspace,
                                            VkrDepotId *out_tree,
                                            VkrDepotFileList *out_files);

/* The cooked files with the size and time they had when last hashed or
   written (`.vkrdepot/cooked`), which a cooked snapshot keeps the blob of
   while they stay so. A missing or torn cache loads empty. */
bool8_t vkr_depot_cooked_cache_load(const VkrDepotWorkspace *workspace,
                                    VkrDepotFileList *out_files);
bool8_t vkr_depot_cooked_cache_save(const VkrDepotWorkspace *workspace,
                                    const VkrDepotFileList *files);

/* Whether `path` ('/' separators, relative to the root) matches the
   ignore `pattern`; `is_directory` for a folder. */
bool8_t vkr_depot_ignore_match(const char *pattern, const char *path,
                               bool8_t is_directory);

/* Every file of `tree`, whose objects must be in the store. */
bool8_t vkr_depot_tree_files(const VkrDepotStore *store, const VkrDepotId *tree,
                             VkrDepotFileList *out_files);

/* Calls `visit` for each path whose blob differs between `from` and `to`. */
bool8_t vkr_depot_file_list_diff(const VkrDepotFileList *from,
                                 const VkrDepotFileList *to,
                                 VkrDepotChangeVisitor visit, void *context);

/* Writes `tree` into the working directory and makes it the index. Files the
   caller marks in `keep` (local changes) stay untouched. Written files are not
   flushed; their index entries carry -1, so the next snapshot hashes them.
   A cancel stops it with false and the index unchanged: files written so far
   show as local changes that equal the tree, which a later checkout of the
   same tree takes over.
   Paths are compared as macOS and Windows do (vkr_depot_name_fold_compare):
   a local change kept under one case keeps every case of the path, and
   removals come before writes, so a case-only rename keeps the file. A path
   the working copy's ignore rules match, as they stand before the
   checkout, is never written or removed and stays untracked; `out_skipped`
   (may be NULL) counts the tree's paths skipped so. */
bool8_t vkr_depot_workspace_checkout(VkrDepotWorkspace *workspace,
                                     const VkrDepotId *tree,
                                     const VkrDepotFileList *keep,
                                     uint32_t *out_skipped);

/* Records, for each index entry still marked -1 whose file `files` (a
   snapshot) found holding the same blob, the size and time it hashed, so
   later snapshots skip it. Returns how many entries changed; the caller
   saves the working copy. */
uint32_t vkr_depot_workspace_settle(VkrDepotWorkspace *workspace,
                                    const VkrDepotFileList *files);

/* Records that a remote wrote `path` (relative, '/'-separated) with `blob`
   into the sparse working copy at `root`. Any thread; one append per file. */
bool8_t vkr_depot_workspace_note_fetched(const char *root, const char *path,
                                         const VkrDepotId *blob);
/* Empties the fetched log once an index recorded what it says: after a
   commit or a checkout. */
bool8_t vkr_depot_workspace_clear_fetched(const VkrDepotWorkspace *workspace);

/* Writes one file of the store to `path` through a temporary. */
bool8_t vkr_depot_write_blob(const VkrDepotStore *store, const VkrDepotId *blob,
                             const char *path);

/* Copies `size` bytes at `offset` of the file `blob` names into
   `destination`, from the chunks that hold them; the store may lack the
   file's other chunks. False when it lacks the blob or one of those chunks,
   or the range passes the file's end. */
bool8_t vkr_depot_read_blob_range(const VkrDepotStore *store,
                                  const VkrDepotId *blob, uint64_t offset,
                                  uint64_t size, uint8_t *destination);
