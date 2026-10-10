#include "brush_test.h"

#include "level/vkr_blockout.h"
#include "level/vkr_brush.h"
#include "level/vkr_surface.h"
#include "memory/vkr_arena_allocator.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool8_t brush_test_near(float32_t a, float32_t b, float32_t tolerance) {
  return fabsf(a - b) <= tolerance;
}

/* Every polygon winds counterclockwise seen from outside and lies on its
   plane. */
static void brush_test_polygons(const VkrBrushGeometry *geometry,
                                const VkrBrushPlane *planes) {
  for (uint32_t face = 0; face < geometry->face_count; ++face) {
    const VkrBrushPolygon polygon = geometry->polygons[face];
    assert(polygon.count >= 3u);
    const Vec3 *v = geometry->vertices + polygon.first;
    Vec3 area = vec3_zero();
    for (uint32_t i = 2; i < polygon.count; ++i) {
      area = vec3_add(
          area, vec3_cross(vec3_sub(v[i - 1u], v[0]), vec3_sub(v[i], v[0])));
    }
    assert(vec3_dot(area, geometry->normals[face]) > 0.0f);
    const float32_t length = vec3_length(planes[face].normal);
    for (uint32_t i = 0; i < polygon.count; ++i) {
      assert(brush_test_near(vec3_dot(v[i], planes[face].normal) / length,
                             planes[face].distance / length, 1.0e-3f));
    }
  }
}

static void brush_test_solids(VkrBrushGeometry *geometry) {
  VkrBrushPlane planes[VKR_BRUSH_FACE_MAX + 1u];
  uint32_t face = 0u;

  /* A box: six quads, its volume and bounds. */
  uint32_t count = vkr_brush_box_planes(vec3_new(-1.0f, 0.0f, 2.0f),
                                        vec3_new(3.0f, 2.5f, 4.0f), planes);
  assert(vkr_brush_build(planes, count, geometry, &face) == VKR_BRUSH_OK);
  assert(geometry->face_count == 6u && geometry->vertex_count == 24u);
  assert(brush_test_near(geometry->volume, 4.0f * 2.5f * 2.0f, 1.0e-3f));
  assert(brush_test_near(geometry->min.x, -1.0f, 1.0e-4f) &&
         brush_test_near(geometry->max.y, 2.5f, 1.0e-4f) &&
         brush_test_near(geometry->max.z, 4.0f, 1.0e-4f));
  brush_test_polygons(geometry, planes);

  /* A wedge in each direction holds half its box. */
  for (uint32_t axis = 0; axis < 4u; ++axis) {
    count = vkr_brush_wedge_planes(vec3_new(0.0f, 0.0f, 0.0f),
                                   vec3_new(4.0f, 1.0f, 2.0f), axis, planes);
    assert(vkr_brush_build(planes, count, geometry, &face) == VKR_BRUSH_OK);
    assert(geometry->face_count == 5u);
    assert(brush_test_near(geometry->volume, 4.0f, 1.0e-3f));
    assert(brush_test_near(geometry->max.y, 1.0f, 1.0e-4f));
    brush_test_polygons(geometry, planes);
  }

  /* An octagonal prism with corners on a 2 m circle: area 2*sqrt(2)*r^2. */
  count = vkr_brush_cylinder_planes(vec3_new(1.0f, 0.5f, -1.0f), 2.0f, 3.0f, 8u,
                                    planes);
  assert(count == 10u);
  assert(vkr_brush_build(planes, count, geometry, &face) == VKR_BRUSH_OK);
  assert(brush_test_near(geometry->volume, 2.0f * sqrtf(2.0f) * 4.0f * 3.0f,
                         1.0e-2f));
  assert(brush_test_near(geometry->min.y, 0.5f, 1.0e-4f) &&
         brush_test_near(geometry->max.y, 3.5f, 1.0e-4f));
  brush_test_polygons(geometry, planes);
  /* A four-sided prism is an axis-aligned box. */
  count = vkr_brush_cylinder_planes(vec3_zero(), sqrtf(2.0f), 1.0f, 4u, planes);
  assert(vkr_brush_build(planes, count, geometry, &face) == VKR_BRUSH_OK);
  assert(brush_test_near(geometry->max.x, 1.0f, 1.0e-4f) &&
         brush_test_near(geometry->min.z, -1.0f, 1.0e-4f));

  /* Rejections name the failure and, where one is to blame, the face. */
  count = vkr_brush_box_planes(vec3_zero(), vec3_one(), planes);
  assert(vkr_brush_build(planes, 5u, geometry, &face) == VKR_BRUSH_ERROR_OPEN);
  planes[6] =
      (VkrBrushPlane){.normal = vec3_new(1.0f, 0.0f, 0.0f), .distance = 3.0f};
  assert(vkr_brush_build(planes, 7u, geometry, &face) ==
             VKR_BRUSH_ERROR_EMPTY_FACE &&
         face == 6u);
  planes[2].normal = vec3_zero();
  assert(vkr_brush_build(planes, count, geometry, &face) ==
             VKR_BRUSH_ERROR_NON_FINITE &&
         face == 2u);
  count = vkr_brush_box_planes(vec3_zero(), vec3_new(1.0f, 0.0f, 1.0f), planes);
  assert(vkr_brush_build(planes, count, geometry, &face) != VKR_BRUSH_OK);
  assert(vkr_brush_build(planes, 3u, geometry, &face) ==
         VKR_BRUSH_ERROR_FACE_COUNT);
}

