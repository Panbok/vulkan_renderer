#include "level/vkr_brush.h"

#include "containers/vkr_sort.h"

#include <math.h>

/* Clipping works in double precision: a face starts as a square
   VKR_BRUSH_EXTENT meters wide, and float error at that size would exceed
   the weld distance. */
typedef struct BrushPoint {
  float64_t x;
  float64_t y;
  float64_t z;
} BrushPoint;

typedef struct BrushPlaneD {
  BrushPoint normal;
  float64_t distance;
} BrushPlaneD;

static float64_t brush_dot(BrushPoint a, BrushPoint b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

static BrushPoint brush_cross(BrushPoint a, BrushPoint b) {
  return (BrushPoint){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
                      a.x * b.y - a.y * b.x};
}

static BrushPoint brush_add(BrushPoint a, BrushPoint b) {
  return (BrushPoint){a.x + b.x, a.y + b.y, a.z + b.z};
}

static BrushPoint brush_sub(BrushPoint a, BrushPoint b) {
  return (BrushPoint){a.x - b.x, a.y - b.y, a.z - b.z};
}

static BrushPoint brush_scale(BrushPoint a, float64_t s) {
  return (BrushPoint){a.x * s, a.y * s, a.z * s};
}

static float64_t brush_length(BrushPoint a) { return sqrt(brush_dot(a, a)); }

/* Keeps the part of `in` inside `plane` (Sutherland-Hodgman). */
static uint32_t brush_clip(const BrushPoint *in, uint32_t count,
                           const BrushPlaneD *plane, BrushPoint *out) {
  const float64_t epsilon = 1.0e-9;
  uint32_t written = 0u;
  for (uint32_t i = 0; i < count; ++i) {
    const BrushPoint a = in[i];
    const BrushPoint b = in[(i + 1u) % count];
    const float64_t da = brush_dot(plane->normal, a) - plane->distance;
    const float64_t db = brush_dot(plane->normal, b) - plane->distance;
    const bool8_t a_inside = da <= epsilon;
    const bool8_t b_inside = db <= epsilon;
    if (a_inside && written < VKR_BRUSH_POLYGON_MAX + 1u) {
      out[written++] = a;
    }
    if (a_inside != b_inside && written < VKR_BRUSH_POLYGON_MAX + 1u) {
      const float64_t t = da / (da - db);
      out[written++] = brush_add(a, brush_scale(brush_sub(b, a), t));
    }
  }
  return written;
}

const char *vkr_brush_error_text(VkrBrushError error) {
  switch (error) {
  case VKR_BRUSH_OK:
    return "valid";
  case VKR_BRUSH_ERROR_FACE_COUNT:
    return "a brush needs 4 to 64 faces";
  case VKR_BRUSH_ERROR_NON_FINITE:
    return "a face plane is zero or not finite";
  case VKR_BRUSH_ERROR_EMPTY_FACE:
    return "a face does not touch the solid";
  case VKR_BRUSH_ERROR_OPEN:
    return "the faces do not enclose a solid";
  case VKR_BRUSH_ERROR_FLAT:
    return "the solid has no volume";
  case VKR_BRUSH_ERROR_CONCAVE:
    return "that dent needs too many convex pieces or folds the brush";
  case VKR_BRUSH_ERROR_NO_CORNER:
    return "no corner of the brush is at the dragged point";
  }
  return "invalid";
}

/* Builds the polygons. With `empty` set, a face whose polygon vanishes is
   marked there and skipped instead of failing the build; every surviving
   polygon is still clipped by every plane. */
static VkrBrushError brush_build(const VkrBrushPlane *planes, uint32_t count,
                                 VkrBrushGeometry *out, uint32_t *out_face,
                                 bool8_t *empty);

VkrBrushError vkr_brush_build(const VkrBrushPlane *planes, uint32_t count,
                              VkrBrushGeometry *out, uint32_t *out_face) {
  return brush_build(planes, count, out, out_face, NULL);
}

static VkrBrushError brush_build(const VkrBrushPlane *planes, uint32_t count,
                                 VkrBrushGeometry *out, uint32_t *out_face,
                                 bool8_t *empty) {
  if (out_face) {
    *out_face = UINT32_MAX;
  }
  if (!planes || !out || count < VKR_BRUSH_FACE_MIN ||
      count > VKR_BRUSH_FACE_MAX) {
    return VKR_BRUSH_ERROR_FACE_COUNT;
  }
  BrushPlaneD normalized[VKR_BRUSH_FACE_MAX];
  for (uint32_t i = 0; i < count; ++i) {
    const BrushPoint normal = {planes[i].normal.x, planes[i].normal.y,
                               planes[i].normal.z};
    const float64_t length = brush_length(normal);
    if (!isfinite(length) || length < 1.0e-6 || !isfinite(planes[i].distance)) {
      if (out_face) {
        *out_face = i;
      }
      return VKR_BRUSH_ERROR_NON_FINITE;
    }
    normalized[i] = (BrushPlaneD){brush_scale(normal, 1.0 / length),
                                  (float64_t)planes[i].distance / length};
  }
  out->face_count = count;
  out->vertex_count = 0u;
  out->min = vec3_new(INFINITY, INFINITY, INFINITY);
  out->max = vec3_new(-INFINITY, -INFINITY, -INFINITY);
  float64_t volume = 0.0;
  for (uint32_t face = 0; face < count; ++face) {
    const BrushPoint n = normalized[face].normal;
    /* u x v = n, so the square, and every clip of it, winds
       counterclockwise seen from outside. */
    const BrushPoint helper = fabs(n.y) < 0.9 ? (BrushPoint){0.0, 1.0, 0.0}
                                              : (BrushPoint){1.0, 0.0, 0.0};
    BrushPoint u = brush_cross(helper, n);
    u = brush_scale(u, 1.0 / brush_length(u));
    const BrushPoint v = brush_cross(n, u);
    const BrushPoint center = brush_scale(n, normalized[face].distance);
    const float64_t r = VKR_BRUSH_EXTENT;
    BrushPoint polygon[2][VKR_BRUSH_POLYGON_MAX + 2u];
    polygon[0][0] = brush_add(center, brush_scale(brush_add(u, v), -r));
    polygon[0][1] = brush_add(center, brush_scale(brush_sub(u, v), r));
    polygon[0][2] = brush_add(center, brush_scale(brush_add(u, v), r));
    polygon[0][3] = brush_add(center, brush_scale(brush_sub(v, u), r));
    uint32_t polygon_count = 4u;
    uint32_t current = 0u;
    for (uint32_t other = 0; other < count && polygon_count; ++other) {
      if (other == face) {
        continue;
      }
      polygon_count = brush_clip(polygon[current], polygon_count,
                                 &normalized[other], polygon[current ^ 1u]);
      current ^= 1u;
      if (polygon_count > VKR_BRUSH_POLYGON_MAX) {
        polygon_count = 0u;
      }
    }
    /* Weld neighbours, including the last to the first. */
    const BrushPoint *clipped = polygon[current];
    BrushPoint welded[VKR_BRUSH_POLYGON_MAX];
    uint32_t welded_count = 0u;
    for (uint32_t i = 0; i < polygon_count; ++i) {
      if (welded_count &&
          brush_length(brush_sub(clipped[i], welded[welded_count - 1u])) <
              VKR_BRUSH_WELD) {
        continue;
      }
      welded[welded_count++] = clipped[i];
    }
    while (welded_count > 1u &&
           brush_length(brush_sub(welded[0], welded[welded_count - 1u])) <
               VKR_BRUSH_WELD) {
      welded_count--;
    }
    if (welded_count < 3u) {
      if (empty) {
        empty[face] = true_v;
        out->polygons[face] = (VkrBrushPolygon){.first = out->vertex_count};
        out->normals[face] =
            vec3_new((float32_t)n.x, (float32_t)n.y, (float32_t)n.z);
        continue;
      }
      if (out_face) {
        *out_face = face;
      }
      return VKR_BRUSH_ERROR_EMPTY_FACE;
    }
    if (empty) {
      empty[face] = false_v;
    }
    BrushPoint area = {0.0, 0.0, 0.0};
    out->polygons[face] =
        (VkrBrushPolygon){.first = out->vertex_count, .count = welded_count};
    out->normals[face] =
        vec3_new((float32_t)n.x, (float32_t)n.y, (float32_t)n.z);
    for (uint32_t i = 0; i < welded_count; ++i) {
      const BrushPoint p = welded[i];
      if (fabs(p.x) > r * 0.5 || fabs(p.y) > r * 0.5 || fabs(p.z) > r * 0.5) {
        if (out_face) {
          *out_face = face;
        }
        return VKR_BRUSH_ERROR_OPEN;
      }
      if (i >= 2u) {
        area = brush_add(area, brush_cross(brush_sub(welded[i - 1u], welded[0]),
                                           brush_sub(p, welded[0])));
      }
      const Vec3 vertex =
          vec3_new((float32_t)p.x, (float32_t)p.y, (float32_t)p.z);
      out->vertices[out->vertex_count++] = vertex;
      out->min = vec3_new(Min(out->min.x, vertex.x), Min(out->min.y, vertex.y),
                          Min(out->min.z, vertex.z));
      out->max = vec3_new(Max(out->max.x, vertex.x), Max(out->max.y, vertex.y),
                          Max(out->max.z, vertex.z));
    }
    /* Divergence theorem: V = 1/3 sum(area * distance). */
    volume += 0.5 * brush_dot(area, n) * normalized[face].distance / 3.0;
  }
  out->volume = (float32_t)volume;
  if (!(volume > 1.0e-6)) {
    return VKR_BRUSH_ERROR_FLAT;
  }
  return VKR_BRUSH_OK;
}

void vkr_brush_texture_axes(Vec3 normal, Vec3 *out_u, Vec3 *out_v) {
  const float32_t ax = fabsf(normal.x);
  const float32_t ay = fabsf(normal.y);
  const float32_t az = fabsf(normal.z);
  if (ay >= ax && ay >= az) {
    /* Floors and ceilings: north (-Z) is up in the texture. */
    *out_u = vec3_new(1.0f, 0.0f, 0.0f);
    *out_v = vec3_new(0.0f, 0.0f, 1.0f);
  } else if (ax >= az) {
    /* Walls read left to right as seen from outside, up the world. */
    *out_u = vec3_new(0.0f, 0.0f, normal.x > 0.0f ? -1.0f : 1.0f);
    *out_v = vec3_new(0.0f, -1.0f, 0.0f);
  } else {
    *out_u = vec3_new(normal.z > 0.0f ? 1.0f : -1.0f, 0.0f, 0.0f);
    *out_v = vec3_new(0.0f, -1.0f, 0.0f);
  }
}

Vec2 vkr_brush_uv(Vec3 point, Vec3 normal, Vec2 offset, Vec2 scale,
                  float32_t rotation) {
  Vec3 u = {0};
  Vec3 v = {0};
  vkr_brush_texture_axes(normal, &u, &v);
  const float32_t s = vec3_dot(point, u);
  const float32_t t = vec3_dot(point, v);
  const float32_t c = cosf(rotation);
  const float32_t sn = sinf(rotation);
  const float32_t su = fabsf(scale.x) > 1.0e-6f ? scale.x : 1.0f;
  const float32_t sv = fabsf(scale.y) > 1.0e-6f ? scale.y : 1.0f;
  return vec2_new((s * c - t * sn) / su + offset.x,
                  (s * sn + t * c) / sv + offset.y);
}

/* An orthonormal basis of the plane with unit normal `normal`: u is
   perpendicular to the world axis least aligned with the normal. */
static void brush_lightmap_basis(Vec3 normal, Vec3 *out_u, Vec3 *out_v) {
  const Vec3 reference = fabsf(normal.y) < 0.9f ? vec3_new(0.0f, 1.0f, 0.0f)
                                                : vec3_new(1.0f, 0.0f, 0.0f);
  *out_u = vec3_normalize(vec3_cross(reference, normal));
  *out_v = vec3_cross(normal, *out_u);
}

/* One packing attempt at `texels_per_unit`; false when the atlas is too
   large. */
static bool8_t brush_lightmap_pack(const VkrBrushGeometry *geometry,
                                   float32_t texels_per_unit,
                                   VkrBrushLightmapLayout *out) {
  const uint32_t padding = VKR_BRUSH_LIGHTMAP_PADDING;
  out->width = 0u;
  out->height = 0u;
  uint32_t widths[VKR_BRUSH_FACE_MAX] = {0};
  uint32_t heights[VKR_BRUSH_FACE_MAX] = {0};
  uint32_t order[VKR_BRUSH_FACE_MAX];
  uint32_t chart_count = 0u;
  uint64_t area = 0u;
  uint32_t widest = 0u;
  for (uint32_t face = 0; face < geometry->face_count; ++face) {
    const VkrBrushPolygon polygon = geometry->polygons[face];
    out->projected_min[face] = vec2_new(0.0f, 0.0f);
    out->chart_corner[face] = vec2_new(0.0f, 0.0f);
    if (polygon.count < 3u) {
      continue;
    }
    Vec3 u;
    Vec3 v;
    brush_lightmap_basis(geometry->normals[face], &u, &v);
    float32_t min_u = INFINITY;
    float32_t min_v = INFINITY;
    float32_t max_u = -INFINITY;
    float32_t max_v = -INFINITY;
    for (uint32_t i = 0; i < polygon.count; ++i) {
      const Vec3 p = geometry->vertices[polygon.first + i];
      const float32_t s = vec3_dot(p, u);
      const float32_t t = vec3_dot(p, v);
      min_u = Min(min_u, s);
      min_v = Min(min_v, t);
      max_u = Max(max_u, s);
      max_v = Max(max_v, t);
    }
    out->projected_min[face] = vec2_new(min_u, min_v);
    widths[face] = Max(1u, (uint32_t)ceilf((max_u - min_u) * texels_per_unit)) +
                   2u * padding;
    heights[face] =
        Max(1u, (uint32_t)ceilf((max_v - min_v) * texels_per_unit)) +
        2u * padding;
    area += (uint64_t)widths[face] * heights[face];
    widest = Max(widest, widths[face]);
    order[chart_count++] = face;
  }
  if (chart_count == 0u) {
    return false_v;
  }
  /* Tallest first; ties keep face order. */
  for (uint32_t i = 1; i < chart_count; ++i) {
    const uint32_t face = order[i];
    uint32_t j = i;
    while (j > 0u && heights[order[j - 1u]] < heights[face]) {
      order[j] = order[j - 1u];
      --j;
    }
    order[j] = face;
  }
  const uint32_t width = Max(widest, (uint32_t)ceil(sqrt((float64_t)area)));
  uint32_t x = 0u;
  uint32_t y = 0u;
  uint32_t row = 0u;
  for (uint32_t i = 0; i < chart_count; ++i) {
    const uint32_t face = order[i];
    if (x + widths[face] > width) {
      y += row;
      x = 0u;
      row = 0u;
    }
    out->chart_corner[face] =
        vec2_new((float32_t)(x + padding), (float32_t)(y + padding));
    x += widths[face];
    row = Max(row, heights[face]);
  }
  out->width = width;
  out->height = y + row;
  out->texels_per_unit = texels_per_unit;
  return out->width <= VKR_BRUSH_LIGHTMAP_MAX_SIZE &&
         out->height <= VKR_BRUSH_LIGHTMAP_MAX_SIZE;
}

bool8_t vkr_brush_lightmap_layout(const VkrBrushGeometry *geometry,
                                  VkrBrushLightmapLayout *out) {
  if (!geometry || !out) {
    return false_v;
  }
  float32_t texels_per_unit = VKR_BRUSH_LIGHTMAP_TEXELS_PER_UNIT;
  for (uint32_t attempt = 0; attempt <= 8u; ++attempt) {
    if (brush_lightmap_pack(geometry, texels_per_unit, out)) {
      return true_v;
    }
    if (out->width == 0u) {
      return false_v;
    }
    texels_per_unit *= 0.5f;
  }
  return false_v;
}

Vec2 vkr_brush_lightmap_uv(const VkrBrushLightmapLayout *layout,
                           const VkrBrushGeometry *geometry, uint32_t face,
                           Vec3 point) {
  Vec3 u;
  Vec3 v;
  brush_lightmap_basis(geometry->normals[face], &u, &v);
  const Vec2 corner = layout->chart_corner[face];
  const Vec2 minimum = layout->projected_min[face];
  return vec2_new(
      (corner.x + (vec3_dot(point, u) - minimum.x) * layout->texels_per_unit) /
          (float32_t)layout->width,
      (corner.y + (vec3_dot(point, v) - minimum.y) * layout->texels_per_unit) /
          (float32_t)layout->height);
}

static VkrBrushPlane brush_plane(float32_t x, float32_t y, float32_t z,
                                 float32_t distance) {
  return (VkrBrushPlane){.normal = vec3_new(x, y, z), .distance = distance};
}

uint32_t vkr_brush_box_planes(Vec3 min, Vec3 max, VkrBrushPlane *out) {
  out[0] = brush_plane(1.0f, 0.0f, 0.0f, max.x);
  out[1] = brush_plane(-1.0f, 0.0f, 0.0f, -min.x);
  out[2] = brush_plane(0.0f, 1.0f, 0.0f, max.y);
  out[3] = brush_plane(0.0f, -1.0f, 0.0f, -min.y);
  out[4] = brush_plane(0.0f, 0.0f, 1.0f, max.z);
  out[5] = brush_plane(0.0f, 0.0f, -1.0f, -min.z);
  return 6u;
}

uint32_t vkr_brush_wedge_planes(Vec3 min, Vec3 max, uint32_t slope_axis,
                                VkrBrushPlane *out) {
  const float32_t height = max.y - min.y;
  uint32_t count = 0u;
  out[count++] = brush_plane(0.0f, -1.0f, 0.0f, -min.y);
  if (slope_axis <= 1u) {
    const float32_t run = max.x - min.x;
    const float32_t length = sqrtf(run * run + height * height);
    const float32_t sign = slope_axis == 0u ? 1.0f : -1.0f;
    const Vec3 normal = vec3_new(sign * height / length, run / length, 0.0f);
    /* The slope meets the floor along the low side's bottom edge. */
    const Vec3 low = vec3_new(slope_axis == 0u ? max.x : min.x, min.y, 0.0f);
    out[count++] =
        brush_plane(normal.x, normal.y, normal.z, vec3_dot(normal, low));
    out[count++] = slope_axis == 0u ? brush_plane(-1.0f, 0.0f, 0.0f, -min.x)
                                    : brush_plane(1.0f, 0.0f, 0.0f, max.x);
    out[count++] = brush_plane(0.0f, 0.0f, 1.0f, max.z);
    out[count++] = brush_plane(0.0f, 0.0f, -1.0f, -min.z);
  } else {
    const float32_t run = max.z - min.z;
    const float32_t length = sqrtf(run * run + height * height);
    const float32_t sign = slope_axis == 2u ? 1.0f : -1.0f;
    const Vec3 normal = vec3_new(0.0f, run / length, sign * height / length);
    const Vec3 low = vec3_new(0.0f, min.y, slope_axis == 2u ? max.z : min.z);
    out[count++] =
        brush_plane(normal.x, normal.y, normal.z, vec3_dot(normal, low));
    out[count++] = slope_axis == 2u ? brush_plane(0.0f, 0.0f, -1.0f, -min.z)
                                    : brush_plane(0.0f, 0.0f, 1.0f, max.z);
    out[count++] = brush_plane(1.0f, 0.0f, 0.0f, max.x);
    out[count++] = brush_plane(-1.0f, 0.0f, 0.0f, -min.x);
  }
  return count;
}

uint32_t vkr_brush_cylinder_planes(Vec3 center, float32_t radius,
                                   float32_t height, uint32_t sides,
                                   VkrBrushPlane *out) {
  sides = Min(Max(sides, 3u), 32u);
  /* Corners lie on the circle, so each side sits at the inscribed radius;
     side k faces 2*pi*k/sides, which keeps a 4- or 8-sided prism aligned to
     the axes. */
  const float32_t inscribed =
      radius * cosf(3.14159265358979f / (float32_t)sides);
  uint32_t count = 0u;
  for (uint32_t k = 0; k < sides; ++k) {
    const float32_t angle = 6.28318530717959f * (float32_t)k / (float32_t)sides;
    const Vec3 normal = vec3_new(cosf(angle), 0.0f, sinf(angle));
    out[count++] = brush_plane(normal.x, 0.0f, normal.z,
                               vec3_dot(normal, center) + inscribed);
  }
  out[count++] = brush_plane(0.0f, 1.0f, 0.0f, center.y + height);
  out[count++] = brush_plane(0.0f, -1.0f, 0.0f, -center.y);
  return count;
}

// =============================================================================
// Editing
// =============================================================================

static VkrBrushPlane brush_flip(VkrBrushPlane plane) {
  return (VkrBrushPlane){.normal = vec3_scale(plane.normal, -1.0f),
                         .distance = -plane.distance};
}

static void brush_piece_remove(VkrBrushPiece *piece, uint32_t index) {
  for (uint32_t i = index + 1u; i < piece->count; ++i) {
    piece->planes[i - 1u] = piece->planes[i];
    piece->source[i - 1u] = piece->source[i];
  }
  piece->count--;
}

static bool8_t brush_piece_push(VkrBrushPiece *piece, VkrBrushPlane plane,
                                uint32_t source) {
  if (piece->count >= VKR_BRUSH_FACE_MAX) {
    return false_v;
  }
  piece->planes[piece->count] = plane;
  piece->source[piece->count++] = source;
  return true_v;
}

bool8_t vkr_brush_prune(VkrBrushPiece *piece, VkrBrushGeometry *scratch) {
  /* A surviving polygon is clipped by every plane, so a face that vanishes
     bounds nothing and may go; when every face vanishes the half-spaces
     share no volume. */
  bool8_t empty[VKR_BRUSH_FACE_MAX];
  MemZero(empty, sizeof(empty));
  if (piece->count < VKR_BRUSH_FACE_MIN ||
      brush_build(piece->planes, piece->count, scratch, NULL, empty) !=
          VKR_BRUSH_OK ||
      !scratch->vertex_count) {
    return false_v;
  }
  for (uint32_t i = piece->count; i-- > 0;) {
    if (empty[i]) {
      brush_piece_remove(piece, i);
    }
  }
  return vkr_brush_build(piece->planes, piece->count, scratch, NULL) ==
         VKR_BRUSH_OK;
}

uint32_t vkr_brush_carve(const VkrBrushPlane *target, uint32_t target_count,
                         const VkrBrushPlane *cutter, uint32_t cutter_count,
                         VkrBrushPiece *out, uint32_t capacity,
                         VkrBrushGeometry *scratch) {
  /* The intersection decides whether the cutter touches the target. */
  VkrBrushPiece remaining = {0};
  for (uint32_t i = 0; i < target_count; ++i) {
    (void)brush_piece_push(&remaining, target[i], i);
  }
  /* A full piece first drops the planes that bound nothing, so it runs out
     of room only when it has VKR_BRUSH_FACE_MAX faces. */
  VkrBrushPiece overlap = remaining;
  for (uint32_t j = 0; j < cutter_count; ++j) {
    if (brush_piece_push(&overlap, cutter[j], target_count + j)) {
      continue;
    }
    if (!vkr_brush_prune(&overlap, scratch)) {
      return UINT32_MAX;
    }
    if (!brush_piece_push(&overlap, cutter[j], target_count + j)) {
      return VKR_BRUSH_CARVE_FAILED;
    }
  }
  if (!vkr_brush_prune(&overlap, scratch)) {
    return UINT32_MAX;
  }
  /* Each cutter plane splits off the part of what remains outside it; what
     remains is pruned at every step, so it holds only its own faces. */
  uint32_t written = 0u;
  for (uint32_t j = 0; j < cutter_count; ++j) {
    VkrBrushPiece piece = remaining;
    if (!brush_piece_push(&piece, brush_flip(cutter[j]), target_count + j)) {
      return VKR_BRUSH_CARVE_FAILED;
    }
    if (vkr_brush_prune(&piece, scratch)) {
      if (written == capacity) {
        return VKR_BRUSH_CARVE_FAILED;
      }
      out[written++] = piece;
    }
    if (!brush_piece_push(&remaining, cutter[j], target_count + j)) {
      return VKR_BRUSH_CARVE_FAILED;
    }
    if (!vkr_brush_prune(&remaining, scratch)) {
      break;
    }
  }
  return written;
}

bool8_t vkr_brush_extrude(const VkrBrushGeometry *geometry,
                          const VkrBrushPlane *planes, uint32_t face,
                          float32_t distance, VkrBrushPiece *out) {
  if (face >= geometry->face_count || !(distance > 0.0f)) {
    return false_v;
  }
  const VkrBrushPolygon polygon = geometry->polygons[face];
  const Vec3 normal = geometry->normals[face];
  const float32_t length = vec3_length(planes[face].normal);
  const float32_t base = planes[face].distance / length;
  *out = (VkrBrushPiece){0};
  (void)brush_piece_push(out, brush_flip((VkrBrushPlane){normal, base}), face);
  (void)brush_piece_push(out, (VkrBrushPlane){normal, base + distance}, face);
  for (uint32_t i = 0; i < polygon.count; ++i) {
    const Vec3 a = geometry->vertices[polygon.first + i];
    const Vec3 b = geometry->vertices[polygon.first + (i + 1u) % polygon.count];
    /* The polygon winds counterclockwise seen from outside, so edge x normal
       points away from its interior. */
    const Vec3 side = vec3_normalize(vec3_cross(vec3_sub(b, a), normal));
    if (!brush_piece_push(out, (VkrBrushPlane){side, vec3_dot(side, a)},
                          VKR_BRUSH_SOURCE_NEW)) {
      return false_v;
    }
  }
  return out->count >= VKR_BRUSH_FACE_MIN;
}

bool8_t vkr_brush_merge(const VkrBrushPlane *const *planes,
                        const uint32_t *counts, uint32_t brush_count,
                        VkrBrushPiece *out, VkrBrushGeometry *scratch) {
  /* Every input's vertices, and the sum of the input volumes. */
  enum { MERGE_VERTEX_MAX = 2048 };
  Vec3 vertices[MERGE_VERTEX_MAX];
  uint32_t vertex_count = 0u;
  float32_t volume = 0.0f;
  for (uint32_t b = 0; b < brush_count; ++b) {
    if (vkr_brush_build(planes[b], counts[b], scratch, NULL) != VKR_BRUSH_OK ||
        vertex_count + scratch->vertex_count > MERGE_VERTEX_MAX) {
      return false_v;
    }
    volume += scratch->volume;
    MemCopy(vertices + vertex_count, scratch->vertices,
            scratch->vertex_count * sizeof(*vertices));
    vertex_count += scratch->vertex_count;
  }
  /* A convex union's faces lie on input faces: keep the input planes every
     vertex lies behind, once each. */
  *out = (VkrBrushPiece){0};
  uint32_t source = 0u;
  for (uint32_t b = 0; b < brush_count; ++b) {
    for (uint32_t i = 0; i < counts[b]; ++i, ++source) {
      const float32_t length = vec3_length(planes[b][i].normal);
      const Vec3 n = vec3_scale(planes[b][i].normal, 1.0f / length);
      const float32_t d = planes[b][i].distance / length;
      bool8_t bounding = true_v;
      for (uint32_t v = 0; v < vertex_count && bounding; ++v) {
        bounding = vec3_dot(n, vertices[v]) <= d + 1.0e-3f;
      }
      for (uint32_t k = 0; k < out->count && bounding; ++k) {
        bounding = vec3_dot(out->planes[k].normal, n) < 0.9999f ||
                   fabsf(out->planes[k].distance - d) > 1.0e-3f;
      }
      if (bounding && !brush_piece_push(out, (VkrBrushPlane){n, d}, source)) {
        return false_v;
      }
    }
  }
  if (!vkr_brush_prune(out, scratch)) {
    return false_v;
  }
  /* Overlapping or non-convex inputs change the volume. */
  return fabsf(scratch->volume - volume) <= 1.0e-3f * Max(volume, 1.0f);
}

// =============================================================================
// Grid editing
// =============================================================================

void vkr_brush_grid_axes(Vec3 normal, Vec3 *out_u, Vec3 *out_v) {
  Vec3 u = {0};
  Vec3 v = {0};
  vkr_brush_texture_axes(normal, &u, &v);
  u = vec3_sub(u, vec3_scale(normal, vec3_dot(u, normal)));
  if (vec3_length(u) < 1.0e-4f) {
    u = vec3_cross(v, normal);
  }
  *out_u = vec3_normalize(u);
  *out_v = vec3_cross(normal, *out_u);
}

/* Appends `point` unless a stored point lies within `weld` of it; false
   when `points` is full. */
static bool8_t brush_point_add(Vec3 *points, uint32_t *count, uint32_t capacity,
                               Vec3 point, float32_t weld) {
  for (uint32_t i = 0; i < *count; ++i) {
    if (vec3_length(vec3_sub(points[i], point)) <= weld) {
      return true_v;
    }
  }
  if (*count >= capacity) {
    return false_v;
  }
  points[(*count)++] = point;
  return true_v;
}

static BrushPoint brush_point(Vec3 v) { return (BrushPoint){v.x, v.y, v.z}; }

uint32_t vkr_brush_hull(const Vec3 *points, uint32_t count, VkrBrushPlane *out,
                        uint32_t capacity) {
  Vec3 unique[VKR_BRUSH_HULL_POINT_MAX];
  uint32_t n = 0u;
  for (uint32_t i = 0; i < count; ++i) {
    if (!brush_point_add(unique, &n, VKR_BRUSH_HULL_POINT_MAX, points[i],
                         VKR_BRUSH_WELD)) {
      return 0u;
    }
  }
  if (n < 4u) {
    return 0u;
  }

  /* Every plane through three points with all points on one side bounds
     the hull; coplanar triples repeat a plane, kept once. */
  const float64_t tolerance = 1.0e-4;
  uint32_t written = 0u;
  for (uint32_t i = 0; i < n; ++i) {
    const BrushPoint a = brush_point(unique[i]);
    for (uint32_t j = i + 1u; j < n; ++j) {
      const BrushPoint ab = brush_sub(brush_point(unique[j]), a);
      for (uint32_t k = j + 1u; k < n; ++k) {
        BrushPoint normal =
            brush_cross(ab, brush_sub(brush_point(unique[k]), a));
        const float64_t length = brush_length(normal);
        if (!(length > 1.0e-9)) {
          continue;
        }
        normal = brush_scale(normal, 1.0 / length);
        float64_t distance = brush_dot(normal, a);
        float64_t above = -INFINITY;
        float64_t below = INFINITY;
        for (uint32_t m = 0; m < n; ++m) {
          const float64_t s =
              brush_dot(normal, brush_point(unique[m])) - distance;
          above = Max(above, s);
          below = Min(below, s);
          if (above > tolerance && below < -tolerance) {
            break;
          }
        }
        if (above > tolerance && below < -tolerance) {
          continue;
        }
        if (above > tolerance) {
          normal = brush_scale(normal, -1.0);
          distance = -distance;
        }

        const VkrBrushPlane plane = {.normal = vec3_new((float32_t)normal.x,
                                                        (float32_t)normal.y,
                                                        (float32_t)normal.z),
                                     .distance = (float32_t)distance};
        bool8_t repeated = false_v;
        for (uint32_t p = 0; p < written && !repeated; ++p) {
          repeated = vec3_dot(out[p].normal, plane.normal) > 1.0f - 1.0e-5f &&
                     fabsf(out[p].distance - plane.distance) < 1.0e-3f;
        }
        if (repeated) {
          continue;
        }
        if (written >= capacity) {
          return 0u;
        }
        out[written++] = plane;
      }
    }
  }
  return written >= VKR_BRUSH_FACE_MIN ? written : 0u;
}

/* Corners one face's overlap with another holds at most: a polygon clipped
   by every edge of the other and by two planes, each adding one corner. */
#define BRUSH_OVERLAP_POINT_MAX (2u * VKR_BRUSH_POLYGON_MAX + 4u)

/* Main axis classes a face's slots take: +X, -X, +Y, -Y, +Z, -Z. */
#define BRUSH_SLOT_CLASSES 6u

/* Slots sort by bucket, then along the sweep axis; ties keep a total
   order. */
static int32_t brush_slot_compare(const void *lhs, const void *rhs) {
  const VkrBrushFaceSlot *a = lhs;
  const VkrBrushFaceSlot *b = rhs;
  if (a->bucket != b->bucket) {
    return a->bucket < b->bucket ? -1 : 1;
  }
  if (a->lo != b->lo) {
    return a->lo < b->lo ? -1 : 1;
  }
  if (a->face != b->face) {
    return a->face < b->face ? -1 : 1;
  }
  return a->echo < b->echo ? -1 : (a->echo > b->echo ? 1 : 0);
}

/* The main axis classes of unit `normal`: its largest component's, and
   every other within `slack` of it, so a normal within the coplanar angle
   of this one has its own main class among them. Bit 2 * axis is the
   positive direction, the next bit the negative. */
static uint32_t brush_normal_classes(Vec3 normal, float32_t slack) {
  const float32_t components[3] = {normal.x, normal.y, normal.z};
  const float32_t largest =
      Max(fabsf(normal.x), Max(fabsf(normal.y), fabsf(normal.z)));
  uint32_t classes = 0u;
  for (uint32_t axis = 0; axis < 3u; ++axis) {
    if (fabsf(components[axis]) >= largest - slack) {
      classes |= 1u << (2u * axis + (components[axis] < 0.0f ? 1u : 0u));
    }
  }
  return classes;
}

/* Keeps the part of convex polygon `in` where dot(normal, p) <= distance
   (Sutherland-Hodgman). */
static uint32_t brush_overlap_clip(const Vec3 *in, uint32_t count, Vec3 normal,
                                   float32_t distance, Vec3 *out) {
  uint32_t written = 0u;
  for (uint32_t i = 0; i < count; ++i) {
    const Vec3 a = in[i];
    const Vec3 b = in[(i + 1u) % count];
    const float32_t da = vec3_dot(normal, a) - distance;
    const float32_t db = vec3_dot(normal, b) - distance;
    if (da <= 0.0f && written < BRUSH_OVERLAP_POINT_MAX) {
      out[written++] = a;
    }
    if ((da <= 0.0f) != (db <= 0.0f) && written < BRUSH_OVERLAP_POINT_MAX) {
      out[written++] = vec3_add(a, vec3_scale(vec3_sub(b, a), da / (da - db)));
    }
  }
  return written;
}

/* The area of face `b` dropped onto face `a`'s plane that lies inside `a`
   and within VKR_BRUSH_COPLANAR_DISTANCE of `b`'s plane, and its middle. */
static float32_t brush_face_overlap(const VkrBrushFaceRef *a,
                                    const VkrBrushFaceRef *b,
                                    const Vec3 *vertices, Vec3 *out_center) {
  Vec3 ping[BRUSH_OVERLAP_POINT_MAX];
  Vec3 pong[BRUSH_OVERLAP_POINT_MAX];
  /* Relative to a corner of `a`, so float precision follows the faces'
     size rather than their distance from the world origin. */
  const Vec3 origin = vertices[a->first];
  uint32_t count = b->count;
  for (uint32_t k = 0; k < count; ++k) {
    const Vec3 p = vec3_sub(vertices[b->first + k], origin);
    ping[k] = vec3_sub(p, vec3_scale(a->normal, vec3_dot(a->normal, p)));
  }

  /* Inside each edge of `a`: cross(normal, edge) points inward for corners
     counterclockwise around the normal. */
  Vec3 *in = ping;
  Vec3 *out = pong;
  for (uint32_t e = 0; e < a->count && count >= 3u; ++e) {
    const Vec3 p0 = vec3_sub(vertices[a->first + e], origin);
    const Vec3 p1 = vec3_sub(vertices[a->first + (e + 1u) % a->count], origin);
    const Vec3 outward = vec3_cross(vec3_sub(p1, p0), a->normal);
    count = brush_overlap_clip(in, count, outward, vec3_dot(outward, p0), out);
    Vec3 *swap = in;
    in = out;
    out = swap;
  }

  /* Only where the planes stay within the tolerance of each other. */
  const float32_t distance =
      vec3_dot(b->normal, vec3_sub(vertices[b->first], origin));
  if (count >= 3u) {
    count = brush_overlap_clip(in, count, b->normal,
                               distance + VKR_BRUSH_COPLANAR_DISTANCE, out);
    count = brush_overlap_clip(out, count, vec3_negate(b->normal),
                               -(distance - VKR_BRUSH_COPLANAR_DISTANCE), in);
  }
  if (count < 3u) {
    return 0.0f;
  }

  Vec3 twice = vec3_zero();
  Vec3 sum = vec3_zero();
  float32_t perimeter = 0.0f;
  for (uint32_t k = 0; k < count; ++k) {
    const Vec3 next = in[(k + 1u) % count];
    twice = vec3_add(twice, vec3_cross(in[k], next));
    sum = vec3_add(sum, in[k]);
    perimeter += vec3_length(vec3_sub(next, in[k]));
  }
  *out_center = vec3_add(origin, vec3_scale(sum, 1.0f / (float32_t)count));
  const float32_t area = 0.5f * fabsf(vec3_dot(twice, a->normal));
  /* A strip about 2 * area / perimeter wide: narrower than the tolerance,
     the faces only touch along an edge, as float error leaves between ring
     pieces sharing a plane, and no pixel shows both. */
  if (!(2.0f * area > VKR_BRUSH_COPLANAR_DISTANCE * perimeter)) {
    return 0.0f;
  }
  return area;
}

uint32_t vkr_brush_coplanar_overlaps(VkrBrushFaceRef *faces, uint32_t count,
                                     const Vec3 *vertices,
                                     VkrBrushFaceSlot *scratch,
                                     float32_t min_area,
                                     VkrBrushFaceOverlap *out,
                                     uint32_t capacity) {
  /* The faces' common center keeps every plane distance small. */
  Vec3 lo = vec3_new(INFINITY, INFINITY, INFINITY);
  Vec3 hi = vec3_new(-INFINITY, -INFINITY, -INFINITY);
  for (uint32_t i = 0; i < count; ++i) {
    VkrBrushFaceRef *face = &faces[i];
    face->lo = vec3_new(INFINITY, INFINITY, INFINITY);
    face->hi = vec3_new(-INFINITY, -INFINITY, -INFINITY);
    if (face->count < 3u || face->count > VKR_BRUSH_POLYGON_MAX) {
      continue;
    }
    for (uint32_t k = 0; k < face->count; ++k) {
      const Vec3 p = vertices[face->first + k];
      face->lo = vec3_new(Min(face->lo.x, p.x), Min(face->lo.y, p.y),
                          Min(face->lo.z, p.z));
      face->hi = vec3_new(Max(face->hi.x, p.x), Max(face->hi.y, p.y),
                          Max(face->hi.z, p.z));
    }
    lo = vec3_new(Min(lo.x, face->lo.x), Min(lo.y, face->lo.y),
                  Min(lo.z, face->lo.z));
    hi = vec3_new(Max(hi.x, face->hi.x), Max(hi.y, face->hi.y),
                  Max(hi.z, face->hi.z));
  }
  const Vec3 center = vec3_scale(vec3_add(lo, hi), 0.5f);
  float32_t reach_max = 0.0f;
  for (uint32_t i = 0; i < count; ++i) {
    VkrBrushFaceRef *face = &faces[i];
    face->key = INFINITY;
    face->reach = 0.0f;
    if (face->count < 3u || face->count > VKR_BRUSH_POLYGON_MAX) {
      continue;
    }
    float32_t reach = 0.0f;
    for (uint32_t k = 0; k < face->count; ++k) {
      reach =
          Max(reach, vec3_length(vec3_sub(vertices[face->first + k], center)));
    }
    const float32_t key =
        vec3_dot(face->normal, vec3_sub(vertices[face->first], center));
    if (isfinite(key) && isfinite(reach)) {
      face->key = key;
      face->reach = reach;
      reach_max = Max(reach_max, reach);
    }
  }

  /* Unit normals within the angle differ by at most 2 sin(angle / 2), so
     planes that meet within the distance at a point `reach` from the
     center have keys at most `window` apart. A face sits in the cell of its
     key and echoes into the next, and in each main axis class its normal
     may share with a fighting face's, so any two that may fight share a
     bucket. A pair meets only in the first bucket they share: never as two
     echoes, and only in the lowest class both take. */
  const float32_t radians = VKR_BRUSH_COPLANAR_DEGREES * 0.01745329252f;
  const float32_t spread = 2.0f * sinf(0.5f * radians);
  const float32_t cos_min = cosf(radians);
  const float32_t margin = VKR_BRUSH_COPLANAR_DISTANCE;
  const float32_t window = margin + spread * reach_max;
  /* Within a bucket, faces sweep along an axis no common face is
     perpendicular to, so faces far apart in their plane never meet. */
  const Vec3 axis = vec3_normalize(vec3_new(0.62f, 0.37f, 0.71f));
  uint32_t slot_count = 0u;
  for (uint32_t i = 0; i < count; ++i) {
    const VkrBrushFaceRef *face = &faces[i];
    if (face->key == INFINITY) {
      continue;
    }
    const int64_t cell = (int64_t)floorf(face->key / window);
    const uint32_t classes =
        brush_normal_classes(face->normal, 2.0f * spread + 1.0e-5f);
    for (uint32_t c = 0; c < BRUSH_SLOT_CLASSES; ++c) {
      if (!(classes & (1u << c))) {
        continue;
      }
      for (uint32_t echo = 0; echo < 2u; ++echo) {
        scratch[slot_count++] = (VkrBrushFaceSlot){
            .bucket = (cell + (int64_t)echo) * BRUSH_SLOT_CLASSES + c,
            .lo = vec3_dot(axis, face->lo),
            .hi = vec3_dot(axis, face->hi),
            .face = i,
            .echo = (uint8_t)echo,
            .classes = (uint8_t)classes,
        };
      }
    }
  }
  vkr_sort(scratch, slot_count, sizeof(*scratch), brush_slot_compare);

  uint32_t found = 0u;
  for (uint32_t s = 0; s < slot_count; ++s) {
    const VkrBrushFaceSlot *first = &scratch[s];
    /* Cells below zero leave a negative remainder. */
    const uint32_t bucket_class =
        (uint32_t)(((first->bucket % BRUSH_SLOT_CLASSES) + BRUSH_SLOT_CLASSES) %
                   BRUSH_SLOT_CLASSES);
    for (uint32_t t = s + 1u;
         t < slot_count && scratch[t].bucket == first->bucket &&
         scratch[t].lo <= first->hi + margin;
         ++t) {
      const VkrBrushFaceSlot *second = &scratch[t];
      const uint32_t shared = (uint32_t)(first->classes & second->classes);
      if ((first->echo && second->echo) ||
          (shared & ((1u << bucket_class) - 1u))) {
        continue;
      }
      const VkrBrushFaceRef *a = &faces[first->face];
      const VkrBrushFaceRef *b = &faces[second->face];
      if (b->owner == a->owner || vec3_dot(a->normal, b->normal) < cos_min ||
          fabsf(b->key - a->key) > margin + spread * Min(a->reach, b->reach)) {
        continue;
      }
      /* The overlap lies in both boxes: its area is at most their common
         box seen along the normal's main axis, which faces that only touch
         along an edge leave empty. */
      const Vec3 n = a->normal;
      const float32_t ex =
          Max(0.0f, Min(a->hi.x, b->hi.x) - Max(a->lo.x, b->lo.x));
      const float32_t ey =
          Max(0.0f, Min(a->hi.y, b->hi.y) - Max(a->lo.y, b->lo.y));
      const float32_t ez =
          Max(0.0f, Min(a->hi.z, b->hi.z) - Max(a->lo.z, b->lo.z));
      const float32_t nx = fabsf(n.x);
      const float32_t ny = fabsf(n.y);
      const float32_t nz = fabsf(n.z);
      const float32_t bound = nx >= ny && nx >= nz ? ey * ez / nx
                              : ny >= nz           ? ex * ez / ny
                                                   : ex * ey / nz;
      if (bound <= min_area) {
        continue;
      }
      Vec3 middle = vec3_zero();
      const float32_t area = brush_face_overlap(a, b, vertices, &middle);
      if (area <= min_area) {
        continue;
      }
      if (found < capacity) {
        out[found] = (VkrBrushFaceOverlap){.owner_a = a->owner,
                                           .owner_b = b->owner,
                                           .area = area,
                                           .center = middle,
                                           .normal = a->normal};
      }
      found++;
    }
  }
  return found;
}

/* The input plane a hull plane copies: the one it lies on, else the one
   facing most like it. */
static uint32_t brush_reshape_source(const VkrBrushPiece *input,
                                     VkrBrushPlane plane) {
  float32_t best = -2.0f;
  uint32_t source = VKR_BRUSH_SOURCE_NEW;
  for (uint32_t i = 0; i < input->count; ++i) {
    const float32_t length = vec3_length(input->planes[i].normal);
    const Vec3 n = vec3_scale(input->planes[i].normal, 1.0f / length);
    const float32_t facing = vec3_dot(n, plane.normal);
    if (facing > 0.9999f &&
        fabsf(input->planes[i].distance / length - plane.distance) < 1.0e-3f) {
      return input->source[i];
    }
    if (facing > best) {
      best = facing;
      source = input->source[i];
    }
  }
  return source;
}

/* A dented solid as faces: each a flat polygon, wound counterclockwise seen
   from outside, with its outward plane and the input face it copies. Faces
   share no corner storage; cutting clips each face on its own. */
#define BRUSH_DENT_FACE_MAX 384u
#define BRUSH_DENT_VERTEX_MAX 2048u
/* Points this close to a cutting plane, in meters, lie on it. */
#define BRUSH_DENT_ON_PLANE 1.0e-5
/* Polygons smaller than this, in square meters, vanish. */
#define BRUSH_DENT_AREA_MIN 1.0e-8

typedef struct BrushDentFace {
  uint32_t first;
  uint32_t count;
  uint32_t source;
  BrushPlaneD plane;
} BrushDentFace;

typedef struct BrushDentSolid {
  uint32_t face_count;
  uint32_t vertex_count;
  BrushDentFace faces[BRUSH_DENT_FACE_MAX];
  Vec3 vertices[BRUSH_DENT_VERTEX_MAX];
} BrushDentSolid;

/* A part of the dented solid: the cuts on its way down the splits, each
   kept behind, and the face each cut ran along. */
typedef struct BrushDentCell {
  uint32_t count;
  BrushPlaneD planes[VKR_BRUSH_RESHAPE_PIECE_MAX];
  uint32_t source[VKR_BRUSH_RESHAPE_PIECE_MAX];
} BrushDentCell;

/* Newell's normal of a polygon: it points the way the polygon winds
   counterclockwise, and its length is twice the area. */
static BrushPoint brush_newell(const BrushPoint *points, uint32_t count) {
  BrushPoint normal = {0.0, 0.0, 0.0};
  for (uint32_t i = 0; i < count; ++i) {
    const BrushPoint a = points[i];
    const BrushPoint b = points[(i + 1u) % count];
    normal.x += (a.y - b.y) * (a.z + b.z);
    normal.y += (a.z - b.z) * (a.x + b.x);
    normal.z += (a.x - b.x) * (a.y + b.y);
  }
  return normal;
}

/* The plane of `count` (up to VKR_BRUSH_POLYGON_MAX + 1) corners through
   their mean, and the farthest any corner lies from it; false when the
   polygon has no area. */
static bool8_t brush_dent_plane(const Vec3 *corners, uint32_t count,
                                BrushPlaneD *out, float64_t *out_deviation) {
  BrushPoint points[VKR_BRUSH_POLYGON_MAX + 1u];
  for (uint32_t i = 0; i < count; ++i) {
    points[i] = brush_point(corners[i]);
  }
  const BrushPoint normal = brush_newell(points, count);
  const float64_t length = brush_length(normal);
  if (!(0.5 * length > BRUSH_DENT_AREA_MIN)) {
    return false_v;
  }
  out->normal = brush_scale(normal, 1.0 / length);
  out->distance = 0.0;
  for (uint32_t i = 0; i < count; ++i) {
    out->distance += brush_dot(out->normal, points[i]);
  }
  out->distance /= (float64_t)count;

  *out_deviation = 0.0;
  for (uint32_t i = 0; i < count; ++i) {
    *out_deviation =
        Max(*out_deviation,
            fabs(brush_dot(out->normal, points[i]) - out->distance));
  }
  return true_v;
}

/* Adds the polygon `corners` as a face, or, when it is not flat, as a fan
   of triangles from its first corner; a polygon without area adds nothing.
   False when the solid is full. */
static bool8_t brush_dent_add(BrushDentSolid *solid, const Vec3 *corners,
                              uint32_t count, uint32_t source) {
  BrushPlaneD plane = {0};
  float64_t deviation = 0.0;
  if (!brush_dent_plane(corners, count, &plane, &deviation)) {
    return true_v;
  }
  if (count > 3u && deviation > VKR_BRUSH_WELD) {
    for (uint32_t i = 1; i + 1u < count; ++i) {
      const Vec3 triangle[3] = {corners[0], corners[i], corners[i + 1u]};
      if (!brush_dent_add(solid, triangle, 3u, source)) {
        return false_v;
      }
    }
    return true_v;
  }
  if (solid->face_count >= BRUSH_DENT_FACE_MAX ||
      solid->vertex_count + count > BRUSH_DENT_VERTEX_MAX) {
    return false_v;
  }
  solid->faces[solid->face_count++] = (BrushDentFace){
      .first = solid->vertex_count,
      .count = count,
      .source = source,
      .plane = plane,
  };
  MemCopy(solid->vertices + solid->vertex_count, corners,
          count * sizeof(*corners));
  solid->vertex_count += count;
  return true_v;
}

/* Adds an input face whose `moved` corners moved to `corners`. A face that
   stays flat stays one face. Otherwise its unmoved corners keep their face
   on the input plane, and each run of moved corners, with the unmoved
   corner on either side, becomes a face of its own: the dent bends the face
   along the line between those two corners. */
static bool8_t brush_dent_face(BrushDentSolid *solid, const Vec3 *corners,
                               const bool8_t *moved, uint32_t count,
                               uint32_t source) {
  BrushPlaneD plane = {0};
  float64_t deviation = 0.0;
  if (brush_dent_plane(corners, count, &plane, &deviation) &&
      deviation <= VKR_BRUSH_WELD) {
    return brush_dent_add(solid, corners, count, source);
  }
  Vec3 kept[VKR_BRUSH_POLYGON_MAX];
  uint32_t kept_count = 0u;
  for (uint32_t i = 0; i < count; ++i) {
    if (!moved[i]) {
      kept[kept_count++] = corners[i];
    }
  }
  if (kept_count == 0u) {
    return brush_dent_add(solid, corners, count, source);
  }
  if (kept_count >= 3u && !brush_dent_add(solid, kept, kept_count, source)) {
    return false_v;
  }

  Vec3 run[VKR_BRUSH_POLYGON_MAX + 1u];
  for (uint32_t start = 0; start < count; ++start) {
    const uint32_t before = (start + count - 1u) % count;
    if (!moved[start] || moved[before]) {
      continue;
    }
    uint32_t run_count = 0u;
    run[run_count++] = corners[before];
    uint32_t i = start;
    while (moved[i]) {
      run[run_count++] = corners[i];
      i = (i + 1u) % count;
    }
    /* With one unmoved corner the run ends where it began. */
    if (i != before) {
      run[run_count++] = corners[i];
    }
    if (!brush_dent_add(solid, run, run_count, source)) {
      return false_v;
    }
  }
  return true_v;
}

/* The part of face `face` inside `cell`, written to `out`, which holds
   VKR_BRUSH_POLYGON_MAX + 2 points; 0 when none of its area is. A face on a
   cell plane stays only when it faces the same way, so the face a cut ran
   along goes to one side. */
static uint32_t brush_dent_fragment(const BrushDentSolid *solid, uint32_t face,
                                    const BrushDentCell *cell,
                                    BrushPoint *out) {
  const BrushDentFace *source = &solid->faces[face];
  BrushPoint polygon[2][VKR_BRUSH_POLYGON_MAX + 2u];
  uint32_t count = Min(source->count, VKR_BRUSH_POLYGON_MAX);
  uint32_t current = 0u;
  for (uint32_t i = 0; i < count; ++i) {
    polygon[0][i] = brush_point(solid->vertices[source->first + i]);
  }
  for (uint32_t c = 0; c < cell->count && count; ++c) {
    const BrushPlaneD *plane = &cell->planes[c];
    float64_t above = -INFINITY;
    float64_t below = INFINITY;
    for (uint32_t i = 0; i < count; ++i) {
      const float64_t s =
          brush_dot(plane->normal, polygon[current][i]) - plane->distance;
      above = Max(above, s);
      below = Min(below, s);
    }
    if (above <= BRUSH_DENT_ON_PLANE && below >= -BRUSH_DENT_ON_PLANE) {
      if (brush_dot(plane->normal, source->plane.normal) < 0.0) {
        return 0u;
      }
      continue;
    }
    if (above <= BRUSH_DENT_ON_PLANE) {
      continue;
    }
    if (below >= -BRUSH_DENT_ON_PLANE) {
      return 0u;
    }
    count = brush_clip(polygon[current], count, plane, polygon[current ^ 1u]);
    current ^= 1u;
    if (count > VKR_BRUSH_POLYGON_MAX) {
      count = 0u;
    }
  }
  if (count < 3u ||
      !(0.5 * brush_length(brush_newell(polygon[current], count)) >
        BRUSH_DENT_AREA_MIN)) {
    return 0u;
  }
  MemCopy(out, polygon[current], count * sizeof(*out));
  return count;
}

/* Adds `plane` to `piece` unless the piece holds it already. */
static bool8_t brush_dent_push(VkrBrushPiece *piece, BrushPlaneD plane,
                               uint32_t source) {
  const VkrBrushPlane value = {
      .normal = vec3_new((float32_t)plane.normal.x, (float32_t)plane.normal.y,
                         (float32_t)plane.normal.z),
      .distance = (float32_t)plane.distance,
  };
  for (uint32_t i = 0; i < piece->count; ++i) {
    if (vec3_dot(piece->planes[i].normal, value.normal) > 1.0f - 1.0e-6f &&
        fabsf(piece->planes[i].distance - value.distance) < VKR_BRUSH_WELD) {
      return true_v;
    }
  }
  return brush_piece_push(piece, value, source);
}

/* The faces of `input` with its corners at `points` moved by `delta`, and
   the volume they enclose. */
static VkrBrushError brush_dent_build(const VkrBrushPiece *input,
                                      const Vec3 *points, uint32_t point_count,
                                      Vec3 delta, BrushDentSolid *solid,
                                      float64_t *out_volume,
                                      VkrBrushGeometry *scratch) {
  const VkrBrushError error =
      vkr_brush_build(input->planes, input->count, scratch, NULL);
  if (error != VKR_BRUSH_OK) {
    return error;
  }
  solid->face_count = 0u;
  solid->vertex_count = 0u;
  for (uint32_t face = 0; face < scratch->face_count; ++face) {
    const VkrBrushPolygon polygon = scratch->polygons[face];
    Vec3 corners[VKR_BRUSH_POLYGON_MAX];
    bool8_t moved[VKR_BRUSH_POLYGON_MAX];
    for (uint32_t i = 0; i < polygon.count; ++i) {
      corners[i] = scratch->vertices[polygon.first + i];
      moved[i] = false_v;
      for (uint32_t k = 0; k < point_count && !moved[i]; ++k) {
        moved[i] = vec3_length(vec3_sub(corners[i], points[k])) <= 1.0e-3f;
      }
      if (moved[i]) {
        corners[i] = vec3_add(corners[i], delta);
      }
    }
    if (!brush_dent_face(solid, corners, moved, polygon.count,
                         input->source[face])) {
      return VKR_BRUSH_ERROR_CONCAVE;
    }
  }
  if (!solid->face_count) {
    return VKR_BRUSH_ERROR_FLAT;
  }

  /* Divergence theorem: tetrahedra from one corner to every face's fan. */
  const BrushPoint origin = brush_point(solid->vertices[0]);
  float64_t volume = 0.0;
  for (uint32_t f = 0; f < solid->face_count; ++f) {
    const BrushDentFace *face = &solid->faces[f];
    const BrushPoint a =
        brush_sub(brush_point(solid->vertices[face->first]), origin);
    for (uint32_t i = 1; i + 1u < face->count; ++i) {
      const BrushPoint b =
          brush_sub(brush_point(solid->vertices[face->first + i]), origin);
      const BrushPoint c =
          brush_sub(brush_point(solid->vertices[face->first + i + 1u]), origin);
      volume += brush_dot(a, brush_cross(b, c)) / 6.0;
    }
  }
  *out_volume = volume;
  return volume > 1.0e-6 ? VKR_BRUSH_OK : VKR_BRUSH_ERROR_FLAT;
}

/* Cuts the dented `solid` of `volume` into convex pieces, appended to `out`
   after `*written` up to `capacity` in all. A part is convex when no corner
   of it lies in front of one of its faces; else the plane of such a face
   with the fewest faces across it cuts the part in two. The pieces must
   add up to the solid's volume, which a fold breaks. */
static VkrBrushError brush_dent_cut(const BrushDentSolid *solid,
                                    float64_t volume, VkrBrushPiece *out,
                                    uint32_t capacity, uint32_t *written,
                                    VkrBrushGeometry *scratch) {
  /* Every part on the stack and every piece written makes a piece, so the
     stack never holds more than `capacity` parts. */
  BrushDentCell stack[VKR_BRUSH_RESHAPE_PIECE_MAX];
  uint32_t stack_count = 1u;
  stack[0] = (BrushDentCell){0};
  bool8_t present[BRUSH_DENT_FACE_MAX];
  bool8_t bent[BRUSH_DENT_FACE_MAX];
  uint32_t crossing[BRUSH_DENT_FACE_MAX];
  BrushPoint fragment[VKR_BRUSH_POLYGON_MAX + 2u];
  float64_t pieces_volume = 0.0;
  while (stack_count) {
    const BrushDentCell cell = stack[--stack_count];
    for (uint32_t f = 0; f < solid->face_count; ++f) {
      present[f] = brush_dent_fragment(solid, f, &cell, fragment) > 0u;
      bent[f] = false_v;
      crossing[f] = 0u;
    }
    for (uint32_t j = 0; j < solid->face_count; ++j) {
      const uint32_t count =
          present[j] ? brush_dent_fragment(solid, j, &cell, fragment) : 0u;
      for (uint32_t f = 0; f < solid->face_count && count; ++f) {
        if (!present[f] || f == j) {
          continue;
        }
        const BrushPlaneD *plane = &solid->faces[f].plane;
        float64_t above = -INFINITY;
        float64_t below = INFINITY;
        for (uint32_t i = 0; i < count; ++i) {
          const float64_t s =
              brush_dot(plane->normal, fragment[i]) - plane->distance;
          above = Max(above, s);
          below = Min(below, s);
        }
        bent[f] = bent[f] || above > VKR_BRUSH_WELD;
        crossing[f] +=
            above > BRUSH_DENT_ON_PLANE && below < -BRUSH_DENT_ON_PLANE ? 1u
                                                                        : 0u;
      }
    }
    uint32_t cut = UINT32_MAX;
    for (uint32_t f = 0; f < solid->face_count; ++f) {
      if (present[f] && bent[f] &&
          (cut == UINT32_MAX || crossing[f] < crossing[cut])) {
        cut = f;
      }
    }

    if (cut != UINT32_MAX) {
      if (*written + stack_count + 2u > capacity ||
          cell.count >= VKR_BRUSH_RESHAPE_PIECE_MAX) {
        return VKR_BRUSH_ERROR_CONCAVE;
      }
      const BrushPlaneD plane = solid->faces[cut].plane;
      BrushDentCell back = cell;
      back.planes[back.count] = plane;
      back.source[back.count++] = solid->faces[cut].source;
      BrushDentCell front = cell;
      front.planes[front.count] = (BrushPlaneD){
          .normal = brush_scale(plane.normal, -1.0),
          .distance = -plane.distance,
      };
      front.source[front.count++] = solid->faces[cut].source;
      stack[stack_count++] = front;
      stack[stack_count++] = back;
      continue;
    }

    /* A convex part is its face planes and the cuts around it. */
    VkrBrushPiece piece = {0};
    for (uint32_t f = 0; f < solid->face_count; ++f) {
      if (present[f] && !brush_dent_push(&piece, solid->faces[f].plane,
                                         solid->faces[f].source)) {
        return VKR_BRUSH_ERROR_CONCAVE;
      }
    }
    for (uint32_t c = 0; c < cell.count; ++c) {
      if (!brush_dent_push(&piece, cell.planes[c], cell.source[c])) {
        return VKR_BRUSH_ERROR_CONCAVE;
      }
    }
    /* A sliver without volume makes no piece; the volume check below
       notices one that mattered. */
    if (!vkr_brush_prune(&piece, scratch)) {
      continue;
    }
    if (*written >= capacity) {
      return VKR_BRUSH_ERROR_CONCAVE;
    }
    pieces_volume += scratch->volume;
    out[(*written)++] = piece;
  }
  if (!(fabs(pieces_volume - volume) <= 1.0e-3 * Max(volume, 1.0e-3))) {
    return VKR_BRUSH_ERROR_CONCAVE;
  }
  return VKR_BRUSH_OK;
}

VkrBrushError vkr_brush_reshape(const VkrBrushPlane *planes, uint32_t count,
                                const VkrBrushPlane *split, const Vec3 *points,
                                uint32_t point_count, Vec3 delta,
                                VkrBrushPiece *out, uint32_t capacity,
                                uint32_t *out_count,
                                VkrBrushGeometry *scratch) {
  *out_count = 0u;
  if (count < VKR_BRUSH_FACE_MIN || count > VKR_BRUSH_FACE_MAX) {
    return VKR_BRUSH_ERROR_FACE_COUNT;
  }
  capacity = Min(capacity, VKR_BRUSH_RESHAPE_PIECE_MAX);
  VkrBrushPiece whole = {0};
  for (uint32_t i = 0; i < count; ++i) {
    (void)brush_piece_push(&whole, planes[i], i);
  }

  /* A split that misses the brush leaves it whole. */
  VkrBrushPiece inputs[2];
  uint32_t input_count = 0u;
  if (split) {
    for (uint32_t side = 0; side < 2u; ++side) {
      VkrBrushPiece piece = whole;
      if (brush_piece_push(&piece, side ? brush_flip(*split) : *split,
                           VKR_BRUSH_SOURCE_NEW) &&
          vkr_brush_prune(&piece, scratch)) {
        inputs[input_count++] = piece;
      }
    }
    if (input_count < 2u) {
      input_count = 0u;
    }
  }
  if (!input_count) {
    inputs[input_count++] = whole;
  }

  bool8_t moved_any = false_v;
  uint32_t written = 0u;
  BrushDentSolid dent;
  for (uint32_t p = 0; p < input_count; ++p) {
    const VkrBrushError error =
        vkr_brush_build(inputs[p].planes, inputs[p].count, scratch, NULL);
    if (error != VKR_BRUSH_OK) {
      return error;
    }
    Vec3 corners[VKR_BRUSH_HULL_POINT_MAX];
    bool8_t moved[VKR_BRUSH_HULL_POINT_MAX];
    uint32_t corner_count = 0u;
    for (uint32_t v = 0; v < scratch->vertex_count; ++v) {
      if (!brush_point_add(corners, &corner_count, VKR_BRUSH_HULL_POINT_MAX,
                           scratch->vertices[v], VKR_BRUSH_WELD)) {
        return VKR_BRUSH_ERROR_FACE_COUNT;
      }
    }
    for (uint32_t c = 0; c < corner_count; ++c) {
      moved[c] = false_v;
      for (uint32_t i = 0; i < point_count && !moved[c]; ++i) {
        moved[c] = vec3_length(vec3_sub(corners[c], points[i])) <= 1.0e-3f;
      }
      if (moved[c]) {
        corners[c] = vec3_add(corners[c], delta);
        moved_any = true_v;
      }
    }

    VkrBrushPlane hull[VKR_BRUSH_FACE_MAX];
    const uint32_t hull_count =
        vkr_brush_hull(corners, corner_count, hull, VKR_BRUSH_FACE_MAX);
    if (!hull_count) {
      return VKR_BRUSH_ERROR_FLAT;
    }
    /* A moved corner the hull swallows dents the solid. */
    bool8_t dented = false_v;
    for (uint32_t c = 0; c < corner_count && !dented; ++c) {
      if (!moved[c]) {
        continue;
      }
      float32_t outside = -INFINITY;
      for (uint32_t h = 0; h < hull_count; ++h) {
        outside = Max(outside,
                      vec3_dot(hull[h].normal, corners[c]) - hull[h].distance);
      }
      dented = outside < -1.0e-3f;
    }
    /* So may one on the hull's surface but not at a hull corner, as an edge
       pushed in along the faces at its ends: then the faces through it fold
       inward and the dented solid holds less than the hull. */
    float64_t dent_volume = 0.0;
    bool8_t dent_built = false_v;
    if (!dented) {
      const VkrBrushError built =
          vkr_brush_build(hull, hull_count, scratch, NULL);
      if (built != VKR_BRUSH_OK) {
        return built;
      }
      const float64_t hull_volume = scratch->volume;
      bool8_t off_corner = false_v;
      for (uint32_t c = 0; c < corner_count && !off_corner; ++c) {
        bool8_t at_corner = !moved[c];
        for (uint32_t v = 0; v < scratch->vertex_count && !at_corner; ++v) {
          at_corner = vec3_length(vec3_sub(corners[c], scratch->vertices[v])) <=
                      1.0e-3f;
        }
        off_corner = !at_corner;
      }
      if (off_corner) {
        const VkrBrushError dent_error =
            brush_dent_build(&inputs[p], points, point_count, delta, &dent,
                             &dent_volume, scratch);
        if (dent_error != VKR_BRUSH_OK) {
          return dent_error;
        }
        dent_built = true_v;
        dented = dent_volume < hull_volume - 1.0e-3 * Max(hull_volume, 1.0e-3);
      }
    }
    if (dented) {
      VkrBrushError dent_error =
          dent_built ? VKR_BRUSH_OK
                     : brush_dent_build(&inputs[p], points, point_count, delta,
                                        &dent, &dent_volume, scratch);
      if (dent_error == VKR_BRUSH_OK) {
        dent_error = brush_dent_cut(&dent, dent_volume, out, capacity, &written,
                                    scratch);
      }
      if (dent_error != VKR_BRUSH_OK) {
        return dent_error;
      }
      continue;
    }

    if (written >= capacity) {
      return VKR_BRUSH_ERROR_CONCAVE;
    }
    VkrBrushPiece *piece = &out[written++];
    *piece = (VkrBrushPiece){0};
    for (uint32_t h = 0; h < hull_count; ++h) {
      (void)brush_piece_push(piece, hull[h],
                             brush_reshape_source(&inputs[p], hull[h]));
    }
    const VkrBrushError built =
        vkr_brush_build(piece->planes, piece->count, scratch, NULL);
    if (built != VKR_BRUSH_OK) {
      return built;
    }
  }
  if (!moved_any) {
    return VKR_BRUSH_ERROR_NO_CORNER;
  }
  *out_count = written;
  return VKR_BRUSH_OK;
}

bool8_t vkr_brush_patch_prism(const VkrBrushPlane *planes, uint32_t count,
                              uint32_t face, const VkrBrushPlane rect[4],
                              float32_t inner, float32_t outer,
                              VkrBrushPiece *out, VkrBrushGeometry *scratch) {
  if (face >= count || !(outer > inner)) {
    return false_v;
  }
  const float32_t length = vec3_length(planes[face].normal);
  const Vec3 normal = vec3_scale(planes[face].normal, 1.0f / length);
  const float32_t base = planes[face].distance / length;
  *out = (VkrBrushPiece){0};
  (void)brush_piece_push(out, brush_flip((VkrBrushPlane){normal, base + inner}),
                         face);
  (void)brush_piece_push(out, (VkrBrushPlane){normal, base + outer}, face);
  VkrBrushPlane sides[4u + VKR_BRUSH_FACE_MAX];
  uint32_t side_count = 0u;
  for (uint32_t r = 0; r < 4u; ++r) {
    sides[side_count++] = rect[r];
  }
  for (uint32_t i = 0; i < count; ++i) {
    const float32_t side_length = vec3_length(planes[i].normal);
    const Vec3 side = vec3_scale(planes[i].normal, 1.0f / side_length);
    if (i != face && fabsf(vec3_dot(side, normal)) < 0.05f) {
      sides[side_count++] =
          (VkrBrushPlane){side, planes[i].distance / side_length};
    }
  }
  /* Of parallel sides, as when the patch reaches the face's edge, the
     tighter one bounds: a repeated plane would repeat its face. */
  for (uint32_t i = 0; i < side_count; ++i) {
    bool8_t looser = false_v;
    for (uint32_t j = 0; j < side_count && !looser; ++j) {
      looser = j != i && vec3_dot(sides[i].normal, sides[j].normal) > 0.9999f &&
               (sides[j].distance < sides[i].distance ||
                (sides[j].distance == sides[i].distance && j < i));
    }
    if (!looser && !brush_piece_push(out, sides[i], face)) {
      return false_v;
    }
  }
  return vkr_brush_prune(out, scratch);
}
