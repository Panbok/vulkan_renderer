#pragma once

#include "core/vkr_atomic.h"
#include "level/vkr_heightfield.h"
#include "renderer/systems/vkr_scene_collision_layers.h"
#include "renderer/systems/vkr_scene_partition.h"
#include "renderer/systems/vkr_scene_physics.h"
#include "renderer/systems/vkr_scene_system.h"
#include "renderer/systems/vkr_scene_types.h"

#define VKR_SCENE_EDIT_NAME_CAPACITY 512u
#define VKR_SCENE_EDIT_UNDO_CAPACITY 4096u
/* Entries one journal group may hold, as many as one agent batch
   (VKR_SAMPLE_EDIT_BATCH_MAX); a larger group fails and rolls back. */
#define VKR_SCENE_EDIT_GROUP_MAX 2048u
/* Undo payload bytes one journal keeps. Past it, or when its allocator is
   full, the oldest steps leave before an edit fails; only the open group
   never leaves. It lets VKR_SCENE_EDIT_UNDO_CAPACITY entity entries (two
   17 KiB value snapshots each) stay, so it binds on terrain strokes. */
#define VKR_SCENE_EDIT_HISTORY_BYTES MB(256)
/* Created objects one overlay or cell document holds. Load rejects a larger
   document, so save refuses to write one. */
#define VKR_SCENE_EDIT_CREATED_MAX 131072u
/* Bytes of an overlay or cell document a load reads, room for
   VKR_SCENE_EDIT_CREATED_MAX objects of a brush face's size (about 450
   bytes each); save refuses to write a larger one, and the project jobs read
   overlays up to it (VKR_PROJECT_MAX_OVERLAY_BYTES). */
#define VKR_SCENE_EDIT_OVERLAY_BYTES MB(128)

typedef enum VkrSceneEditAction {
  VKR_SCENE_EDIT_NONE,
  VKR_SCENE_EDIT_SELECT,
  VKR_SCENE_EDIT_APPLY,
  VKR_SCENE_EDIT_APPLY_COLLISION_LAYERS,
  VKR_SCENE_EDIT_APPLY_PHYSICS_BATCH,
  VKR_SCENE_EDIT_UNDO,
  VKR_SCENE_EDIT_REDO,
  VKR_SCENE_EDIT_SAVE,
  VKR_SCENE_EDIT_FRAME,
  VKR_SCENE_EDIT_LOAD,
  VKR_SCENE_EDIT_UNLOAD,
  VKR_SCENE_EDIT_RELOAD,
  /* Structure (ADR-076): values.component_type names the component. */
  VKR_SCENE_EDIT_ADD_COMPONENT,
  VKR_SCENE_EDIT_REMOVE_COMPONENT,
  /* Replace `replaced_type` with values.component_type in one undo entry. */
  VKR_SCENE_EDIT_REPLACE_COMPONENT,
  /* Create under `parent`, or at the root of `container`, from `values`. */
  VKR_SCENE_EDIT_CREATE,
  VKR_SCENE_EDIT_DELETE,
  /* Copy `entity` and its descendants beside it. */
  VKR_SCENE_EDIT_DUPLICATE,
  /* Move `entity` under `parent`; an invalid parent makes it a root. */
  VKR_SCENE_EDIT_REPARENT,
  /* Scene-level settings of `container` from `scene_settings`. */
  VKR_SCENE_EDIT_APPLY_SCENE_SETTINGS,
  /* `terrain`, in world space, on `entity`'s terrain (ADR-084). */
  VKR_SCENE_EDIT_TERRAIN,
  /* World partition (ADR-086): load and pin the open scene's cells in
     `partition_cells`, or with `partition_unload` unpin and unload them. */
  VKR_SCENE_EDIT_PARTITION,
  /* Batch only: record the document entities from index `placement_first`
     on, which an add_entities job appended and the reload loaded, as
     created (vkr_scene_edit_record_placement). */
  VKR_SCENE_EDIT_RECORD_PLACEMENT,
} VkrSceneEditAction;