/* Carve, prune, extrude and merge keep volume and convexity. */
static void brush_test_editing(VkrBrushGeometry *geometry) {
  VkrBrushPlane wall[6];
  VkrBrushPlane door[6];
  (void)vkr_brush_box_planes(vec3_new(0.0f, 0.0f, 0.0f),
                             vec3_new(4.0f, 3.0f, 0.25f), wall);
  (void)vkr_brush_box_planes(vec3_new(1.5f, 0.0f, -1.0f),
                             vec3_new(2.5f, 2.0f, 1.0f), door);
  VkrBrushPiece *pieces = malloc(sizeof(*pieces) * 6u);
  assert(pieces);
  /* A doorway through a wall leaves the wall's volume minus the opening,
     in pieces that do not overlap. */
  const uint32_t count =
      vkr_brush_carve(wall, 6u, door, 6u, pieces, 6u, geometry);
  assert(count >= 3u && count <= 6u);
  float32_t volume = 0.0f;
  for (uint32_t i = 0; i < count; ++i) {
    assert(vkr_brush_build(pieces[i].planes, pieces[i].count, geometry, NULL) ==
           VKR_BRUSH_OK);
    volume += geometry->volume;
    for (uint32_t p = 0; p < pieces[i].count; ++p) {
      assert(pieces[i].source[p] < 12u);
    }
  }
  assert(brush_test_near(volume, 4.0f * 3.0f * 0.25f - 1.0f * 2.0f * 0.25f,
                         1.0e-3f));
  /* A cutter beside the wall does not touch it; one around it removes it. */
  VkrBrushPlane beside[6];
  (void)vkr_brush_box_planes(vec3_new(10.0f, 0.0f, 0.0f),
                             vec3_new(11.0f, 1.0f, 1.0f), beside);
  assert(vkr_brush_carve(wall, 6u, beside, 6u, pieces, 6u, geometry) ==
         UINT32_MAX);
  VkrBrushPlane around[6];
  (void)vkr_brush_box_planes(vec3_new(-1.0f, -1.0f, -1.0f),
                             vec3_new(5.0f, 4.0f, 1.0f), around);
  assert(vkr_brush_carve(wall, 6u, around, 6u, pieces, 6u, geometry) == 0u);

  /* A 32-sided cylinder carved out of a wider one leaves a tube: the two
     hold 68 planes, more than a brush, so the carve used to answer 0 and
     the editor deleted both brushes. */
  VkrBrushPlane outer[VKR_BRUSH_FACE_MAX];
  VkrBrushPlane inner[VKR_BRUSH_FACE_MAX];
  const uint32_t outer_count = vkr_brush_cylinder_planes(
      vec3_new(0.0f, 0.0f, 0.0f), 8.0f, 10.0f, 32u, outer);
  const uint32_t inner_count = vkr_brush_cylinder_planes(
      vec3_new(0.0f, 0.5f, 0.0f), 7.5f, 9.0f, 32u, inner);
  assert(vkr_brush_build(outer, outer_count, geometry, NULL) == VKR_BRUSH_OK);
  const float32_t outer_volume = geometry->volume;
  assert(vkr_brush_build(inner, inner_count, geometry, NULL) == VKR_BRUSH_OK);
  const float32_t inner_volume = geometry->volume;
  VkrBrushPiece *tube = malloc(sizeof(*tube) * VKR_BRUSH_FACE_MAX);
  assert(tube);
  const uint32_t tube_count =
      vkr_brush_carve(outer, outer_count, inner, inner_count, tube,
                      VKR_BRUSH_FACE_MAX, geometry);
  assert(tube_count > 0u && tube_count <= inner_count);
  float32_t tube_volume = 0.0f;
  for (uint32_t i = 0; i < tube_count; ++i) {
    assert(vkr_brush_build(tube[i].planes, tube[i].count, geometry, NULL) ==
           VKR_BRUSH_OK);
    tube_volume += geometry->volume;
  }
  assert(brush_test_near(tube_volume, outer_volume - inner_volume, 1.0e-2f));
  /* Turned half a side, the same cylinder meets the first in a 64-sided
     prism of 66 faces, which no brush holds: the carve says so instead of
     answering with pieces. */
  VkrBrushPlane turned[VKR_BRUSH_FACE_MAX];
  const uint32_t turned_count = vkr_brush_cylinder_planes(
      vec3_new(0.0f, 0.0f, 0.0f), 8.0f, 10.0f, 32u, turned);
  const float32_t half = 3.14159265358979f / 32.0f;
  for (uint32_t i = 0; i < 32u; ++i) {
    const Vec3 n = turned[i].normal;
    turned[i].normal = vec3_new(n.x * cosf(half) - n.z * sinf(half), 0.0f,
                                n.x * sinf(half) + n.z * cosf(half));
  }
  assert(vkr_brush_carve(outer, outer_count, turned, turned_count, tube,
                         VKR_BRUSH_FACE_MAX,
                         geometry) == VKR_BRUSH_CARVE_FAILED);
  free(tube);

  /* Pruning drops a plane that bounds nothing. */
  VkrBrushPiece box = {0};
  box.count = vkr_brush_box_planes(vec3_zero(), vec3_one(), box.planes);
  box.planes[box.count] = (VkrBrushPlane){vec3_new(1.0f, 0.0f, 0.0f), 2.0f};
  box.count++;
  for (uint32_t i = 0; i < box.count; ++i) {
    box.source[i] = i;
  }
  assert(vkr_brush_prune(&box, geometry) && box.count == 6u);
  for (uint32_t i = 0; i < box.count; ++i) {
    assert(box.source[i] != 6u);
  }

  /* Extruding a box's top by 2 m stacks a 2 m prism on it. */
  VkrBrushPlane base[6];
  (void)vkr_brush_box_planes(vec3_zero(), vec3_new(2.0f, 1.0f, 3.0f), base);
  assert(vkr_brush_build(base, 6u, geometry, NULL) == VKR_BRUSH_OK);
  VkrBrushPiece prism = {0};
  assert(vkr_brush_extrude(geometry, base, 2u, 2.0f, &prism));
  assert(vkr_brush_build(prism.planes, prism.count, geometry, NULL) ==
         VKR_BRUSH_OK);
  assert(brush_test_near(geometry->volume, 2.0f * 2.0f * 3.0f, 1.0e-3f));
  assert(brush_test_near(geometry->min.y, 1.0f, 1.0e-4f) &&
         brush_test_near(geometry->max.y, 3.0f, 1.0e-4f));

  /* Two touching boxes merge; boxes with a gap or overlap do not. */
  VkrBrushPlane left[6];
  VkrBrushPlane right[6];
  (void)vkr_brush_box_planes(vec3_zero(), vec3_new(1.0f, 1.0f, 1.0f), left);
  (void)vkr_brush_box_planes(vec3_new(1.0f, 0.0f, 0.0f),
                             vec3_new(3.0f, 1.0f, 1.0f), right);
  const VkrBrushPlane *both[2] = {left, right};
  const uint32_t counts[2] = {6u, 6u};
  VkrBrushPiece merged = {0};
  assert(vkr_brush_merge(both, counts, 2u, &merged, geometry));
  assert(merged.count == 6u &&
         brush_test_near(geometry->volume, 3.0f, 1.0e-3f));
  (void)vkr_brush_box_planes(vec3_new(1.5f, 0.0f, 0.0f),
                             vec3_new(3.0f, 1.0f, 1.0f), right);
  assert(!vkr_brush_merge(both, counts, 2u, &merged, geometry));
  (void)vkr_brush_box_planes(vec3_new(0.5f, 0.0f, 0.0f),
                             vec3_new(3.0f, 1.0f, 1.0f), right);
  assert(!vkr_brush_merge(both, counts, 2u, &merged, geometry));
  free(pieces);
}

/* The face of `planes` whose normal is `normal`. */
static uint32_t brush_test_face(const VkrBrushPlane *planes, uint32_t count,
                                Vec3 normal) {
  for (uint32_t i = 0; i < count; ++i) {
    if (vec3_dot(vec3_normalize(planes[i].normal), normal) > 0.999f) {
      return i;
    }
  }
  assert(false && "no face with that normal");
  return 0u;
}

