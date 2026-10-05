#include "brush_test.h"

#include "level/vkr_brush.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

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

bool32_t run_brush_tests(void) {
  printf("--- Brush Tests ---\n");
  VkrBrushGeometry *geometry = malloc(sizeof(*geometry));
  assert(geometry);
  brush_test_solids(geometry);
  brush_test_editing(geometry);
  brush_test_uv();
  free(geometry);
  printf("--- Brush Tests Completed ---\n");
  return true_v;
}
