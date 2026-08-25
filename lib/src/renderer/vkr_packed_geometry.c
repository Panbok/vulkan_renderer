#include "renderer/vkr_packed_geometry.h"

#include <math.h>

static float32_t vkr_packed_clamp(float32_t value, float32_t low,
                                  float32_t high) {
  return value < low ? low : (value > high ? high : value);
}

static uint32_t vkr_packed_float_bits(float32_t value) {
  uint32_t bits = 0;
  MemCopy(&bits, &value, sizeof(bits));
  return bits;
}

static float32_t vkr_packed_bits_float(uint32_t bits) {
  float32_t value = 0.0f;
  MemCopy(&value, &bits, sizeof(value));
  return value;
}

static uint16_t vkr_packed_unorm16(float32_t value) {
  const float32_t clamped = vkr_packed_clamp(value, 0.0f, 1.0f);
  return (uint16_t)roundf(clamped * 65535.0f);
}

static float32_t vkr_packed_unorm16_decode(uint16_t value) {
  return (float32_t)value / 65535.0f;
}

static uint16_t vkr_packed_snorm16(float32_t value) {
  const float32_t clamped = vkr_packed_clamp(value, -1.0f, 1.0f);
  return (uint16_t)(int16_t)roundf(clamped * 32767.0f);
}

static float32_t vkr_packed_snorm16_decode(uint16_t value) {
  const int16_t signed_value = (int16_t)value;
  return vkr_packed_clamp((float32_t)signed_value / 32767.0f, -1.0f, 1.0f);
}

static Vec3 vkr_packed_normalize(Vec3 value) {
  const float32_t length_squared =
      value.x * value.x + value.y * value.y + value.z * value.z;
  if (length_squared <= 1.0e-20f) {
    return vec3_new(0.0f, 0.0f, 1.0f);
  }
  const float32_t inverse_length = 1.0f / sqrtf(length_squared);
  return vec3_scale(value, inverse_length);
}

static uint32_t vkr_packed_oct_encode(Vec3 value) {
  value = vkr_packed_normalize(value);
  const float32_t denominator =
      fabsf(value.x) + fabsf(value.y) + fabsf(value.z);
  float32_t x = value.x / denominator;
  float32_t y = value.y / denominator;
  if (value.z < 0.0f) {
    const float32_t old_x = x;
    x = (1.0f - fabsf(y)) * (old_x < 0.0f ? -1.0f : 1.0f);
    y = (1.0f - fabsf(old_x)) * (y < 0.0f ? -1.0f : 1.0f);
  }
  return (uint32_t)vkr_packed_snorm16(x) |
         ((uint32_t)vkr_packed_snorm16(y) << 16u);
}

static Vec3 vkr_packed_oct_decode(uint32_t value) {
  float32_t x = vkr_packed_snorm16_decode((uint16_t)value);
  float32_t y = vkr_packed_snorm16_decode((uint16_t)(value >> 16u));
  Vec3 result = vec3_new(x, y, 1.0f - fabsf(x) - fabsf(y));
  if (result.z < 0.0f) {
    const float32_t old_x = result.x;
    result.x = (1.0f - fabsf(result.y)) * (old_x < 0.0f ? -1.0f : 1.0f);
    result.y = (1.0f - fabsf(old_x)) * (result.y < 0.0f ? -1.0f : 1.0f);
  }
  return vkr_packed_normalize(result);
}

static uint8_t vkr_packed_unorm8(float32_t value) {
  return (uint8_t)roundf(vkr_packed_clamp(value, 0.0f, 1.0f) * 255.0f);
}

static float32_t vkr_packed_unorm8_decode(uint8_t value) {
  return (float32_t)value / 255.0f;
}

static float32_t vkr_packed_angle_degrees(Vec3 a, Vec3 b) {
  a = vkr_packed_normalize(a);
  b = vkr_packed_normalize(b);
  const float32_t cosine =
      vkr_packed_clamp(a.x * b.x + a.y * b.y + a.z * b.z, -1.0f, 1.0f);
  return acosf(cosine) * (180.0f / 3.14159265358979323846f);
}

