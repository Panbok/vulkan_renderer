#pragma once

#include "core/input.h"
#include "core/ui/vkr_ui_dock.h"
#include "core/vkr_json_writer.h"
#include "core/vkr_text.h"
#include "renderer/systems/vkr_editor_viewport.h"
#include "renderer/systems/vkr_scene_edit.h"
#include "renderer/systems/vkr_ui_system.h"
#include "vkr_graphics_settings.h"
#include "vkr_renderer.h"

typedef struct VkrSampleUiText {
  String8 camera;
  String8 performance;
  String8 metrics;
  String8 memory;
  /** CPU/GPU identity and periodically sampled process/device memory. */
  String8 system;
} VkrSampleUiText;

typedef enum VkrSampleTransportAction {
  VKR_SAMPLE_TRANSPORT_NONE = 0,
  VKR_SAMPLE_TRANSPORT_START_SIMULATION,
  VKR_SAMPLE_TRANSPORT_PAUSE_SIMULATION,
  VKR_SAMPLE_TRANSPORT_START_RENDERING,
  VKR_SAMPLE_TRANSPORT_STOP_RENDERING,
  VKR_SAMPLE_TRANSPORT_TOGGLE_CAMERA,
} VkrSampleTransportAction;

/* Consumed after UI build. Paths are copied before the frame scratch expires.
 * Selection replaces the old scene only after the dirty-edit decision. */
typedef struct VkrSampleSceneRequest {
  String8 path;
  String8 sidecar_path;
  bool8_t select;
  bool8_t unload;
  bool8_t discard_edits;
} VkrSampleSceneRequest;

typedef struct VkrSampleEntityIdentity {
  uint32_t scene_entity;
  uint32_t gltf_node;
  uint64_t source_fingerprint;
} VkrSampleEntityIdentity;

typedef struct VkrSampleRuntimePreferences {
  uint32_t filter_mode;
  uint32_t gizmo_mode;
  uint32_t gizmo_space;
  float32_t gizmo_size;
  float32_t camera_speed;
  float32_t camera_sensitivity;
  float32_t move_multiplier;
  float32_t rotation_multiplier;
  uint32_t ibl_debug_mode;
  float32_t ibl_debug_scalar;
  bool8_t pass_gpu_timings;
} VkrSampleRuntimePreferences;

typedef struct VkrSampleSceneRecall {
  bool8_t camera_valid;
  Vec3 position;
  float32_t yaw;
  float32_t pitch;
  float32_t field_of_view;
  float32_t near_plane;
  float32_t far_plane;
  bool8_t selection_valid;
  VkrSampleEntityIdentity selection;
} VkrSampleSceneRecall;

typedef struct VkrSampleEditorStateRequest {
  bool8_t apply_preferences;
  bool8_t apply_recall;
  VkrSampleRuntimePreferences preferences;
  VkrSampleSceneRecall recall;
} VkrSampleEditorStateRequest;

bool8_t vkr_sample_runtime_preferences_write_json(
    const VkrSampleRuntimePreferences *value, VkrJsonWriter *writer);
bool8_t
vkr_sample_runtime_preferences_read_json(String8 json,
                                         VkrSampleRuntimePreferences *value);
bool8_t vkr_sample_scene_recall_write_json(const VkrSampleSceneRecall *value,
                                           VkrJsonWriter *writer);
bool8_t vkr_sample_scene_recall_read_json(String8 json,
                                          VkrSampleSceneRecall *value);
bool8_t vkr_sample_entity_identity(const VkrScene *scene, VkrEntityId entity,
                                   VkrSampleEntityIdentity *identity);
VkrEntityId vkr_sample_entity_find(const VkrScene *scene,
                                   const VkrSampleEntityIdentity *identity);
bool8_t
vkr_sample_entity_identity_write_json(const VkrSampleEntityIdentity *identity,
                                      VkrJsonWriter *writer);
