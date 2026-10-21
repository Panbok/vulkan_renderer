#pragma once

#include "vkr_depot_client.h"
#include "vkr_depot_workspace.h"

/* A working copy's exchanges with its depot server, for the vkr_depot
 * command and the editor (ADR-105, ADR-106): the user's identity key and
 * display name, a client connection over its own UDP host, and the clone,
 * status, log, commit, push, fetch and checkout steps. Every call that
 * connects blocks on the network; one thread uses a connection and a
 * working copy at a time. A long step reports through the working copy's
 * progress (vkr_depot_progress.h), which another thread reads and may
 * cancel. */

#define VKR_DEPOT_DEFAULT_PORT 7320u
#define VKR_DEPOT_TIMEOUT_MS 30000u

/* Where the user's identity key lives: `$VKR_DEPOT_IDENTITY`, else
   `<user data>/depot/identity.key` (vkr_bakery_user_data_directory). */
bool8_t vkr_depot_identity_path(char *out, uint32_t capacity);

/* The user's identity key, made when missing and `create`. A key missing
   from the default path is first copied from the user cache folder that
   held it before (`<user cache>/vkr/depot/identity.key`), so access lists
   that name it stay valid. */
bool8_t vkr_depot_identity(VkrNetKeyPair *out, bool8_t create);

/* The name a client shows a server: `<user>@<computer>`, cleaned as
   vkr_depot_display_name_clean cleans a credential. */
void vkr_depot_display_name(char out[VKR_DEPOT_DISPLAY_NAME]);

/* A numeric address or a host name, with an optional port (default
   VKR_DEPOT_DEFAULT_PORT). */
bool8_t vkr_depot_address(const char *text, VkrNetAddress *out);

typedef struct VkrDepotConnection VkrDepotConnection;

typedef enum VkrDepotConnectStatus {
  VKR_DEPOT_CONNECT_OK = 0,
  /* An invalid address or no identity key. */
  VKR_DEPOT_CONNECT_INVALID,
  /* No answer in time: a wrong address or port, or a firewall. */
  VKR_DEPOT_CONNECT_UNREACHABLE,
  /* The server does not know this key yet. It recorded an access request
     with the display name; retry once its owner gives the key a role. */
  VKR_DEPOT_CONNECT_DENIED,
  /* Refused for another reason; the message names the code. */
  VKR_DEPOT_CONNECT_REFUSED,
  VKR_DEPOT_CONNECT_CANCELED,
} VkrDepotConnectStatus;

typedef struct VkrDepotConnectConfig {
  /* A numeric address or host name with an optional port. */
  const char *address;
  /* The server's public key, 32 bytes. */
  const uint8_t *server_key;
  /* Where fetched objects go; NULL for a connection that only lists refs
     and locks. */
  const VkrDepotStore *store;
  /* The display name sent as the credential; NULL for
     vkr_depot_display_name. */
  const char *name;
  /* How long to wait for the handshake; zero for VKR_DEPOT_TIMEOUT_MS. */
  uint32_t timeout_ms;
  /* Borrowed; NULL for none. The handshake wait stops when it is
     canceled, and the connection's client reports through it. */
  VkrDepotProgress *progress;
} VkrDepotConnectConfig;

/* Connects with the user's identity and opens a client. NULL with a
   message; `out_status` (may be NULL) says why. */
VkrDepotConnection *vkr_depot_connect(VkrAllocator *allocator,
                                      const VkrDepotConnectConfig *config,
                                      VkrDepotConnectStatus *out_status,
                                      char *error, uint32_t capacity);
VkrDepotClient *vkr_depot_connection_client(VkrDepotConnection *connection);
void vkr_depot_disconnect(VkrDepotConnection *connection);

/* The server's branches over a connection of its own, for a caller that has
   no working copy yet. */
bool8_t vkr_depot_branches(VkrAllocator *allocator,
                           const VkrDepotConnectConfig *config,
                           VkrDepotRef *out_refs, uint32_t capacity,
                           uint32_t *out_count,
                           VkrDepotConnectStatus *out_status, char *error,
                           uint32_t error_capacity);

/* Where `branch` points on the server; `*out_exists` false for a branch with
   no commit yet. */
bool8_t vkr_depot_find_ref(VkrDepotClient *client, const char *branch,
                           VkrDepotId *out_commit, bool8_t *out_exists);

typedef struct VkrDepotCloneConfig {
  const char *address;
  const uint8_t *server_key;
  /* The working copy's root, made when missing. */
  const char *directory;
  /* NULL for "main". */
  const char *branch;
  /* A store to share, such as the server's own beside it; NULL keeps one
     inside the working copy. */
  const char *store_root;
  /* NULL for vkr_depot_display_name. */
  const char *name;
  /* The store's compressor; NULL stores raw only. */
  const VkrCodec *codec;
  /* Borrowed; NULL for none. */
  VkrDepotProgress *progress;
  /* A sparse working copy (vkr_depot_workspace.h): the trees and the root
     `.vkrdepotignore` arrive, every other file when a reader opens it. A
     working copy the call continues keeps what it is. */
  bool8_t sparse;
} VkrDepotCloneConfig;

