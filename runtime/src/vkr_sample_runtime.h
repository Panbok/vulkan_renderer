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

/* Entity IO during a session (ADR-084): one input sent to an entity as a
   connection would, or the `[io]` trace switched. The runtime answers in
   VkrSampleIoResult with the same token. */
typedef struct VkrSampleIoRequest {
  uint64_t token;
  VkrEntityId target;
  char input[64];
  char value[64];
  bool8_t send;
  bool8_t set_trace;
  bool8_t trace;
} VkrSampleIoRequest;

typedef struct VkrSampleIoResult {
  uint64_t token;
  bool8_t ok;
  char message[160];
} VkrSampleIoResult;

typedef enum VkrSampleCameraView {
  VKR_SAMPLE_CAMERA_PERSPECTIVE = 0,
  VKR_SAMPLE_CAMERA_TOP,
  VKR_SAMPLE_CAMERA_LEFT,
  VKR_SAMPLE_CAMERA_RIGHT,
  VKR_SAMPLE_CAMERA_BOTTOM,
  /* From +Z looking toward -Z, and from -Z toward +Z; Y is up in both. */
  VKR_SAMPLE_CAMERA_FRONT,
  VKR_SAMPLE_CAMERA_BACK,
  VKR_SAMPLE_CAMERA_VIEW_COUNT,
} VkrSampleCameraView;

/* Runtime-owned viewport controls; the UI submits a copied request. Grid
 * spacing is in world units. Orthographic selection enables the grid on
 * transition. */
typedef struct VkrSampleViewState {
  VkrSampleCameraView camera_view;
  VkrRenderMode render_mode;
  float32_t grid_spacing;
  /* World Y of the ground grid and of placement onto it, per scene: a floor
     modelled above the origin hides a grid at zero. */
  float32_t grid_height;
  /* VkrGizmoMode tool: NONE (Select) shows no handles, and dragging an
     object moves it; another mode shows that handle family. */
  uint32_t gizmo_tool;
  /* VkrGizmoSpace of move and rotate handles: world or local. */
  uint32_t gizmo_space;
  /* Free-camera flight speed in world units per second. */
  float32_t camera_speed;
  /* Free-camera mouse-look multiplier (VkrCamera.sensitivity). */
  float32_t camera_sensitivity;
  /* VKR_SCENE_SHOW_HIDE_* geometry kinds the Scene neither draws nor
     picks. */
  uint32_t hidden_kinds;
  /* Collision shapes drawn: 0 off, 1 the selected body, 2 all (bounded). */
  uint32_t collision_display;
  bool8_t grid_enabled;
  /* Screen-order cell numbers and letters along the grid's edges. */
  bool8_t grid_labels;
  /* The grid stays visible through opaque geometry instead of behind it. */
  bool8_t grid_through_geometry;
  /* Every brush face shows its surface's greybox look, art-owned materials
     too (vkr_surface.h). */
  bool8_t greybox_view;
} VkrSampleViewState;

/* The free-camera speeds, in world units per second, that the Scene's speed
 * control and the wheel while flying span on a logarithmic scale. */
#define VKR_SAMPLE_CAMERA_SPEED_MIN 0.1f
#define VKR_SAMPLE_CAMERA_SPEED_MAX 100.0f

/* Where `speed` sits on the speed span, in [0, 1]. */
float32_t vkr_sample_camera_speed_fraction(float32_t speed);
/* The speed at `fraction` of the span, rounded to two significant digits. */
float32_t vkr_sample_camera_speed_value(float32_t fraction);
/* The speed `wheel_lines` wheel lines away from `speed`; 24 lines cross the
 * whole span. */
float32_t vkr_sample_camera_speed_step(float32_t speed, float32_t wheel_lines);

/* The editor's Hide and Isolate (ADR-084): objects of any loaded container
 * the Scene neither draws nor picks, with everything under them, or with
 * `isolate` the only ones it draws. The runtime keeps the latest applied
 * set and uses it while the editor edits; Play draws everything. Nothing
 * saves or journals it. */
typedef struct VkrSampleHideRequest {
  bool8_t apply;
  bool8_t isolate;
  uint32_t count;
  VkrEntityId entities[VKR_SCENE_EDITOR_HIDDEN_MAX];
} VkrSampleHideRequest;

typedef struct VkrSampleViewRequest {
  VkrSampleViewState value;
  bool8_t apply;
  /* After `value` applies, frame the world box [frame_min, frame_max]. */
  bool8_t frame_box;
  Vec3 frame_min;
  Vec3 frame_max;
} VkrSampleViewRequest;

