#pragma once

#include "editor_ui.h"

/* Source control through the asset depot (ADR-105, ADR-110, ADR-133), with
 * no command line and no settings to type:
 *
 * - A project is a repository when its workspace is a depot working copy.
 *   Make repository copies a project that is not one, with every file it
 *   uses, into a workspace of its own and commits it there.
 * - Commit records the local changes in the repository, with no network.
 *   Push, Pull and Sync exchange the branch with a remote: another
 *   computer's repository, found on the network or added by address and
 *   key. Pull merges when both sides moved the branch; scenes merge whole
 *   through Bakery, and a conflict is settled with Use theirs or Keep mine.
 * - Branches are made and switched from the Source Control window, which
 *   also lists the changes, the commits to push and to pull, and the
 *   remotes with their state.
 * - Let others fetch and push serves the repository to the network while
 *   this editor runs; other computers ask for access and are allowed with
 *   one click. Clone copies a repository another computer serves.
 *
 * Network and file work runs on one worker thread at a time; the server
 * runs on its own thread (vkr_depot_service.h). The UI thread only reads
 * finished results and progress counters. */

typedef struct VkrEditorDepot VkrEditorDepot;

VkrEditorDepot *vkr_editor_depot_create(VkrAllocator *allocator);
/* Stops the server and waits for a running step, which it cancels. */
void vkr_editor_depot_destroy(VkrEditorDepot *depot);

/* Once per UI build: follows the open workspace, starts or stops sharing,
   finishes steps and schedules status checks. */
void vkr_editor_depot_update(VkrEditorDepot *depot, VkrEditorUi *editor,
                             const VkrSampleUiFrame *frame);

/* The top bar's source control button in `column`: the branch, how far it
   is ahead and behind, and the local changes; it opens the Source Control
   window. */
void vkr_editor_depot_chip_build(VkrEditorDepot *depot, VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame,
                                 uint32_t column);

/* The Source Control window's body. */
void vkr_editor_depot_window_build(VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame,
                                   VkrUiRect bounds);

/* Floating requests for access and the step progress strip. */
void vkr_editor_depot_overlay_build(VkrEditorDepot *depot, VkrEditorUi *editor,
                                    const VkrSampleUiFrame *frame);

/* The project hub's repository part at (x, y) of `width` points: the
   repositories on this computer and those other computers serve, to clone.
   Returns the height it used. */
float32_t vkr_editor_depot_hub_build(VkrEditorDepot *depot, VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame, float32_t x,
                                     float32_t y, float32_t width);

/* Whether the scripts of the workspace at `folder` may build and run: true
   unless it is a copy of another computer's project whose user has not
   trusted its scripts, or a pull brought script changes since. */
bool8_t vkr_editor_depot_scripts_trusted(VkrEditorDepot *depot,
                                         const char *folder);
/* The user trusts the open shared project's scripts: they build and load. */
void vkr_editor_depot_trust_scripts(VkrEditorDepot *depot, VkrEditorUi *editor);

/* Whether the hub's Clone dialog is open, so the hub leaves the pointer to
   it. */
bool8_t vkr_editor_depot_dialog_open(const VkrEditorDepot *depot);

/* Whether a depot step (commit, pull, clone) is running and may be writing
   the open project's files. */
bool8_t vkr_editor_depot_busy(const VkrEditorDepot *depot);

// =============================================================================
// Agent operations (editor_ops.c)
// =============================================================================

struct VkrBakeryJson;
struct Arena;

/* The state as JSON: repository, branch and branches, ahead and behind,
   remotes, server, changes, conflicts, step and its progress, access
   requests and the servers found. */
struct VkrBakeryJson *vkr_editor_depot_status_json(VkrEditorDepot *depot,
                                                   struct Arena *arena);

/* Asset streaming (docs/proposals/network-protocol.md, "Asset
   streaming"): `directory` becomes a remote content directory the depot
   server at `address` fills on demand (vkr_depot_remote.h) and the
   workspace overlay, so every `assets/` read the branch lists comes from
   the server, cooked textures from its side branch for this computer. With
   a NULL `address`, `directory` is a sparse working copy's root, whose
   unfetched files come from the server it names; `key_hex` and `branch`
   go unused. Call before the scene loads. False with the reason; reads
   then stay local. */
