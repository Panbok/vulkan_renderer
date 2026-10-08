#include "level/vkr_blockout.h"

#include "math/vkr_math.h"

#include <math.h>
#include <stdio.h>

const char *const vkr_blockout_stairs_kinds[] = {"straight", "l",      "u",
                                                 "curved",   "spiral", NULL};

/* A piece spanning the box `lo`-`hi`. */
static void layout_box(VkrBlockoutPiece *piece, VkrBlockoutPieceKind kind,
                       Vec3 lo, Vec3 hi) {
  piece->kind = kind;
  piece->point_count = 8u;
  for (uint32_t c = 0; c < 8u; ++c) {
    piece->points[c] = vec3_new((c & 1u) ? hi.x : lo.x, (c & 2u) ? hi.y : lo.y,
                                (c & 4u) ? hi.z : lo.z);
  }
}

/* A piece over the floor outline `corners` (four, in order), from
   `bottom[i]` to `top[i]` above corner i. */
static void layout_prism(VkrBlockoutPiece *piece, VkrBlockoutPieceKind kind,
                         const Vec3 corners[4], const float32_t bottom[4],
                         const float32_t top[4]) {
  piece->kind = kind;
  piece->point_count = 8u;
  for (uint32_t c = 0; c < 4u; ++c) {
    piece->points[c] = vec3_new(corners[c].x, bottom[c], corners[c].z);
    piece->points[c + 4u] = vec3_new(corners[c].x, top[c], corners[c].z);
  }
}

static uint32_t layout_fail(char *error, uint64_t size, const char *message) {
  snprintf(error, size, "%s", message);
  return 0u;
}

/* The bottom of a step whose tread lies at `top`: the floor for solid steps,
   else `slab` below the tread. */
static float32_t layout_step_bottom(float32_t top, float32_t slab) {
  return slab > 0.0f ? Max(0.0f, top - slab) : 0.0f;
}

/* Steps along an arc: around `center` at radii `inner` to `outer`, where
   `along(a, r)` places angle `a` (radians) at radius `r`. */
typedef Vec3 (*LayoutArc)(Vec3 center, float32_t side, float32_t a,
                          float32_t r);

static Vec3 layout_arc_beside(Vec3 center, float32_t side, float32_t a,
                              float32_t r) {
  /* From a center beside the stairs: at a = 0 the step lies across the
     start line and the stairs head along +Z. */
  return vec3_add(center, vec3_new(-side * cosf(a) * r, 0.0f, sinf(a) * r));
}

static Vec3 layout_arc_around(Vec3 center, float32_t side, float32_t a,
                              float32_t r) {
  /* Around a pole at the origin, starting along +Z. */
  return vec3_add(center, vec3_new(side * sinf(a) * r, 0.0f, cosf(a) * r));
}

static uint32_t layout_arc_steps(VkrBlockoutPiece *out, uint32_t count,
                                 uint32_t steps, float32_t rise, float32_t slab,
                                 float32_t turn, float32_t inner,
                                 float32_t outer, Vec3 center, float32_t side,
                                 LayoutArc along) {
  for (uint32_t i = 0; i < steps; ++i) {
    const float32_t a0 = turn * (float32_t)i;
    const float32_t a1 = turn * (float32_t)(i + 1u);
    const Vec3 corners[4] = {
        along(center, side, a0, inner), along(center, side, a0, outer),
        along(center, side, a1, outer), along(center, side, a1, inner)};
    const float32_t top = rise * (float32_t)(i + 1u);
    const float32_t bottom = layout_step_bottom(top, slab);
    const float32_t bottoms[4] = {bottom, bottom, bottom, bottom};
    const float32_t tops[4] = {top, top, top, top};
    layout_prism(&out[count++], VKR_BLOCKOUT_PIECE_STEP, corners, bottoms,
                 tops);
  }
  return count;
}

