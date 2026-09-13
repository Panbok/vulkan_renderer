#pragma once

#include "animation/vkr_animation_graph.h"
#include "core/vkr_json.h"
#include "core/vkr_json_writer.h"
#include "vkr_sample_runtime.h"

#define VKR_EDITOR_ANIMATION_NODES 8u
#define VKR_EDITOR_ANIMATION_BLOCKS 16u
#define VKR_EDITOR_ANIMATION_UNDO 16u

typedef struct VkrEditorAnimationDocument {
  VkrAnimationGraph graph;
  uint32_t blocks[VKR_EDITOR_ANIMATION_BLOCKS];
  uint32_t block_count;
  Vec2 node_positions[VKR_EDITOR_ANIMATION_NODES];
  Vec2 output_position;
  float32_t crossfade_seconds;
} VkrEditorAnimationDocument;

typedef struct VkrEditorAnimation {
  /* The player owns its pose arena. The source bank is borrowed only while the
   * live scene identity still matches; update checks this before sampling. */
  VkrAnimationPlayer *player;
  const VkrAnimationPlayer *source_player;
  VkrEntityId wrapper;
  uint64_t scene_generation;
  uint64_t fingerprint;
  VkrAnimationGraphInstance graph_instance;
  VkrEditorAnimationDocument document;
  bool8_t graph_dirty;
  bool8_t graph_parameter_dirty;
  bool8_t graph_applied;
  VkrEditorAnimationDocument undo[VKR_EDITOR_ANIMATION_UNDO];
  uint32_t undo_count;
  uint32_t undo_cursor;
  uint32_t selected_clip;
  uint32_t selected_block;
  uint32_t clip_page;
  uint32_t selected_node;
  uint32_t selected_sample;
  uint32_t selected_triangle;
  uint32_t selected_state;
  uint32_t selected_transition;
  uint32_t authoring_page;
  bool8_t pose_seek;
  bool8_t property_edit_active;
  uint32_t graph_drag_node;
  Vec2 graph_drag_grab;
  Vec2 graph_drag_origin;
  bool8_t graph_drag_modified;
  float64_t time;
  float32_t rate;
  float32_t preview_yaw;
  float32_t preview_pitch;
  float32_t preview_distance;
  bool8_t orbit_dragging;
  bool8_t sequence;
  bool8_t clip_preview;
  bool8_t playing;
  bool8_t loop;
  const char *error;
} VkrEditorAnimation;

struct VkrEditorUi;
void vkr_editor_animation_update(struct VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame);
void vkr_editor_animation_build(struct VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame);
void vkr_editor_animation_shutdown(VkrEditorAnimation *animation);
/* Workspace-owned, bounded graph and sequence state. */
bool8_t vkr_editor_animation_write_settings(const VkrEditorAnimation *animation,
                                            VkrJsonWriter *writer);
void vkr_editor_animation_read_settings(VkrEditorAnimation *animation,
                                        String8 json);
