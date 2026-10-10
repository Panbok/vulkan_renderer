#pragma once

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
#define VKR_SCENE_EDIT_CREATED_MAX 65536u

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
  bool8_t sidecar_conflict;
  /* World partition (ADR-086): the directory of the scene's cell
     documents, empty when it has none, and the cell size its table uses. */
  char cells_root[1024];
  float32_t cell_size;
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
bool8_t vkr_scene_edit_load(VkrSceneEditState *state, VkrScene *scene,
                            String8 path);
/** Reads only `scene_settings` from the sidecar at `path`, before its scene
    loads; vkr_scene_edit_load applies the validated settings later. A
    missing file or object leaves the defaults. Returns false when the file
    exists but cannot be read. */
bool8_t vkr_scene_edit_peek_settings(VkrAllocator *allocator, String8 path,
                                     VkrSceneSettings *out_settings);

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
/** Whether delete can snapshot and restore the entity exactly: it has no
 * children and only identity, name, transform, visibility, light and world
 * components. `reason` receives a static message when it cannot. */
bool8_t vkr_scene_edit_can_delete(const VkrScene *scene, VkrEntityId entity,
                                  const char **reason);
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