typedef enum VkrSceneEditField {
  VKR_SCENE_EDIT_TRANSFORM = 1u << 0,
  VKR_SCENE_EDIT_NAME = 1u << 1,
  VKR_SCENE_EDIT_VISIBILITY = 1u << 2,
  VKR_SCENE_EDIT_POINT_LIGHT = 1u << 3,
  VKR_SCENE_EDIT_DIRECTIONAL_LIGHT = 1u << 4,
  VKR_SCENE_EDIT_RECTANGLE_LIGHT = 1u << 5,
  VKR_SCENE_EDIT_PHYSICS = 1u << 6,
  /* One descriptor-typed world component (ADR-076). */
  VKR_SCENE_EDIT_COMPONENT = 1u << 7,
} VkrSceneEditField;

typedef struct VkrSceneEditValues {
  uint32_t fields;
  char name[VKR_SCENE_EDIT_NAME_CAPACITY];
  Vec3 position;
  VkrQuat rotation;
  Vec3 scale;
  SceneVisibility visibility;
  ScenePointLight point_light;
  SceneDirectionalLight directional_light;
  SceneRectangleLight rectangle_light;
  VkrScenePhysicsSnapshot physics;
  /* VKR_SCENE_EDIT_COMPONENT: a registered world type and its bytes. */
  const VkrTypeDesc *component_type;
  _Alignas(16) uint8_t component[VKR_TYPE_VALUE_MAX];
  /* Creation only: the new entity's document-stable id, chosen ahead so a
     batch can reference it; zero makes a fresh one. DUPLICATE: the seed of
     the copies' ids; zero makes fresh ones. Reads leave it zero. */
  VkrEntityRef ref;
} VkrSceneEditValues;

typedef struct VkrSceneEditRequest {
  VkrSceneEditAction action;
  VkrEntityId entity;
  VkrSceneEditValues values;
  /* Borrowed until runtime dispatch after this UI build. */
  const VkrSceneCollisionLayers *collision_layers;
  const VkrScenePhysicsChange *physics_batch;
  uint32_t physics_batch_count;
  /* Nonzero groups consecutive APPLY requests of one continuous gesture, such
   * as a slider or value scrub, into a single undo entry. */
  uint64_t gesture;
  /* CREATE and REPARENT: the new parent, or invalid for a root. */
  VkrEntityId parent;
  /* CREATE without a parent: 0 for the primary scene, 1 to
   * VKR_SCENE_ADDITIVE_MAX for an added scene, VKR_SCENE_WORLD_ROOT_ID for the
   * World. */
  uint16_t container;
  VkrSceneSettings scene_settings;
  /* REPLACE_COMPONENT: the component type that leaves. */
  const VkrTypeDesc *replaced_type;
  /* TERRAIN: the edit, folded with others of the same `gesture`. */
  VkrHeightfieldOp terrain;
  /* PARTITION: the inclusive cell range x0, z0, x1, z1, or every loaded
     cell with `partition_all`. */
  int32_t partition_cells[4];
  bool8_t partition_unload;
  bool8_t partition_all;
  /* RECORD_PLACEMENT: the first document entity index the job added. */
  uint32_t placement_first;
} VkrSceneEditRequest;

typedef enum VkrSceneEditEntryKind {
  VKR_SCENE_EDIT_ENTRY_ENTITY,
  VKR_SCENE_EDIT_ENTRY_COLLISION_LAYERS,
  VKR_SCENE_EDIT_ENTRY_PHYSICS_BATCH,
  /* Component added or removed, entity created or deleted, parent changed. */
  VKR_SCENE_EDIT_ENTRY_STRUCTURE,
  VKR_SCENE_EDIT_ENTRY_SCENE_SETTINGS,
  /* A rectangle of terrain samples before and after an edit. */
  VKR_SCENE_EDIT_ENTRY_TERRAIN,
} VkrSceneEditEntryKind;

