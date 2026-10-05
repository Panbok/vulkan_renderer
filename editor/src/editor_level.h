#pragma once

#include "editor_ui.h"
#include "level/vkr_brush.h"

/* Level checks against the player capsule (ADR-084). A region's walkable floor
 * is sampled on a grid of capsule-radius cells with physics raycasts: every
 * floor a downward ray finds from start heights one capsule height apart, so
 * rooms under roofs count. A floor is walkable when its slope is within the
 * limit and the capsule fits on it. Neighbouring floors connect when the step
 * up is within `step_up`, a drop is at most VKR_EDITOR_LEVEL_DROP_MAX, and
 * nothing blocks the way at knee height. Only collision counts: geometry
 * without collision is invisible to the checks. */

#define VKR_EDITOR_LEVEL_DROP_MAX 4.0f
/* Grid cells one check samples at most; a larger region uses larger
   cells. */
#define VKR_EDITOR_LEVEL_CELL_MAX 65536u

typedef struct VkrEditorLevelCapsule {
  float32_t radius;
  float32_t height;
  float32_t step_up;
  float32_t max_slope_radians;
} VkrEditorLevelCapsule;

typedef enum VkrEditorLevelIssueKind {
  VKR_EDITOR_LEVEL_STEP_TOO_HIGH = 0,
  VKR_EDITOR_LEVEL_TOO_STEEP,
  VKR_EDITOR_LEVEL_LOW_CEILING,
  VKR_EDITOR_LEVEL_TOO_NARROW,
  VKR_EDITOR_LEVEL_VOID_EDGE,
  VKR_EDITOR_LEVEL_UNREACHABLE,
  VKR_EDITOR_LEVEL_OVERLAP,
  VKR_EDITOR_LEVEL_INVALID_BRUSH,
  /* An IO connection whose source lies in the region and that will not
     route (ADR-084). */
  VKR_EDITOR_LEVEL_BROKEN_CONNECTION,
  VKR_EDITOR_LEVEL_ISSUE_COUNT,
} VkrEditorLevelIssueKind;

typedef struct VkrEditorLevelIssue {
  VkrEditorLevelIssueKind kind;
  Vec3 position;
  /* The brush or entity at fault, when one is. */
  VkrEntityId entity;
  VkrEntityId other;
  /* Step height, slope degrees, headroom, gap width or area, by kind. */
  float32_t value;
} VkrEditorLevelIssue;

typedef struct VkrEditorLevelStats {
  float32_t cell;
  uint32_t samples;
  uint32_t walkable;
  uint32_t reachable;
} VkrEditorLevelStats;

/* The engine's default character capsule. */
VkrEditorLevelCapsule vkr_editor_level_capsule_default(void);

/* Checks the region [min, max] of `scene` and writes up to `capacity`
   issues, nearby duplicates merged; returns how many it found. With `start`,
   walkable areas the start cannot reach are reported. */
uint32_t vkr_editor_level_lint(const VkrScene *scene, Vec3 min, Vec3 max,
                               const VkrEditorLevelCapsule *capsule,
                               const Vec3 *start, VkrEditorLevelIssue *out,
                               uint32_t capacity, VkrEditorLevelStats *stats);

/* Whether a capsule standing at `from` can walk to `to` inside the region
   both points span, padded by 16 m. Writes up to `path_capacity` floor
   points of one route and its length. */
bool8_t vkr_editor_level_reachable(const VkrScene *scene, Vec3 from, Vec3 to,
                                   const VkrEditorLevelCapsule *capsule,
                                   Vec3 *path, uint32_t path_capacity,
                                   uint32_t *path_count, float32_t *length);

const char *vkr_editor_level_issue_name(VkrEditorLevelIssueKind kind);

/* The first face of `brush` a ray from `origin` along unit `direction`
   enters, within `max_distance`; the brush's planes come from its faces in
   world space. */
bool8_t vkr_editor_brush_ray(const VkrScene *scene, VkrEntityId brush,
                             Vec3 origin, Vec3 direction,
                             float32_t max_distance, float32_t *out_distance,
                             VkrEntityId *out_face);

/* The nearest brush face a ray enters, among the brushes of every loaded
   scene. */
bool8_t vkr_editor_brush_pick(const VkrSampleUiFrame *frame, Vec3 origin,
                              Vec3 direction, float32_t max_distance,
                              VkrEntityId *out_face);

/* The world-space planes of `brush`'s faces, at most `capacity`, in the
   order vkr_scene_brush_faces lists them; zero when it has more. */
uint32_t vkr_editor_brush_world_planes(const VkrScene *scene, VkrEntityId brush,
                                       VkrBrushPlane *out, uint32_t capacity);

/* Builds `brush` in world space into `scratch` from its faces, whose
   entities `faces` (VKR_BRUSH_FACE_MAX) receives parallel to the polygons;
   returns the face count, or zero when the brush does not build. */
uint32_t vkr_editor_brush_build(const VkrScene *scene, VkrEntityId brush,
                                VkrBrushGeometry *scratch, VkrEntityId *faces);

/* The world-space corners of brush face `face`, counterclockwise from
   outside, when its brush builds; returns how many it wrote. `scratch`
   holds the brush's geometry. */
uint32_t vkr_editor_brush_face_outline(const VkrScene *scene, VkrEntityId face,
                                       VkrBrushGeometry *scratch, Vec3 *out,
                                       uint32_t capacity);

/* Issues the Level checks window shows, and its last check. */
#define VKR_EDITOR_LEVEL_SHOWN_MAX 64u

typedef struct VkrEditorLevelReport {
  bool8_t checked;
  Vec3 min;
  Vec3 max;
  uint32_t found;
  uint32_t count;
  VkrEditorLevelIssue issues[VKR_EDITOR_LEVEL_SHOWN_MAX];
  VkrEditorLevelStats stats;
} VkrEditorLevelReport;

/* The report the Level checks window and overlay share, created on first
   use and released with the editor. */
VkrEditorLevelReport *vkr_editor_level_report(VkrEditorUi *editor);

/* The Level checks window body: Check runs the lint over 40 m around the
   point the Scene's center looks at, from the Player Start when there is
   one; each issue has Focus. */
void vkr_editor_level_window_build(VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame,
                                   VkrUiRect bounds);

/* The Level Design workbench's Tools palette: drawing and creating brushes,
   brush operations on the selection, dev materials, snapping and Level
   checks. Every button runs an existing command or agent operation. */
void vkr_editor_level_palette_build(VkrEditorUi *editor,
                                    const VkrSampleUiFrame *frame,
                                    VkrUiRect bounds);
