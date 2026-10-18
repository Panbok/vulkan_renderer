#include "core/vkr_debug_draw.h"

#include "containers/str.h"
#include "containers/vkr_hashtable.h"

vkr_global const char *const VKR_DEBUG_DRAW_COLOR_NAMES[] = {
    [VKR_DEBUG_DRAW_RED] = "red",     [VKR_DEBUG_DRAW_GREEN] = "green",
    [VKR_DEBUG_DRAW_BLUE] = "blue",   [VKR_DEBUG_DRAW_YELLOW] = "yellow",
    [VKR_DEBUG_DRAW_CYAN] = "cyan",   [VKR_DEBUG_DRAW_MAGENTA] = "magenta",
    [VKR_DEBUG_DRAW_WHITE] = "white", [VKR_DEBUG_DRAW_ORANGE] = "orange",
};

_Static_assert(ArrayCount(VKR_DEBUG_DRAW_COLOR_NAMES) ==
                   VKR_DEBUG_DRAW_COLOR_COUNT,
               "a name per debug draw colour");

uint32_t vkr_debug_draw_group(const char *name) {
  if (!name || !name[0]) {
    return 0u;
  }
  return (uint32_t)vkr_hash_table_key_cstr(name).hash;
}

bool8_t vkr_debug_draw_color_from_name(const char *name,
                                       VkrDebugDrawColor *out_color) {
  for (uint32_t i = 0u; name && i < VKR_DEBUG_DRAW_COLOR_COUNT; ++i) {
    if (string_equals(name, VKR_DEBUG_DRAW_COLOR_NAMES[i])) {
      *out_color = (VkrDebugDrawColor)i;
      return true_v;
    }
  }
  return false_v;
}

uint32_t vkr_debug_draw_line(VkrDebugDraw *draw, Vec3 from, Vec3 to,
                             VkrDebugDrawStyle style) {
  if (!vkr_is_finite_f32(from.x) || !vkr_is_finite_f32(from.y) ||
      !vkr_is_finite_f32(from.z) || !vkr_is_finite_f32(to.x) ||
      !vkr_is_finite_f32(to.y) || !vkr_is_finite_f32(to.z)) {
    return 0u;
  }
  if (draw->segment_count >= VKR_DEBUG_DRAW_SEGMENT_MAX) {
    ++draw->dropped;
    return 0u;
  }
  draw->segments[draw->segment_count++] =
      (VkrDebugDrawSegment){.from = from, .to = to, .style = style};
  ++draw->revision;
  return 1u;
}

uint32_t vkr_debug_draw_box(VkrDebugDraw *draw, Vec3 min, Vec3 max,
                            VkrDebugDrawStyle style) {
  /* Corner i takes max on each axis whose bit is set: x 1, y 2, z 4. */
  Vec3 corners[8];
  for (uint32_t i = 0u; i < 8u; ++i) {
    corners[i] = vec3_new(i & 1u ? max.x : min.x, i & 2u ? max.y : min.y,
                          i & 4u ? max.z : min.z);
  }
  static const uint8_t edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7},
                                       {0, 2}, {1, 3}, {4, 6}, {5, 7},
                                       {0, 4}, {1, 5}, {2, 6}, {3, 7}};
  uint32_t stored = 0u;
  for (uint32_t e = 0u; e < 12u; ++e) {
    stored += vkr_debug_draw_line(draw, corners[edges[e][0]],
                                  corners[edges[e][1]], style);
  }
  return stored;
}

uint32_t vkr_debug_draw_sphere(VkrDebugDraw *draw, Vec3 center,
                               float32_t radius, VkrDebugDrawStyle style) {
  if (!vkr_is_finite_f32(radius) || radius <= 0.0f) {
    return 0u;
  }
  uint32_t stored = 0u;
  for (uint32_t plane = 0u; plane < 3u; ++plane) {
    Vec3 previous = vec3_zero();
    for (uint32_t i = 0u; i <= VKR_DEBUG_DRAW_CIRCLE_SEGMENTS; ++i) {
      const float32_t angle =
          VKR_PI_2 * (float32_t)i / (float32_t)VKR_DEBUG_DRAW_CIRCLE_SEGMENTS;
      const float32_t c = vkr_cos_f32(angle) * radius;
      const float32_t s = vkr_sin_f32(angle) * radius;
      const Vec3 offset = plane == 0u   ? vec3_new(c, s, 0.0f)
                          : plane == 1u ? vec3_new(c, 0.0f, s)
                                        : vec3_new(0.0f, c, s);
      const Vec3 point = vec3_add(center, offset);
      if (i > 0u) {
        stored += vkr_debug_draw_line(draw, previous, point, style);
      }
      previous = point;
    }
  }
  return stored;
}

