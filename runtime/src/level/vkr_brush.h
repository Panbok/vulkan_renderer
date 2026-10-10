#pragma once

#include "defines.h"
#include "math/vec.h"

/* Convex brush geometry (ADR-084).
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
  /* A reshape dents the solid into more convex pieces than allowed, or
     folds a face through another. */
  VKR_BRUSH_ERROR_CONCAVE,
  /* No corner of the brush lies at a point a reshape moves. */
  VKR_BRUSH_ERROR_NO_CORNER,
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

/* Lightmap UVs of a built brush (ADR-088). Every face with a polygon is one
   chart: its polygon projected on an orthonormal basis of the face plane,
   its longer extent along the atlas's x, at
   VKR_BRUSH_LIGHTMAP_TEXELS_PER_UNIT texels per brush unit, with
   VKR_BRUSH_LIGHTMAP_PADDING texels around it, shelf-packed tallest first
   into an atlas about as wide as it is tall. A brush whose atlas would pass
   VKR_BRUSH_LIGHTMAP_MAX_SIZE halves the density, up to eight times. The
   layout depends only on the geometry, so a bake and the runtime that
   samples it compute the same UVs. A blockout shape packs the charts of
   all its pieces into one atlas the same way (vkr_blockout.h). */
#define VKR_BRUSH_LIGHTMAP_TEXELS_PER_UNIT 8.0f
#define VKR_BRUSH_LIGHTMAP_PADDING 2u
#define VKR_BRUSH_LIGHTMAP_MAX_SIZE 1024u

/* One chart of a lightmap atlas: a face polygon projected on an orthonormal
   basis of its plane. */
typedef struct VkrBrushLightmapChart {
  /* False for a face without a polygon, which takes no atlas space. */
  bool8_t present;
  /* The face's unit normal, which picks the basis. */
  Vec3 normal;
  /* The basis axes swap so that the polygon's longer extent runs along the
     atlas's x. */
  bool8_t rotated;
  /* The polygon's bounds on the (swapped) basis, in brush units. */
  Vec2 projected_min;
  Vec2 projected_max;
  /* Set by vkr_brush_lightmap_pack: the chart's lower corner in texels,
     inside its padding. */
  Vec2 corner;
} VkrBrushLightmapChart;

typedef struct VkrBrushLightmapAtlas {
  uint32_t width;
  uint32_t height;
  /* Texels per brush unit after any halving. */
  float32_t texels_per_unit;
} VkrBrushLightmapAtlas;

typedef struct VkrBrushLightmapLayout {
  VkrBrushLightmapAtlas atlas;
  /* Per face of the brush. */
  VkrBrushLightmapChart charts[VKR_BRUSH_FACE_MAX];
} VkrBrushLightmapLayout;

/* The chart of face `face` of a built brush, not present for a face without
   a polygon. */
VkrBrushLightmapChart vkr_brush_lightmap_chart(const VkrBrushGeometry *geometry,
                                               uint32_t face);
/* Packs the present charts of `charts` into one atlas, halving the density
   until it fits. `order` holds `count` indices. False when no chart is
   present or the atlas stays too large. */
bool8_t vkr_brush_lightmap_pack(VkrBrushLightmapChart *charts, uint32_t count,
                                uint32_t *order, VkrBrushLightmapAtlas *out);
/* The normalized lightmap UV of `point`, in the chart's space, on `chart`
   of `atlas`. */
Vec2 vkr_brush_lightmap_chart_uv(const VkrBrushLightmapAtlas *atlas,
                                 const VkrBrushLightmapChart *chart,
                                 Vec3 point);

/* Lays out the charts of a built brush. False when no face has a polygon or
   the atlas stays too large. */
bool8_t vkr_brush_lightmap_layout(const VkrBrushGeometry *geometry,
                                  VkrBrushLightmapLayout *out);
