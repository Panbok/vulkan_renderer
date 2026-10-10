#pragma once

#include "editor_ui.h"
#include "memory/arena.h"
#include "vkr_bakery_json.h"

/* The operation table the agent channel runs (ADR-084). Operations run on the
 * UI thread during the editor's build; scene edits travel as one
 * VkrSampleEditBatchRequest that the runtime applies after the build as one
 * journal group. */
typedef struct VkrEditorOps VkrEditorOps;

/* Bytes of an author name, with its terminator: an agent's name and, in a
   collaborative session, `@` and its editor's name (ADR-106). */
#define VKR_EDITOR_AUTHOR_CAPACITY 80u
/* Bytes of an agent's own name, with its terminator. */
#define VKR_EDITOR_AGENT_NAME_CAPACITY 32u

typedef enum VkrEditorOpStatus {
  VKR_EDITOR_OP_DONE = 0,
  /* The operation continues in a later build: a batch result, a capture or
     a Cmd statement that holds its queue. */
  VKR_EDITOR_OP_WAIT,
} VkrEditorOpStatus;

/* One request's call. The agent owns it, and the arena that holds `args`,
 * `result` and `state`, until the operation reports DONE. A failure sets
 * `error_code` and `error` and leaves `result` NULL. */
typedef struct VkrEditorOpCall {
  Arena *arena;
  String8 op;
  const VkrBakeryJson *args;
  VkrBakeryJson *result;
  const char *error_code;
  char error[256];
  /* State of an operation across builds. */
  uint32_t stage;
  uint32_t frames;
  uint64_t token;
  void *state;
  /* Who asks: an agent's name or client slot, recorded on its changes and
     limiting its undo to its own steps; empty for the editor's own
     requests. */
  char author[VKR_EDITOR_AUTHOR_CAPACITY];
  /* Builds a read waited for the scene to settle, whether it is done
     waiting and whether it gave up first. */
  uint32_t settle_frames;
  bool8_t settle_done;
  bool8_t settle_timeout;
  /* When a capture began waiting for the designer to leave the editor
     alone, zero before it waits. */
  float64_t idle_since;
  /* The request already waited outside the queue (editor_agent.c), so a
     capture checks the designer once instead of waiting again. */
  bool8_t waited;
} VkrEditorOpCall;

/* Pending changes the designer can review; a long agent build fills 64 in
   about an hour, past which its batches applied unreviewed. */
#define VKR_EDITOR_CHANGE_MAX 512u
#define VKR_EDITOR_CHANGE_ENTITY_MAX 128u

/* A batch an agent applied for review: its journal group in `container`,
 * its author and the entities it created or edited. Accept drops the mark;
 * Reject reverts the group. */
typedef struct VkrEditorChange {
  uint32_t id;
  uint64_t group;
  uint16_t container;
  char label[96];
  char author[VKR_EDITOR_AUTHOR_CAPACITY];
  /* Platform time the batch applied, for its age in the window. */
  float64_t created;
  /* A reject waits in the queue or runs; the window holds its actions. */
  bool8_t rejecting;
  /* Why the last reject was refused, empty otherwise. */
  char problem[160];
  uint32_t entity_count;
  VkrEntityId entities[VKR_EDITOR_CHANGE_ENTITY_MAX];
  /* A change to a material document instead of a scene: the first file its
     group wrote, which Reject reverts through the material journal
     (editor_material.h); empty for a scene change. */
  char document[256];
} VkrEditorChange;

#define VKR_EDITOR_CLAIM_MAX 64u

/* A box of one container an agent claimed (claims.set). Other agents'
 * writes that touch it are refused, so the agents of a swarm build their own
 * regions without conflicts; the designer's edits are not limited. */
typedef struct VkrEditorClaim {
  uint32_t id;
  uint16_t container;
  char author[VKR_EDITOR_AUTHOR_CAPACITY];
  char name[48];
  Vec3 min;
  Vec3 max;
} VkrEditorClaim;

#define VKR_EDITOR_TASK_MAX 256u

typedef enum VkrEditorTaskState {
  VKR_EDITOR_TASK_OPEN = 0,
  VKR_EDITOR_TASK_ASSIGNED,
  VKR_EDITOR_TASK_DONE,
  VKR_EDITOR_TASK_FAILED,
} VkrEditorTaskState;

/* A task of the board (task.*): work of one kind, such as layout, material
 * or lighting, optionally in a region, that one agent takes with task.next
 * and finishes with task.done. In a collaborative session the host keeps the
 * board, so agents of every editor divide its tasks (ADR-106). */
typedef struct VkrEditorTask {
  uint32_t id;
  uint8_t state; /**< VkrEditorTaskState. */
  bool8_t has_region;
  uint16_t container;
  char kind[32];
  char title[96];
  /* Capabilities the assignee's editor must have, separated by commas, such
     as `tiled` for the Metal pipeline or `desktop` for Vulkan (ADR-087);
     empty for any editor. */
  char
    requires[
        64];
  char assignee[VKR_EDITOR_AUTHOR_CAPACITY];
  char note[96];
  Vec3 min;
  Vec3 max;
} VkrEditorTask;

VkrEditorOps *vkr_editor_ops_create(VkrAllocator *allocator);
void vkr_editor_ops_destroy(VkrEditorOps *ops);

/* Runs or continues `call`. Returns WAIT while it needs later builds. */
VkrEditorOpStatus vkr_editor_ops_run(VkrEditorOps *ops, VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame,
                                     VkrEditorOpCall *call);

/* Whether `op` is a cheap read that changes nothing, so it may run in a
   build after other cheap reads (ADR-084). */
bool8_t vkr_editor_ops_quick(String8 op);

/* Drops changes whose group left its journal, as after a scene reload or an
   undo followed by another edit. Call once per build. */
void vkr_editor_ops_update(VkrEditorOps *ops, const VkrEditorUi *editor,
                           const VkrSampleUiFrame *frame);

/* `value` of `type` as the JSON object documents and `values` arguments
   take, into `out`; false when it does not fit. */
bool8_t vkr_editor_ops_component_text(const VkrTypeDesc *type,
                                      const void *value, char *out,
                                      uint64_t capacity);

uint32_t vkr_editor_ops_change_count(const VkrEditorOps *ops);
const VkrEditorChange *vkr_editor_ops_change(const VkrEditorOps *ops,
                                             uint32_t index);
/* Accepts change `id`, or every change for zero; false when none matched. */
bool8_t vkr_editor_ops_accept(VkrEditorOps *ops, uint32_t id);

/* Whether the designer left the editor alone long enough for an agent's
   capture to switch the Scene's view. */
bool8_t vkr_editor_ops_idle(const VkrEditorOps *ops);

/* The newest change-feed sequence, zero before the first event. */
uint64_t vkr_editor_ops_feed_latest(const VkrEditorOps *ops);

uint32_t vkr_editor_ops_claim_count(const VkrEditorOps *ops);
const VkrEditorClaim *vkr_editor_ops_claim(const VkrEditorOps *ops,
                                           uint32_t index);

/* The Agent changes window body: each pending change with Focus, Reject
   and Accept. Reject runs as a changes.reject request from the editor. */
void vkr_editor_changes_build(VkrEditorUi *editor,
                              const VkrSampleUiFrame *frame, VkrUiRect bounds);

/* Whether `entity` belongs to a pending change. */
bool8_t vkr_editor_ops_entity_pending(const VkrEditorOps *ops,
                                      VkrEntityId entity);