/* Stairs in their group's space: the origin at the bottom front center (a
   spiral's pole), +Z along the first flight, +Y up. */
static uint32_t layout_stairs(const SceneBlockout *shape, VkrBlockoutPiece *out,
                              uint32_t capacity, char *error,
                              uint64_t error_size) {
  const float32_t w = shape->width;
  const float32_t h = shape->height;
  if (!(w > 0.0f) || !(h > 0.0f) || !(shape->step_height > 0.0f) ||
      !isfinite(w + h + shape->length + shape->radius + shape->turn)) {
    return layout_fail(error, error_size,
                       "Stairs need a positive height, width and step "
                       "height");
  }
  const float32_t steps_needed = ceilf(h / shape->step_height - 1.0e-4f);
  if (steps_needed > (float32_t)VKR_BLOCKOUT_STEP_MAX) {
    return layout_fail(error, error_size,
                       "Stairs have at most 4096 steps; raise the step "
                       "height or lower the stairs");
  }
  const uint32_t steps = (uint32_t)Max(1.0f, steps_needed);
  const float32_t rise = h / (float32_t)steps;
  const float32_t half = w * 0.5f;
  /* +1 turns the second flight, the arc or the spiral to the right (+X). */
  const float32_t side = shape->left ? -1.0f : 1.0f;
  const float32_t slab = Max(shape->thickness, 0.0f);
  if (capacity < steps + 2u) {
    return layout_fail(error, error_size, "Too many stair pieces");
  }
  uint32_t count = 0u;

  switch (shape->stairs) {
  case SCENE_STAIRS_STRAIGHT: {
    if (!(shape->length > 0.0f)) {
      return layout_fail(error, error_size, "Stairs need a positive length");
    }
    const float32_t depth = shape->length / (float32_t)steps;
    for (uint32_t i = 0; i < steps; ++i) {
      const float32_t top = rise * (float32_t)(i + 1u);
      layout_box(
          &out[count++], VKR_BLOCKOUT_PIECE_STEP,
          vec3_new(-half, layout_step_bottom(top, slab), depth * (float32_t)i),
          vec3_new(half, top, depth * (float32_t)(i + 1u)));
    }
    break;
  }
  case SCENE_STAIRS_L:
  case SCENE_STAIRS_U: {
    if (steps < 2u || shape->length <= w + 1.0e-3f) {
      return layout_fail(error, error_size,
                         "Turning stairs need two steps and a length longer "
                         "than their width");
    }
    const uint32_t first = steps / 2u;
    const uint32_t second = steps - first;
    const float32_t depth = (shape->length - w) / (float32_t)first;
    const float32_t landing = depth * (float32_t)first;
    for (uint32_t i = 0; i < first; ++i) {
      const float32_t top = rise * (float32_t)(i + 1u);
      layout_box(
          &out[count++], VKR_BLOCKOUT_PIECE_STEP,
          vec3_new(-half, layout_step_bottom(top, slab), depth * (float32_t)i),
          vec3_new(half, top, depth * (float32_t)(i + 1u)));
    }
    const float32_t landing_top = rise * (float32_t)first;
    const float32_t landing_bottom = layout_step_bottom(landing_top, slab);
    if (shape->stairs == SCENE_STAIRS_L) {
      layout_box(&out[count++], VKR_BLOCKOUT_PIECE_LANDING,
                 vec3_new(-half, landing_bottom, landing),
                 vec3_new(half, landing_top, landing + w));
      /* The second flight leaves the landing's side toward `side`. */
      for (uint32_t j = 0; j < second; ++j) {
        const float32_t a = side * (half + depth * (float32_t)j);
        const float32_t b = side * (half + depth * (float32_t)(j + 1u));
        const float32_t top = rise * (float32_t)(first + j + 1u);
        layout_box(&out[count++], VKR_BLOCKOUT_PIECE_STEP,
                   vec3_new(Min(a, b), layout_step_bottom(top, slab), landing),
                   vec3_new(Max(a, b), top, landing + w));
      }
    } else {
      /* The landing spans both lanes; the second lane lies one width to
         `side`. */
      const float32_t lane = side * w;
      layout_box(&out[count++], VKR_BLOCKOUT_PIECE_LANDING,
                 vec3_new(Min(-half, lane - half), landing_bottom, landing),
                 vec3_new(Max(half, lane + half), landing_top, landing + w));
      for (uint32_t j = 0; j < second; ++j) {
        const float32_t top = rise * (float32_t)(first + j + 1u);
        layout_box(&out[count++], VKR_BLOCKOUT_PIECE_STEP,
                   vec3_new(lane - half, layout_step_bottom(top, slab),
                            landing - depth * (float32_t)(j + 1u)),
                   vec3_new(lane + half, top, landing - depth * (float32_t)j));
      }
    }
    break;
  }
  case SCENE_STAIRS_CURVED:
  case SCENE_STAIRS_SPIRAL: {
    const bool8_t spiral = shape->stairs == SCENE_STAIRS_SPIRAL;
    const float32_t sweep = Max(shape->turn, 1.0f) * 0.0174532925f;
    const float32_t turn = sweep / (float32_t)steps;
    if (turn > 1.5707964f) {
      return layout_fail(error, error_size,
                         "A step turns at most 90 degrees; add steps or turn "
                         "less");
    }
    /* A spiral turning more than once would bury its lower steps under
       solid ones, so its steps float, each a rise thick at least. */
    const float32_t step_slab =
        spiral && sweep > 6.2831853f + 1.0e-3f && !(slab > 0.0f)
            ? Max(rise, 0.15f)
            : slab;
    const float32_t inner =
        spiral ? Max(shape->radius, 0.1f) : Max(shape->radius, 0.0f);
    const float32_t outer = inner + w;
    const Vec3 center =
        spiral ? vec3_zero() : vec3_new(side * (inner + half), 0.0f, 0.0f);
    count = layout_arc_steps(out, count, steps, rise, step_slab, turn,
                             Max(inner, 0.01f), outer, center, side,
                             spiral ? layout_arc_around : layout_arc_beside);
    if (spiral) {
      /* The pole, twelve sides up to 4 mm below the top tread: level with
         it, the pole's top shares the tread's and the landing's plane where
         their chords cross it, and they flicker (ADR-084 z_fight). */
      VkrBlockoutPiece *piece = &out[count++];
      piece->kind = VKR_BLOCKOUT_PIECE_POLE;
      piece->point_count = 24u;
      const float32_t pole_top = Max(h - 0.004f, 0.5f * h);
      for (uint32_t k = 0; k < 12u; ++k) {
        const float32_t a = 6.28318530718f * (float32_t)k / 12.0f;
        const Vec3 at = vec3_new(sinf(a) * inner, 0.0f, cosf(a) * inner);
        piece->points[k] = at;
        piece->points[k + 12u] = vec3_new(at.x, pole_top, at.z);
      }
      /* A landing past the top step, a width deep along the way the stairs
         turn, so the climb ends on a tread as wide as it is deep rather
         than off the top step's narrow inner end. */
      const Vec3 radial = layout_arc_around(vec3_zero(), side, sweep, 1.0f);
      const Vec3 tangent = vec3_new(side * cosf(sweep), 0.0f, -sinf(sweep));
      const Vec3 near_inner = vec3_scale(radial, Max(inner, 0.01f));
      const Vec3 near_outer = vec3_scale(radial, outer);
      const Vec3 corners[4] = {near_inner, near_outer,
                               vec3_add(near_outer, vec3_scale(tangent, w)),
                               vec3_add(near_inner, vec3_scale(tangent, w))};
      /* From three quarters of a turn the landing reaches back over the
         lowest steps; solid to the floor it would fill their headroom, so
         it floats as the steps of a longer spiral do. */
      const float32_t landing_slab = sweep >= 4.71238898f && !(step_slab > 0.0f)
                                         ? Max(rise, 0.15f)
                                         : step_slab;
      const float32_t bottom = layout_step_bottom(h, landing_slab);
      const float32_t bottoms[4] = {bottom, bottom, bottom, bottom};
      const float32_t tops[4] = {h, h, h, h};
      layout_prism(&out[count++], VKR_BLOCKOUT_PIECE_LANDING, corners, bottoms,
                   tops);
    }
    break;
  }
  default:
    return layout_fail(error, error_size, "Unknown stairs kind");
  }
  return count;
}