/* Why the UI asked for a Scene pick; the answer carries it back. */
typedef enum VkrSamplePickPurpose {
  /* A right click: open the picked object's menu. */
  VKR_SAMPLE_PICK_MENU = 0,
  /* A Script asset dropped: attach it to the picked object. */
  VKR_SAMPLE_PICK_SCRIPT_DROP,
  /* A Ctrl+click (Cmd on macOS): add the picked object to the editor's
     selection, or take it out. */
  VKR_SAMPLE_PICK_SELECT_TOGGLE,
} VkrSamplePickPurpose;

/* Set the live time of day (ADR-090): the rendered scene's clock hour, and
 * one light group's intensity in every loaded container. Both last until
 * the simulation resets. */
typedef struct VkrSampleTimeOfDayRequest {
  bool8_t set_hour;
  float64_t hour;
  bool8_t set_group;
  char group[VKR_LIGHT_GROUP_NAME_BYTES];
  float32_t intensity;
} VkrSampleTimeOfDayRequest;

/* Lift the ground grid onto the surface at a window pixel of the Scene:
 * collision answers at once, else the GPU depth at that pixel a few frames
 * later; the frame's grid_status reports the outcome. */
typedef struct VkrSampleGridFitRequest {
  bool8_t request;
  Vec2 position_px;
} VkrSampleGridFitRequest;

/* Pick the Scene at a window pixel; the result arrives in a later frame as
 * the frame's context fields. */
typedef struct VkrSamplePickRequest {
  bool8_t request;
  Vec2 position_px;
  VkrSamplePickPurpose purpose;
} VkrSamplePickRequest;

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
  /* Read and written for compatibility; the Scene keeps
     VKR_STANDARD_SCENE_CAMERA_FOV_DEGREES. */
  float32_t field_of_view;
  float32_t near_plane;
  float32_t far_plane;
  /* The scene's grid height; files written before it read zero. */
  float32_t grid_height;
  bool8_t selection_valid;
  VkrSampleEntityIdentity selection;
} VkrSampleSceneRecall;