typedef struct VkrSceneEditEntry {
  VkrEntityId entity;
  /* Process-wide edit order; containers keep separate journals and undo
     picks the one holding the most recent entry. */
  uint64_t sequence;
  /* Nonzero for every entry of one group, which undoes and redoes as one
     step (vkr_scene_edit_group_begin). */
  uint64_t group;
  VkrSceneEditEntryKind kind;
  void *payload;
  uint64_t payload_size;
} VkrSceneEditEntry;

/* An entity the editor created, keyed in the overlay by an id that stays
   stable across saves; its ECS id changes when undo or redo recreates it. */
typedef struct VkrSceneEditCreated {
  VkrEntityId entity;
  uint32_t id;
} VkrSceneEditCreated;

/* Overlay records that bound to nothing when the overlay loaded: the
   source they name is missing, ambiguous or already claimed, its fingerprint
   changed and neither the saved name nor a document id binds it again, or
   their fields no longer fit the entity. Each is kept as read, `lengths[i]`
   bytes of `text` one after another, its document indices moved to this
   scene's, and the save writes it back unchanged, so a reimport or a
   document change never drops an edit. `ids` are the document ids they name
   that the scene's document lacks; the save lists them after the
   document's own, at the indices the records carry. */
typedef struct VkrSceneEditKept {
  uint8_t *text;
  uint64_t bytes;
  uint64_t capacity;
  uint32_t *lengths;
  uint32_t count;
  uint32_t lengths_capacity;
  VkrEntityRef *ids;
  uint32_t id_count;
  uint32_t id_capacity;
} VkrSceneEditKept;

/* Runtime owns the journal; UI borrows snapshots only for its current build.
   Journal storage is bounded and released when the scene is replaced. */
typedef struct VkrSceneEditState {
  VkrAllocator *allocator;
  VkrSceneEditEntry *undo;
  VkrEntityId *touched;
  uint32_t touched_count;
  uint32_t touched_capacity;
  uint32_t undo_count;
  uint32_t undo_cursor;
  /* Sum of the undo entries' payload sizes. */
  uint64_t payload_bytes;
  uint64_t generation;
  uint64_t revision;
  uint64_t saved_revision;
  /* Gesture that produced the newest undo entry; zero after any other edit. */
  uint64_t gesture;
  /* Group a gesture outside an open group gave its entries once it outgrew
     one entry, as a long terrain stroke does; zero otherwise. */
  uint64_t gesture_group;
  /* Group that new entries join, zero when none is open, and its entries. */
  uint64_t group_open;
  uint32_t group_entries;
  /* Structure the overlay persists: created entities (dead ones are skipped
     on save) and deleted document entities by source identity. */
  VkrSceneEditCreated *created;
  uint32_t created_count;
  uint32_t created_capacity;
  uint32_t next_created_id;
  SceneSourceIdentity *deleted;
  uint32_t deleted_count;
  uint32_t deleted_capacity;
  /* The overlay did not load; saving would drop its edits, so the save
     refuses until the scene opens again. */
  bool8_t sidecar_conflict;
  VkrSceneEditKept kept;
  /* What the last overlay load could not apply: records kept unapplied, or
     the whole file rejected (sidecar_conflict); empty when it all loaded.
     Unlike `status`, later edits leave it. */
  char overlay_problem[192];
  /* World partition (ADR-086): the directory of the scene's cell
     documents, empty when it has none, and the cell size its table uses. */
  char cells_root[1024];
  float32_t cell_size;
  /* The mesh manager's asset_swap_generation the scene's source identities
     last followed (vkr_scene_edit_follow_sources). */
  uint64_t mesh_swap_generation;
  char status[192];
} VkrSceneEditState;