uint32_t vkr_debug_draw_frustum(VkrDebugDraw *draw,
                                Mat4 inverse_view_projection,
                                VkrDebugDrawStyle style) {
  Vec3 corners[8];
  for (uint32_t i = 0u; i < 8u; ++i) {
    const Vec4 clip = {i & 1u ? 1.0f : -1.0f, i & 2u ? 1.0f : -1.0f,
                       i & 4u ? 1.0f : 0.0f, 1.0f};
    const Vec4 world = mat4_mul_vec4(inverse_view_projection, clip);
    if (!vkr_is_finite_f32(world.w) || vkr_abs_f32(world.w) < 1.0e-12f) {
      return 0u;
    }
    corners[i] =
        vec3_new(world.x / world.w, world.y / world.w, world.z / world.w);
  }
  static const uint8_t edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7},
                                       {0, 2}, {1, 3}, {4, 6}, {5, 7},
                                       {0, 4}, {1, 5}, {2, 6}, {3, 7}};
  uint32_t stored = 0u;
  for (uint32_t e = 0u; e < 12u; ++e) {
    stored += vkr_debug_draw_line(draw, corners[edges[e][0]],
                                  corners[edges[e][1]], style);
  }
  return stored;
}

uint32_t vkr_debug_draw_label(VkrDebugDraw *draw, Vec3 at, const char *text,
                              VkrDebugDrawStyle style) {
  if (!text || !vkr_is_finite_f32(at.x) || !vkr_is_finite_f32(at.y) ||
      !vkr_is_finite_f32(at.z)) {
    return 0u;
  }
  if (draw->label_count >= VKR_DEBUG_DRAW_LABEL_MAX) {
    ++draw->dropped;
    return 0u;
  }
  VkrDebugDrawLabel *label = &draw->labels[draw->label_count++];
  *label = (VkrDebugDrawLabel){.at = at, .style = style};
  string_format(label->text, sizeof(label->text), "%s", text);
  ++draw->revision;
  return 1u;
}

/* Keeps the segments and labels `keep` accepts, in order. */
vkr_internal uint32_t vkr_debug_draw_filter(VkrDebugDraw *draw, uint32_t group,
                                            bool8_t all, float64_t now,
                                            bool8_t expire) {
  uint32_t removed = 0u;
  uint32_t kept = 0u;
  for (uint32_t i = 0u; i < draw->segment_count; ++i) {
    const VkrDebugDrawStyle *style = &draw->segments[i].style;
    const bool8_t drop = expire ? style->expires > 0.0 && style->expires <= now
                                : all || style->group == group;
    if (drop) {
      ++removed;
      continue;
    }
    draw->segments[kept++] = draw->segments[i];
  }
  draw->segment_count = kept;
  kept = 0u;
  for (uint32_t i = 0u; i < draw->label_count; ++i) {
    const VkrDebugDrawStyle *style = &draw->labels[i].style;
    const bool8_t drop = expire ? style->expires > 0.0 && style->expires <= now
                                : all || style->group == group;
    if (drop) {
      ++removed;
      continue;
    }
    draw->labels[kept++] = draw->labels[i];
  }
  draw->label_count = kept;
  if (removed > 0u) {
    ++draw->revision;
  }
  return removed;
}

uint32_t vkr_debug_draw_clear(VkrDebugDraw *draw, uint32_t group, bool8_t all) {
  if (all) {
    draw->dropped = 0u;
  }
  return vkr_debug_draw_filter(draw, group, all, 0.0, false_v);
}

void vkr_debug_draw_expire(VkrDebugDraw *draw, float64_t now) {
  (void)vkr_debug_draw_filter(draw, 0u, false_v, now, true_v);
}