static void brush_test_grid_editing(VkrBrushGeometry *geometry) {
  /* The hull of a cube's corners is the cube. */
  const Vec3 cube[8] = {
      {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
      {1.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 1.0f},
      {0.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f},
  };
  VkrBrushPlane hull[VKR_BRUSH_FACE_MAX];
  const uint32_t hull_count =
      vkr_brush_hull(cube, 8u, hull, VKR_BRUSH_FACE_MAX);
  assert(hull_count == 6u);
  assert(vkr_brush_build(hull, hull_count, geometry, NULL) == VKR_BRUSH_OK);
  assert(brush_test_near(geometry->volume, 1.0f, 1.0e-4f));
  /* Four points in one plane span nothing. */
  assert(vkr_brush_hull(cube, 4u, hull, VKR_BRUSH_FACE_MAX) == 0u);

  /* Raising a unit box's top back edge by 1 m adds a triangular prism of
     half a cubic meter; the bottom keeps its face. */
  VkrBrushPlane box[6];
  (void)vkr_brush_box_planes(vec3_zero(), vec3_one(), box);
  VkrBrushPiece pieces[2];
  uint32_t piece_count = 0u;
  const Vec3 edge[2] = {{0.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}};
  assert(vkr_brush_reshape(box, 6u, NULL, edge, 2u, vec3_new(0.0f, 1.0f, 0.0f),
                           pieces, 2u, &piece_count, geometry) == VKR_BRUSH_OK);
  assert(piece_count == 1u);
  assert(vkr_brush_build(pieces[0].planes, pieces[0].count, geometry, NULL) ==
         VKR_BRUSH_OK);
  assert(brush_test_near(geometry->volume, 1.5f, 1.0e-3f));
  const uint32_t bottom = brush_test_face(box, 6u, vec3_new(0.0f, -1.0f, 0.0f));
  const uint32_t reshaped_bottom = brush_test_face(
      pieces[0].planes, pieces[0].count, vec3_new(0.0f, -1.0f, 0.0f));
  assert(pieces[0].source[reshaped_bottom] == bottom);

  /* Raising the grid line across the middle of a 2 x 1 x 1 box's top by
     half a meter splits it into two pieces that each gain a quarter of a
     cubic meter; their shared face is new. */
  VkrBrushPlane long_box[6];
  (void)vkr_brush_box_planes(vec3_zero(), vec3_new(2.0f, 1.0f, 1.0f), long_box);
  const VkrBrushPlane split = {vec3_new(1.0f, 0.0f, 0.0f), 1.0f};
  const Vec3 line[2] = {{1.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}};
  assert(vkr_brush_reshape(long_box, 6u, &split, line, 2u,
                           vec3_new(0.0f, 0.5f, 0.0f), pieces, 2u, &piece_count,
                           geometry) == VKR_BRUSH_OK);
  assert(piece_count == 2u);
  for (uint32_t p = 0; p < 2u; ++p) {
    assert(vkr_brush_build(pieces[p].planes, pieces[p].count, geometry, NULL) ==
           VKR_BRUSH_OK);
    assert(brush_test_near(geometry->volume, 1.25f, 1.0e-3f));
    const uint32_t inner =
        brush_test_face(pieces[p].planes, pieces[p].count,
                        vec3_new(p ? -1.0f : 1.0f, 0.0f, 0.0f));
    assert(pieces[p].source[inner] == VKR_BRUSH_SOURCE_NEW);
  }

  /* A point off the brush moves nothing. */
  const Vec3 away = {5.0f, 5.0f, 5.0f};
  assert(vkr_brush_reshape(box, 6u, NULL, &away, 1u, vec3_one(), pieces, 2u,
                           &piece_count,
                           geometry) == VKR_BRUSH_ERROR_NO_CORNER);

  /* A 1 m cell of a 4 x 1 x 4 slab's top pulled up 1 m is a 1 m cube on
     it; pushed in half a meter it carves half a cubic meter out. */
  VkrBrushPlane slab[6];
  (void)vkr_brush_box_planes(vec3_zero(), vec3_new(4.0f, 1.0f, 4.0f), slab);
  const uint32_t top = brush_test_face(slab, 6u, vec3_new(0.0f, 1.0f, 0.0f));
  const VkrBrushPlane cell[4] = {
      {{1.0f, 0.0f, 0.0f}, 2.0f},
      {{-1.0f, 0.0f, 0.0f}, -1.0f},
      {{0.0f, 0.0f, 1.0f}, 2.0f},
      {{0.0f, 0.0f, -1.0f}, -1.0f},
  };
  VkrBrushPiece prism = {0};
  assert(
      vkr_brush_patch_prism(slab, 6u, top, cell, 0.0f, 1.0f, &prism, geometry));
  assert(brush_test_near(geometry->volume, 1.0f, 1.0e-3f));
  assert(brush_test_near(geometry->min.y, 1.0f, 1.0e-4f) &&
         brush_test_near(geometry->max.y, 2.0f, 1.0e-4f));
  for (uint32_t i = 0; i < prism.count; ++i) {
    assert(prism.source[i] == top);
  }
  const VkrBrushPlane *with_cell[2] = {slab, prism.planes};
  const uint32_t cell_counts[2] = {6u, prism.count};
  VkrBrushPiece merged = {0};
  assert(!vkr_brush_merge(with_cell, cell_counts, 2u, &merged, geometry));
  assert(vkr_brush_patch_prism(slab, 6u, top, cell, -0.5f, 0.01f, &prism,
                               geometry));
  VkrBrushPiece *carved = malloc(sizeof(*carved) * VKR_BRUSH_FACE_MAX);
  assert(carved);
  const uint32_t carved_count =
      vkr_brush_carve(slab, 6u, prism.planes, prism.count, carved,
                      VKR_BRUSH_FACE_MAX, geometry);
  assert(carved_count != UINT32_MAX && carved_count > 0u);
  float32_t volume = 0.0f;
  for (uint32_t i = 0; i < carved_count; ++i) {
    assert(vkr_brush_build(carved[i].planes, carved[i].count, geometry, NULL) ==
           VKR_BRUSH_OK);
    volume += geometry->volume;
  }
  assert(brush_test_near(volume, 16.0f - 0.5f, 1.0e-3f));
  free(carved);

  /* The whole top pulled up 1 m merges into one 4 x 2 x 4 brush. */
  const VkrBrushPlane whole[4] = {
      {{1.0f, 0.0f, 0.0f}, 4.0f},
      {{-1.0f, 0.0f, 0.0f}, 0.0f},
      {{0.0f, 0.0f, 1.0f}, 4.0f},
      {{0.0f, 0.0f, -1.0f}, 0.0f},
  };
  assert(vkr_brush_patch_prism(slab, 6u, top, whole, 0.0f, 1.0f, &prism,
                               geometry));
  const uint32_t whole_counts[2] = {6u, prism.count};
  assert(vkr_brush_merge(with_cell, whole_counts, 2u, &merged, geometry));
  assert(brush_test_near(geometry->volume, 32.0f, 1.0e-3f));

  /* A top face's grid runs along world X and Z. */
  Vec3 u = {0};
  Vec3 v = {0};
  vkr_brush_grid_axes(vec3_new(0.0f, 1.0f, 0.0f), &u, &v);
  assert(brush_test_near(fabsf(u.x), 1.0f, 1.0e-5f) &&
         brush_test_near(fabsf(v.z), 1.0f, 1.0e-5f));
}

static void brush_test_uv(void) {
  /* A +X wall reads left to right toward -Z, and down the image is down. */
  const Vec2 uv =
      vkr_brush_uv(vec3_new(1.0f, 2.0f, -3.0f), vec3_new(1.0f, 0.0f, 0.0f),
                   vec2_zero(), vec2_new(1.0f, 1.0f), 0.0f);
  assert(brush_test_near(uv.x, 3.0f, 1.0e-5f) &&
         brush_test_near(uv.y, -2.0f, 1.0e-5f));
  const Vec2 scaled =
      vkr_brush_uv(vec3_new(1.0f, 2.0f, -3.0f), vec3_new(1.0f, 0.0f, 0.0f),
                   vec2_new(0.25f, 0.0f), vec2_new(2.0f, 4.0f), 0.0f);
  assert(brush_test_near(scaled.x, 1.75f, 1.0e-5f) &&
         brush_test_near(scaled.y, -0.5f, 1.0e-5f));
  const Vec2 turned =
      vkr_brush_uv(vec3_new(1.0f, 2.0f, -3.0f), vec3_new(1.0f, 0.0f, 0.0f),
                   vec2_zero(), vec2_new(1.0f, 1.0f), 1.5707963f);
  assert(brush_test_near(turned.x, 2.0f, 1.0e-4f) &&
         brush_test_near(turned.y, 3.0f, 1.0e-4f));
  /* Floors use X and Z; adjacent walls share the same world axes. */
  const Vec2 floor =
      vkr_brush_uv(vec3_new(5.0f, 0.0f, 7.0f), vec3_new(0.0f, 1.0f, 0.0f),
                   vec2_zero(), vec2_new(1.0f, 1.0f), 0.0f);
  assert(brush_test_near(floor.x, 5.0f, 1.0e-5f) &&
         brush_test_near(floor.y, 7.0f, 1.0e-5f));
}

/* Lightmap charts of a box and a wedge stay inside the atlas, keep the
   brush's proportions at the layout density, and never share texels; a
   brush too large for one atlas halves its density until it fits. */
static void brush_test_lightmap(VkrBrushGeometry *geometry) {
  VkrBrushPlane planes[VKR_BRUSH_FACE_MAX];
  const Vec3 shapes[2][2] = {{{0.0f, 0.0f, 0.0f}, {4.0f, 3.0f, 6.0f}},
                             {{-2.0f, 0.0f, -1.0f}, {3.0f, 2.0f, 5.0f}}};
  for (uint32_t shape = 0; shape < 2u; ++shape) {
    const uint32_t count =
        shape == 0u
            ? vkr_brush_box_planes(shapes[0][0], shapes[0][1], planes)
            : vkr_brush_wedge_planes(shapes[1][0], shapes[1][1], 0u, planes);
    assert(vkr_brush_build(planes, count, geometry, NULL) == VKR_BRUSH_OK);
    VkrBrushLightmapLayout layout;
    assert(vkr_brush_lightmap_layout(geometry, &layout));
    assert(layout.atlas.texels_per_unit == VKR_BRUSH_LIGHTMAP_TEXELS_PER_UNIT);
    Vec2 lower[VKR_BRUSH_FACE_MAX];
    Vec2 upper[VKR_BRUSH_FACE_MAX];
    for (uint32_t face = 0; face < geometry->face_count; ++face) {
      const VkrBrushPolygon polygon = geometry->polygons[face];
      lower[face] = vec2_new(INFINITY, INFINITY);
      upper[face] = vec2_new(-INFINITY, -INFINITY);
      for (uint32_t i = 0; i < polygon.count; ++i) {
        const Vec3 point = geometry->vertices[polygon.first + i];
        const Vec2 uv = vkr_brush_lightmap_uv(&layout, geometry, face, point);
        assert(uv.x >= 0.0f && uv.x <= 1.0f && uv.y >= 0.0f && uv.y <= 1.0f);
        const Vec2 texel = vec2_new(uv.x * (float32_t)layout.atlas.width,
                                    uv.y * (float32_t)layout.atlas.height);
        lower[face] = vec2_new(fminf(lower[face].x, texel.x),
                               fminf(lower[face].y, texel.y));
        upper[face] = vec2_new(fmaxf(upper[face].x, texel.x),
                               fmaxf(upper[face].y, texel.y));
      }
      /* The first edge keeps its length in texels. */
      const Vec3 a = geometry->vertices[polygon.first];
      const Vec3 b = geometry->vertices[polygon.first + 1u];
      const Vec2 ua = vkr_brush_lightmap_uv(&layout, geometry, face, a);
      const Vec2 ub = vkr_brush_lightmap_uv(&layout, geometry, face, b);
      const float32_t texels =
          sqrtf(powf((ub.x - ua.x) * (float32_t)layout.atlas.width, 2.0f) +
                powf((ub.y - ua.y) * (float32_t)layout.atlas.height, 2.0f));
      assert(brush_test_near(
          texels, vec3_length(vec3_sub(b, a)) * layout.atlas.texels_per_unit,
          1.0e-2f));
    }
    for (uint32_t f = 0; f < geometry->face_count; ++f) {
      for (uint32_t g = f + 1u; g < geometry->face_count; ++g) {
        const bool8_t apart =
            upper[f].x <= lower[g].x || upper[g].x <= lower[f].x ||
            upper[f].y <= lower[g].y || upper[g].y <= lower[f].y;
        assert(apart);
      }
    }
  }
  /* 600 units at 8 texels each would pass the atlas limit. */
  const uint32_t count = vkr_brush_box_planes(
      vec3_new(0.0f, 0.0f, 0.0f), vec3_new(600.0f, 4.0f, 600.0f), planes);
  assert(vkr_brush_build(planes, count, geometry, NULL) == VKR_BRUSH_OK);
  VkrBrushLightmapLayout layout;
  assert(vkr_brush_lightmap_layout(geometry, &layout));
  assert(layout.atlas.texels_per_unit < VKR_BRUSH_LIGHTMAP_TEXELS_PER_UNIT);
  assert(layout.atlas.width <= VKR_BRUSH_LIGHTMAP_MAX_SIZE &&
         layout.atlas.height <= VKR_BRUSH_LIGHTMAP_MAX_SIZE);
}

/* Stairs or a corridor with every size set and no corner of its own. */
static SceneBlockout brush_test_shape(SceneBlockoutShape kind,
                                      SceneStairsKind stairs) {
  SceneBlockout shape;
  MemZero(&shape, sizeof(shape));
  shape.shape = kind;
  shape.stairs = stairs;
  shape.height = 3.0f;
  shape.width = 1.5f;
  shape.length = 4.0f;
  shape.step_height = 0.2f;
  shape.turn = 180.0f;
  shape.radius = 1.0f;
  for (uint32_t i = 0; i < SCENE_BLOCKOUT_POINT_MAX; ++i) {
    shape.corners[i] = -1.0f;
  }
  return shape;
}

/* Lays `shape` out into exactly the pieces vkr_blockout_piece_capacity asks
   for, followed by guard pieces the layout must leave untouched. */
static uint32_t brush_test_layout(const SceneBlockout *shape,
                                  VkrBlockoutPiece **out) {
  const uint32_t capacity = vkr_blockout_piece_capacity(shape);
  const uint32_t guard = 4u;
  VkrBlockoutPiece *pieces = malloc((capacity + guard) * sizeof(*pieces));
  assert(pieces);
  MemSet(pieces, 0xA5, (capacity + guard) * sizeof(*pieces));
  char error[160] = {0};
  const uint32_t count =
      vkr_blockout_layout(shape, pieces, capacity, error, sizeof(error));
  assert(count <= capacity);
  const uint8_t *tail = (const uint8_t *)(pieces + capacity);
  for (uint64_t i = 0; i < guard * sizeof(*pieces); ++i) {
    assert(tail[i] == 0xA5);
  }
  *out = pieces;
  return count;
}

/* The box around a piece's points. */
static void brush_test_piece_box(const VkrBlockoutPiece *piece, Vec3 *lo,
                                 Vec3 *hi) {
  *lo = vec3_new(INFINITY, INFINITY, INFINITY);
  *hi = vec3_new(-INFINITY, -INFINITY, -INFINITY);
  for (uint32_t p = 0; p < piece->point_count; ++p) {
    const Vec3 at = piece->points[p];
    *lo = vec3_new(fminf(lo->x, at.x), fminf(lo->y, at.y), fminf(lo->z, at.z));
    *hi = vec3_new(fmaxf(hi->x, at.x), fmaxf(hi->y, at.y), fmaxf(hi->z, at.z));
  }
}

/* Blockout layouts (ADR-084) fit the capacity they ask for, every piece is
   a convex solid, a spiral climbs hundreds of meters at an even pitch with
   no flight buried in the one below, and the step limit refuses more. */
/* Coplanar overlaps among blockout pieces, each a hull brush as the level
   checks build them (ADR-084 z_fight), but for faces down on the floor. */
static uint32_t brush_test_pieces_fight(const VkrBlockoutPiece *pieces,
                                        uint32_t count,
                                        VkrBrushGeometry *geometry) {
  enum { FACE_CAP = 1024, VERTEX_CAP = 8192 };
  VkrBrushFaceRef *faces = malloc(FACE_CAP * sizeof(*faces));
  Vec3 *vertices = malloc(VERTEX_CAP * sizeof(*vertices));
  VkrBrushFaceSlot *slots =
      malloc(FACE_CAP * VKR_BRUSH_FACE_SLOT_MAX * sizeof(*slots));
  assert(faces && vertices && slots);
  uint32_t face_count = 0u;
  uint32_t vertex_count = 0u;
  for (uint32_t i = 0; i < count; ++i) {
    VkrBrushPlane planes[VKR_BRUSH_FACE_MAX];
    const uint32_t plane_count = vkr_brush_hull(
        pieces[i].points, pieces[i].point_count, planes, VKR_BRUSH_FACE_MAX);
    assert(plane_count >= 4u);
    assert(vkr_brush_build(planes, plane_count, geometry, NULL) ==
           VKR_BRUSH_OK);
    for (uint32_t f = 0; f < geometry->face_count; ++f) {
      const VkrBrushPolygon polygon = geometry->polygons[f];
      /* A face down on the base rests on the floor, which hides it. */
      if (geometry->normals[f].y < -0.99f &&
          fabsf(geometry->vertices[polygon.first].y) < 1.0e-4f) {
        continue;
      }
      assert(face_count < FACE_CAP &&
             vertex_count + polygon.count <= VERTEX_CAP);
      faces[face_count++] = (VkrBrushFaceRef){
          .normal = geometry->normals[f],
          .first = vertex_count,
          .count = polygon.count,
          .owner = i,
      };
      for (uint32_t k = 0; k < polygon.count; ++k) {
        vertices[vertex_count++] = geometry->vertices[polygon.first + k];
      }
    }
  }
  VkrBrushFaceOverlap fight = {0};
  const uint32_t found = vkr_brush_coplanar_overlaps(
      faces, face_count, vertices, slots, 1.0e-4f, &fight, 1u);
  if (found) {
    printf("  pieces %u and %u fight over %g m2 at (%g, %g, %g), normal "
           "(%g, %g, %g)\n",
           fight.owner_a, fight.owner_b, (double)fight.area,
           (double)fight.center.x, (double)fight.center.y,
           (double)fight.center.z, (double)fight.normal.x,
           (double)fight.normal.y, (double)fight.normal.z);
  }
  free(faces);
  free(vertices);
  free(slots);
  return found;
}

static void brush_test_blockout(VkrBrushGeometry *geometry) {
  VkrBlockoutPiece *pieces = NULL;
  VkrBrushPlane planes[VKR_BRUSH_FACE_MAX];

  /* Every kind of stairs, solid and floating. */
  for (uint32_t kind = 0; kind < SCENE_STAIRS_KIND_COUNT; ++kind) {
    for (uint32_t floating = 0; floating < 2u; ++floating) {
      SceneBlockout stairs =
          brush_test_shape(SCENE_BLOCKOUT_STAIRS, (SceneStairsKind)kind);
      stairs.thickness = floating ? 0.1f : 0.0f;
      const uint32_t count = brush_test_layout(&stairs, &pieces);
      assert(count >= 15u);
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t plane_count =
            vkr_brush_hull(pieces[i].points, pieces[i].point_count, planes,
                           VKR_BRUSH_FACE_MAX);
        assert(plane_count >= VKR_BRUSH_FACE_MIN);
        assert(vkr_brush_build(planes, plane_count, geometry, NULL) ==
               VKR_BRUSH_OK);
      }
      free(pieces);
    }
  }

  /* An 800 m spiral: 4000 steps, its pole and a landing, an even rise, and
     each step clear above the one a full turn below it. */
  SceneBlockout spiral =
      brush_test_shape(SCENE_BLOCKOUT_STAIRS, SCENE_STAIRS_SPIRAL);
  spiral.height = 800.0f;
  spiral.turn = 72000.0f;
  const uint32_t steps = 4000u;
  uint32_t count = brush_test_layout(&spiral, &pieces);
  assert(count == steps + 2u);
  const uint32_t per_turn = steps / 200u;
  for (uint32_t i = 0; i < steps; ++i) {
    Vec3 lo = {0};
    Vec3 hi = {0};
    brush_test_piece_box(&pieces[i], &lo, &hi);
    assert(pieces[i].kind == VKR_BLOCKOUT_PIECE_STEP);
    assert(brush_test_near(hi.y, 0.2f * (float32_t)(i + 1u), 2.0e-3f));
    if (i >= per_turn) {
      Vec3 below_lo = {0};
      Vec3 below_hi = {0};
      brush_test_piece_box(&pieces[i - per_turn], &below_lo, &below_hi);
      assert(lo.y >= below_hi.y - 1.0e-3f);
    }
  }
  assert(pieces[steps].kind == VKR_BLOCKOUT_PIECE_POLE);
  free(pieces);

  /* A one-turn spiral ends on a landing level with its top whose near edge
     is the top step's far edge, the radius at the full sweep. */
  spiral.height = 3.0f;
  spiral.turn = 360.0f;
  count = brush_test_layout(&spiral, &pieces);
  assert(count == 15u + 2u);
  const VkrBlockoutPiece *top = &pieces[14];
  const VkrBlockoutPiece *landing = &pieces[16];
  assert(landing->kind == VKR_BLOCKOUT_PIECE_LANDING);
  Vec3 landing_lo = {0};
  Vec3 landing_hi = {0};
  brush_test_piece_box(landing, &landing_lo, &landing_hi);
  assert(brush_test_near(landing_hi.y, 3.0f, 1.0e-4f));
  /* Corners 2 and 3 of the top step (its far edge, top face) are corners 1
     and 0 of the landing. */
  for (uint32_t k = 0; k < 2u; ++k) {
    const Vec3 a = top->points[4u + 3u - k];
    const Vec3 b = landing->points[4u + k];
    assert(brush_test_near(a.x, b.x, 1.0e-4f) &&
           brush_test_near(a.z, b.z, 1.0e-4f));
  }
  /* Its pieces as the level checks build them share no drawn plane. */
  assert(brush_test_pieces_fight(pieces, count, geometry) == 0u);
  free(pieces);

  /* More steps than the limit is refused. */
  spiral.height = 0.2f * (float32_t)(VKR_BLOCKOUT_STEP_MAX + 1u);
  assert(brush_test_layout(&spiral, &pieces) == 0u);
  free(pieces);

  /* The longest corridor: 16 points zigzagging at 90 degrees, every corner
     rounded and eight openings, in the room it asks for. */
  SceneBlockout corridor =
      brush_test_shape(SCENE_BLOCKOUT_CORRIDOR, SCENE_STAIRS_STRAIGHT);
  corridor.width = 2.0f;
  corridor.thickness = 0.25f;
  corridor.radius = 2.0f;
  corridor.ceiling = true_v;
  corridor.point_count = SCENE_BLOCKOUT_POINT_MAX;
  for (uint32_t i = 0; i < SCENE_BLOCKOUT_POINT_MAX; ++i) {
    corridor.points[i] = vec3_new((float32_t)((i + 1u) / 2u) * 10.0f, 0.0f,
                                  (float32_t)(i / 2u) * 10.0f);
  }
  corridor.opening_count = SCENE_BLOCKOUT_OPENING_MAX;
  for (uint32_t i = 0; i < SCENE_BLOCKOUT_OPENING_MAX; ++i) {
    corridor.walls[i] = i;
    corridor.openings[i] = vec4_new(3.0f, 5.0f, 0.5f, 2.0f);
  }
  count = brush_test_layout(&corridor, &pieces);
  assert(count > 0u);
  free(pieces);

  /* With sharp corners each stretch is one of the path: two walls each,
     and an opening inside a wall adds the pieces after, above and below
     it. */
  corridor.radius = 0.0f;
  count = brush_test_layout(&corridor, &pieces);
  uint32_t walls = 0u;
  for (uint32_t i = 0; i < count; ++i) {
    walls += pieces[i].kind == VKR_BLOCKOUT_PIECE_WALL;
  }
  assert(walls == 2u * (SCENE_BLOCKOUT_POINT_MAX - 1u) +
                      3u * SCENE_BLOCKOUT_OPENING_MAX);
  free(pieces);
}

