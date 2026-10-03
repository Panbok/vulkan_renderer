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

VkrBrushError vkr_brush_build(const VkrBrushPlane *planes, uint32_t count,
                              VkrBrushGeometry *out, uint32_t *out_face) {
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
      if (out_face) {
        *out_face = face;
      }
      return VKR_BRUSH_ERROR_EMPTY_FACE;
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