static bool8_t
vkr_packed_budgets_are_valid(const VkrGeometryQuantizationBudgets *budgets) {
  return budgets && isfinite(budgets->position_relative) &&
         isfinite(budgets->normal_degrees) &&
         isfinite(budgets->tangent_degrees) && isfinite(budgets->uv_absolute) &&
         isfinite(budgets->color_absolute) &&
         budgets->position_relative > 0.0f && budgets->normal_degrees > 0.0f &&
         budgets->tangent_degrees > 0.0f && budgets->uv_absolute > 0.0f &&
         budgets->color_absolute > 0.0f;
}

static bool8_t vkr_packed_source_vertex_is_valid(const VkrVertex3d *vertex) {
  const Vec3 normal = vkr_vertex_unpack_vec3(vertex->normal);
  const Vec3 tangent =
      vec3_new(vertex->tangent.x, vertex->tangent.y, vertex->tangent.z);
  const float32_t normal_length_squared =
      normal.x * normal.x + normal.y * normal.y + normal.z * normal.z;
  const float32_t tangent_length_squared =
      tangent.x * tangent.x + tangent.y * tangent.y + tangent.z * tangent.z;
  return isfinite(vertex->position.x) && isfinite(vertex->position.y) &&
         isfinite(vertex->position.z) && isfinite(normal.x) &&
         isfinite(normal.y) && isfinite(normal.z) &&
         isfinite(vertex->texcoord.x) && isfinite(vertex->texcoord.y) &&
         isfinite(vertex->colour.x) && isfinite(vertex->colour.y) &&
         isfinite(vertex->colour.z) && isfinite(vertex->colour.w) &&
         isfinite(tangent.x) && isfinite(tangent.y) && isfinite(tangent.z) &&
         isfinite(vertex->tangent.w) && isfinite(normal_length_squared) &&
         isfinite(tangent_length_squared) && normal_length_squared > 1.0e-20f &&
         tangent_length_squared > 1.0e-20f &&
         fabsf(fabsf(vertex->tangent.w) - 1.0f) <= 1.0e-6f;
}

VkrGeometryQuantizationBudgets vkr_packed_geometry_default_budgets(void) {
  return (VkrGeometryQuantizationBudgets){
      .position_relative = 3.0e-5f,
      .normal_degrees = 0.05f,
      .tangent_degrees = 0.05f,
      .uv_absolute = 0.02f,
      .color_absolute = 0.0021f,
  };
}

bool8_t
vkr_packed_geometry_decode_is_valid(const VkrGpuGeometryDecodeRecord *decode) {
  if (!decode || decode->flags != VKR_GPU_GEOMETRY_DECODE_STATIC_V1 ||
      decode->reserved != 0u) {
    return false_v;
  }
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    if (!isfinite(decode->position_bias[axis]) ||
        !isfinite(decode->position_scale[axis]) ||
        decode->position_scale[axis] < 0.0f ||
        !isfinite(decode->position_bias[axis] + decode->position_scale[axis])) {
      return false_v;
    }
  }
  return true_v;
}

bool8_t vkr_packed_geometry_vertices_are_valid(
    const VkrPackedStaticVertex *vertices, uint32_t vertex_count,
    const VkrGpuGeometryDecodeRecord *decode) {
  if (!vertices || vertex_count == 0u ||
      !vkr_packed_geometry_decode_is_valid(decode)) {
    return false_v;
  }
  for (uint32_t i = 0u; i < vertex_count; ++i) {
    if ((vertices[i].words[1] & 0xfffe0000u) != 0u ||
        vertices[i].words[7] != 0u ||
        !isfinite(vkr_packed_bits_float(vertices[i].words[4])) ||
        !isfinite(vkr_packed_bits_float(vertices[i].words[5]))) {
      return false_v;
    }
  }
  return true_v;
}