typedef struct VkrDepotCloneResult {
  VkrDepotConnectStatus connect;
  /* The branch has no commit yet: the first commit creates it. */
  bool8_t empty;
  VkrDepotId commit;
  uint32_t files;
} VkrDepotCloneResult;

/* Makes a working copy of `branch` at `directory`: saves its remote, key and
   branch first, then connects, fetches and checks out. A directory that
   already holds a working copy continues it, so a clone refused, canceled
   or cut off is retried with the same call; `pull` also completes it. */
bool8_t vkr_depot_clone(VkrAllocator *allocator,
                        const VkrDepotCloneConfig *config,
                        VkrDepotCloneResult *out_result, char *error,
                        uint32_t capacity);

typedef struct VkrDepotChangeEntry {
  /* malloc storage. */
  char *path;
  VkrDepotChange change;
} VkrDepotChangeEntry;

/* Paths with their change, in path order. */
typedef struct VkrDepotChangeList {
  VkrDepotChangeEntry *entries;
  uint32_t count;
  uint32_t capacity;
} VkrDepotChangeList;

void vkr_depot_change_list_free(VkrDepotChangeList *list);

/* The working copy's local changes since its base: a snapshot (honouring
   the progress) diffed against the index. Saves the times of files a
   checkout wrote that the snapshot verified, so later snapshots skip
   them. */
bool8_t vkr_depot_status(VkrDepotWorkspace *workspace,
                         VkrDepotChangeList *out_changes, char *error,
                         uint32_t capacity);

typedef struct VkrDepotLogEntry {
  VkrDepotId id;
  /* The first parent; zero for a branch's first commit. */
  VkrDepotId parent;
  uint8_t author[32];
  /* Unix microseconds. */
  uint64_t time_us;
  /* Borrowed for the visit; not terminated. */
  const char *message;
  uint32_t message_size;
} VkrDepotLogEntry;

/* Returning false stops the walk. */
typedef bool8_t (*VkrDepotLogVisitor)(void *context,
                                      const VkrDepotLogEntry *entry);

/* Walks first parents from `start` through the commits `store` holds, at
   most `count`; returns how many it visited. The walk ends at a commit the
   store lacks: a working copy holds the history it fetched. */
uint32_t vkr_depot_log(const VkrDepotStore *store, const VkrDepotId *start,
                       uint32_t count, VkrDepotLogVisitor visit, void *context);

/* A commit made in the working copy's store and not pushed yet, with the
   snapshot's files, which become the index once it is pushed. */
typedef struct VkrDepotPendingCommit {
  VkrDepotId commit;
  VkrDepotFileList files;
  /* What vkr_depot_commit_push uploaded. */
  VkrDepotTransferStats uploaded;
} VkrDepotPendingCommit;

/* Snapshots the working directory and makes a commit with `parent` (none
   when zero). Needs no connection: a snapshot of a large file outlasts the
   transport's idle timeout, so connect after it. Honours the working
   copy's progress. */
bool8_t vkr_depot_commit_snapshot(VkrDepotWorkspace *workspace,
                                  const VkrDepotId *parent, const char *message,
                                  VkrDepotPendingCommit *out_pending,
                                  char *error, uint32_t capacity);
void vkr_depot_pending_commit_free(VkrDepotPendingCommit *pending);

/* Uploads what the server lacks of `pending` and moves `branch` from
   `expected` (zero: the branch must not exist) to it. On success the commit
   is the working copy's base and `branch` its branch, saved. A branch that
   moved meanwhile fails with a message naming where it is. The upload
   honours the working copy's progress; a cancel moves no branch. */
bool8_t vkr_depot_commit_push(VkrDepotWorkspace *workspace,
                              VkrDepotClient *client, const char *branch,
                              const VkrDepotId *parent,
                              const VkrDepotId *expected,
                              VkrDepotPendingCommit *pending, char *error,
                              uint32_t capacity);

/* `<branch>.cooked.<family>`: `bc` for BC7 and BC5 blocks, `astc` on Apple
   GPUs, as the texture producer encodes for this computer. */
void vkr_depot_cooked_branch(const char *branch, char *out, uint32_t capacity);

/* A cooked commit's message: this, then the full id of the project commit
   its files were cooked for. */
#define VKR_DEPOT_COOKED_MESSAGE "Cooked for "

typedef struct VkrDepotCookedResult {
  /* `<branch>.cooked.<family>` (vkr_depot_cooked_branch). */
  char branch[VKR_DEPOT_BRANCH_NAME];
  VkrDepotId commit;
  uint32_t files;
  uint64_t uploaded_bytes;
  /* The side branch held these files already; nothing was committed. */
  bool8_t unchanged;
} VkrDepotCookedResult;