/* A corridor's lightmap atlas holds a chart for every face of every piece
   that builds: each polygon maps inside the atlas at the atlas density, and
   no two charts share texels, so neither the bake nor the runtime bleeds
   one face's light into another. */
static void brush_test_blockout_lightmap(VkrBrushGeometry *geometry) {
  SceneBlockout corridor =
      brush_test_shape(SCENE_BLOCKOUT_CORRIDOR, SCENE_STAIRS_STRAIGHT);
  corridor.width = 3.0f;
  corridor.height = 3.2f;
  corridor.thickness = 0.25f;
  corridor.radius = 0.0f;
  corridor.ceiling = true_v;
  corridor.point_count = 2u;
  corridor.points[0] = vec3_zero();
  corridor.points[1] = vec3_new(45.75f, 0.0f, 0.0f);
  corridor.opening_count = 2u;
  corridor.walls[0] = 0u;
  corridor.openings[0] = vec4_new(9.5f, 10.7f, 0.0f, 2.2f);
  corridor.walls[1] = 1u;
  corridor.openings[1] = vec4_new(23.5f, 24.7f, 0.0f, 2.2f);
  VkrBlockoutPiece *pieces = NULL;
  const uint32_t count = brush_test_layout(&corridor, &pieces);
  assert(count > 0u);
  const uint32_t chart_count =
      vkr_blockout_lightmap_chart_count(pieces, count, geometry);
  assert(chart_count >= count * 4u);
  VkrBrushLightmapChart *charts = malloc(chart_count * sizeof(*charts));
  uint32_t *order = malloc(chart_count * sizeof(*order));
  uint32_t *first = malloc(count * sizeof(*first));
  Vec2 *lower = malloc(chart_count * sizeof(*lower));
  Vec2 *upper = malloc(chart_count * sizeof(*upper));
  assert(charts && order && first && lower && upper);
  VkrBrushLightmapAtlas atlas;
  assert(vkr_blockout_lightmap_layout(pieces, count, geometry, charts, order,
                                      first, &atlas));
  assert(atlas.texels_per_unit == VKR_BRUSH_LIGHTMAP_TEXELS_PER_UNIT);
  uint32_t used = 0u;
  for (uint32_t i = 0; i < count; ++i) {
    assert(first[i] != UINT32_MAX);
    assert(vkr_blockout_piece_build(&pieces[i], geometry));
    for (uint32_t f = 0; f < geometry->face_count; ++f) {
      const uint32_t chart = first[i] + f;
      const VkrBrushPolygon polygon = geometry->polygons[f];
      lower[chart] = vec2_new(INFINITY, INFINITY);
      upper[chart] = vec2_new(-INFINITY, -INFINITY);
      for (uint32_t c = 0; c < polygon.count; ++c) {
        const Vec2 uv = vkr_brush_lightmap_chart_uv(
            &atlas, &charts[chart], geometry->vertices[polygon.first + c]);
        assert(uv.x >= 0.0f && uv.x <= 1.0f && uv.y >= 0.0f && uv.y <= 1.0f);
        const Vec2 texel = vec2_new(uv.x * (float32_t)atlas.width,
                                    uv.y * (float32_t)atlas.height);
        lower[chart] = vec2_new(fminf(lower[chart].x, texel.x),
                                fminf(lower[chart].y, texel.y));
        upper[chart] = vec2_new(fmaxf(upper[chart].x, texel.x),
                                fmaxf(upper[chart].y, texel.y));
      }
      const Vec3 a = geometry->vertices[polygon.first];
      const Vec3 b = geometry->vertices[polygon.first + 1u];
      const Vec2 ua = vkr_brush_lightmap_chart_uv(&atlas, &charts[chart], a);
      const Vec2 ub = vkr_brush_lightmap_chart_uv(&atlas, &charts[chart], b);
      const float32_t texels =
          sqrtf(powf((ub.x - ua.x) * (float32_t)atlas.width, 2.0f) +
                powf((ub.y - ua.y) * (float32_t)atlas.height, 2.0f));
      assert(brush_test_near(
          texels, vec3_length(vec3_sub(b, a)) * atlas.texels_per_unit,
          1.0e-1f));
      ++used;
    }
  }
  assert(used == chart_count);
  for (uint32_t f = 0; f < chart_count; ++f) {
    for (uint32_t g = f + 1u; g < chart_count; ++g) {
      const bool8_t apart =
          upper[f].x <= lower[g].x || upper[g].x <= lower[f].x ||
          upper[f].y <= lower[g].y || upper[g].y <= lower[f].y;
      assert(apart);
    }
  }
  free(upper);
  free(lower);
  free(first);
  free(order);
  free(charts);
  free(pieces);
}

