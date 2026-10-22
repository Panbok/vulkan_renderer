#pragma once

#include "vkr_sample_runtime.h"

typedef struct VkrEditorUi VkrEditorUi;
typedef struct VkrEditorProjects VkrEditorProjects;
typedef struct VkrEditorBakery VkrEditorBakery;

/* Editor-lifetime owner, independent of loaded world/scene assets. */
VkrEditorProjects *vkr_editor_projects_create(VkrAllocator *allocator, int argc,
                                              char **argv);
bool8_t vkr_editor_projects_destroy(VkrEditorProjects *projects,
                                    VkrEditorUi *editor,
                                    const VkrUiDockTree *dock);
bool8_t vkr_editor_projects_modal(const VkrEditorProjects *projects);
/* Whether a floating project dialog covers the pixel. */
bool8_t vkr_editor_projects_dialog_contains(const VkrEditorProjects *projects,
                                            float32_t x_px, float32_t y_px);
/** The chooser or create flow runs before any project is open, so the window
 * shows a compact launcher instead of the editor shell. */
bool8_t vkr_editor_projects_launcher(const VkrEditorProjects *projects);
bool8_t vkr_editor_projects_loading(const VkrEditorProjects *projects);

/* A project scene opening as the primary scene, from the request that
   unloads the open one until the opened scene is usable: Bakery prepares
   it, the runtime reads and activates it, then its brushes, terrain and
   scatter copies build over the next frames. Texture streaming after that
   is not part of it. */
typedef enum VkrEditorSceneOpenPhase {
  VKR_EDITOR_SCENE_OPEN_NONE,
  VKR_EDITOR_SCENE_OPEN_PREPARING,
  VKR_EDITOR_SCENE_OPEN_LOADING,
  VKR_EDITOR_SCENE_OPEN_BUILDING,
} VkrEditorSceneOpenPhase;

typedef struct VkrEditorSceneOpening {
  VkrEditorSceneOpenPhase phase;
  /* The project scene's id and name; borrowed until the project changes. */
  const char *scene_id;
  String8 name;
  /* What runs now, such as "Reading scene files"; borrowed for the build. */
  String8 step;
  /* Seconds since the request. */
  float64_t seconds;
  /* The step's countable work, such as brushes built of all that build;
     `total` is zero when the step has no count. */
  uint32_t done;
  uint32_t total;
} VkrEditorSceneOpening;

/* The scene opening this build, as the last update found it; false and
   VKR_EDITOR_SCENE_OPEN_NONE when no scene opens. */
bool8_t vkr_editor_projects_opening(const VkrEditorProjects *projects,
                                    VkrEditorSceneOpening *out);
/* A scene job, its activation or a Set primary swap is still in progress.
   A failed or cancelled job waiting for Retry or Back is not. */
bool8_t vkr_editor_projects_busy(const VkrEditorProjects *projects,
                                 VkrEditorBakery *bakery);
/** Background finalize progress for the Cmd `stats` root: whether a job
 * runs and how many finished materials its (or the last) job applied. */
void vkr_editor_projects_finalize_stats(const VkrEditorProjects *projects,
                                        bool8_t *out_running,
                                        uint32_t *out_applied);
void vkr_editor_projects_update(VkrEditorProjects *projects,
                                VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame);
void vkr_editor_projects_build(VkrEditorProjects *projects, VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame);
void vkr_editor_projects_build_scene_progress(VkrEditorProjects *projects,
                                              VkrEditorUi *editor,
                                              const VkrSampleUiFrame *frame);
/** Projects and Scenes dropdown buttons in top-bar columns `first_column`
 * and `first_column + 1`. */
void vkr_editor_projects_navigation(VkrEditorProjects *projects,
                                    VkrEditorUi *editor,
                                    const VkrSampleUiFrame *frame,
                                    uint32_t first_column);

bool8_t vkr_editor_projects_save_scene(VkrEditorProjects *projects,
                                       VkrSceneEditState *edits,
                                       VkrScene *scene,
                                       String8 runtime_scene_path);
void vkr_editor_projects_scene_action(VkrEditorProjects *projects,
                                      VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame);

/* Whether the open primary scene is the writable managed scene of the
   project, whose document holds its reflection probes (ADR-103). */
bool8_t vkr_editor_projects_managed_scene(const VkrEditorProjects *projects,
                                          const VkrSampleUiFrame *frame);
bool8_t vkr_editor_projects_can_add_entity(const VkrEditorProjects *projects,
                                           VkrEditorUi *editor,
                                           const VkrSampleUiFrame *frame);
void vkr_editor_projects_add_entity(VkrEditorProjects *projects,
                                    VkrEditorUi *editor,
                                    const VkrSampleUiFrame *frame);

bool8_t vkr_editor_projects_flush(VkrEditorProjects *projects,
                                  VkrEditorUi *editor,
                                  const VkrUiDockTree *dock);

/* Create or import window (ADR-076), opened by Content's Import button: a
   scene of this project, or an asset copied into the open scene. */
