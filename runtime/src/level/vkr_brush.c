#include "level/vkr_brush.h"

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
  VkrBrushPiece overlap = remaining;
  for (uint32_t j = 0; j < cutter_count; ++j) {
    if (!brush_piece_push(&overlap, cutter[j], target_count + j)) {
      return 0u;
    }
  }
  if (!vkr_brush_prune(&overlap, scratch)) {
    return UINT32_MAX;
  }
  /* Each cutter plane splits off the part of what remains outside it. */
  uint32_t written = 0u;
  for (uint32_t j = 0; j < cutter_count && written < capacity; ++j) {
    VkrBrushPiece piece = remaining;
    if (!brush_piece_push(&piece, brush_flip(cutter[j]), target_count + j)) {
      return written;
    }
    if (vkr_brush_prune(&piece, scratch)) {
      out[written++] = piece;
    }
    if (!brush_piece_push(&remaining, cutter[j], target_count + j) ||
        !vkr_brush_prune(&remaining, scratch)) {
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
