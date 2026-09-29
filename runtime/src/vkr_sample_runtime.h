#pragma once

#include "animation/vkr_animation_player.h"
#include "core/input.h"
#include "core/ui/vkr_ui_dock.h"
#include "core/vkr_json_writer.h"
#include "core/vkr_text.h"
#include "renderer/systems/vkr_editor_viewport.h"
#include "renderer/systems/vkr_scene_edit.h"
#include "renderer/systems/vkr_ui_system.h"
#include "script/vkr_script_host.h"
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
  VKR_SAMPLE_TRANSPORT_STEP_SIMULATION,
  VKR_SAMPLE_TRANSPORT_RESET_SIMULATION,
  VKR_SAMPLE_TRANSPORT_TOGGLE_PHYSICS,
  VKR_SAMPLE_TRANSPORT_CYCLE_COLLISION_DISPLAY,
} VkrSampleTransportAction;

typedef struct VkrSamplePhysicsRequest {
  VkrEntityId entity;
  Vec3 impulse;
  Vec3 world_point;
  bool8_t apply_impulse;
  bool8_t at_point;
  bool8_t set_body_disabled;
  bool8_t body_disabled;
} VkrSamplePhysicsRequest;

typedef enum VkrSampleCameraView {
  VKR_SAMPLE_CAMERA_PERSPECTIVE = 0,
  VKR_SAMPLE_CAMERA_TOP,
  VKR_SAMPLE_CAMERA_LEFT,
  VKR_SAMPLE_CAMERA_RIGHT,
  VKR_SAMPLE_CAMERA_BOTTOM,
  VKR_SAMPLE_CAMERA_VIEW_COUNT,
} VkrSampleCameraView;

/* Runtime-owned viewport controls; the UI submits a copied request. Grid
 * spacing is in world units. Orthographic selection enables the grid on
 * transition. */
typedef struct VkrSampleViewState {
  VkrSampleCameraView camera_view;
  VkrRenderMode render_mode;
  float32_t grid_spacing;
  /* VkrGizmoMode tool filter; NONE shows every transform handle. */
  uint32_t gizmo_tool;
  /* Free-camera flight speed in world units per second. */
  float32_t camera_speed;
  bool8_t grid_enabled;
  /* Screen-order cell numbers and letters along the grid's edges. */
  bool8_t grid_labels;
} VkrSampleViewState;

typedef struct VkrSampleViewRequest {
  VkrSampleViewState value;
  bool8_t apply;
} VkrSampleViewRequest;

/* Consumed after UI build. Paths are copied before the frame scratch expires.
 * Selection replaces the old scene only after the dirty-edit decision. */