void vkr_editor_projects_build_create_window(VkrEditorProjects *projects,
                                             VkrEditorUi *editor,
                                             const VkrSampleUiFrame *frame,
                                             VkrUiRect bounds);

/* Start the job that creates an empty scene named `name` in the open project
   and opens it; false with a message when no job can start. */
bool8_t vkr_editor_projects_create_scene(VkrEditorProjects *projects,
                                         VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame,
                                         const char *name);

/* Cmd's scene.bake (ADR-075, ADR-088): start the Bake lighting job for the
   open project scene, with lightmaps when `lightmap` is set, traced with
   `samples` per texel (0 keeps the baker's default). False when it cannot
   start (no writable project scene, unsaved edits, or a job, load or dialog
   in the way); vkr_editor_projects_message says why. */
bool8_t vkr_editor_projects_bake_lighting(VkrEditorProjects *projects,
                                          VkrEditorUi *editor,
                                          const VkrSampleUiFrame *frame,
                                          bool8_t lightmap, uint32_t samples);
/* The bake settings every Bake lighting job sends (ADR-100); the project
   window owns them for the session. */
struct VkrEditorBakeSettings;
struct VkrEditorBakeSettings *
vkr_editor_projects_bake_settings(VkrEditorProjects *projects);

/* The outcome of the last bake vkr_editor_projects_bake_lighting started,
   taken once its job and the reload after it settle; a job that ended
   without recording one reports the status line as a failure. */
const char *vkr_editor_projects_take_bake_result(VkrEditorProjects *projects,
                                                 bool8_t *out_succeeded);

/* The open project scene's display name, or empty outside a project. */
String8 vkr_editor_projects_scene_name(const VkrEditorProjects *projects);
/* The display name of the project scene with `scene_id`, or empty. */
String8 vkr_editor_projects_scene_name_of(const VkrEditorProjects *projects,
                                          const char *scene_id);

/* Open the project scene named `name` (exact, else first containing it);
   false when none matches or a job is running. The scene already loading or
   open stays as it is and sets *out_current. */
bool8_t vkr_editor_projects_open_scene(VkrEditorProjects *projects,
                                       VkrEditorUi *editor,
                                       const VkrSampleUiFrame *frame,
                                       String8 name, bool8_t *out_current);

/* The open project scene's id (empty when only the World is open), whether
   a switch can start now, and switching; workbench tabs keep a scene each
   (ADR-089). */
String8 vkr_editor_projects_scene_id(const VkrEditorProjects *projects);
bool8_t vkr_editor_projects_switch_ready(const VkrEditorProjects *projects);
/* Open the scene with `scene_id`, or with an empty id close the open scene
   so only the World shows. Unsaved edits ask first. */
bool8_t vkr_editor_projects_show_scene(VkrEditorProjects *projects,
                                       VkrEditorUi *editor,
                                       const VkrSampleUiFrame *frame,
                                       const char *scene_id);

/* Added scenes (ADR-076): the project name of the scene an added runtime
   document came from, or empty; adding the project scene named `name`
   beside the open one; and making the added scene in `container` the
   primary scene, adding the previous primary back beside it. Unsaved edits
   in either scene ask first. False when nothing could start. */
String8 vkr_editor_projects_added_name(const VkrEditorProjects *projects,
                                       String8 path);
/* The project id of the scene an added runtime document came from, or "". */
const char *vkr_editor_projects_added_id(const VkrEditorProjects *projects,
                                         String8 path);
bool8_t vkr_editor_projects_add_scene(VkrEditorProjects *projects,
                                      VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame,
                                      String8 name);
bool8_t vkr_editor_projects_set_primary(VkrEditorProjects *projects,
                                        VkrEditorUi *editor,
                                        const VkrSampleUiFrame *frame,
                                        uint16_t container);
/* One model a placement job adds to the open scene (model.place): a model
   file the job imports (glTF, GLB, OBJ or cooked VKB, absolute or relative
   to the workspace), else an existing mesh asset, with its name, pose,
   scale and canonical tags. */
#define VKR_EDITOR_MODEL_PLACE_MAX 512u
typedef struct VkrEditorModelPlacement {
  char source[1024];
  char scope[8];
  char asset[37];
  char name[128];
  Vec3 position;
  VkrQuat rotation;
  Vec3 scale;
  char tags[SCENE_TAGS_CAPACITY];
} VkrEditorModelPlacement;

/* Adds `count` models to the open project scene in one add_entities job,
   which imports each source file once and reloads the scene. False, with
   vkr_editor_projects_message saying why, when no job could start: no
   project scene, another job running, or unsaved edits the reload would
   lose. */
bool8_t vkr_editor_projects_place_models(VkrEditorProjects *projects,
                                         VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame,
                                         const VkrEditorModelPlacement *models,
                                         uint32_t count, bool8_t lightmap_uvs);