/* The normalized lightmap UV of `point`, in brush space, on face `face`. */
Vec2 vkr_brush_lightmap_uv(const VkrBrushLightmapLayout *layout,
                           const VkrBrushGeometry *geometry, uint32_t face,
                           Vec3 point);

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

/* A derived brush: its planes and, per plane, the input it copies. A source
   below the target's plane count names a target face; one at or above it
   names cutter face `source - target_count`; VKR_BRUSH_SOURCE_NEW marks a
   plane the operation made. */
#define VKR_BRUSH_SOURCE_NEW UINT32_MAX

typedef struct VkrBrushPiece {
  uint32_t count;
  VkrBrushPlane planes[VKR_BRUSH_FACE_MAX];
  uint32_t source[VKR_BRUSH_FACE_MAX];
} VkrBrushPiece;

/* Drops planes that bound no face, keeping the others in order. Returns
   false when what remains is not a valid solid. `scratch` holds the last
   build. */
bool8_t vkr_brush_prune(VkrBrushPiece *piece, VkrBrushGeometry *scratch);

/* vkr_brush_carve's answer when a piece needs more than VKR_BRUSH_FACE_MAX
   faces or more pieces than `capacity`; nothing it wrote is complete. */
#define VKR_BRUSH_CARVE_FAILED (UINT32_MAX - 1u)

/* The parts of `target` outside `cutter`, as non-overlapping convex pieces:
   one per cutter plane at most. Returns how many it wrote, 0 when nothing
   of the target lies outside, UINT32_MAX when the solids do not overlap,
   or VKR_BRUSH_CARVE_FAILED. Planes that stop bounding a face leave as the
   cut proceeds, so a cylinder carves out of a cylinder. */
uint32_t vkr_brush_carve(const VkrBrushPlane *target, uint32_t target_count,
                         const VkrBrushPlane *cutter, uint32_t cutter_count,
                         VkrBrushPiece *out, uint32_t capacity,
                         VkrBrushGeometry *scratch);

/* A prism grown from face `face` of the built `geometry` by `distance`
   meters along the face normal. Its back plane copies the face flipped
   (source `face`), its front plane copies it moved (source `face`), and its
   sides are new. */
bool8_t vkr_brush_extrude(const VkrBrushGeometry *geometry,
                          const VkrBrushPlane *planes, uint32_t face,
                          float32_t distance, VkrBrushPiece *out);

/* One brush equal to the union of `brush_count` brushes in one space, when
   that union is convex and the brushes do not overlap; sources index the
   concatenated input planes. */
bool8_t vkr_brush_merge(const VkrBrushPlane *const *planes,
                        const uint32_t *counts, uint32_t brush_count,
                        VkrBrushPiece *out, VkrBrushGeometry *scratch);

/* The editing grid of a face with unit normal `normal`: its texture axes
   made perpendicular to the normal and unit, so an axis-aligned face gets
   world axes and grid lines on world grid crossings. */
void vkr_brush_grid_axes(Vec3 normal, Vec3 *out_u, Vec3 *out_v);

/* The convex hull of `count` points as at most `capacity` planes with unit
   normals. Returns 0 when the points span no volume or need more planes. */
#define VKR_BRUSH_HULL_POINT_MAX 128u
uint32_t vkr_brush_hull(const Vec3 *points, uint32_t count, VkrBrushPlane *out,
                        uint32_t capacity);

/* Z-fighting (ADR-084): two drawn faces of different solids that lie in one
   plane and face the same way, so the depth test picks between them pixel
   by pixel. Their normals lie within VKR_BRUSH_COPLANAR_DEGREES of each
   other, and their overlap counts only where it lies within
   VKR_BRUSH_COPLANAR_DISTANCE of both planes. Faces back to back, or 2 mm
   apart, do not fight. */
#define VKR_BRUSH_COPLANAR_DISTANCE 1.0e-3f
#define VKR_BRUSH_COPLANAR_DEGREES 0.5f