/* The radius of corner `i`: its own, else the corridor's. */
static float32_t layout_corner_radius(const SceneBlockout *shape, uint32_t i) {
  return shape->corners[i] >= 0.0f ? shape->corners[i] : shape->radius;
}

/* The corridor's floor path: its points, with each corner replaced by an
   arc of its radius (shrunk to fit the stretches beside it) in pieces of
   about 10 degrees, at most twelve, as many as the stretch budget allows.
   Returns the path's point count, or 0. */
static uint32_t layout_corridor_path(const SceneBlockout *shape, Vec3 *path,
                                     char *error, uint64_t error_size) {
  const uint32_t n = shape->point_count;
  const Vec3 *p = shape->points;
  uint32_t samples[SCENE_BLOCKOUT_POINT_MAX] = {0};
  float32_t angles[SCENE_BLOCKOUT_POINT_MAX] = {0};
  uint32_t total = n - 1u;
  for (uint32_t i = 1; i + 1u < n; ++i) {
    if (!(layout_corner_radius(shape, i) > 0.0f)) {
      continue;
    }
    Vec3 d0 = vec3_sub(p[i], p[i - 1u]);
    Vec3 d1 = vec3_sub(p[i + 1u], p[i]);
    d0.y = 0.0f;
    d1.y = 0.0f;
    if (vec3_length(d0) < 1.0e-3f || vec3_length(d1) < 1.0e-3f) {
      continue;
    }
    angles[i] = acosf(vkr_clamp_f32(
        vec3_dot(vec3_normalize(d0), vec3_normalize(d1)), -1.0f, 1.0f));
    if (angles[i] > 0.0175f) {
      samples[i] =
          (uint32_t)vkr_clamp_f32(ceilf(angles[i] / 0.1745329f), 1.0f, 12.0f);
      total += samples[i];
    }
  }
  /* The arcs give up samples, the most finely sampled first, until the
     corridor fits its stretch budget. */
  while (total > VKR_BLOCKOUT_CORRIDOR_SEGMENT_MAX) {
    uint32_t most = 0u;
    for (uint32_t i = 1; i + 1u < n; ++i) {
      most = samples[i] > samples[most] ? i : most;
    }
    if (samples[most] <= 1u) {
      return layout_fail(error, error_size,
                         "The corridor has too many points or bends; remove "
                         "some or lower its radius");
    }
    samples[most]--;
    total--;
  }

  uint32_t count = 0u;
  path[count++] = p[0];
  for (uint32_t i = 1; i + 1u < n; ++i) {
    if (!samples[i]) {
      path[count++] = p[i];
      continue;
    }
    const Vec3 a = p[i - 1u];
    const Vec3 c = p[i + 1u];
    Vec3 d0 = vec3_sub(p[i], a);
    Vec3 d1 = vec3_sub(c, p[i]);
    d0.y = 0.0f;
    d1.y = 0.0f;
    const float32_t len0 = vec3_length(d0);
    const float32_t len1 = vec3_length(d1);
    d0 = vec3_scale(d0, 1.0f / len0);
    d1 = vec3_scale(d1, 1.0f / len1);
    /* The arc meets each stretch `reach` from the corner, at most half the
       stretch so neighbouring arcs do not overlap. */
    const float32_t tangent = tanf(angles[i] * 0.5f);
    const float32_t reach =
        Min(layout_corner_radius(shape, i) * tangent, Min(len0, len1) * 0.5f);
    const float32_t radius = reach / tangent;
    Vec3 start = vec3_sub(p[i], vec3_scale(d0, reach));
    Vec3 end = vec3_add(p[i], vec3_scale(d1, reach));
    start.y = p[i].y + (a.y - p[i].y) * (reach / len0);
    end.y = p[i].y + (c.y - p[i].y) * (reach / len1);
    /* The center lies inside the bend, one radius from the start. */
    Vec3 inward = vec3_new(d0.z, 0.0f, -d0.x);
    if (vec3_dot(inward, d1) < 0.0f) {
      inward = vec3_scale(inward, -1.0f);
    }
    const Vec3 center = vec3_add(start, vec3_scale(inward, radius));
    const Vec3 from = vec3_sub(start, center);
    /* The arc turns the way whose full sweep reaches its end. */
    const Vec3 to = vec3_sub(end, center);
    const float32_t cs_full = cosf(angles[i]);
    const float32_t sn_full = sinf(angles[i]);
    const float32_t sign =
        fabsf(from.x * cs_full + from.z * sn_full - to.x) +
                    fabsf(-from.x * sn_full + from.z * cs_full - to.z) <
                fabsf(from.x * cs_full - from.z * sn_full - to.x) +
                    fabsf(from.x * sn_full + from.z * cs_full - to.z)
            ? 1.0f
            : -1.0f;
    path[count++] = start;
    for (uint32_t k = 1; k < samples[i]; ++k) {
      const float32_t t = (float32_t)k / (float32_t)samples[i];
      const float32_t angle = sign * angles[i] * t;
      const float32_t cs = cosf(angle);
      const float32_t sn = sinf(angle);
      Vec3 at = vec3_add(center, vec3_new(from.x * cs + from.z * sn, 0.0f,
                                          -from.x * sn + from.z * cs));
      at.y = start.y + (end.y - start.y) * t;
      path[count++] = at;
    }
    path[count++] = end;
  }
  path[count++] = p[n - 1u];

  /* Arcs that meet leave twin points behind. */
  uint32_t kept = 1u;
  for (uint32_t i = 1; i < count; ++i) {
    Vec3 step = vec3_sub(path[i], path[kept - 1u]);
    step.y = 0.0f;
    if (vec3_length(step) >= 1.0e-2f) {
      path[kept++] = path[i];
    }
  }
  return kept;
}