/* True when `point` lies inside every plane of `piece`. */
static bool8_t brush_test_inside(const VkrBrushPiece *piece, Vec3 point) {
  for (uint32_t i = 0; i < piece->count; ++i) {
    const float32_t length = vec3_length(piece->planes[i].normal);
    if (vec3_dot(piece->planes[i].normal, point) / length >
        piece->planes[i].distance / length + 1.0e-5f) {
      return false_v;
    }
  }
  return true_v;
}

/* Dents a unit cube by moving `points` by `delta` and checks the pieces
   against the dented solid: `volume`, a point `kept` inside it and a point
   `removed` in the dent. Returns the piece count. */
static uint32_t brush_test_dent(const Vec3 *points, uint32_t point_count,
                                Vec3 delta, float32_t volume, Vec3 kept,
                                Vec3 removed, VkrBrushGeometry *geometry) {
  VkrBrushPlane cube[6];
  (void)vkr_brush_box_planes(vec3_zero(), vec3_one(), cube);
  VkrBrushPiece *pieces =
      malloc(sizeof(*pieces) * (VKR_BRUSH_RESHAPE_PIECE_MAX + 1u));
  assert(pieces);
  uint32_t count = 0u;
  assert(vkr_brush_reshape(cube, 6u, NULL, points, point_count, delta, pieces,
                           VKR_BRUSH_RESHAPE_PIECE_MAX, &count,
                           geometry) == VKR_BRUSH_OK);
  assert(count >= 2u && count <= VKR_BRUSH_RESHAPE_PIECE_MAX);

  /* Convex pieces inside the cube, each face copying a cube face, that
     fill the dented solid's volume. */
  float32_t sum = 0.0f;
  uint32_t holding_kept = 0u;
  for (uint32_t a = 0; a < count; ++a) {
    assert(vkr_brush_build(pieces[a].planes, pieces[a].count, geometry, NULL) ==
           VKR_BRUSH_OK);
    sum += geometry->volume;
    for (uint32_t v = 0; v < geometry->vertex_count; ++v) {
      const Vec3 p = geometry->vertices[v];
      assert(p.x > -1.0e-4f && p.y > -1.0e-4f && p.z > -1.0e-4f &&
             p.x < 1.0f + 1.0e-4f && p.y < 1.0f + 1.0e-4f &&
             p.z < 1.0f + 1.0e-4f);
    }
    for (uint32_t f = 0; f < pieces[a].count; ++f) {
      assert(pieces[a].source[f] < 6u);
    }
    holding_kept += brush_test_inside(&pieces[a], kept) ? 1u : 0u;
    assert(!brush_test_inside(&pieces[a], removed));
  }
  assert(fabsf(sum - volume) <= 1.0e-4f * volume);
  assert(holding_kept == 1u);

  /* Two pieces share at most a face: their planes together bound no
     volume. */
  for (uint32_t a = 0; a < count; ++a) {
    for (uint32_t b = a + 1u; b < count; ++b) {
      VkrBrushPiece *both = &pieces[VKR_BRUSH_RESHAPE_PIECE_MAX];
      *both = pieces[a];
      for (uint32_t f = 0; f < pieces[b].count; ++f) {
        assert(both->count < VKR_BRUSH_FACE_MAX);
        both->planes[both->count] = pieces[b].planes[f];
        both->source[both->count++] = pieces[b].source[f];
      }
      assert(!vkr_brush_prune(both, geometry) || geometry->volume < 1.0e-5f);
    }
  }
  free(pieces);
  return count;
}