void vkr_packed_geometry_unpack(const VkrPackedStaticVertex *source,
                                uint32_t vertex_count,
                                const VkrGpuGeometryDecodeRecord *decode,
                                VkrVertex3d *destination) {
  const Vec3 bias = vec3_new(decode->position_bias[0], decode->position_bias[1],
                             decode->position_bias[2]);
  const Vec3 scale =
      vec3_new(decode->position_scale[0], decode->position_scale[1],
               decode->position_scale[2]);
  for (uint32_t i = 0; i < vertex_count; ++i) {
    const VkrPackedStaticVertex *packed = &source[i];
    const float32_t px = vkr_packed_unorm16_decode((uint16_t)packed->words[0]);
    const float32_t py =
        vkr_packed_unorm16_decode((uint16_t)(packed->words[0] >> 16u));
    const float32_t pz = vkr_packed_unorm16_decode((uint16_t)packed->words[1]);
    const uint16_t tangent_flags = (uint16_t)(packed->words[1] >> 16u);
    const Vec3 normal = vkr_packed_oct_decode(packed->words[2]);
    const Vec3 tangent = vkr_packed_oct_decode(packed->words[3]);
    const float32_t u = vkr_packed_bits_float(packed->words[4]);
    const float32_t v = vkr_packed_bits_float(packed->words[5]);
    const uint32_t color = packed->words[6];
    destination[i] = (VkrVertex3d){
        .position = {bias.x + px * scale.x, bias.y + py * scale.y,
                     bias.z + pz * scale.z},
        .normal = {normal.x, normal.y, normal.z},
        .texcoord = {u, v},
        .colour = {vkr_packed_unorm8_decode((uint8_t)color),
                   vkr_packed_unorm8_decode((uint8_t)(color >> 8u)),
                   vkr_packed_unorm8_decode((uint8_t)(color >> 16u)),
                   vkr_packed_unorm8_decode((uint8_t)(color >> 24u))},
        .tangent = {tangent.x, tangent.y, tangent.z,
                    (tangent_flags & 1u) ? -1.0f : 1.0f},
    };
  }
}

