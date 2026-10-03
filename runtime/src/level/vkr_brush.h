#pragma once

#include "defines.h"
#include "math/vec.h"

/* Convex brush geometry (docs/proposals/level-design-toolkit.md, phase 1).
 * A brush is the intersection of half-spaces `dot(normal, p) <= distance`
 * in the brush's local space; each plane is one face. Building clips a large
 * square on every plane by all the others, in double precision, and rejects
 * solids that are open, empty or flat. The module owns no state. */

#define VKR_BRUSH_FACE_MIN 4u
#define VKR_BRUSH_FACE_MAX 64u
/* A face polygon has at most one edge per other face. */
#define VKR_BRUSH_POLYGON_MAX VKR_BRUSH_FACE_MAX
/* Vertices closer than this, in meters, are one vertex. */
#define VKR_BRUSH_WELD 1.0e-4f
/* Half the side of the square each face starts from; a vertex this far out
   means another plane never closed the solid. */
#define VKR_BRUSH_EXTENT 65536.0

typedef struct VkrBrushPlane {
  Vec3 normal;
  float32_t distance;
} VkrBrushPlane;

/* `count` corners of one face, counterclockwise seen from outside, starting
   at `first` in VkrBrushGeometry.vertices. A face may share no vertex
   storage with another. */
typedef struct VkrBrushPolygon {
  uint32_t first;
  uint32_t count;
} VkrBrushPolygon;

typedef struct VkrBrushGeometry {
  uint32_t face_count;
  VkrBrushPolygon polygons[VKR_BRUSH_FACE_MAX];
  /* Unit face normals, parallel to `polygons`. */
  Vec3 normals[VKR_BRUSH_FACE_MAX];
  uint32_t vertex_count;
  Vec3 vertices[VKR_BRUSH_FACE_MAX * VKR_BRUSH_POLYGON_MAX];
  Vec3 min;
  Vec3 max;
  float32_t volume;
} VkrBrushGeometry;

typedef enum VkrBrushError {
  VKR_BRUSH_OK = 0,
  VKR_BRUSH_ERROR_FACE_COUNT,
  VKR_BRUSH_ERROR_NON_FINITE,
  VKR_BRUSH_ERROR_EMPTY_FACE,
  VKR_BRUSH_ERROR_OPEN,
  VKR_BRUSH_ERROR_FLAT,
} VkrBrushError;

/* Builds the polygons of a convex brush. On failure `out` is unspecified
   and `out_face` names the offending face, or UINT32_MAX. */
VkrBrushError vkr_brush_build(const VkrBrushPlane *planes, uint32_t count,
                              VkrBrushGeometry *out, uint32_t *out_face);
const char *vkr_brush_error_text(VkrBrushError error);

/* The texture axes Hammer uses for a face: the world plane closest to the
   face, oriented so a texture reads upright on walls and unmirrored from
   outside. */
void vkr_brush_texture_axes(Vec3 normal, Vec3 *out_u, Vec3 *out_v);
/* UV of `point` on a face whose texture axes come from `normal`: rotated by
   `rotation` radians, `scale` meters per repeat, then shifted by `offset`
   repeats. A zero scale component counts as one meter. */
Vec2 vkr_brush_uv(Vec3 point, Vec3 normal, Vec2 offset, Vec2 scale,
                  float32_t rotation);

/* Planes of common solids in local space; each returns the plane count.
   `out` holds at least VKR_BRUSH_FACE_MAX planes. */
uint32_t vkr_brush_box_planes(Vec3 min, Vec3 max, VkrBrushPlane *out);
/* A box whose top slopes from full height at the side opposite `slope_axis`
   down to its floor at that side: `slope_axis` is 0 (+X), 1 (-X), 2 (+Z) or
   3 (-Z). */
uint32_t vkr_brush_wedge_planes(Vec3 min, Vec3 max, uint32_t slope_axis,
                                VkrBrushPlane *out);
/* A Y-axis prism of `sides` (3 to 32) whose corners lie on the circle of
   `radius` around (center.x, center.z), from `center.y` up by `height`. */
uint32_t vkr_brush_cylinder_planes(Vec3 center, float32_t radius,
                                   float32_t height, uint32_t sides,
                                   VkrBrushPlane *out);