bool8_t vkr_scene_edit_read(const VkrScene *scene, VkrEntityId entity,
                            VkrSceneEditValues *out);
/** Converts the values of `type` that undo entries hold, and `pending` when
    given, from the `previous` layout, as vkr_scene_migrate_world_type does
    for live components (ADR-079). */
void vkr_scene_edit_migrate_type(VkrSceneEditState *s,
                                 VkrSceneEditValues *pending,
                                 const VkrTypeDesc *type,
                                 const VkrTypeDesc *previous);
bool8_t vkr_scene_edit_validate(const VkrSceneEditValues *values);

/** Add the entity's world component of `type` to edit values read by
 * vkr_scene_edit_read; false when the entity lacks it. */
bool8_t vkr_scene_edit_read_component(const VkrScene *scene, VkrEntityId entity,
                                      const VkrTypeDesc *type,
                                      VkrSceneEditValues *values);

/** Descriptor-typed view of the components VkrSceneEditValues carries:
 * transform, visibility, the three light types and the physics body settings.
 * Index iteration returns NULL past the end. */
const VkrTypeDesc *vkr_scene_edit_component_type(uint32_t index);
/** Edit field of a component type, or zero when edit values do not carry it. */
uint32_t vkr_scene_edit_component_field(const VkrTypeDesc *type);
/** Copy one component out of edit values; false when the entity lacks it.
 * `out` holds `type->size` bytes. */
bool8_t vkr_scene_edit_component_get(const VkrSceneEditValues *values,
                                     const VkrTypeDesc *type, void *out);
/** Copy one component into edit values without changing `fields`. */
bool8_t vkr_scene_edit_component_set(VkrSceneEditValues *values,
                                     const VkrTypeDesc *type, const void *in);
void vkr_scene_edit_reset(VkrSceneEditState *state, VkrAllocator *allocator,
                          uint64_t generation);
bool8_t vkr_scene_edit_apply(VkrSceneEditState *state, VkrScene *scene,
                             VkrEntityId entity,
                             const VkrSceneEditValues *values);
/** Apply like vkr_scene_edit_apply, but fold into the newest undo entry when
 * it came from the same nonzero gesture, entity and fields. Physics edits
 * always append. */
bool8_t vkr_scene_edit_apply_gesture(VkrSceneEditState *state, VkrScene *scene,
                                     VkrEntityId entity,
                                     const VkrSceneEditValues *values,
                                     uint64_t gesture);
/** Append an already-applied edit without changing the scene. If allocation
 * fails, the caller must restore its pre-drag values. */
bool8_t vkr_scene_edit_record_external(VkrSceneEditState *state,
                                       VkrScene *scene, VkrEntityId entity,
                                       const VkrSceneEditValues *before,
                                       const VkrSceneEditValues *after);
/* Opens a journal group: entries appended until vkr_scene_edit_group_end
   undo and redo as one step. Groups do not nest; returns zero when one is
   already open. */
uint64_t vkr_scene_edit_group_begin(VkrSceneEditState *state);
void vkr_scene_edit_group_end(VkrSceneEditState *state);
/* Undoes the open group's entries, drops them and closes the group. */
bool8_t vkr_scene_edit_group_rollback(VkrSceneEditState *state,
                                      VkrScene *scene);
/* Whether `group` still has entries in the journal. */
bool8_t vkr_scene_edit_group_present(const VkrSceneEditState *state,
                                     uint64_t group);
/* Reverts a closed group and removes its entries, out of order when needed.
   Entries above the undo cursor are dropped. Fails, naming the conflicting
   entity in `out_conflict`, when a later applied entry touches an entity of
   the group or a current descendant of one. */
bool8_t vkr_scene_edit_group_revert(VkrSceneEditState *state, VkrScene *scene,
                                    uint64_t group, VkrEntityId *out_conflict);
/** Runs `op` on `entity`'s terrain, its coordinates in world space, as one
 * undo entry; consecutive edits of one nonzero gesture, such as a brush
 * stroke, fold into one entry. */