bool8_t vkr_packed_geometry_pack(const VkrVertex3d *source,
                                 uint32_t vertex_count, Vec3 min_extents,
                                 Vec3 max_extents,
                                 const VkrGeometryQuantizationBudgets *budgets,
                                 VkrPackedStaticVertex *destination,
                                 VkrGpuGeometryDecodeRecord *out_decode,
                                 VkrGeometryQuantizationMetrics *out_metrics) {
  if (!source || vertex_count == 0 || !destination || !out_decode ||
      !out_metrics || !vkr_packed_budgets_are_valid(budgets) ||
      !isfinite(min_extents.x) || !isfinite(min_extents.y) ||
      !isfinite(min_extents.z) || !isfinite(max_extents.x) ||
      !isfinite(max_extents.y) || !isfinite(max_extents.z) ||
      min_extents.x > max_extents.x || min_extents.y > max_extents.y ||
      min_extents.z > max_extents.z) {
    return false_v;
  }
  const Vec3 scale = vec3_sub(max_extents, min_extents);
  *out_decode = (VkrGpuGeometryDecodeRecord){
      .position_bias = {min_extents.x, min_extents.y, min_extents.z},
      .flags = VKR_GPU_GEOMETRY_DECODE_STATIC_V1,
      .position_scale = {scale.x, scale.y, scale.z},
  };
  if (!vkr_packed_geometry_decode_is_valid(out_decode)) {
    return false_v;
  }
  *out_metrics = (VkrGeometryQuantizationMetrics){0};
  for (uint32_t i = 0; i < vertex_count; ++i) {
    if (!vkr_packed_source_vertex_is_valid(&source[i])) {
      return false_v;
    }
    const Vec3 position = vkr_vertex_unpack_vec3(source[i].position);
    if (position.x < min_extents.x || position.x > max_extents.x ||
        position.y < min_extents.y || position.y > max_extents.y ||
        position.z < min_extents.z || position.z > max_extents.z) {
      return false_v;
    }
    const Vec3 normal = vkr_vertex_unpack_vec3(source[i].normal);
    const Vec3 tangent =
        vec3_new(source[i].tangent.x, source[i].tangent.y, source[i].tangent.z);
    const float32_t nx =
        scale.x > 0.0f ? (position.x - min_extents.x) / scale.x : 0.0f;
    const float32_t ny =
        scale.y > 0.0f ? (position.y - min_extents.y) / scale.y : 0.0f;
    const float32_t nz =
        scale.z > 0.0f ? (position.z - min_extents.z) / scale.z : 0.0f;
    const uint16_t qx = vkr_packed_unorm16(nx);
    const uint16_t qy = vkr_packed_unorm16(ny);
    const uint16_t qz = vkr_packed_unorm16(nz);
    const uint16_t tangent_flags = source[i].tangent.w < 0.0f ? 1u : 0u;
    destination[i] = (VkrPackedStaticVertex){
        .words =
            {
                (uint32_t)qx | ((uint32_t)qy << 16u),
                (uint32_t)qz | ((uint32_t)tangent_flags << 16u),
                vkr_packed_oct_encode(normal),
                vkr_packed_oct_encode(tangent),
                vkr_packed_float_bits(source[i].texcoord.x),
                vkr_packed_float_bits(source[i].texcoord.y),
                (uint32_t)vkr_packed_unorm8(source[i].colour.x) |
                    ((uint32_t)vkr_packed_unorm8(source[i].colour.y) << 8u) |
                    ((uint32_t)vkr_packed_unorm8(source[i].colour.z) << 16u) |
                    ((uint32_t)vkr_packed_unorm8(source[i].colour.w) << 24u),
                0u,
            },
    };
  }

  VkrVertex3d decoded = {0};
  for (uint32_t i = 0; i < vertex_count; ++i) {
    vkr_packed_geometry_unpack(&destination[i], 1u, out_decode, &decoded);
    const Vec3 position = vkr_vertex_unpack_vec3(source[i].position);
    const Vec3 decoded_position = vkr_vertex_unpack_vec3(decoded.position);
    const Vec3 delta = vec3_sub(decoded_position, position);
    out_metrics->position_max =
        Max(out_metrics->position_max,
            sqrtf(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z));
    out_metrics->normal_degrees_max =
        Max(out_metrics->normal_degrees_max,
            vkr_packed_angle_degrees(vkr_vertex_unpack_vec3(source[i].normal),
                                     vkr_vertex_unpack_vec3(decoded.normal)));
    out_metrics->tangent_degrees_max = Max(
        out_metrics->tangent_degrees_max,
        vkr_packed_angle_degrees(
            vec3_new(source[i].tangent.x, source[i].tangent.y,
                     source[i].tangent.z),
            vec3_new(decoded.tangent.x, decoded.tangent.y, decoded.tangent.z)));
    out_metrics->uv_max =
        Max(out_metrics->uv_max,
            Max(fabsf(decoded.texcoord.x - source[i].texcoord.x),
                fabsf(decoded.texcoord.y - source[i].texcoord.y)));
    out_metrics->color_max =
        Max(out_metrics->color_max,
            Max(Max(fabsf(decoded.colour.x - source[i].colour.x),
                    fabsf(decoded.colour.y - source[i].colour.y)),
                Max(fabsf(decoded.colour.z - source[i].colour.z),
                    fabsf(decoded.colour.w - source[i].colour.w))));
  }
  const float32_t diagonal = hypotf(hypotf(scale.x, scale.y), scale.z);
  const float32_t position_budget =
      Max(diagonal * budgets->position_relative, 1.0e-7f);
  return isfinite(position_budget) &&
         vkr_packed_geometry_vertices_are_valid(destination, vertex_count,
                                                out_decode) &&
         out_metrics->position_max <= position_budget &&
         out_metrics->normal_degrees_max <= budgets->normal_degrees &&
         out_metrics->tangent_degrees_max <= budgets->tangent_degrees &&
         out_metrics->uv_max <= budgets->uv_absolute &&
         out_metrics->color_max <= budgets->color_absolute;
}
