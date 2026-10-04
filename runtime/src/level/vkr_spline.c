#include "level/vkr_spline.h"

#include <math.h>

/* Knot spacing of centripetal Catmull-Rom between two points; a floor keeps
   coincident points from dividing by zero. */
static float32_t spline_knot(Vec3 a, Vec3 b) {
  return Max(sqrtf(vec3_length(vec3_sub(b, a))), 1.0e-4f);
}

static Vec3 spline_lerp(Vec3 a, Vec3 b, float32_t t0, float32_t t1,
                        float32_t t) {
  return vec3_add(vec3_scale(a, (t1 - t) / (t1 - t0)),
                  vec3_scale(b, (t - t0) / (t1 - t0)));
}

uint32_t vkr_spline_segment_count(uint32_t count, bool8_t closed) {
  if (count < 2u) {
    return 0u;
  }
  return closed ? count : count - 1u;
}

/* The four points around segment `segment`; an open curve reflects the
   neighbour of an end point past it. */
static void spline_neighbours(const Vec3 *points, uint32_t count,
                              bool8_t closed, uint32_t segment, Vec3 *out) {
  const uint32_t i1 = segment;
  const uint32_t i2 = (segment + 1u) % count;
  out[1] = points[i1];
  out[2] = points[i2];
  if (closed) {
    out[0] = points[(segment + count - 1u) % count];
    out[3] = points[(segment + 2u) % count];
    return;
  }
  out[0] =
      i1 > 0u ? points[i1 - 1u] : vec3_sub(vec3_scale(out[1], 2.0f), out[2]);
  out[3] = i2 + 1u < count ? points[i2 + 1u]
                           : vec3_sub(vec3_scale(out[2], 2.0f), out[1]);
}

static Vec3 spline_point(const Vec3 *p, float32_t u) {
  const float32_t t0 = 0.0f;
  const float32_t t1 = t0 + spline_knot(p[0], p[1]);
  const float32_t t2 = t1 + spline_knot(p[1], p[2]);
  const float32_t t3 = t2 + spline_knot(p[2], p[3]);
  const float32_t t = t1 + u * (t2 - t1);
  const Vec3 a1 = spline_lerp(p[0], p[1], t0, t1, t);
  const Vec3 a2 = spline_lerp(p[1], p[2], t1, t2, t);
  const Vec3 a3 = spline_lerp(p[2], p[3], t2, t3, t);
  const Vec3 b1 = spline_lerp(a1, a2, t0, t2, t);
  const Vec3 b2 = spline_lerp(a2, a3, t1, t3, t);
  return spline_lerp(b1, b2, t1, t2, t);
}

Vec3 vkr_spline_evaluate(const Vec3 *points, uint32_t count, bool8_t closed,
                         uint32_t segment, float32_t t, Vec3 *out_derivative) {
  Vec3 p[4];
  spline_neighbours(points, count, closed, segment, p);
  t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
  if (out_derivative) {
    const float32_t h = 1.0e-3f;
    const float32_t lo = Max(0.0f, t - h);
    const float32_t hi = Min(1.0f, t + h);
    *out_derivative = vec3_scale(
        vec3_sub(spline_point(p, hi), spline_point(p, lo)), 1.0f / (hi - lo));
  }
  return spline_point(p, t);
}

float32_t vkr_spline_length(const Vec3 *points, uint32_t count,
                            bool8_t closed) {
  float32_t length = 0.0f;
  const uint32_t segments = vkr_spline_segment_count(count, closed);
  for (uint32_t s = 0; s < segments; ++s) {
    Vec3 previous = vkr_spline_evaluate(points, count, closed, s, 0.0f, NULL);
    for (uint32_t step = 1; step <= VKR_SPLINE_STEPS; ++step) {
      const Vec3 next = vkr_spline_evaluate(
          points, count, closed, s,
          (float32_t)step / (float32_t)VKR_SPLINE_STEPS, NULL);
      length += vec3_length(vec3_sub(next, previous));
      previous = next;
    }
  }
  return length;
}

uint32_t vkr_spline_sample(const Vec3 *points, uint32_t count, bool8_t closed,
                           float32_t spacing, VkrSplineSample *out,
                           uint32_t capacity) {
  const uint32_t segments = vkr_spline_segment_count(count, closed);
  if (segments == 0u || !(spacing > 0.0f) || capacity == 0u) {
    return 0u;
  }
  uint32_t written = 0u;
  float32_t travelled = 0.0f;
  float32_t next_target = 0.0f;
  Vec3 previous = vkr_spline_evaluate(points, count, closed, 0u, 0.0f, NULL);
  Vec3 last_tangent = vec3_new(1.0f, 0.0f, 0.0f);
  for (uint32_t s = 0; s < segments && written < capacity; ++s) {
    for (uint32_t step = 1; step <= VKR_SPLINE_STEPS && written < capacity;
         ++step) {
      const Vec3 next = vkr_spline_evaluate(
          points, count, closed, s,
          (float32_t)step / (float32_t)VKR_SPLINE_STEPS, NULL);
      const Vec3 chord = vec3_sub(next, previous);
      const float32_t chord_length = vec3_length(chord);
      if (chord_length > 1.0e-6f) {
        last_tangent = vec3_scale(chord, 1.0f / chord_length);
      }
      /* Every target inside this chord is one sample. */
      while (written < capacity && next_target <= travelled + chord_length) {
        const float32_t along = chord_length > 1.0e-6f
                                    ? (next_target - travelled) / chord_length
                                    : 0.0f;
        out[written++] = (VkrSplineSample){
            .position = vec3_add(previous, vec3_scale(chord, along)),
            .tangent = last_tangent,
            .distance = next_target,
        };
        next_target += spacing;
      }
      travelled += chord_length;
      previous = next;
    }
  }
  /* An open curve ends on its last point unless a sample already does. */
  if (!closed && written < capacity &&
      (written == 0u || travelled - out[written - 1u].distance > 1.0e-3f)) {
    out[written++] = (VkrSplineSample){
        .position = points[count - 1u],
        .tangent = last_tangent,
        .distance = travelled,
    };
  }
  return written;
}
