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

/* The cell edge and the columns and rows a level map of [min, max] with
   cells of at least `cell` gets. */
float32_t vkr_editor_level_map_size(Vec3 min, Vec3 max,
                                    const VkrEditorLevelCapsule *capsule,
                                    float32_t cell, uint32_t *out_columns,
                                    uint32_t *out_rows);

/* A level check spread over builds (ADR-084): a job samples a slice of its
   grid's cells each step, so a large region never stalls a frame, then one
   finisher reads the grid, once, after a step returned true. The caller
   owns the job until vkr_editor_level_job_end.

   The finishers: _lint writes up to `capacity` issues of the region, nearby
   duplicates merged, and returns how many it found; with `start`, walkable
   areas the start cannot reach count too. _reachable says whether a capsule
   at `from` walks to `to` and writes up to `path_capacity` floor points of
   one route and its length; its job covers
   vkr_editor_level_reachable_region. _map writes the text map
   (level.map): one character a cell, row by row from min z, each row
   running +x, as a top capture shows it. A cell shows its highest walkable
   floor, else its highest floor: '.' walkable (and reached from `start`
   when one is given), ',' walkable but out of reach, 'S' the start, '#'
   too close to a wall, 'n' a gap narrower than the capsule, '_' a ceiling
   too low, '/' too steep and '-' no floor; `heights`, when not NULL,
   receives each shown floor's world y, NAN where none; false when the grid
   holds more than `capacity` cells. */
typedef struct VkrEditorLevelJob VkrEditorLevelJob;

VkrEditorLevelJob *vkr_editor_level_job_begin(
    Vec3 min, Vec3 max, const VkrEditorLevelCapsule *capsule, float32_t cell);
/* Samples cells for about `seconds`; true once every cell is sampled. */
bool8_t vkr_editor_level_job_step(VkrEditorLevelJob *job, const VkrScene *scene,
                                  float64_t seconds);
/* The share of cells sampled, 0 to 1. */
float32_t vkr_editor_level_job_progress(const VkrEditorLevelJob *job);
uint32_t vkr_editor_level_job_lint(VkrEditorLevelJob *job,
                                   const VkrScene *scene, const Vec3 *start,
                                   VkrEditorLevelIssue *out, uint32_t capacity,
                                   VkrEditorLevelStats *stats);
/* Why a route failed: whether each end stands on a walkable floor, and the
   reached floor nearest `to` with the distance left. */
typedef struct VkrEditorLevelRoute {
  bool8_t from_found;
  bool8_t to_found;
  Vec3 closest;
  float32_t gap;
} VkrEditorLevelRoute;

/* Whether the capsule walks from `from` to `to`; the route to `to`, or when
   it does not reach it, to the reached floor nearest it. */
bool8_t vkr_editor_level_job_reachable(VkrEditorLevelJob *job,
                                       const VkrScene *scene, Vec3 from,
                                       Vec3 to, Vec3 *path,
                                       uint32_t path_capacity,
                                       uint32_t *path_count, float32_t *length,
                                       VkrEditorLevelRoute *route);
bool8_t vkr_editor_level_job_map(VkrEditorLevelJob *job, const VkrScene *scene,
                                 const Vec3 *start, char *text,
                                 float32_t *heights, uint32_t capacity,
                                 VkrEditorLevelStats *stats,
                                 bool8_t *out_start_found);
void vkr_editor_level_job_end(VkrEditorLevelJob *job);
/* The region query.reachable samples between two floor points. */
void vkr_editor_level_reachable_region(Vec3 from, Vec3 to, Vec3 *out_min,
                                       Vec3 *out_max);

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
  /* A check in progress: its job, sampled a slice each frame while the
     window shows, the view point issues sort by, the walk's start and the
     scene generation it samples. The report owns the job. */
  VkrEditorLevelJob *job;
  Vec3 center;
  Vec3 start;
  bool8_t has_start;
  uint64_t generation;
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

/* Whether `entity` carries an enabled free_placement: the editor never snaps
   it. */
bool8_t vkr_editor_entity_free(const VkrScene *scene, VkrEntityId entity);

/* The pieces of blockout shape `entity` in world space, in memory the caller
   frees with free(); zero when it is not a shape or does not lay out. */
uint32_t vkr_editor_shape_pieces(const VkrScene *scene, VkrEntityId entity,
                                 VkrBlockoutPiece **out);

/* The world box around the solids of `entity` and below it, its brushes and
   the pieces of its blockout shapes, else around its meshes and shapes;
   false without geometry. `scratch` holds the last brush built. */
bool8_t vkr_editor_entity_world_box(const VkrScene *scene, VkrEntityId entity,
                                    VkrBrushGeometry *scratch, Vec3 *out_lo,
                                    Vec3 *out_hi);

/* Whether box `lo`-`hi` is a slab: at most 0.5 m thick, or a quarter of
   its shorter side, as a floor is. */
bool8_t vkr_editor_box_slab(Vec3 lo, Vec3 hi);

/* The height of the floor of `entity`, whose world box is `lo`-`hi`: the top
   of the slabs (vkr_editor_box_slab) at the bottom of its box when they
   cover a quarter of its footprint, as a room's or a corridor's floor, or a
   floor slab's top; else the bottom of its box, as for stairs, a box
   standing on the ground or a mesh. */
float32_t vkr_editor_entity_floor(const VkrScene *scene, VkrEntityId entity,
                                  VkrBrushGeometry *scratch, Vec3 lo, Vec3 hi);

/* Brush magnet (ADR-084): with the Snapping menu's magnet on, a brush moved
   or drawn within reach of another brush, about 1.5 % of its distance from
   the camera, snaps flush against it or level with its sides, top or
   bottom. It snaps against the boxes around the other brushes, gathered
   once when a move or a drawn box starts. */

/* The world position a move puts `entity` (a brush, a group of brushes or a
   mesh, unless free) at, from the one the pointer asks for (`to`); `start`
   gathers the brushes of `scene` outside it, on a move's first call. */
Vec3 vkr_editor_magnet_move(VkrEditorUi *editor, const VkrScene *scene,
                            VkrEntityId entity, Vec3 from, Vec3 to, Vec3 eye,
                            bool8_t start);

/* Gathers the brushes of every loaded scene for a box being drawn. */
void vkr_editor_magnet_begin(VkrEditorUi *editor,
                             const VkrSampleUiFrame *frame);

/* `value` on `axis` (0 X, 1 Y, 2 Z), moved onto the nearest side, top or
   bottom on that axis of a gathered brush within reach of it, seen from
   `eye`, and of the region `lo`-`hi` on the other axes. */
float32_t vkr_editor_magnet_value(const VkrEditorUi *editor, uint32_t axis,
                                  float32_t value, Vec3 lo, Vec3 hi, Vec3 eye);

void vkr_editor_magnet_destroy(VkrEditorUi *editor);

/* The Level Design workbench's Tools palette: drawing and creating brushes,
   brush operations on the selection, dev materials, snapping and Level
   checks. Every button runs an existing command or agent operation. */
void vkr_editor_level_palette_build(VkrEditorUi *editor,
                                    const VkrSampleUiFrame *frame,
                                    VkrUiRect bounds);