/* Reimports scene or project asset `asset_id` (`scope` "scene" or
   "project") from the model file `source` in one reimport_asset job; the
   open scene stays loaded and swaps the rebuilt mesh in, so entities keep
   their poses and material overrides. False, with vkr_editor_projects_message
   saying why, when no writable project scene is open or another job runs.
   For both jobs, `lightmap_uvs` cooks the models with lightmap UVs, as the
   Bakery panel's setting does when it is on; a reimport without either
   cooks them without. */
bool8_t vkr_editor_projects_reimport_asset(
    VkrEditorProjects *projects, VkrEditorUi *editor,
    const VkrSampleUiFrame *frame, const char *scope, const char *asset_id,
    const char *source, bool8_t lightmap_uvs);

/* Prefab instance (ADR-076): copy the project scene named `name` into the
   open scene under one new root at `position`. The copy keeps no link to
   its source and reloads the scene; unsaved edits ask first. */
bool8_t vkr_editor_projects_instantiate_scene(VkrEditorProjects *projects,
                                              VkrEditorUi *editor,
                                              const VkrSampleUiFrame *frame,
                                              String8 name, Vec3 position);

/* Import one file (model, texture, font or material) into the project's
   shared assets (ADR-076); false when no job could start. */
/* Open the Create window's scene step importing `source`, as if chosen with
   Browse; the import preflight starts at once. */
bool8_t vkr_editor_projects_import_scene_form(VkrEditorProjects *projects,
                                              VkrEditorUi *editor,
                                              const char *source);
/* Files dropped from the OS (ADR-076): the Create or import window opens at
   its import step, filing the imports into Content `folder`. */
bool8_t vkr_editor_projects_import_files(VkrEditorProjects *projects,
                                         VkrEditorUi *editor,
                                         const VkrWindowFileDrop *drop,
                                         const char *folder);
bool8_t vkr_editor_projects_import_asset(VkrEditorProjects *projects,
                                         VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame,
                                         const char *source);
/* Runs the import step for one file as its form would (ADR-076): `target`
   is "world", "new" (a scene named `name`, else the file's name), "scene"
   (the project scene named `name`) or "content". False when nothing could
   start; vkr_editor_projects_message says why. Unsaved edits the placement
   would reload open the save prompt instead. */
bool8_t vkr_editor_projects_import_to(VkrEditorProjects *projects,
                                      VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame,
                                      const char *source, String8 target,
                                      String8 name);
/* Build (docs/proposals/project-packaging.md). The preflight opens the
   Save/Discard/Cancel prompt while any container has unsaved edits, then
   drains the preference writer; call it each frame until it stops waiting. */
typedef enum VkrEditorBuildPreflight {
  VKR_EDITOR_BUILD_PREFLIGHT_WAITING,
  VKR_EDITOR_BUILD_PREFLIGHT_READY,
  VKR_EDITOR_BUILD_PREFLIGHT_CANCELLED,
  VKR_EDITOR_BUILD_PREFLIGHT_UNAVAILABLE,
} VkrEditorBuildPreflight;
VkrEditorBuildPreflight
vkr_editor_projects_build_preflight(VkrEditorProjects *projects,
                                    VkrEditorUi *editor,
                                    const VkrSampleUiFrame *frame);
/* The open project and its workspace; NULL and "" outside a project. The
   project stays valid until another project opens. */
struct VkrEditorProject;
const struct VkrEditorProject *
vkr_editor_projects_project(const VkrEditorProjects *projects);
const char *
vkr_editor_projects_workspace_root(const VkrEditorProjects *projects);
bool8_t vkr_editor_projects_read_only(const VkrEditorProjects *projects);
/* Closes the open project, makes `directory` the workspace and opens
   project `project_id` there once its cards are read (empty: the project
   hub). False with the reason in the hub's message while edits are unsaved
   or when the folder does not open as a workspace. */
bool8_t vkr_editor_projects_open_workspace(VkrEditorProjects *projects,
                                           VkrEditorUi *editor,
                                           const VkrSampleUiFrame *frame,
                                           const char *directory,
                                           const char *project_id);
/* The Scene viewport of project scene `scene_id`: the live view while that
   scene is open, else the view this machine last recalled for it. False
   when neither has a camera. */
bool8_t vkr_editor_projects_scene_view(const VkrEditorProjects *projects,
                                       const VkrSampleUiFrame *frame,
                                       const char *scene_id,
                                       VkrSampleSceneRecall *out);
/* After a build that finalized project assets: publishes the report's
   inventory while the project's inventory still has the SHA-256 `expected`
   it had when the build started, then points World models at the new
   revisions. False when the inventory changed meanwhile or publication
   failed; the next build finalizes again from the cache. */
bool8_t vkr_editor_projects_adopt_inventory(VkrEditorProjects *projects,
                                            VkrEditorUi *editor,
                                            const VkrSampleUiFrame *frame,
                                            String8 assets,
                                            const uint8_t expected[32]);

/* The Projects status line: the last outcome or problem. */
const char *vkr_editor_projects_message(const VkrEditorProjects *projects);
