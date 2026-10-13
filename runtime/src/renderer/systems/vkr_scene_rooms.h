#pragma once

#include "renderer/systems/vkr_scene_system.h"

/* Rooms and portals (large-level rendering, section 4). A `room` entity's
 * world transform places a box, [-0.5, 0.5] on each axis, as a look
 * volume's: the room's interior, inside its walls. A room entity whose
 * parent is a room adds its box to the parent's room, for L-shaped spaces.
 * A `portal` entity's transform places a rectangle, [-0.5, 0.5] in its X
 * and Y with its normal along Z, in an opening between two rooms or a room
 * and the outside, the space no room holds.
 *
 * Each frame the camera's room, or the outside, sees the rooms and the
 * outside reachable through open portals inside the view, each portal
 * narrowing the view to its screen rectangle. The scene keeps every static
 * draw's rooms: up to two of the rooms and the outside that its bounds touch
 * (rooms within VKR_SCENE_ROOM_MARGIN metres; the outside where a corner or
 * the centre of its box lies in no room). A draw touching more is
 * unassigned; terrains and scatter copies belong to the outside; moving
 * draws are unassigned. The camera leaves out a draw whose rooms are all
 * hidden, and every cascade leaves out a draw whose rooms are all sealed. */

typedef struct s_VkrSceneRooms VkrSceneRooms;

#define VKR_SCENE_ROOM_MAX 1024u
#define VKR_SCENE_ROOM_BOX_MAX 4096u
#define VKR_SCENE_PORTAL_MAX 2048u
/* How far a draw's bounds may lie from a room's box and still belong to
   it: a wall between two rooms belongs to both. */
#define VKR_SCENE_ROOM_MARGIN 0.5f

/* A draw's rooms: two 16-bit entries, each a room index, the outside or
   none; VKR_SCENE_ROOMS_UNASSIGNED never hides. */
#define VKR_SCENE_ROOM_NONE 0xFFFFu
#define VKR_SCENE_ROOM_OUTSIDE 0xFFFEu
#define VKR_SCENE_ROOMS_UNASSIGNED 0xFFFFFFFFu

/** One frame's room visibility for one camera, and every static draw's
    rooms. Borrowed until the next vkr_scene_rooms_frame of the scene. */
typedef struct VkrSceneRoomFrame {
  /* Per mesh-manager mesh slot and instance slot, the index in `words` of
     its first submesh's rooms, or UINT32_MAX when its draws are
     unassigned. */
  const uint32_t *mesh_first;
  uint32_t mesh_slot_count;
  const uint32_t *instance_first;
  uint32_t instance_slot_count;
  const uint32_t *words;
  uint32_t word_count;
  /* Bit r: room r is visible, or sealed. */
  const uint64_t *visible;
  const uint64_t *sealed;
  uint32_t room_count;
  bool8_t outside_visible;
  /* The rooms' boxes, for vkr_scene_rooms_light_hidden and _box_hidden. */
  const VkrSceneRooms *owner;
} VkrSceneRoomFrame;

/** The room state of `scene` for a camera at `camera` with
    `view_projection`: rebuilds the room graph when a room or portal changed
    and the draws' rooms when the mesh manager's content did, then walks the
    portals. NULL when the scene has no room. */
const VkrSceneRoomFrame *vkr_scene_rooms_frame(VkrScene *scene, Vec3 camera,
                                               Mat4 view_projection);

/** The draw-distance bits (VKR_GPU_DRAW_CULL_HIDDEN_BIT,
    VKR_GPU_DRAW_CULL_SEALED_BIT) of submesh `submesh` of mesh or instance
    slot `slot` under `frame`; zero without a frame. */
uint32_t vkr_scene_room_cull_bits(const VkrSceneRoomFrame *frame,
                                  bool8_t instance, uint32_t slot,
                                  uint32_t submesh);

/** Whether a light at `position` reaching `range` metres lights nothing the
    camera sees under `frame`: its room, or the outside, is hidden, and its
    sphere reaches no visible room's box nor, while the outside shows, past
    its own room's box. A range of zero or infinity never hides. */
bool8_t vkr_scene_rooms_light_hidden(const VkrSceneRoomFrame *frame,
                                     Vec3 position, float32_t range);

/** Whether an axis-aligned box of `center` and `half_extents`, such as a
    reflection probe's influence, overlaps nothing the camera sees under
    `frame`, by the same rule as a light's sphere. */
bool8_t vkr_scene_rooms_box_hidden(const VkrSceneRoomFrame *frame, Vec3 center,
                                   Vec3 half_extents);

/** The rooms of `scene`, up to `capacity`: each one's owner entity and the
    world box around its boxes. Returns how many it wrote. */
uint32_t vkr_scene_rooms_list(const VkrScene *scene, VkrEntityId *out_entities,
                              Vec3 *out_min, Vec3 *out_max, uint32_t capacity);

/* What a room check finds: a portal with one space on both sides, which
   joins nothing, and a gap in a room's walls, floor or ceiling that no
   solid, no other room and no portal closes, through which the camera sees
   what the room hides. */
typedef enum VkrSceneRoomIssueKind {
  VKR_SCENE_ROOM_ISSUE_PORTAL = 0,
  VKR_SCENE_ROOM_ISSUE_GAP,
} VkrSceneRoomIssueKind;

typedef struct VkrSceneRoomIssue {
  VkrSceneRoomIssueKind kind;
  /* The portal, or the middle of the gap's open samples. */
  Vec3 position;
  /* The portal entity, or the room box whose face has the gap. */
  VkrEntityId entity;
  /* The gap's open area in square metres; zero for a portal. */
  float32_t value;
} VkrSceneRoomIssue;

/* Spacing of the samples a room check takes over each face, the depth past
   the face they lie at (inside a wall), and the smallest open area that
   counts as a gap. */
#define VKR_SCENE_ROOM_GAP_STEP 0.5f
#define VKR_SCENE_ROOM_GAP_DEPTH 0.1f
#define VKR_SCENE_ROOM_GAP_AREA_MIN 0.25f

/** Checks `scene`'s rooms and portals into `out`, up to `capacity`, and
    returns how many issues it found. `solid` answers whether collision
    fills a point (the editor's physics). */
uint32_t vkr_scene_rooms_check(const VkrScene *scene,
                               bool8_t (*solid)(void *context, Vec3 point),
                               void *context, VkrSceneRoomIssue *out,
                               uint32_t capacity);

/** Index of the room holding `point`, the smallest when boxes overlap, or
    VKR_SCENE_ROOM_OUTSIDE. */
uint32_t vkr_scene_room_at(const VkrScene *scene, Vec3 point);

void vkr_scene_rooms_shutdown(VkrScene *scene);