bool8_t vkr_scene_edit_terrain(VkrSceneEditState *state, VkrScene *scene,
                               VkrEntityId entity, const VkrHeightfieldOp *op,
                               uint64_t gesture);
/** The samples of `entity`'s terrain that `op`, in world space, may change;
 * false when it has no loaded terrain or the op misses it. */
bool8_t vkr_scene_edit_terrain_rect(const VkrScene *scene, VkrEntityId entity,
                                    const VkrHeightfieldOp *op,
                                    VkrHeightfieldRect *out);
bool8_t vkr_scene_edit_undo(VkrSceneEditState *state, VkrScene *scene,
                            bool8_t redo);
/** Sequence of the entry undo (or redo) would apply next, or zero. */
uint64_t vkr_scene_edit_next_sequence(const VkrSceneEditState *state,
                                      bool8_t redo);
/** The next step and group numbers of the counters every journal draws
 * from, for another journal whose steps undo in order with scene steps,
 * such as the editor's material documents. */
uint64_t vkr_scene_edit_take_sequence(void);
uint64_t vkr_scene_edit_take_group(void);
bool8_t vkr_scene_edit_save(VkrSceneEditState *state, VkrScene *scene,
                            String8 path);
/** Reads, binds and applies the overlay at `path` in one call, as
    vkr_scene_edit_overlay_read, _begin and an unbounded _step do. */
bool8_t vkr_scene_edit_load(VkrSceneEditState *state, VkrScene *scene,
                            String8 path);
/** Reads only `scene_settings` from the sidecar at `path`, before its scene
    loads; vkr_scene_edit_load applies the validated settings later. A file
    vkr_scene_edit_save wrote holds them in its last bytes, so only those are
    read; another layout is read whole. A missing file or object leaves the
    defaults. Returns false when the file exists but cannot be read. */
bool8_t vkr_scene_edit_peek_settings(VkrAllocator *allocator, String8 path,
                                     VkrSceneSettings *out_settings);

/* An overlay loaded in parts (ADR-076), so a large one neither reads nor
   applies inside one frame. Create it on the frame thread. Read it on any
   thread: the file and every record that does not depend on the scene,
   which is all of them except the overrides of document entities. Then, on
   the frame thread, begin binds those overrides to the scene and commits
   them, as the whole file still fails together, and each step creates the
   overlay's objects, parent links and physics bodies within a time budget.
   The overlay owns all its memory, a private pool that only the thread
   holding it uses, and is freed by destroy after the last step, or earlier
   to cancel. A read borrows the world type registry, so no script reload
   may change a component layout from the read until the last step. */
typedef struct s_VkrSceneEditOverlay VkrSceneEditOverlay;

/** An empty overlay; `allocator` holds the handle and frees it at destroy.
    NULL when its memory cannot be reserved. */
VkrSceneEditOverlay *vkr_scene_edit_overlay_create(VkrAllocator *allocator);
/** Reads and parses the file at `path` once, on any thread. A missing file
    leaves nothing to apply; a rejected one is reported by begin. A set
    `cancel` stops the read early, leaving it rejected. */
void vkr_scene_edit_overlay_read(VkrSceneEditOverlay *overlay, String8 path,
                                 const VkrAtomicBool *cancel);
/** Binds the read overlay to `scene` and commits its overrides. False when
    the overlay is rejected, which sets the state's status and leaves the
    scene unchanged; nothing remains to apply then. */
bool8_t vkr_scene_edit_overlay_begin(VkrSceneEditState *state, VkrScene *scene,
                                     VkrSceneEditOverlay *overlay);
/** Creates the begun overlay's objects, then its parent links and bodies,
    and last commits its scene and collision settings. Works until
    `budget_seconds` pass, checked after each object, link or body, or to the
    end when the budget is zero or less. True once nothing remains. Nothing
    else may change `scene` or `state` between steps. */
