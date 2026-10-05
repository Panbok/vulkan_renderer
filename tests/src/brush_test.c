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
                           pieces, &piece_count, geometry) == VKR_BRUSH_OK);
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
                           vec3_new(0.0f, 0.5f, 0.0f), pieces, &piece_count,
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

  /* Pushing a corner deep inside would dent the cube; a point off the brush
     moves nothing. */
  const Vec3 corner = {1.0f, 1.0f, 1.0f};
  assert(vkr_brush_reshape(box, 6u, NULL, &corner, 1u,
                           vec3_new(-0.6f, -0.6f, -0.6f), pieces, &piece_count,
                           geometry) == VKR_BRUSH_ERROR_CONCAVE);
  const Vec3 away = {5.0f, 5.0f, 5.0f};
  assert(vkr_brush_reshape(box, 6u, NULL, &away, 1u, vec3_one(), pieces,
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
    assert(layout.texels_per_unit == VKR_BRUSH_LIGHTMAP_TEXELS_PER_UNIT);
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
        const Vec2 texel = vec2_new(uv.x * (float32_t)layout.width,
                                    uv.y * (float32_t)layout.height);
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
          sqrtf(powf((ub.x - ua.x) * (float32_t)layout.width, 2.0f) +
                powf((ub.y - ua.y) * (float32_t)layout.height, 2.0f));
      assert(brush_test_near(
          texels, vec3_length(vec3_sub(b, a)) * layout.texels_per_unit,
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
  assert(layout.texels_per_unit < VKR_BRUSH_LIGHTMAP_TEXELS_PER_UNIT);
  assert(layout.width <= VKR_BRUSH_LIGHTMAP_MAX_SIZE &&
         layout.height <= VKR_BRUSH_LIGHTMAP_MAX_SIZE);
}

bool32_t run_brush_tests(void) {
  printf("--- Brush Tests ---\n");
  VkrBrushGeometry *geometry = malloc(sizeof(*geometry));
  assert(geometry);
  brush_test_solids(geometry);
  brush_test_editing(geometry);
  brush_test_grid_editing(geometry);
  brush_test_uv();
  brush_test_lightmap(geometry);
  free(geometry);
  printf("--- Brush Tests Completed ---\n");
  return true_v;
}