typedef struct VkrSampleEditorStateRequest {
  bool8_t apply_preferences;
  bool8_t apply_recall;
  VkrSampleRuntimePreferences preferences;
  VkrSampleSceneRecall recall;
  /* Moves the Scene camera as free flight does: the pose only, no lens
     change and no temporal cut. */
  bool8_t move_camera;
  Vec3 camera_position;
  float32_t camera_yaw;
  float32_t camera_pitch;
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
  /* A project library listing its modules, else one module's library. */
  bool8_t project;
  /* Opens `path` itself instead of a byte copy, as a packaged game does. */
  bool8_t in_place;
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

/* Most edits one batch request may carry (ADR-084): a blockout shape's
   rebuild, which deletes its old brushes and creates 240 stair steps or 48
   corridor stretches, fits. */
#define VKR_SAMPLE_EDIT_BATCH_MAX 2048u

/* One edit of a batch. A nonnegative `entity_ref` or `parent_ref` names the
 * entity that batch edit `k` created and replaces `request.entity` or
 * `request.parent` when the batch applies. */
typedef struct VkrSampleEditBatchItem {
  VkrSceneEditRequest request;
  int32_t entity_ref;
  int32_t parent_ref;
} VkrSampleEditBatchItem;

/* Edits that apply in order as one journal group of `container`: 0 for the
 * primary scene, 1 to VKR_SCENE_ADDITIVE_MAX for an added scene and
 * VKR_SCENE_WORLD_ROOT_ID for the World. A failed edit rolls the group back.
 * A nonzero `revert_group` instead reverts that closed group of `container`
 * (vkr_scene_edit_group_revert) and `items` is ignored. Items are borrowed
 * until runtime dispatch after this UI build. */
typedef struct VkrSampleEditBatchRequest {
  uint64_t token;
  const VkrSampleEditBatchItem *items;
  uint32_t count;
  uint16_t container;
  uint64_t revert_group;
} VkrSampleEditBatchRequest;

/* The outcome of the latest batch or revert, kept until the next one. */
typedef struct VkrSampleEditBatchResult {
  uint64_t token;
  bool8_t ok;
  /* Index of the edit that failed, or UINT32_MAX. */
  uint32_t failed_index;
  uint64_t group;
  /* A revert's conflicting entity, or invalid. */
  VkrEntityId conflict;
  char message[192];
  /* Per item: the entity a CREATE or DUPLICATE made, else invalid. */
  VkrEntityId created[VKR_SAMPLE_EDIT_BATCH_MAX];
} VkrSampleEditBatchResult;

/* Captures the presented window color once (final_color). The runtime
 * renders it with a later frame and lends the poll result to one build
 * through `capture_ready`; it releases the capture after that build. */
typedef struct VkrSampleCaptureRequest {
  bool8_t request;
  uint64_t token;
} VkrSampleCaptureRequest;

typedef struct VkrSampleCaptureReady {
  uint64_t token;
  bool8_t failed;
  /* The final_color item; borrowed for this build only. */
  const VkrCaptureItemResult *item;
} VkrSampleCaptureReady;

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
  /* A Scene pick's answer, once: a right click or a pick_request. The
   * object it picked, or invalid for empty space, the window pixel and why
   * it was asked. */
  bool8_t context_requested;
  VkrEntityId context_entity;
  Vec2 context_position_px;
  VkrSamplePickPurpose context_purpose;
  VkrSamplePickRequest *pick_request;
  VkrSampleGridFitRequest *grid_fit_request;
  VkrSampleTimeOfDayRequest *time_of_day_request;
  /* The last grid fit's outcome, or empty. */
  String8 grid_status;
  uint64_t scene_generation;
  const VkrSceneEditState *edits;
  VkrSceneEditRequest *scene_edit;
  /** One batch or group revert, consumed after build; and the latest
   * outcome, borrowed for the build. */
  VkrSampleEditBatchRequest *edit_batch;
  const VkrSampleEditBatchResult *edit_batch_result;
  VkrSampleCaptureRequest *capture_request;
  /** A finished capture, for this build only; NULL otherwise. */
  const VkrSampleCaptureReady *capture_ready;
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
  /** The game plays with script instances or engine IO components (relays,
   * timers, movers), so inputs fired at entities reach them. */
  bool8_t io_running;
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
  VkrSampleHideRequest *hide_request;
  VkrSamplePhysicsRequest *physics_request;
  VkrSampleIoRequest *io_request;
  const VkrSampleIoResult *io_result;
  bool8_t mapping_valid;
  /* The Scene fills the window without the dock: the startup Scene-only
     mode, or a maximized Scene. */
  bool8_t scene_only;
  /* The Scene is maximized (scene_only holds too) and the top navigation bar
     is hidden. Transient; nothing saves it. */
  bool8_t scene_maximized;
  /* The maximized state from the next frame; starts as scene_maximized. */
  bool8_t *scene_maximized_next;
  /* The runtime spent this frame's Escape: it released the flying camera or
     cancelled a gizmo pick or edit. */
  bool8_t escape_taken;
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
  /** Optional: adds synthetic input, such as scripted pointer clicks, at the
   * start of a frame, before the UI and scene read input. */
  void (*feed_input)(void *state, InputState *input);
  VkrUiDockInputCapture (*build)(void *state, const VkrSampleUiFrame *frame);
  /** Managed editor publication; absent callbacks retain legacy sidecar saves.
   */
  bool8_t (*save_scene_edits)(void *state, VkrSceneEditState *edits,
                              VkrScene *scene, String8 runtime_scene_path);
  /** Optional projection of current UI anchors after Scene camera input. */
  void (*project_scene)(void *state, const VkrSampleUiFrame *frame);
  /** Optional: the world position a move drag gives `entity`, from the one
   * the pointer asks for (`to`), such as one snapped against nearby
   * objects. The drag started at `from`; the camera is at `eye`; `start`
   * marks a drag's first call, before the entity moved. The runtime keeps
   * the result on the handle's axis or plane. */
  Vec3 (*snap_move)(void *state, const VkrScene *scene, VkrEntityId entity,
                    Vec3 from, Vec3 to, Vec3 eye, bool8_t start);
  /** Optional: the angle in radians a rotate drag turns by, from the one the
   * pointer asks for, such as rounded to whole steps. */
  float32_t (*snap_turn)(void *state, float32_t radians);
  /** Optional: other entities a move of `primary` carries by the same world
   * offset, such as the rest of a selection; returns how many it wrote. Those
   * outside `primary`'s scene stay. The move undoes as one step. */
  uint32_t (*move_companions)(void *state, VkrEntityId primary,
                              VkrEntityId *out, uint32_t capacity);
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
   * names a per-user file. In project mode, the machine-local file the editor
   * starts with, whatever the variable says; NULL keeps no file. */
  const char *graphics_settings_path;
  /** Graphics settings JSON applied over the backend defaults before the
   * preferences file, such as a packaged game's `graphics`; may be empty. */
  String8 graphics_defaults;
  VkrSamplePresentationConfig presentation;
  VkrSampleUiClient ui;
  /** C script modules linked into the executable (ADR-079), registered in
   * order before any scene loads. */
  const VkrModuleEntry *script_modules;
  uint32_t script_module_count;
} VkrSampleRuntimeConfig;

VkrSampleRuntimeConfig vkr_sample_runtime_config_default(void);
int vkr_sample_runtime_run(int argc, char **argv,
                           const VkrSampleRuntimeConfig *config);