bool8_t vkr_scene_edit_overlay_step(VkrSceneEditState *state, VkrScene *scene,
                                    VkrSceneEditOverlay *overlay,
                                    float64_t budget_seconds);
/** Objects, links and bodies the begun overlay has applied, of `total`;
    zero of zero before begin and when it has none. */
void vkr_scene_edit_overlay_progress(const VkrSceneEditOverlay *overlay,
                                     uint32_t *out_done, uint32_t *out_total);
/** Frees the overlay. One begun and not finished drops its staged
    collision settings, so it is destroyed before the scene it began on. */
void vkr_scene_edit_overlay_destroy(VkrSceneEditOverlay *overlay);

/** Add a world component with `value`, or the type's defaults when NULL. */
bool8_t vkr_scene_edit_add_component(VkrSceneEditState *state, VkrScene *scene,
                                     VkrEntityId entity,
                                     const VkrTypeDesc *type,
                                     const void *value);
bool8_t vkr_scene_edit_remove_component(VkrSceneEditState *state,
                                        VkrScene *scene, VkrEntityId entity,
                                        const VkrTypeDesc *type);
/** Replace the entity's `replaced` component with `type` holding `value`, or
 * its defaults when NULL, as one undo entry; `type` must be absent. */
bool8_t vkr_scene_edit_replace_component(VkrSceneEditState *state,
                                         VkrScene *scene, VkrEntityId entity,
                                         const VkrTypeDesc *replaced,
                                         const VkrTypeDesc *type,
                                         const void *value);
/** Create an entity with a transform, visibility and the name, transform,
 * visibility, light and world component fields of `values`, under `parent`
 * or at the root. Returns the entity, or invalid with a status message. */
VkrEntityId vkr_scene_edit_create(VkrSceneEditState *state, VkrScene *scene,
                                  VkrEntityId parent,
                                  const VkrSceneEditValues *values);
/** Whether delete can snapshot and restore the entity exactly: it has only
 * identity, name, transform, visibility, light, world and generated shape
 * or text components and a mesh instance undo can load again from its file
 * (not skinned, not of a spawned model), and its only children are its
 * parts and, for a placed model, its own source nodes, which delete with
 * it. `reason` receives a static message when it cannot. */
bool8_t vkr_scene_edit_can_delete(const VkrScene *scene, VkrEntityId entity,
                                  const char **reason);
/** Delete `entity` with its parts and source nodes as one undo step,
 * leaves first. A document entity's deletion is saved by source identity,
 * so the next load removes it again; undo recreates the entities and
 * requests each mesh from its file (vkr_scene_request_mesh), so it draws
 * again once the load is ready. */
bool8_t vkr_scene_edit_delete(VkrSceneEditState *state, VkrScene *scene,
                              VkrEntityId entity);
/** Whether `entity` is placed and it and its descendants are made only of
 * the parts delete can restore. `reason` receives a static message when it
 * cannot. */
bool8_t vkr_scene_edit_can_duplicate(const VkrScene *scene, VkrEntityId entity,
                                     const char **reason);
/** Copy `entity` and its descendants under the same parent, at the same
 * place, as one undo step. The copy is named like the original with the
 * first free " (n)" number and gets new ids: random ones, or with a `seed`
 * ids derived from the seed and each original's id, so editors that apply
 * the same duplicate to the same scene make the same ids (ADR-106).
 * Returns the copy, or invalid with a status message. */
VkrEntityId vkr_scene_edit_duplicate(VkrSceneEditState *state, VkrScene *scene,
                                     VkrEntityId entity,
                                     const VkrEntityRef *seed);
/** Records the document entities from index `first` on, which an
 * add_entities job appended before the scene reloaded (model.place, the
 * Add entity form), as one created step of the open group: undo removes
 * them as a delete does, saving their deletion by source identity, and redo
 * restores them, meshes loading again from their files. The scene does not
 * change, and a journal without unsaved edits stays saved. Fails, recording
 * nothing, when none is loaded or one could not be removed and restored. */
