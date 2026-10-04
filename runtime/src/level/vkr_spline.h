#pragma once

#include "defines.h"
#include "math/vec.h"

/* Splines through control points (ADR-084,
 * phase 5). A curve passes through every point as a centripetal
 * Catmull-Rom spline, which neither overshoots nor loops between unevenly
 * spaced points; an open curve continues past each end point by reflecting
 * its neighbour. Samples sit at equal arc length along the curve. */

/* Control points one spline holds. */
#define VKR_SPLINE_POINT_MAX 256u
/* Chords per segment that lengths and samples measure along. */
#define VKR_SPLINE_STEPS 32u

typedef struct VkrSplineSample {
  Vec3 position;
  /* Unit direction of travel. */
  Vec3 tangent;
  /* Arc length from the start. */
  float32_t distance;
} VkrSplineSample;

/* Point on segment `segment` (from point `segment` to the next) at `t` in
   [0, 1], and its unnormalized derivative in `out_derivative` (NULL
   allowed). `count` is at least two. */
Vec3 vkr_spline_evaluate(const Vec3 *points, uint32_t count, bool8_t closed,
                         uint32_t segment, float32_t t, Vec3 *out_derivative);

/* Segments of a curve of `count` points. */
uint32_t vkr_spline_segment_count(uint32_t count, bool8_t closed);

/* Arc length, measured over VKR_SPLINE_STEPS chords per segment. */
float32_t vkr_spline_length(const Vec3 *points, uint32_t count, bool8_t closed);

/* Up to `capacity` samples `spacing` apart along the curve, from its start
   (an open curve also ends with its last point). Returns the count written,
   zero for fewer than two points or a non-positive spacing. */
uint32_t vkr_spline_sample(const Vec3 *points, uint32_t count, bool8_t closed,
                           float32_t spacing, VkrSplineSample *out,
                           uint32_t capacity);