/* One face for vkr_brush_coplanar_overlaps. The caller sets the unit
   `normal`, the `count` corners from `first` in its vertex array,
   counterclockwise seen from outside, and the solid it belongs to
   (`owner`); faces of one owner never fight. The function writes the
   rest. */
typedef struct VkrBrushFaceRef {
  Vec3 normal;
  Vec3 lo;
  Vec3 hi;
  uint32_t first;
  uint32_t count;
  uint32_t owner;
  /* The plane's distance from the faces' common center, and the farthest
     corner from that center. */
  float32_t key;
  float32_t reach;
} VkrBrushFaceRef;

/* Scratch of vkr_brush_coplanar_overlaps: where a face sits among the
   faces about in its plane and facing about its way. A face takes up to
   VKR_BRUSH_FACE_SLOT_MAX. */
#define VKR_BRUSH_FACE_SLOT_MAX 6u

typedef struct VkrBrushFaceSlot {
  int64_t bucket;
  float32_t lo;
  float32_t hi;
  uint32_t face;
  uint8_t echo;
  uint8_t classes;
} VkrBrushFaceSlot;

typedef struct VkrBrushFaceOverlap {
  uint32_t owner_a;
  uint32_t owner_b;
  /* Square meters the two faces share, the middle of that area, and the
     way the first face looks. */
  float32_t area;
  Vec3 center;
  Vec3 normal;
} VkrBrushFaceOverlap;

/* The pairs of `faces` that z-fight over more than `min_area` square
   meters, at most `capacity` of them in `out`; returns how many it found.
   `scratch` holds VKR_BRUSH_FACE_SLOT_MAX * `count` slots. Faces group by
   plane and main axis and sweep along one axis, so each meets only faces
   near its plane, facing about its way, around its extent. */
uint32_t vkr_brush_coplanar_overlaps(VkrBrushFaceRef *faces, uint32_t count,
                                     const Vec3 *vertices,
                                     VkrBrushFaceSlot *scratch,
                                     float32_t min_area,
                                     VkrBrushFaceOverlap *out,
                                     uint32_t capacity);

/* Moves the corners of the brush `planes` that lie at one of `points`
   (within 1 mm) by `delta`, after cutting it with `split` when that is not
   NULL, and writes the result to `out` as at most `capacity` (up to
   VKR_BRUSH_RESHAPE_PIECE_MAX) convex pieces and their count to
   `out_count`. A piece whose moved corners stay on its hull becomes that
   hull: a hull plane copies the source of the input plane it lies on, or of
   the input plane facing most like it, and the split's planes are
   VKR_BRUSH_SOURCE_NEW. A piece a moved corner dents becomes the solid
   bounded by its faces through the moved corners, a face no longer flat
   split into triangles from its unmoved corners, cut along those faces'
   planes until every part is convex; each part's plane copies the source of
   the face it lies on, or of the face whose plane cut it. Fails with
   VKR_BRUSH_ERROR_CONCAVE when the result needs more than `capacity`
   pieces or their volumes do not add up to the dented solid's, as when a
   face folds through another. */
#define VKR_BRUSH_RESHAPE_PIECE_MAX 8u
VkrBrushError vkr_brush_reshape(const VkrBrushPlane *planes, uint32_t count,
                                const VkrBrushPlane *split, const Vec3 *points,
                                uint32_t point_count, Vec3 delta,
                                VkrBrushPiece *out, uint32_t capacity,
                                uint32_t *out_count, VkrBrushGeometry *scratch);

/* The part of the slab between `inner` and `outer` meters out from face `face`
   (negative is inside) within the four `rect` planes, also bounded by the
   brush planes perpendicular to the face so it stays over the face. Every
   plane's source is `face`. */
bool8_t vkr_brush_patch_prism(const VkrBrushPlane *planes, uint32_t count,
                              uint32_t face, const VkrBrushPlane rect[4],
                              float32_t inner, float32_t outer,
                              VkrBrushPiece *out, VkrBrushGeometry *scratch);