/* One wall of a corridor stretch, from the quad `i0`, `o0` (inner and
   outer at its start) to `o1`, `i1`, standing on floors at `y0` and `y1`;
   its openings (wall `wall`) split it into the stretches beside them and the
   lintels above and sills below them. */
static bool8_t layout_corridor_wall(const SceneBlockout *shape, uint32_t wall,
                                    Vec3 i0, Vec3 o0, Vec3 o1, Vec3 i1,
                                    float32_t y0, float32_t y1,
                                    VkrBlockoutPiece *out, uint32_t *count,
                                    uint32_t capacity) {
  const float32_t h = shape->height;
  Vec3 along = vec3_scale(vec3_add(vec3_sub(i1, i0), vec3_sub(o1, o0)), 0.5f);
  along.y = 0.0f;
  const float32_t length = vec3_length(along);
  /* The wall's openings, in order along it, as fractions of it. */
  float32_t spans[SCENE_BLOCKOUT_OPENING_MAX][4];
  uint32_t span_count = 0u;
  for (uint32_t i = 0; i < shape->opening_count &&
                       i < SCENE_BLOCKOUT_OPENING_MAX && length > 1.0e-3f;
       ++i) {
    if (shape->walls[i] != wall) {
      continue;
    }
    const Vec4 o = shape->openings[i];
    const float32_t a = vkr_clamp_f32(Min(o.x, o.y) / length, 0.0f, 1.0f);
    const float32_t b = vkr_clamp_f32(Max(o.x, o.y) / length, 0.0f, 1.0f);
    if (b - a < 1.0e-3f) {
      continue;
    }
    uint32_t at = span_count++;
    while (at > 0u && spans[at - 1u][0] > a) {
      MemCopy(spans[at], spans[at - 1u], sizeof(spans[at]));
      at--;
    }
    spans[at][0] = a;
    spans[at][1] = b;
    spans[at][2] = vkr_clamp_f32(Min(o.z, o.w), 0.0f, h);
    spans[at][3] = vkr_clamp_f32(Max(o.z, o.w), 0.0f, h);
  }
  /* Up to three pieces per opening and one past the last. */
  if (*count + 3u * span_count + 1u > capacity) {
    return false_v;
  }
  float32_t cursor = 0.0f;
  for (uint32_t i = 0; i <= span_count; ++i) {
    const float32_t from = i < span_count ? spans[i][0] : 1.0f;
    /* The part from `t0` to `t1` between `low` and `high` above the floor. */
    const float32_t parts[3][4] = {
        {cursor, Max(from, cursor), 0.0f, h},
        {i < span_count ? Max(from, cursor) : 0.0f,
         i < span_count ? spans[i][1] : 0.0f, i < span_count ? spans[i][3] : h,
         h},
        {i < span_count ? Max(from, cursor) : 0.0f,
         i < span_count ? spans[i][1] : 0.0f, 0.0f,
         i < span_count ? spans[i][2] : 0.0f},
    };
    for (uint32_t p = 0; p < (i < span_count ? 3u : 1u); ++p) {
      const float32_t t0 = parts[p][0];
      const float32_t t1 = parts[p][1];
      const float32_t low = parts[p][2];
      const float32_t high = parts[p][3];
      if (t1 - t0 < 1.0e-3f || high - low < 1.0e-3f) {
        continue;
      }
      const Vec3 corners[4] = {vec3_lerp(i0, i1, t0), vec3_lerp(o0, o1, t0),
                               vec3_lerp(o0, o1, t1), vec3_lerp(i0, i1, t1)};
      const float32_t floor0 = y0 + (y1 - y0) * t0;
      const float32_t floor1 = y0 + (y1 - y0) * t1;
      const float32_t bottoms[4] = {floor0 + low, floor0 + low, floor1 + low,
                                    floor1 + low};
      const float32_t tops[4] = {floor0 + high, floor0 + high, floor1 + high,
                                 floor1 + high};
      layout_prism(&out[(*count)++], VKR_BLOCKOUT_PIECE_WALL, corners, bottoms,
                   tops);
    }
    if (i < span_count) {
      cursor = Max(cursor, spans[i][1]);
    }
  }
  return true_v;
}