bool8_t vkr_editor_depot_stream(VkrEditorDepot *depot, const char *address,
                                const char *key_hex, const char *directory,
                                const char *branch, bool8_t no_cooked,
                                char *error, uint32_t capacity);

/* The stream's commits, files listed, fetched and present, bytes and the
   time readers waited; `streaming` false without one. */
struct VkrBakeryJson *vkr_editor_depot_stream_json(VkrEditorDepot *depot,
                                                   struct Arena *arena);

/* Each starts a step and returns false with the reason when it cannot. */
/* Makes the open project a repository: copies it and every file it uses
   into `destination` (empty for ~/VKR Projects/<name>), commits it there
   and opens it; `serve` lets others fetch and push from the start. */
bool8_t vkr_editor_depot_init(VkrEditorDepot *depot, VkrEditorUi *editor,
                              const VkrSampleUiFrame *frame,
                              const char *destination, bool8_t serve,
                              char *error, uint32_t capacity);
/* Commits every local change with `message`; `sync` then pulls and pushes
   the branch with the remote. */
bool8_t vkr_editor_depot_commit(VkrEditorDepot *depot, const char *message,
                                bool8_t sync, char *error, uint32_t capacity);
/* `remote`: a remote's name, or NULL or empty for the selected one. */
bool8_t vkr_editor_depot_push(VkrEditorDepot *depot, const char *remote,
                              char *error, uint32_t capacity);
/* `policy`: "fail", "theirs" or "mine" for paths changed on both sides. */
bool8_t vkr_editor_depot_pull(VkrEditorDepot *depot, const char *remote,
                              const char *policy, char *error,
                              uint32_t capacity);
/* Pull, then push what is left to push. */
bool8_t vkr_editor_depot_sync(VkrEditorDepot *depot, const char *remote,
                              char *error, uint32_t capacity);
/* Fetches every remote's branches, so the window shows what to pull. */
bool8_t vkr_editor_depot_fetch(VkrEditorDepot *depot, char *error,
                               uint32_t capacity);
/* Switches to `branch`, making it from the current commit when `create`;
   the scene reloads once its edits are saved. */
bool8_t vkr_editor_depot_switch(VkrEditorDepot *depot, const char *branch,
                                bool8_t create, char *error, uint32_t capacity);
/* Adds or replaces a remote, or removes it when `address` is NULL. */
bool8_t vkr_editor_depot_remote(VkrEditorDepot *depot, const char *name,
                                const char *address, const char *key_hex,
                                char *error, uint32_t capacity);
/* Starts or stops serving the open repository to the network. */
bool8_t vkr_editor_depot_serve(VkrEditorDepot *depot, bool8_t serve,
                               char *error, uint32_t capacity);
/* Clones the repository at `address` with `key_hex` into `destination`
   (empty for the default folder) and opens it. `sparse` makes a sparse working
   copy (ADR-123): files, cooked textures included, arrive when the editor
   or its Bakery jobs read them, and the Get cooks nothing. */
bool8_t vkr_editor_depot_clone(VkrEditorDepot *depot, const char *address,
                               const char *key_hex, const char *name,
                               const char *destination, bool8_t sparse,
                               char *error, uint32_t capacity);

/* Makes `directory`, when it is a sparse depot working copy, a remote
   content directory whose unfetched files arrive when read, for this
   process and, through VKR_DEPOT_SPARSE_WORKSPACE_VARIABLE, for the Bakery
   jobs it starts; ends the previous one. NULL ends it. One per process, as
   the VFS remote is. Call before the workspace's files are read. */
void vkr_editor_depot_workspace_sparse(const char *directory);
/* Gives the key a role on this editor's server: "write", "read" or
   "none". */
bool8_t vkr_editor_depot_allow(VkrEditorDepot *depot, const char *key_hex,
                               const char *role, char *error,
                               uint32_t capacity);
/* Probes `address` for a server besides the broadcast. */
bool8_t vkr_editor_depot_find(VkrEditorDepot *depot, const char *address,
                              char *error, uint32_t capacity);
void vkr_editor_depot_cancel(VkrEditorDepot *depot);
