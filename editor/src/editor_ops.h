#pragma once

#include "editor_ui.h"
#include "memory/arena.h"
#include "vkr_bakery_json.h"

/* The operation table the agent channel runs (ADR-084). Operations run on the
 * UI thread during the editor's build; scene edits travel as one
 * VkrSampleEditBatchRequest that the runtime applies after the build as one
 * journal group. */
typedef struct VkrEditorOps VkrEditorOps;

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
} VkrEditorOpCall;

#define VKR_EDITOR_CHANGE_MAX 64u
#define VKR_EDITOR_CHANGE_ENTITY_MAX 128u

/* A batch an agent applied for review: its journal group in `container` and
 * the entities it created or edited. Accept drops the mark; Reject reverts
 * the group. */
typedef struct VkrEditorChange {
  uint32_t id;
  uint64_t group;
  uint16_t container;
  char label[96];
  uint32_t entity_count;
  VkrEntityId entities[VKR_EDITOR_CHANGE_ENTITY_MAX];
} VkrEditorChange;

VkrEditorOps *vkr_editor_ops_create(VkrAllocator *allocator);
void vkr_editor_ops_destroy(VkrEditorOps *ops);

/* Runs or continues `call`. Returns WAIT while it needs later builds. */
VkrEditorOpStatus vkr_editor_ops_run(VkrEditorOps *ops, VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame,
                                     VkrEditorOpCall *call);

/* Drops changes whose group left its journal, as after a scene reload or an
   undo followed by another edit. Call once per build. */
void vkr_editor_ops_update(VkrEditorOps *ops, const VkrSampleUiFrame *frame);

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

/* The Agent changes window body: each pending change with Focus, Reject
   and Accept. Reject runs as a changes.reject request from the editor. */
void vkr_editor_changes_build(VkrEditorUi *editor,
                              const VkrSampleUiFrame *frame, VkrUiRect bounds);

/* Whether `entity` belongs to a pending change. */
bool8_t vkr_editor_ops_entity_pending(const VkrEditorOps *ops,
                                      VkrEntityId entity);