/* A corridor in its group's space along its floor points: per stretch a
   floor, two walls mitred at the bends with their openings, and a
   ceiling. */
static uint32_t layout_corridor(const SceneBlockout *shape,
                                VkrBlockoutPiece *out, uint32_t capacity,
                                char *error, uint64_t error_size) {
  if (shape->point_count < 2u ||
      shape->point_count > SCENE_BLOCKOUT_POINT_MAX || !(shape->width > 0.0f) ||
      !(shape->height > 0.0f) || !(shape->thickness > 0.0f)) {
    return layout_fail(error, error_size,
                       "A corridor needs 2 to 16 points and a positive "
                       "width, height and thickness");
  }
  Vec3 path[VKR_BLOCKOUT_CORRIDOR_SEGMENT_MAX + SCENE_BLOCKOUT_POINT_MAX];
  const uint32_t path_count =
      layout_corridor_path(shape, path, error, error_size);
  if (path_count < 2u) {
    return path_count ? layout_fail(error, error_size,
                                    "The corridor's points must lie apart")
                      : 0u;
  }
  const uint32_t segments = path_count - 1u;
  const uint32_t per_segment = shape->ceiling ? 4u : 3u;
  if (segments > VKR_BLOCKOUT_CORRIDOR_SEGMENT_MAX ||
      segments * per_segment > capacity) {
    return layout_fail(error, error_size, "The corridor is too long");
  }

  /* Each path point's side direction, mitred where the path bends, and its
     stretch along that direction to keep walls parallel. */
  Vec3 sides[VKR_BLOCKOUT_CORRIDOR_SEGMENT_MAX + 1u];
  float32_t stretch[VKR_BLOCKOUT_CORRIDOR_SEGMENT_MAX + 1u];
  for (uint32_t j = 0; j < path_count; ++j) {
    Vec3 in = j ? vec3_sub(path[j], path[j - 1u]) : vec3_zero();
    Vec3 next =
        j + 1u < path_count ? vec3_sub(path[j + 1u], path[j]) : vec3_zero();
    in.y = 0.0f;
    next.y = 0.0f;
    if (vec3_length(in) > 1.0e-5f) {
      in = vec3_normalize(in);
    }
    if (vec3_length(next) > 1.0e-5f) {
      next = vec3_normalize(next);
    }
    Vec3 forward = vec3_add(in, next);
    if (vec3_length(forward) < 1.0e-4f) {
      forward = vec3_length(next) > 0.5f ? next : in;
    }
    if (vec3_length(forward) < 1.0e-4f) {
      return layout_fail(error, error_size,
                         "Corridor points must lie apart horizontally");
    }
    forward = vec3_normalize(forward);
    sides[j] = vec3_new(forward.z, 0.0f, -forward.x);
    const Vec3 along = vec3_length(next) > 0.5f ? next : in;
    const float32_t cosine = vec3_dot(forward, along);
    if (cosine < 0.35f) {
      return layout_fail(error, error_size,
                         "A corridor bends at most about 140 degrees at a "
                         "point");
    }
    stretch[j] = 1.0f / cosine;
  }

  const float32_t inside = shape->width * 0.5f;
  const float32_t t = shape->thickness;
  const float32_t h = shape->height;
  uint32_t count = 0u;
  for (uint32_t k = 0; k < segments; ++k) {
    const uint32_t ends[2] = {k, k + 1u};
    Vec3 inner[2][2];
    Vec3 outer[2][2];
    for (uint32_t e = 0; e < 2u; ++e) {
      const uint32_t j = ends[e];
      for (uint32_t s = 0; s < 2u; ++s) {
        const Vec3 direction =
            vec3_scale(sides[j], (s ? -1.0f : 1.0f) * stretch[j]);
        inner[e][s] = vec3_add(path[j], vec3_scale(direction, inside));
        outer[e][s] = vec3_add(path[j], vec3_scale(direction, inside + t));
      }
    }
    const float32_t y0 = path[k].y;
    const float32_t y1 = path[k + 1u].y;
    /* Floor and ceiling span both walls. */
    const Vec3 span[4] = {outer[0][0], outer[0][1], outer[1][1], outer[1][0]};
    if (count + 2u + 2u * SCENE_BLOCKOUT_OPENING_MAX > capacity) {
      return layout_fail(error, error_size, "The corridor is too long");
    }
    const float32_t below[4] = {y0 - t, y0 - t, y1 - t, y1 - t};
    const float32_t floor_top[4] = {y0, y0, y1, y1};
    layout_prism(&out[count++], VKR_BLOCKOUT_PIECE_FLOOR, span, below,
                 floor_top);
    const float32_t wall_top[4] = {y0 + h, y0 + h, y1 + h, y1 + h};
    for (uint32_t s = 0; s < 2u; ++s) {
      if (!layout_corridor_wall(shape, k * 2u + s, inner[0][s], outer[0][s],
                                outer[1][s], inner[1][s], y0, y1, out, &count,
                                capacity)) {
        return layout_fail(error, error_size,
                           "The corridor has too many pieces; remove "
                           "openings or points");
      }
    }
    (void)wall_top;
    if (shape->ceiling) {
      const float32_t roof[4] = {y0 + h + t, y0 + h + t, y1 + h + t,
                                 y1 + h + t};
      layout_prism(&out[count++], VKR_BLOCKOUT_PIECE_CEILING, span, wall_top,
                   roof);
    }
  }
  return count;
}