bool8_t vkr_scene_edit_record_placement(VkrSceneEditState *state,
                                        VkrScene *scene, uint32_t first);
/** Follows a live mesh rebuild: once a mesh swap completed
 * (VkrMeshManager.asset_swap_generation moved), gives the models of the
 * rebuilt meshes the source identity a fresh load computes
 * (vkr_scene_refresh_source_fingerprints) and moves the identities the
 * journal keeps for deleted entities and undo snapshots with them. When an
 * identity the overlay saves changed, the journal becomes unsaved, so a
 * bake waits for the save that rewrites its overrides. Call once per frame;
 * cheap while no swap completes. */
void vkr_scene_edit_follow_sources(VkrSceneEditState *state, VkrScene *scene);
/** Move `entity` under `parent` (invalid: root) keeping its world transform.
 * Rejects cycles and parents in another container. */
bool8_t vkr_scene_edit_reparent(VkrSceneEditState *state, VkrScene *scene,
                                VkrEntityId entity, VkrEntityId parent);

/** Replace the scene's settings through the journal. */
bool8_t vkr_scene_edit_apply_scene_settings(VkrSceneEditState *state,
                                            VkrScene *scene,
                                            const VkrSceneSettings *settings);

bool8_t
vkr_scene_edit_apply_collision_layers(VkrSceneEditState *state, VkrScene *scene,
                                      const VkrSceneCollisionLayers *settings);

bool8_t vkr_scene_edit_apply_physics_batch(VkrSceneEditState *state,
                                           VkrScene *scene,
                                           const VkrScenePhysicsChange *changes,
                                           uint32_t count);

/* World partition cells (ADR-086). A partitioned scene's objects that
 * stream with a cell live in `<cells_root>/<x>_<z>.json` instead of the
 * overlay, as `{"version":1,"cell":[x,z],"created":[records]}` with the
 * overlay's record schema; `index.json` lists the documents, their cell
 * size and the next overlay id, so ids stay unique across unloaded cells.
 * Saving writes each loaded cell whose bytes changed and removes the documents
 * of loaded cells left empty; unloaded cells keep theirs. Every document
 * and the overlay are written beside their files before any replaces its
 * file, and a save refuses an object whose cell's document did not load. */
void vkr_scene_edit_set_cells_root(VkrSceneEditState *state, String8 root);
/* Reads the cell index into the scene's cell table. */
bool8_t vkr_scene_edit_cells_open(VkrSceneEditState *state, VkrScene *scene);
/* Creates the objects of `cell`'s document and marks it loaded; an id a
   loaded object holds is replaced, and roots move by the scene's origin
   offset. A cell without a document, or whose listed document is missing,
   loads empty; an unreadable one is marked so and stays unloaded. */
bool8_t vkr_scene_edit_cell_load(VkrSceneEditState *state, VkrScene *scene,
                                 VkrScenePartitionCell cell);
/* Whether `cell` can unload without losing an edit: no journal entry names
   one of its objects, and nothing is unsaved or the cell holds what its
   document does. The comparison is remembered until the next edit. */
bool8_t vkr_scene_edit_cell_unloadable(VkrSceneEditState *state,
                                       VkrScene *scene,
                                       VkrScenePartitionCell cell);
/* Destroys `cell`'s objects, forgets their overlay ids and marks it
   unloaded. */
void vkr_scene_edit_cell_unload(VkrSceneEditState *state, VkrScene *scene,
                                VkrScenePartitionCell cell);
/* Marks every cell holding objects loaded, merging its document first, and
   after a cell size change loads every cell to be saved anew. */
bool8_t vkr_scene_edit_cells_track(VkrSceneEditState *state, VkrScene *scene);