typedef struct VkrDepotCookedFetch {
  char branch[VKR_DEPOT_BRANCH_NAME];
  /* The side branch's newest commit; zero when the server has none. */
  VkrDepotId commit;
  /* Cooked files it lists, those already here, those cooked from a source
     this working copy changed, and those written. */
  uint32_t listed;
  uint32_t present;
  uint32_t stale;
  uint32_t written;
  uint64_t stored_bytes;
} VkrDepotCookedFetch;

/* Writes the cooked files of `branch`'s side branch for this computer into
   the working copy, before a cook would make them. A file already here
   stays; a `<source>.vkt` whose source this working copy holds changed
   since the commit it was cooked for stays out, so the cook makes it; a
   path the index tracks is never written. Written files join the cooked
   cache. True, with nothing written, when the server has no side branch. */
bool8_t vkr_depot_fetch_cooked(VkrDepotWorkspace *workspace,
                               VkrDepotClient *client, const char *branch,
                               VkrDepotCookedFetch *out_fetch, char *error,
                               uint32_t capacity);

/* Hashes the working copy's cooked platform files into its store and
   returns their tree. Needs no connection: hashing hundreds of megabytes
   outlasts the transport's idle timeout, so connect after it. False with a
   message, also when there are none. */
bool8_t vkr_depot_cooked_snapshot(VkrDepotWorkspace *workspace,
                                  VkrDepotId *out_tree, uint32_t *out_files,
                                  char *error, uint32_t capacity);

/* Publishes a cooked snapshot's `tree` of `files` files to the cooked side
   branch of `branch` for this computer's GPU family, so a streaming client
   of the same family (vkr_depot_remote.h) fetches them instead of cooking.
   The commit holds only those files, names the working copy's base in its
   message and follows the side branch's newest commit; the project's
   history and the working copy's index stay as they are. */
bool8_t vkr_depot_publish_cooked(VkrDepotWorkspace *workspace,
                                 VkrDepotClient *client, const char *branch,
                                 const VkrDepotId *tree, uint32_t files,
                                 VkrDepotCookedResult *out_result, char *error,
                                 uint32_t capacity);

typedef enum VkrDepotMirrorAction {
  VKR_DEPOT_MIRROR_SAME = 0,
  /* The peer moved to this server's commit. */
  VKR_DEPOT_MIRROR_TO_PEER,
  /* This server moved to the peer's commit. */
  VKR_DEPOT_MIRROR_FROM_PEER,
  /* Each side has commits the other lacks: a working copy of one server
     takes the other's change over by hand and commits, and the next mirror
     fast-forwards the other. */
  VKR_DEPOT_MIRROR_DIVERGED,
  VKR_DEPOT_MIRROR_FAILED,
} VkrDepotMirrorAction;

typedef void (*VkrDepotMirrorVisit)(void *context, const char *branch,
                                    VkrDepotMirrorAction action,
                                    const VkrDepotId *commit,
                                    const char *detail);

/* Makes the branches of two depot servers equal: the local server at
   `local_address`, which serves `store`, and the peer at `peer_address`.
   A branch on one side only is created on the other; a branch that one side
   moved past the other's commit fast-forwards there. Objects go through
   `store`, which the local server shares, and each branch moves only by
   the server's compare-and-swap. `branch` NULL mirrors every branch. Calls
   `visit` per branch; false with a message when a server is unreachable. */
bool8_t vkr_depot_mirror(VkrAllocator *allocator, const VkrDepotStore *store,
                         const char *local_address, const uint8_t local_key[32],
                         const char *peer_address, const uint8_t peer_key[32],
                         const char *branch, VkrDepotMirrorVisit visit,
                         void *context, char *error, uint32_t capacity);

/* What a checkout does with a path changed both here and in the commit
   to different content. */
typedef enum VkrDepotConflictPolicy {
  /* Change nothing and fail. */
  VKR_DEPOT_CONFLICT_FAIL = 0,
  /* The commit's content replaces the local change. */
  VKR_DEPOT_CONFLICT_TAKE_THEIRS,
  /* The local content stays over the new base, so it shows as a local
     change and the next commit carries it. */
  VKR_DEPOT_CONFLICT_KEEP_MINE,
} VkrDepotConflictPolicy;

/* Fetches `commit` and checks it out over the working directory. Files
   changed here since the base stay as they are; a path the commit changed
   too, to other content, is a conflict that `policy` resolves.
   `out_conflicts` (may be NULL) lists the conflicting paths with their
   local change: under FAIL the call then fails without changing anything.
   On success the commit is the working copy's base, saved; `out_kept`
   counts the local changes kept. Snapshot, fetch and checkout honour the
   working copy's progress; a cancel leaves the base where it was. A sparse
   working copy fetches the commit's trees and only the files its checkout
   writes: those on disk that the commit changed, and the root ignore
   rules. */
bool8_t vkr_depot_fetch_checkout(
    VkrDepotWorkspace *workspace, VkrDepotClient *client,
    const VkrDepotId *commit, VkrDepotConflictPolicy policy, uint32_t *out_kept,
    VkrDepotChangeList *out_conflicts, char *error, uint32_t capacity);