bool8_t vkr_blockout_corridor_wall_at(const SceneBlockout *shape, Vec3 local,
                                      uint32_t *out_wall, float32_t *out_along,
                                      float32_t *out_floor) {
  char error[8];
  Vec3 path[VKR_BLOCKOUT_CORRIDOR_SEGMENT_MAX + SCENE_BLOCKOUT_POINT_MAX];
  if (shape->shape != SCENE_BLOCKOUT_CORRIDOR || shape->point_count < 2u) {
    return false_v;
  }
  const uint32_t count =
      layout_corridor_path(shape, path, error, sizeof(error));
  float32_t best = INFINITY;
  for (uint32_t k = 0; k + 1u < count; ++k) {
    Vec3 along = vec3_sub(path[k + 1u], path[k]);
    along.y = 0.0f;
    const float32_t length = vec3_length(along);
    if (length < 1.0e-3f) {
      continue;
    }
    along = vec3_scale(along, 1.0f / length);
    Vec3 offset = vec3_sub(local, path[k]);
    offset.y = 0.0f;
    const float32_t at = vec3_dot(offset, along);
    const float32_t clamped = vkr_clamp_f32(at, 0.0f, length);
    const Vec3 side = vec3_new(along.z, 0.0f, -along.x);
    const float32_t across = vec3_dot(offset, side);
    /* The point lies in a wall: as far across as the wall from the path. */
    const float32_t miss =
        fabsf(fabsf(across) - (shape->width * 0.5f + shape->thickness * 0.5f)) +
        fabsf(at - clamped);
    if (miss < best) {
      best = miss;
      *out_wall = k * 2u + (across >= 0.0f ? 0u : 1u);
      *out_along = clamped;
      *out_floor =
          path[k].y + (path[k + 1u].y - path[k].y) * (clamped / length);
    }
  }
  return best < shape->thickness + 0.5f;
}