static void brush_test_dents(VkrBrushGeometry *geometry) {
  /* A unit cube's top front edge pushed 0.75 m down and 0.75 m back ends
     inside the solid: the cross-section is the square without the quad
     between the old edge and the new one, 0.25 m^2. */
  const Vec3 edge[2] = {{0.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}};
  const Vec3 edge_delta = vec3_new(0.0f, -0.75f, -0.75f);
  (void)brush_test_dent(edge, 2u, edge_delta, 0.25f, vec3_new(0.5f, 0.1f, 0.1f),
                        vec3_new(0.5f, 0.4f, 0.4f), geometry);

  /* A corner pushed to (0.4, 0.4, 0.4) bends each of its faces along the
     line between its unmoved neighbours: the cube loses the corner's
     tetrahedron (1/6 m^3) and the pit under it to the new corner (0.8/6 m^3).
   */
  const Vec3 corner = {1.0f, 1.0f, 1.0f};
  (void)brush_test_dent(&corner, 1u, vec3_new(-0.6f, -0.6f, -0.6f), 0.7f,
                        vec3_new(0.1f, 0.1f, 0.1f), vec3_new(0.6f, 0.6f, 0.6f),
                        geometry);

  /* A dent needing more pieces than the caller takes is refused. */
  VkrBrushPlane cube[6];
  (void)vkr_brush_box_planes(vec3_zero(), vec3_one(), cube);
  VkrBrushPiece *piece = malloc(sizeof(*piece));
  assert(piece);
  uint32_t count = 0u;
  assert(vkr_brush_reshape(cube, 6u, NULL, edge, 2u, edge_delta, piece, 1u,
                           &count, geometry) == VKR_BRUSH_ERROR_CONCAVE &&
         count == 0u);
  free(piece);
}