bool8_t vkr_sample_entity_identity_read_json(String8 json,
                                             VkrSampleEntityIdentity *identity);

typedef enum VkrSampleCloseResponse {
  VKR_SAMPLE_CLOSE_NONE = 0,
  VKR_SAMPLE_CLOSE_CANCEL,
  VKR_SAMPLE_CLOSE_CONFIRM,
} VkrSampleCloseResponse;

typedef struct VkrSampleUiFrame {
  VkrUiSystem *ui;
  VkrWindow *window;
  struct VkrRenderAssets *assets;
  VkrUiDockTree *dock;
  InputState *input;
  VkrViewportMapping mapping;
  Mat4 view_projection; /* Unjittered camera, same Y-down convention as picking.
                         */
  VkrSampleUiText text;
  const VkrGraphicsSettingsState *graphics;
  VkrGraphicsSettingsRequest *graphics_request;
  const VkrScene *scene; /* Borrowed until build returns; edits are requests. */
  VkrEntityId selected_entity;
  uint64_t scene_generation;
  const VkrSceneEditState *edits;
  VkrSceneEditRequest *scene_edit;
  VkrSampleSceneRequest *scene_request;
  VkrSampleRuntimePreferences runtime_preferences;
  VkrSampleSceneRecall scene_recall;
  VkrSampleEditorStateRequest *editor_state_request;
  bool8_t close_requested;
  VkrSampleCloseResponse *close_response;
  String8 scene_path;
  String8 scene_status;
  bool8_t scene_loading;
  /* A modal client sets this to suppress world input and global edit keys. */
  bool8_t *modal;
  float64_t simulation_time;
  bool8_t simulation_running;
  bool8_t scene_rendering_stopped;
  VkrRendererError scene_error;
  float32_t scene_output_scale;
  uint32_t scene_render_width;
  uint32_t scene_render_height;
  uint32_t scene_output_width;
  uint32_t scene_output_height;
  uint32_t texture_pending_count;
  uint32_t texture_demanded_missing_count;
  /** One typed request, consumed by the runtime after build returns. */
  VkrSampleTransportAction *transport_action;
  bool8_t mapping_valid;
  bool8_t scene_only;
  bool8_t mouse_captured;
  /* Runtime owns Scene keyboard focus; editor updates it during build. */
  bool8_t *scene_keyboard_focus;
  /** Frame-local request to blur the retained Scene image during preparation. */
  bool8_t *scene_backdrop_blur;
} VkrSampleUiFrame;

typedef struct VkrSampleUiClient {
  void *state;
  bool8_t (*initialize)(void *state, VkrUiDockTree *dock, VkrUiSystem *ui);
  void (*handle_input)(void *state, const InputState *input);
  VkrUiDockInputCapture (*build)(void *state, const VkrSampleUiFrame *frame);
  /** Managed editor publication; absent callbacks retain legacy sidecar saves.
   */
  bool8_t (*save_scene_edits)(void *state, VkrSceneEditState *edits,
                              const VkrScene *scene,
                              String8 runtime_scene_path);
  /** Optional projection of current UI anchors after Scene camera input. */
  void (*project_scene)(void *state, const VkrSampleUiFrame *frame);
  bool8_t (*shutdown)(void *state, const VkrUiDockTree *dock, VkrUiSystem *ui);
} VkrSampleUiClient;

typedef struct VkrSamplePresentationConfig {
  float32_t render_scale;
  bool8_t paneled;
  bool8_t scene_only;
} VkrSamplePresentationConfig;

typedef struct VkrSampleRuntimeConfig {
  const char *title;
  /* The editor owns startup selection and preference persistence in this mode.
   */
  bool8_t project_managed;
  VkrSamplePresentationConfig presentation;
  VkrSampleUiClient ui;
} VkrSampleRuntimeConfig;

VkrSampleRuntimeConfig vkr_sample_runtime_config_default(void);
int vkr_sample_runtime_run(int argc, char **argv,
                           const VkrSampleRuntimeConfig *config);