/* Steps of stairs `shape`, at least one; zero when its sizes are not
   positive or finite. */
static uint32_t layout_step_count(const SceneBlockout *shape) {
  if (!(shape->height > 0.0f) || !(shape->step_height > 0.0f) ||
      !isfinite(shape->height / shape->step_height)) {
    return 0u;
  }
  return (uint32_t)Max(1.0f,
                       Min(ceilf(shape->height / shape->step_height - 1.0e-4f),
                           (float32_t)VKR_BLOCKOUT_STEP_MAX + 1.0f));
}

uint32_t vkr_blockout_piece_capacity(const SceneBlockout *shape) {
  if (shape->shape != SCENE_BLOCKOUT_CORRIDOR) {
    /* Steps, a landing or a pole, and one spare. */
    return Min(layout_step_count(shape), VKR_BLOCKOUT_STEP_MAX) + 2u;
  }
  /* Per stretch a floor, a ceiling and two walls; each opening splits its
     wall into three more, and each stretch checks room for all of them. */
  return 4u * VKR_BLOCKOUT_CORRIDOR_SEGMENT_MAX +
         6u * SCENE_BLOCKOUT_OPENING_MAX + 4u;
}

uint32_t vkr_blockout_layout(const SceneBlockout *shape, VkrBlockoutPiece *out,
                             uint32_t capacity, char *error,
                             uint64_t error_size) {
  return shape->shape == SCENE_BLOCKOUT_CORRIDOR
             ? layout_corridor(shape, out, capacity, error, error_size)
             : layout_stairs(shape, out, capacity, error, error_size);
}