/* Two boxes built as owners 0 and 1, every corner turned `angle` radians
   about Y and moved by `offset`, and the pairs of their faces that
   z-fight; `out` receives the first. */
static uint32_t brush_test_fights(Vec3 a_lo, Vec3 a_hi, Vec3 b_lo, Vec3 b_hi,
                                  float32_t angle, Vec3 offset,
                                  VkrBrushGeometry *geometry,
                                  VkrBrushFaceOverlap *out) {
  VkrBrushFaceRef faces[2u * VKR_BRUSH_FACE_MAX];
  Vec3 vertices[64u];
  uint32_t face_count = 0u;
  uint32_t vertex_count = 0u;
  const float32_t c = cosf(angle);
  const float32_t s = sinf(angle);
  for (uint32_t owner = 0; owner < 2u; ++owner) {
    VkrBrushPlane planes[VKR_BRUSH_FACE_MAX];
    const uint32_t plane_count =
        vkr_brush_box_planes(owner ? b_lo : a_lo, owner ? b_hi : a_hi, planes);
    assert(vkr_brush_build(planes, plane_count, geometry, NULL) ==
           VKR_BRUSH_OK);
    for (uint32_t f = 0; f < geometry->face_count; ++f) {
      const VkrBrushPolygon polygon = geometry->polygons[f];
      const Vec3 n = geometry->normals[f];
      faces[face_count++] = (VkrBrushFaceRef){
          .normal = vec3_new(c * n.x + s * n.z, n.y, -s * n.x + c * n.z),
          .first = vertex_count,
          .count = polygon.count,
          .owner = owner,
      };
      for (uint32_t k = 0; k < polygon.count; ++k) {
        const Vec3 p = geometry->vertices[polygon.first + k];
        assert(vertex_count < ArrayCount(vertices));
        vertices[vertex_count++] = vec3_add(
            offset, vec3_new(c * p.x + s * p.z, p.y, -s * p.x + c * p.z));
      }
    }
  }
  VkrBrushFaceSlot slots[VKR_BRUSH_FACE_SLOT_MAX * ArrayCount(faces)];
  return vkr_brush_coplanar_overlaps(faces, face_count, vertices, slots,
                                     1.0e-4f, out, 1u);
}

/* Z-fighting (ADR-084): boxes whose tops share a plane and overlap fight
   over the shared square, turned and far from the origin too; a box on
   another's top, back to back, a top 2 mm higher and tops sharing a strip
   0.2 mm wide do not. */
static void brush_test_coplanar(VkrBrushGeometry *geometry) {
  const Vec3 lo = vec3_new(0.0f, 0.0f, 0.0f);
  const Vec3 hi = vec3_new(2.0f, 1.0f, 2.0f);
  VkrBrushFaceOverlap overlap = {0};

  uint32_t found = brush_test_fights(lo, hi, vec3_new(1.0f, 0.5f, 1.0f),
                                     vec3_new(3.0f, 1.0f, 3.0f), 0.0f,
                                     vec3_zero(), geometry, &overlap);
  assert(found == 1u);
  assert(brush_test_near(overlap.area, 1.0f, 1.0e-4f));
  assert(brush_test_near(overlap.center.x, 1.5f, 1.0e-4f));
  assert(brush_test_near(overlap.center.y, 1.0f, 1.0e-4f));
  assert(brush_test_near(overlap.center.z, 1.5f, 1.0e-4f));
  assert(overlap.owner_a != overlap.owner_b);

  const Vec3 distant = vec3_new(1000.0f, 20.0f, -500.0f);
  found = brush_test_fights(lo, hi, vec3_new(1.0f, 0.5f, 1.0f),
                            vec3_new(3.0f, 1.0f, 3.0f), 0.5236f, distant, geometry,
                            &overlap);
  assert(found == 1u);
  assert(brush_test_near(overlap.area, 1.0f, 1.0e-2f));
  assert(brush_test_near(overlap.center.y, 21.0f, 1.0e-3f));

  /* Back to back: the upper box's bottom on the lower one's top. Their
     sides share planes but touch only along an edge. */
  found = brush_test_fights(lo, hi, vec3_new(0.0f, 1.0f, 0.0f),
                            vec3_new(2.0f, 2.0f, 2.0f), 0.0f, vec3_zero(),
                            geometry, &overlap);
  assert(found == 0u);

  found = brush_test_fights(lo, hi, vec3_new(1.0f, 0.5f, 1.0f),
                            vec3_new(3.0f, 1.002f, 3.0f), 0.0f, vec3_zero(),
                            geometry, &overlap);
  assert(found == 0u);

  /* Tops sharing a plane over a 0.2 mm strip, 4 cm2 along a 2 m edge: an
     edge contact float error leaves, not a fight. */
  found = brush_test_fights(lo, hi, vec3_new(1.9998f, 0.5f, 0.0f),
                            vec3_new(3.0f, 1.0f, 2.0f), 0.0f, vec3_zero(),
                            geometry, &overlap);
  assert(found == 0u);
}

/* Surface tags (vkr_surface.h): orientations, which look a face shows,
   the retired dev palette's replacements, and that every greybox look the
   table names is on disk, so a tag cannot point at a missing file. */
