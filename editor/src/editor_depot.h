#pragma once

#include "editor_ui.h"

/* Project sharing through the asset depot (ADR-105, ADR-106), with no
 * command line and no settings to type:
 *
 * - Share copies the open project and every file it uses into a workspace
 *   of its own, makes that folder a depot working copy and serves it to the
 *   local network from this editor while it is open. Other computers find
 *   it, ask for access, and the sharing editor allows them with one click.
 * - Get copies a project another editor shares into a new folder, prepares
 *   its textures for this computer and opens it.
 * - In a shared workspace the editor counts local changes and new commits,
 *   submits with a message and gets the latest commit, then reloads the
 *   scene and cooks the textures the pull brought.
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

/* The top bar's sharing button in `column`: Share, or the state of a shared
   project; it opens the Project sharing window. */
void vkr_editor_depot_chip_build(VkrEditorDepot *depot, VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame,
                                 uint32_t column);

/* The Project sharing window's body. */
void vkr_editor_depot_window_build(VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame,
                                   VkrUiRect bounds);

/* Floating requests for access and the step progress strip. */
void vkr_editor_depot_overlay_build(VkrEditorDepot *depot, VkrEditorUi *editor,
                                    const VkrSampleUiFrame *frame);

/* The project hub's network part at (x, y) of `width` points: projects
   other editors share and the shared workspaces on this computer. Returns
   the height it used. */
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

/* Whether the hub's Get dialog is open, so the hub leaves the pointer to
   it. */
bool8_t vkr_editor_depot_dialog_open(const VkrEditorDepot *depot);

/* Whether a depot step (sync, submit, clone) is running and may be writing
   the open project's files. */
bool8_t vkr_editor_depot_busy(const VkrEditorDepot *depot);

// =============================================================================
// Agent operations (editor_ops.c)
// =============================================================================

struct VkrBakeryJson;
struct Arena;

/* The state as JSON: role, server, changes, incoming, step and its
   progress, access requests and the servers found. */
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
bool8_t vkr_editor_depot_share(VkrEditorDepot *depot, VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame,
                               const char *destination, char *error,
                               uint32_t capacity);
bool8_t vkr_editor_depot_submit(VkrEditorDepot *depot, const char *message,
                                char *error, uint32_t capacity);
/* `policy`: "fail", "theirs" or "mine" for paths changed on both sides. */
bool8_t vkr_editor_depot_get_latest(VkrEditorDepot *depot, const char *policy,
                                    char *error, uint32_t capacity);
/* Gets the project at `address` with `key_hex` into `destination` (empty
   for the default folder) and opens it. `sparse` makes a sparse working
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