typedef struct VkrSampleSceneRequest {
  String8 path;
  String8 sidecar_path;
  String8 asset_root; /* Managed workspace root; empty uses repository asset
                         root. */
  bool8_t select;
  bool8_t unload;
  bool8_t discard_edits;
  /** Load `path` beside the active scene as an additive container, or remove
   * the additive container whose world id is `container` (ADR-076). */
  bool8_t add;
  bool8_t remove;
  uint16_t container;
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

/* UI borrows a scene bank through its independent player. Runtime consumes only
 * while scene_generation still matches; it copies pose data before submission.
 * Distance is a multiplier of the preview model's bounding radius. */
typedef struct VkrAnimationPreviewRequest {
  VkrEntityId wrapper;
  const VkrAnimationPlayer *player;
  uint64_t scene_generation;
  float32_t yaw;
  float32_t pitch;
  float32_t distance;
} VkrAnimationPreviewRequest;

/** Root World container request (ADR-076), consumed after the UI build. The
 * root World is always loaded while a project is open: its entities carry
 * project-wide world components the rendered scene falls back to. */
typedef struct VkrSampleWorldRequest {
  /** Runtime world document; a missing file starts an empty World. */
  String8 path;
  /** Edit overlay beside the document; empty disables saving. */
  String8 sidecar_path;
  bool8_t load;
  bool8_t unload;
  bool8_t save;
  /** With load: drop unsaved World edits instead of refusing. */
  bool8_t discard_edits;
  /** With load: the same World's document changed; added scenes and the
   * camera stay. */
  bool8_t reload;
} VkrSampleWorldRequest;

#define VKR_SAMPLE_SCRIPT_LOAD_MAX 8u

typedef struct VkrSampleScriptLoad {
  char name[VKR_SCRIPT_MODULE_NAME_CAPACITY];
  char path[VKR_SCRIPT_PATH_CAPACITY];
} VkrSampleScriptLoad;

/** Script library requests (ADR-079), applied after the UI build and before
 * scene and World requests, so a project's component types register before
 * its documents load. Retiring comes before the loads. */
typedef struct VkrSampleScriptRequest {
  bool8_t retire_libraries;
  uint32_t load_count;
  VkrSampleScriptLoad loads[VKR_SAMPLE_SCRIPT_LOAD_MAX];
} VkrSampleScriptRequest;

/** Outcome of one applied load; `serial` increases with every load. */
typedef struct VkrSampleScriptResult {
  uint64_t serial;
  char name[VKR_SCRIPT_MODULE_NAME_CAPACITY];
  VkrScriptReload result;
  char message[256];
} VkrSampleScriptResult;

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
  VkrAnimationPreviewRequest *animation_preview;
  /* Floating document editors can consume undo/save without editing the scene.
   */
  bool8_t *scene_shortcuts_blocked;
  VkrEntityId selected_entity;
  uint64_t scene_generation;
  const VkrSceneEditState *edits;
  VkrSceneEditRequest *scene_edit;
  VkrSampleSceneRequest *scene_request;
  /** Root World container and its journal, or NULL; entities carry
   * VKR_SCENE_WORLD_ROOT_ID in their world field. Edits of its entities use
   * the same scene_edit request and route by that field. */
  const VkrScene *world;
  const VkrSceneEditState *world_edits;
  VkrSampleWorldRequest *world_request;
  String8 world_status;
  /** Additive scene containers by slot, NULL when empty; slot i owns world id
   * i + 1. `additive_names` borrow the document paths. */
  const VkrScene *additive[VKR_SCENE_ADDITIVE_MAX];
  const VkrSceneEditState *additive_edits[VKR_SCENE_ADDITIVE_MAX];
  String8 additive_names[VKR_SCENE_ADDITIVE_MAX];
  VkrSampleRuntimePreferences runtime_preferences;
  VkrSampleSceneRecall scene_recall;
  VkrSampleEditorStateRequest *editor_state_request;
  bool8_t close_requested;
  VkrSampleCloseResponse *close_response;
  /** Ends the application after this build, as the auto-close timer does; the
   * client decides whether unsaved work allows it. */
  bool8_t *quit_request;
  String8 scene_path;
  String8 scene_status;
  bool8_t scene_loading;
  /** An added scene's load is still pending in some additive slot. */
  bool8_t additive_loading;
  /** The World document's load is still pending. */
  bool8_t world_loading;
  /* A modal client sets this to suppress world input and global edit keys. */
  bool8_t *modal;
  float64_t simulation_time;
  bool8_t simulation_running;
  /** Script modules drive the Scene camera and HUD this frame. */
  bool8_t scripts_running;
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
  VkrSampleScriptRequest *script_request;
  /** The latest applied loads, most recent last; borrowed for the build. */
  const VkrSampleScriptResult *script_results;
  uint32_t script_result_count;
  /** Registered modules and the session; read-only. */
  const VkrScriptHost *scripts;
  VkrSampleViewState view_state;
  VkrSampleViewRequest *view_request;
  VkrSamplePhysicsRequest *physics_request;
  uint32_t collision_display; /* 0 off, 1 selected body, 2 all (bounded). */
  bool8_t mapping_valid;
  bool8_t scene_only;
  bool8_t mouse_captured;
  /* Runtime owns Scene keyboard focus; editor updates it during build. */
  bool8_t *scene_keyboard_focus;
  /** Frame-local request to blur the retained Scene image during preparation.
   */
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
  /** Initial client size in points, centered on screen; zero keeps the
   * runtime default size and position. */
  uint32_t window_width_pt;
  uint32_t window_height_pt;
  /** Display mode entered once the window exists; headless ignores it. */
  VkrWindowMode window_mode;
  /** No window: frames render into an offscreen target of the window size in
   * pixels at content scale 1, with no input and no presentation. */
  bool8_t headless;
} VkrSamplePresentationConfig;

typedef struct VkrSampleRuntimeConfig {
  const char *title;
  /* The editor owns startup selection and preference persistence in this mode.
   */
  bool8_t project_managed;
  /** Graphics preferences file when $VKR_GRAPHICS_SETTINGS_PATH is unset;
   * NULL keeps `<content root>.vkr-graphics-settings.json`. A packaged game
   * names a per-user file. Ignored in project mode. */
  const char *graphics_settings_path;
  /** Graphics settings JSON applied over the backend defaults before the
   * preferences file, such as a packaged game's `graphics`; may be empty. */
  String8 graphics_defaults;
  VkrSamplePresentationConfig presentation;
  VkrSampleUiClient ui;
  /** C script modules linked into the executable (ADR-079), registered in
   * order before any scene loads. */
  const VkrScriptModuleEntry *script_modules;
  uint32_t script_module_count;
} VkrSampleRuntimeConfig;

VkrSampleRuntimeConfig vkr_sample_runtime_config_default(void);
int vkr_sample_runtime_run(int argc, char **argv,
                           const VkrSampleRuntimeConfig *config);