static void brush_test_surfaces(void) {
  assert(vkr_surface_orientation(1.0f) == VKR_SURFACE_FLOOR);
  assert(vkr_surface_orientation(0.71f) == VKR_SURFACE_FLOOR);
  assert(vkr_surface_orientation(0.69f) == VKR_SURFACE_WALL);
  assert(vkr_surface_orientation(-0.69f) == VKR_SURFACE_WALL);
  assert(vkr_surface_orientation(-1.0f) == VKR_SURFACE_CEILING);

  assert(
      strcmp(vkr_surface_face_material(VKR_SURFACE_WOOD, VKR_SURFACE_MARK_NONE,
                                       "", NULL, NULL, 0.0f, false_v),
             "assets/materials/greybox/wood_wall.mt") == 0);
  assert(strcmp(vkr_surface_face_material(VKR_SURFACE_WOOD,
                                          VKR_SURFACE_MARK_HAZARD, NULL, NULL,
                                          NULL, 1.0f, false_v),
                "assets/materials/greybox/mark_hazard_floor.mt") == 0);
  const char *art = "assets/materials/bistro/wall.mt";
  assert(vkr_surface_face_material(VKR_SURFACE_BRICK, VKR_SURFACE_MARK_NONE,
                                   art, NULL, NULL, 0.0f, false_v) == art);
  assert(
      strcmp(vkr_surface_face_material(VKR_SURFACE_BRICK, VKR_SURFACE_MARK_NONE,
                                       art, NULL, NULL, -1.0f, true_v),
             "assets/materials/greybox/brick_ceiling.mt") == 0);

  /* Themes: a container's binding over the World's, the face's own
     material over both, marks ignored, and the greybox view over all. */
  const char *theme_json =
      "{\"version\": 1, \"materials\": {\"brick\": "
      "\"assets/materials/art/brick.mt\", \"wood\": \"\"}}";
  const char *world_json = "{\"version\": 1, \"materials\": {\"brick\": "
                           "\"assets/materials/art/old_brick.mt\", \"wood\": "
                           "\"assets/materials/art/oak.mt\"}}";
  VkrSurfaceTheme theme = {0};
  VkrSurfaceTheme world = {0};
  char error[160] = {0};
  assert(vkr_surface_theme_read(
      string8_create_from_cstr((const uint8_t *)theme_json, strlen(theme_json)),
      &theme, error, sizeof(error)));
  assert(vkr_surface_theme_read(
      string8_create_from_cstr((const uint8_t *)world_json, strlen(world_json)),
      &world, error, sizeof(error)));
  assert(strcmp(vkr_surface_face_material(VKR_SURFACE_BRICK,
                                          VKR_SURFACE_MARK_HAZARD, "", &theme,
                                          &world, 0.0f, false_v),
                "assets/materials/art/brick.mt") == 0);
  assert(
      strcmp(vkr_surface_face_material(VKR_SURFACE_WOOD, VKR_SURFACE_MARK_NONE,
                                       "", &theme, &world, 0.0f, false_v),
             "assets/materials/art/oak.mt") == 0);
  assert(vkr_surface_face_material(VKR_SURFACE_BRICK, VKR_SURFACE_MARK_NONE,
                                   art, &theme, &world, 0.0f, false_v) == art);
  assert(
      strcmp(vkr_surface_face_material(VKR_SURFACE_METAL, VKR_SURFACE_MARK_NONE,
                                       "", &theme, &world, 1.0f, false_v),
             "assets/materials/greybox/metal_floor.mt") == 0);
  assert(
      strcmp(vkr_surface_face_material(VKR_SURFACE_BRICK, VKR_SURFACE_MARK_NONE,
                                       "", &theme, &world, 0.0f, true_v),
             "assets/materials/greybox/brick_wall.mt") == 0);
  assert(vkr_surface_theme_material(&theme, &world, VKR_SURFACE_NONE) == NULL);

  /* A written theme reads back the same; an unknown tag fails. */
  Arena *arena = arena_create(KB(64), KB(64));
  assert(arena);
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));
  String8 written = {0};
  VkrSurfaceTheme read_back = {0};
  assert(vkr_surface_theme_write(&world, &allocator, &written));
  assert(vkr_surface_theme_read(written, &read_back, error, sizeof(error)));
  assert(MemCompare(&read_back, &world, sizeof(world)) == 0);
  const char *typo = "{\"version\": 1, \"materials\": {\"brik\": \"a.mt\"}}";
  assert(!vkr_surface_theme_read(
      string8_create_from_cstr((const uint8_t *)typo, strlen(typo)), &read_back,
      error, sizeof(error)));
  assert(strstr(error, "brik") != NULL);
  arena_destroy(arena);

  VkrSurface surface = VKR_SURFACE_COUNT;
  VkrSurfaceMark mark = VKR_SURFACE_MARK_COUNT;
  assert(vkr_surface_from_legacy_material("assets/materials/dev/dev_metal.mt",
                                          &surface, &mark));
  assert(surface == VKR_SURFACE_METAL && mark == VKR_SURFACE_MARK_NONE);
  assert(vkr_surface_from_legacy_material("assets/materials/dev/dev_dark.mt",
                                          &surface, &mark));
  assert(surface == VKR_SURFACE_NONE && mark == VKR_SURFACE_MARK_DARK);
  assert(vkr_surface_from_legacy_material("assets/materials/dev/dev_ceiling.mt",
                                          &surface, &mark));
  assert(surface == VKR_SURFACE_NONE && mark == VKR_SURFACE_MARK_NONE);
  assert(!vkr_surface_from_legacy_material("assets/materials/dev/dev_decal.mt",
                                           &surface, &mark));
  assert(!vkr_surface_from_legacy_material("assets/materials/dev/dev_.mt",
                                           &surface, &mark));
  assert(!vkr_surface_from_legacy_material("", &surface, &mark));

  for (uint32_t side = 0; side < VKR_SURFACE_ORIENTATION_COUNT; ++side) {
    for (uint32_t tag = 0; tag < VKR_SURFACE_COUNT; ++tag) {
      FILE *file = fopen(
          vkr_surface_greybox_material((VkrSurface)tag, VKR_SURFACE_MARK_NONE,
                                       (VkrSurfaceOrientation)side),
          "rb");
      assert(file);
      fclose(file);
    }
    for (uint32_t m = 1; m < VKR_SURFACE_MARK_COUNT; ++m) {
      FILE *file = fopen(
          vkr_surface_greybox_material(VKR_SURFACE_NONE, (VkrSurfaceMark)m,
                                       (VkrSurfaceOrientation)side),
          "rb");
      assert(file);
      fclose(file);
    }
  }
  const char *const roles[] = {VKR_SURFACE_CLIP_MATERIAL,
                               VKR_SURFACE_TRIGGER_MATERIAL};
  for (uint32_t i = 0; i < ArrayCount(roles); ++i) {
    FILE *file = fopen(roles[i], "rb");
    assert(file);
    fclose(file);
  }
}

bool32_t run_brush_tests(void) {
  printf("--- Brush Tests ---\n");
  VkrBrushGeometry *geometry = malloc(sizeof(*geometry));
  assert(geometry);
  brush_test_solids(geometry);
  brush_test_editing(geometry);
  brush_test_grid_editing(geometry);
  brush_test_dents(geometry);
  brush_test_uv();
  brush_test_lightmap(geometry);
  brush_test_blockout(geometry);
  brush_test_blockout_lightmap(geometry);
  brush_test_coplanar(geometry);
  brush_test_surfaces();
  free(geometry);
  printf("--- Brush Tests Completed ---\n");
  return true_v;
}
