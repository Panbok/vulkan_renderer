#include "bake/vkr_bake_lightmap_inputs.h"

extern "C" {
#include "core/vkr_hash.h"
}
#include "bake/vkr_bake_atmosphere.h"
#include "filesystem/vkr_filesystem_cpp.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <new>
#include <system_error>
#include <thread>

namespace {

/* "VKLI" in little-endian. */
constexpr uint32_t kIndexMagic = 0x494c4b56u;
/* Version 2 compares floats within a tolerance (moments and light fields);
   a version 1 index, whose digests covered exact corners, bakes in full. */
constexpr uint32_t kIndexVersion = 2u;
constexpr uint32_t kIndexHeaderBytes = 64u;
constexpr uint32_t kIndexInstanceBytes = 104u + 4u * kVkrBakeInstanceMoments;
constexpr uint32_t kIndexLightBytes = 32u + 4u * kVkrBakeLightFields + 12u +
                                      4u + 4u + 4u + VKR_LIGHT_GROUP_NAME_BYTES;
/* Global floats round to this grid before they are hashed. */
constexpr float64_t kGlobalGrid = 1.0e5;
/* Bounds on a file a later bake reads. */
constexpr uint32_t kIndexMaxInstances = 1u << 24;
constexpr uint32_t kIndexMaxLights = 1u << 20;

/* SHA-256 over values in a fixed order, so equal inputs give equal
   digests whatever their padding or vector lanes hold. */
class DigestWriter {
public:
  DigestWriter() { vkr_sha256_init(&hash_); }

  void bytes(const void *data, uint64_t size) {
    vkr_sha256_update(&hash_, data, size);
  }

  void u32(uint32_t value) {
    uint8_t le[4] = {(uint8_t)value, (uint8_t)(value >> 8),
                     (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
    bytes(le, sizeof(le));
  }

  void f32(float32_t value) {
    uint32_t bits = 0u;
    std::memcpy(&bits, &value, sizeof(bits));
    u32(bits);
  }

  void v2(Vec2 value) {
    f32(value.x);
    f32(value.y);
  }

  void v3(Vec3 value) {
    f32(value.x);
    f32(value.y);
    f32(value.z);
  }

  void v4(Vec4 value) {
    f32(value.x);
    f32(value.y);
    f32(value.z);
    f32(value.w);
  }

  void text(const char *value) {
    const uint32_t length = (uint32_t)std::strlen(value);
    u32(length);
    bytes(value, length);
  }

  void digest(const VkrBakeDigest &value) { bytes(value.data(), value.size()); }

  /* A float rounded to the global grid, so a value an editor save
     rewrites a few ulps off hashes the same. */
  void rounded(float32_t value) {
    const int64_t steps = (int64_t)std::llround((float64_t)value * kGlobalGrid);
    u32((uint32_t)(uint64_t)steps);
    u32((uint32_t)((uint64_t)steps >> 32));
  }

  void rounded3(Vec3 value) {
    rounded(value.x);
    rounded(value.y);
    rounded(value.z);
  }

  VkrBakeDigest finish() {
    VkrBakeDigest out = {};
    vkr_sha256_final(&hash_, out.data());
    return out;
  }

private:
  VkrSha256 hash_ = {};
};

/* A material's content: its factors and modes and its textures' content
   digests, never its index or path. */
VkrBakeDigest material_digest(const VkrBakeScene &scene,
                              const VkrBakeMaterial &material) {
  DigestWriter w;
  w.u32((uint32_t)material.alpha_mode);
  w.u32(material.alpha_mode_explicit ? 1u : 0u);
  w.u32(material.double_sided ? 1u : 0u);
  w.f32(material.alpha_cutoff);
  w.v4(material.base_color);
  w.f32(material.metallic);
  w.f32(material.roughness);
  w.f32(material.normal_scale);
  w.f32(material.occlusion_strength);
  w.v3(material.emissive_factor);
  w.v3(material.dielectric_specular);
  w.f32(material.transmission_factor);
  w.f32(material.ior);
  w.f32(material.thickness_factor);
  w.v3(material.attenuation_color);
  w.f32(material.attenuation_distance);
  w.f32(material.clearcoat_factor);
  w.f32(material.clearcoat_roughness);
  w.f32(material.clearcoat_normal_scale);
  w.v3(material.sheen_color);
  w.f32(material.sheen_roughness);
  w.f32(material.subsurface_strength);
  w.u32(material.subsurface_profile);
  w.f32(material.diffuse_transmission_strength);
  w.v3(material.diffuse_transmission_color);
  w.f32(material.anisotropy_strength);
  w.f32(material.anisotropy_rotation);
  w.f32(material.diffuse_roughness);
  w.f32(material.smooth_terminator);
  w.f32(material.smooth_terminator_length);
  w.f32(material.dual_specular_opacity);
  w.f32(material.dual_specular_roughness_scale);
  w.v3(material.specular_f82_tint);
  w.v2(material.world_size);
  for (const VkrBakeMaterialTextureRef &ref : material.textures) {
    w.u32(ref.present ? 1u : 0u);
    if (!ref.present) {
      continue;
    }
    w.u32(ref.srgb ? 1u : 0u);
    const char *path = nullptr;
    const char *sha256 = nullptr;
    uint64_t bytes = 0u;
    if (vkr_bake_texture_store_dependency(
            scene.texture_store, ref.texture_index, &path, &sha256, &bytes) &&
        sha256) {
      w.text(sha256);
    } else {
      /* A texture without a digest never matches, so its instance always
         relights. */
      w.u32(ref.texture_index);
      w.text(path ? path : "");
    }
  }
  return w.finish();
}

/* A light's fields the bake compares within the tolerance, in a fixed
   order. */
void light_floats(const VkrBakeSceneLight &light,
                  float32_t out[kVkrBakeLightFields]) {
  const Vec3 vectors[5] = {light.position, light.direction, light.right,
                           light.up, light.color};
  uint32_t at = 0u;
  for (const Vec3 &vector : vectors) {
    out[at++] = vector.x;
    out[at++] = vector.y;
    out[at++] = vector.z;
  }
  const float32_t scalars[11] = {
      light.intensity,        light.range,        light.constant,
      light.linear,           light.quadratic,    light.inner_cone_angle,
      light.outer_cone_angle, light.half_width,   light.half_height,
      light.radiance,         light.source_radius};
  for (float32_t scalar : scalars) {
    out[at++] = scalar;
  }
}

/* A light's exact fields: kind, flags and group. */
void light_exact(DigestWriter *w, const VkrBakeSceneLight &light) {
  w->u32((uint32_t)light.kind);
  w->u32(light.casts_shadow ? 1u : 0u);
  w->u32(light.enabled ? 1u : 0u);
  w->text(light.group);
}

/* A directional light into the globals, its floats rounded. */
void light_rounded(DigestWriter *w, const VkrBakeSceneLight &light) {
  light_exact(w, light);
  float32_t fields[kVkrBakeLightFields];
  light_floats(light, fields);
  for (float32_t field : fields) {
    w->rounded(field);
  }
}

bool near(float32_t a, float32_t b) {
  return std::fabs(a - b) <=
         kVkrBakeInputTolerance + kVkrBakeInputRelativeTolerance *
                                      std::max(std::fabs(a), std::fabs(b));
}

bool near3(Vec3 a, Vec3 b) {
  return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z);
}

/* Area-weighted sums of one instance's geometry, in double. */
struct MomentSums {
  float64_t area = 0.0;
  float64_t centroid[3] = {};
  float64_t second[6] = {};
  float64_t corner_sums[18] = {};
  uint64_t corners = 0u;
};

void add_triangle(MomentSums *sums, const VkrBakeTriangle &triangle) {
  const Vec3 a = triangle.vertex[0].position;
  const Vec3 e1 = vec3_sub(triangle.vertex[1].position, a);
  const Vec3 e2 = vec3_sub(triangle.vertex[2].position, a);
  const Vec3 n = vec3_cross(e1, e2);
  const float64_t area =
      0.5 * std::sqrt((float64_t)n.x * n.x + (float64_t)n.y * n.y +
                      (float64_t)n.z * n.z);
  float64_t c[3] = {0.0, 0.0, 0.0};
  for (const VkrBakeVertex &vertex : triangle.vertex) {
    c[0] += vertex.position.x / 3.0;
    c[1] += vertex.position.y / 3.0;
    c[2] += vertex.position.z / 3.0;
    const float32_t corner[18] = {vertex.normal.x,
                                  vertex.normal.y,
                                  vertex.normal.z,
                                  vertex.uv.x,
                                  vertex.uv.y,
                                  vertex.uv.x * vertex.uv.x,
                                  vertex.uv.y * vertex.uv.y,
                                  vertex.lightmap_uv.x,
                                  vertex.lightmap_uv.y,
                                  vertex.lightmap_uv.x * vertex.lightmap_uv.x,
                                  vertex.lightmap_uv.y * vertex.lightmap_uv.y,
                                  vertex.color.x,
                                  vertex.color.y,
                                  vertex.color.z,
                                  vertex.color.w,
                                  vertex.tangent.x,
                                  vertex.tangent.y,
                                  vertex.tangent.z};
    for (uint32_t k = 0u; k < 18u; ++k) {
      sums->corner_sums[k] += corner[k];
    }
    sums->corners += 1u;
  }
  sums->area += area;
  for (uint32_t k = 0u; k < 3u; ++k) {
    sums->centroid[k] += area * c[k];
  }
  sums->second[0] += area * c[0] * c[0];
  sums->second[1] += area * c[1] * c[1];
  sums->second[2] += area * c[2] * c[2];
  sums->second[3] += area * c[0] * c[1];
  sums->second[4] += area * c[0] * c[2];
  sums->second[5] += area * c[1] * c[2];
}

/* The instance's moments: area, centroid, covariance about the centroid
   and the corners' means. */
void finish_moments(const MomentSums &sums,
                    float32_t out[kVkrBakeInstanceMoments]) {
  const float64_t area = sums.area > 0.0 ? sums.area : 1.0;
  const float64_t c[3] = {sums.centroid[0] / area, sums.centroid[1] / area,
                          sums.centroid[2] / area};
  out[0] = (float32_t)sums.area;
  out[1] = (float32_t)c[0];
  out[2] = (float32_t)c[1];
  out[3] = (float32_t)c[2];
  out[4] = (float32_t)(sums.second[0] / area - c[0] * c[0]);
  out[5] = (float32_t)(sums.second[1] / area - c[1] * c[1]);
  out[6] = (float32_t)(sums.second[2] / area - c[2] * c[2]);
  out[7] = (float32_t)(sums.second[3] / area - c[0] * c[1]);
  out[8] = (float32_t)(sums.second[4] / area - c[0] * c[2]);
  out[9] = (float32_t)(sums.second[5] / area - c[1] * c[2]);
  const float64_t corners = sums.corners ? (float64_t)sums.corners : 1.0;
  for (uint32_t k = 0u; k < 18u; ++k) {
    out[10u + k] = (float32_t)(sums.corner_sums[k] / corners);
  }
}

/* The key's identity: the document id when the entity has one, else its
   index, then node and kind. */
std::string key_string(const VkrBakeSceneInstance &key) {
  std::string text(key.has_document_id ? "d" : "i");
  if (key.has_document_id) {
    text.append(reinterpret_cast<const char *>(key.document_id.data()),
                key.document_id.size());
  } else {
    text.append(reinterpret_cast<const char *>(&key.entity_index),
                sizeof(key.entity_index));
  }
  text.append(reinterpret_cast<const char *>(&key.source_node_index),
              sizeof(key.source_node_index));
  text.push_back((char)key.kind);
  return text;
}

void put_u32(std::vector<uint8_t> *out, uint32_t value) {
  for (uint32_t i = 0u; i < 4u; ++i) {
    out->push_back((uint8_t)(value >> (8u * i)));
  }
}

void put_u64(std::vector<uint8_t> *out, uint64_t value) {
  for (uint32_t i = 0u; i < 8u; ++i) {
    out->push_back((uint8_t)(value >> (8u * i)));
  }
}

void put_f32(std::vector<uint8_t> *out, float32_t value) {
  uint32_t bits = 0u;
  std::memcpy(&bits, &value, sizeof(bits));
  put_u32(out, bits);
}

void put_bytes(std::vector<uint8_t> *out, const void *data, size_t size) {
  const uint8_t *bytes = static_cast<const uint8_t *>(data);
  out->insert(out->end(), bytes, bytes + size);
}

/* Little-endian reads from a checked byte range. */
struct Reader {
  const uint8_t *data = nullptr;
  size_t size = 0u;
  size_t at = 0u;
  bool ok = true;

  const uint8_t *take(size_t count) {
    if (!ok || count > size - at) {
      ok = false;
      return nullptr;
    }
    const uint8_t *start = data + at;
    at += count;
    return start;
  }

  uint32_t u32() {
    const uint8_t *b = take(4u);
    return b ? (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
                   ((uint32_t)b[3] << 24)
             : 0u;
  }

  uint64_t u64() {
    const uint64_t low = u32();
    const uint64_t high = u32();
    return low | (high << 32);
  }

  float32_t f32() {
    const uint32_t bits = u32();
    float32_t value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }

  void copy(void *out, size_t count) {
    const uint8_t *b = take(count);
    if (b) {
      std::memcpy(out, b, count);
    }
  }
};

Vec3 read_vec3(Reader *r) {
  const float32_t x = r->f32();
  const float32_t y = r->f32();
  const float32_t z = r->f32();
  return vec3_new(x, y, z);
}

void put_vec3(std::vector<uint8_t> *out, Vec3 value) {
  put_f32(out, value.x);
  put_f32(out, value.y);
  put_f32(out, value.z);
}

/* Squared distance between two boxes; zero when they meet. */
float32_t box_distance_squared(Vec3 a_min, Vec3 a_max, Vec3 b_min, Vec3 b_max) {
  const float32_t dx =
      std::max(std::max(b_min.x - a_max.x, a_min.x - b_max.x), 0.0f);
  const float32_t dy =
      std::max(std::max(b_min.y - a_max.y, a_min.y - b_max.y), 0.0f);
  const float32_t dz =
      std::max(std::max(b_min.z - a_max.z, a_min.z - b_max.z), 0.0f);
  return dx * dx + dy * dy + dz * dz;
}

float32_t point_box_distance(Vec3 p, Vec3 box_min, Vec3 box_max) {
  return std::sqrt(box_distance_squared(p, p, box_min, box_max));
}

/* Whether the segment from `origin` along `direction` for `length` meets
   the box (slab test). */
bool segment_meets_box(Vec3 origin, Vec3 direction, float32_t length,
                       Vec3 box_min, Vec3 box_max) {
  float32_t enter = 0.0f;
  float32_t leave = length;
  const float32_t o[3] = {origin.x, origin.y, origin.z};
  const float32_t d[3] = {direction.x, direction.y, direction.z};
  const float32_t lo[3] = {box_min.x, box_min.y, box_min.z};
  const float32_t hi[3] = {box_max.x, box_max.y, box_max.z};
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    if (std::fabs(d[axis]) < 1.0e-12f) {
      if (o[axis] < lo[axis] || o[axis] > hi[axis]) {
        return false;
      }
      continue;
    }
    float32_t t0 = (lo[axis] - o[axis]) / d[axis];
    float32_t t1 = (hi[axis] - o[axis]) / d[axis];
    if (t0 > t1) {
      std::swap(t0, t1);
    }
    enter = std::max(enter, t0);
    leave = std::min(leave, t1);
    if (enter > leave) {
      return false;
    }
  }
  return true;
}

struct ChangedBox {
  Vec3 min;
  Vec3 max;
};

} // namespace

bool vkr_bake_lightmap_instance_same(const VkrBakeLightmapInputInstance &a,
                                     const VkrBakeLightmapInputInstance &b) {
  if (a.digest != b.digest || !near3(a.bounds_min, b.bounds_min) ||
      !near3(a.bounds_max, b.bounds_max)) {
    return false;
  }
  for (uint32_t k = 0u; k < kVkrBakeInstanceMoments; ++k) {
    if (!near(a.moments[k], b.moments[k])) {
      return false;
    }
  }
  return true;
}

bool vkr_bake_lightmap_light_same(const VkrBakeLightmapInputLight &a,
                                  const VkrBakeLightmapInputLight &b) {
  if (a.lamp != b.lamp || a.digest != b.digest) {
    return false;
  }
  for (uint32_t k = 0u; k < kVkrBakeLightFields; ++k) {
    if (!near(a.fields[k], b.fields[k])) {
      return false;
    }
  }
  return true;
}

bool vkr_bake_lightmap_inputs_build(const VkrBakeScene &scene,
                                    const std::string &settings,
                                    const std::vector<VkrLightLayer> &layers,
                                    const std::vector<Vec3> &sun_keys,
                                    const VkrBakeLightmapLayout &layout,
                                    const VkrBakeLightmapLayout &direct_layout,
                                    uint32_t threads,
                                    VkrBakeLightmapInputs *out_inputs) {
  if (!out_inputs) {
    return false;
  }
  try {
    VkrBakeLightmapInputs inputs;
    const size_t instance_count = scene.instances.size();
    for (const VkrBakeTriangle &triangle : scene.triangles) {
      if (triangle.source_instance_index >= instance_count) {
        return false;
      }
    }
    std::vector<VkrBakeDigest> materials(scene.materials.size());
    for (size_t m = 0u; m < scene.materials.size(); ++m) {
      materials[m] = material_digest(scene, scene.materials[m]);
    }

    /* Triangles grouped by instance in scene order, so each instance's
       digest reads its triangles in the order the loader appended them. */
    std::vector<uint32_t> first(instance_count + 1u, 0u);
    for (const VkrBakeTriangle &triangle : scene.triangles) {
      first[triangle.source_instance_index + 1u] += 1u;
    }
    for (size_t i = 0u; i < instance_count; ++i) {
      first[i + 1u] += first[i];
    }
    std::vector<uint32_t> order(scene.triangles.size());
    {
      std::vector<uint32_t> cursor(first.begin(), first.end() - 1);
      for (uint32_t t = 0u; t < scene.triangles.size(); ++t) {
        order[cursor[scene.triangles[t].source_instance_index]++] = t;
      }
    }
    inputs.instances.resize(instance_count);
    auto hash_instances = [&](size_t begin, size_t end) {
      for (size_t i = begin; i < end; ++i) {
        VkrBakeLightmapInputInstance &instance = inputs.instances[i];
        instance.key = scene.instances[i];
        Vec3 lo = vec3_new(INFINITY, INFINITY, INFINITY);
        Vec3 hi = vec3_new(-INFINITY, -INFINITY, -INFINITY);
        DigestWriter w;
        MomentSums sums;
        w.u32(first[i + 1u] - first[i]);
        for (uint32_t k = first[i]; k < first[i + 1u]; ++k) {
          const VkrBakeTriangle &triangle = scene.triangles[order[k]];
          add_triangle(&sums, triangle);
          for (const VkrBakeVertex &vertex : triangle.vertex) {
            lo = vec3_new(std::min(lo.x, vertex.position.x),
                          std::min(lo.y, vertex.position.y),
                          std::min(lo.z, vertex.position.z));
            hi = vec3_new(std::max(hi.x, vertex.position.x),
                          std::max(hi.y, vertex.position.y),
                          std::max(hi.z, vertex.position.z));
          }
          w.u32(triangle.flags);
          w.digest(materials[triangle.material_index]);
        }
        instance.digest = w.finish();
        finish_moments(sums, instance.moments);
        if (first[i] == first[i + 1u]) {
          lo = hi = vec3_zero();
        }
        instance.bounds_min = lo;
        instance.bounds_max = hi;
        if (i < layout.rect_by_instance.size() &&
            layout.rect_by_instance[i] != UINT32_MAX) {
          const VkrBakeLightmapRect &rect =
              layout.rects[layout.rect_by_instance[i]];
          instance.width = rect.width;
          instance.height = rect.height;
        }
        if (i < direct_layout.rect_by_instance.size() &&
            direct_layout.rect_by_instance[i] != UINT32_MAX) {
          const VkrBakeLightmapRect &rect =
              direct_layout.rects[direct_layout.rect_by_instance[i]];
          instance.direct_width = rect.width;
          instance.direct_height = rect.height;
        }
      }
    };
    const size_t workers = std::max<size_t>(
        1u, std::min<size_t>(threads ? threads : 1u, instance_count / 64u));
    if (workers <= 1u) {
      hash_instances(0u, instance_count);
    } else {
      std::vector<std::thread> pool;
      for (size_t w = 0u; w < workers; ++w) {
        pool.emplace_back(hash_instances, instance_count * w / workers,
                          instance_count * (w + 1u) / workers);
      }
      for (std::thread &thread : pool) {
        thread.join();
      }
    }

    DigestWriter globals;
    globals.text("vkr-lightmap-globals 2");
    globals.text(settings.c_str());
    globals.u32(layout.page_size);
    globals.u32(scene.atmosphere.enabled ? 1u : 0u);
    if (scene.atmosphere.enabled) {
      const uint64_t recipe =
          vkr_bake_atmosphere_recipe_hash(&scene.atmosphere);
      globals.bytes(&recipe, sizeof(recipe));
    }
    globals.u32((uint32_t)scene.environment.kind);
    globals.u32(scene.environment.enabled ? 1u : 0u);
    globals.rounded3(scene.environment.constant_radiance);
    globals.rounded(scene.environment.intensity);
    globals.rounded(scene.environment.diffuse_intensity);
    globals.rounded(scene.environment.specular_intensity);
    globals.rounded(scene.environment.sh_deringing);
    globals.u32(scene.subsurface_profile_count);
    for (uint32_t p = 0u; p < scene.subsurface_profile_count; ++p) {
      globals.rounded3(scene.subsurface_profiles[p].diffusion_distance);
    }
    globals.u32((uint32_t)sun_keys.size());
    for (const Vec3 &key : sun_keys) {
      globals.rounded3(key);
    }
    globals.u32((uint32_t)layers.size());
    for (const VkrLightLayer &layer : layers) {
      globals.u32(layer.kind);
      globals.u32(layer.index);
      globals.rounded3(layer.sun_direction);
      globals.text(layer.name);
    }
    inputs.lights.resize(scene.lights.size());
    for (size_t l = 0u; l < scene.lights.size(); ++l) {
      const VkrBakeSceneLight &light = scene.lights[l];
      VkrBakeLightmapInputLight &input = inputs.lights[l];
      if (light.kind == VkrBakeSceneLightKind::Directional) {
        /* The sun and other directional lights reach everything. */
        light_rounded(&globals, light);
        continue;
      }
      DigestWriter w;
      light_exact(&w, light);
      input.digest = w.finish();
      light_floats(light, input.fields);
      input.lamp = true;
      input.position = light.position;
      input.range = light.range;
      input.source_radius = light.source_radius;
      input.unbounded = !((light.kind == VkrBakeSceneLightKind::Point ||
                           light.kind == VkrBakeSceneLightKind::Spot) &&
                          light.range > 0.0f);
      std::memcpy(input.group, light.group, sizeof(input.group));
    }
    inputs.globals = globals.finish();
    *out_inputs = std::move(inputs);
    return true;
  } catch (const std::bad_alloc &) {
    return false;
  } catch (const std::system_error &) {
    return false;
  }
}

bool vkr_bake_lightmap_index_write(const char *path,
                                   const VkrBakeLightmapInputs &inputs,
                                   uint32_t set_payload_crc,
                                   uint64_t set_bytes) {
  try {
    std::vector<uint8_t> bytes;
    put_u32(&bytes, kIndexMagic);
    put_u32(&bytes, kIndexVersion);
    put_u32(&bytes, kIndexHeaderBytes);
    put_u32(&bytes, (uint32_t)inputs.instances.size());
    put_u32(&bytes, (uint32_t)inputs.lights.size());
    put_u32(&bytes, set_payload_crc);
    put_u64(&bytes, set_bytes);
    put_bytes(&bytes, inputs.globals.data(), inputs.globals.size());
    bytes.resize(kIndexHeaderBytes, 0u);
    for (const VkrBakeLightmapInputInstance &instance : inputs.instances) {
      put_bytes(&bytes, instance.key.document_id.data(), 16u);
      put_u32(&bytes, instance.key.has_document_id ? 1u : 0u);
      put_u32(&bytes, instance.key.entity_index);
      put_u32(&bytes, instance.key.source_node_index);
      put_u32(&bytes, (uint32_t)instance.key.kind);
      put_bytes(&bytes, instance.digest.data(), instance.digest.size());
      for (float32_t moment : instance.moments) {
        put_f32(&bytes, moment);
      }
      put_vec3(&bytes, instance.bounds_min);
      put_vec3(&bytes, instance.bounds_max);
      put_u32(&bytes, instance.width);
      put_u32(&bytes, instance.height);
      put_u32(&bytes, instance.direct_width);
      put_u32(&bytes, instance.direct_height);
    }
    for (const VkrBakeLightmapInputLight &light : inputs.lights) {
      put_bytes(&bytes, light.digest.data(), light.digest.size());
      for (float32_t field : light.fields) {
        put_f32(&bytes, field);
      }
      put_vec3(&bytes, light.position);
      put_f32(&bytes, light.range);
      put_f32(&bytes, light.source_radius);
      put_u32(&bytes, (light.unbounded ? 1u : 0u) | (light.lamp ? 2u : 0u));
      put_bytes(&bytes, light.group, sizeof(light.group));
    }
    put_u32(&bytes, vkr_crc32(bytes.data(), bytes.size()));
    const std::filesystem::path target = vkr_filesystem_native_utf8_path(path);
    std::ofstream stream(target, std::ios::binary | std::ios::trunc);
    stream.write((const char *)bytes.data(), (std::streamsize)bytes.size());
    return (bool)stream;
  } catch (const std::bad_alloc &) {
    return false;
  }
}

bool vkr_bake_lightmap_index_read(const char *path,
                                  VkrBakeLightmapInputs *out_inputs,
                                  uint32_t *out_set_payload_crc,
                                  uint64_t *out_set_bytes) {
  try {
    const std::filesystem::path source = vkr_filesystem_native_utf8_path(path);
    std::ifstream stream(source, std::ios::binary);
    if (!stream) {
      return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)),
                               std::istreambuf_iterator<char>());
    if (bytes.size() < kIndexHeaderBytes + 4u) {
      return false;
    }
    Reader tail = {bytes.data() + bytes.size() - 4u, 4u};
    if (tail.u32() != vkr_crc32(bytes.data(), bytes.size() - 4u)) {
      return false;
    }
    Reader r = {bytes.data(), bytes.size() - 4u};
    VkrBakeLightmapInputs inputs;
    const uint32_t magic = r.u32();
    const uint32_t version = r.u32();
    const uint32_t header_bytes = r.u32();
    const uint32_t instance_count = r.u32();
    const uint32_t light_count = r.u32();
    const uint32_t set_crc = r.u32();
    const uint64_t set_bytes = r.u64();
    r.copy(inputs.globals.data(), inputs.globals.size());
    if (!r.ok || magic != kIndexMagic || version != kIndexVersion ||
        header_bytes != kIndexHeaderBytes ||
        instance_count > kIndexMaxInstances || light_count > kIndexMaxLights ||
        r.size != kIndexHeaderBytes +
                      (size_t)instance_count * kIndexInstanceBytes +
                      (size_t)light_count * kIndexLightBytes) {
      return false;
    }
    r.at = kIndexHeaderBytes;
    inputs.instances.resize(instance_count);
    for (VkrBakeLightmapInputInstance &instance : inputs.instances) {
      r.copy(instance.key.document_id.data(), 16u);
      instance.key.has_document_id = r.u32() != 0u;
      instance.key.entity_index = r.u32();
      instance.key.source_node_index = r.u32();
      const uint32_t kind = r.u32();
      if (kind > (uint32_t)VkrBakeSceneInstanceKind::Blockout) {
        return false;
      }
      instance.key.kind = (VkrBakeSceneInstanceKind)kind;
      r.copy(instance.digest.data(), instance.digest.size());
      for (float32_t &moment : instance.moments) {
        moment = r.f32();
      }
      instance.bounds_min = read_vec3(&r);
      instance.bounds_max = read_vec3(&r);
      instance.width = r.u32();
      instance.height = r.u32();
      instance.direct_width = r.u32();
      instance.direct_height = r.u32();
    }
    inputs.lights.resize(light_count);
    for (VkrBakeLightmapInputLight &light : inputs.lights) {
      r.copy(light.digest.data(), light.digest.size());
      for (float32_t &field : light.fields) {
        field = r.f32();
      }
      light.position = read_vec3(&r);
      light.range = r.f32();
      light.source_radius = r.f32();
      const uint32_t flags = r.u32();
      light.unbounded = (flags & 1u) != 0u;
      light.lamp = (flags & 2u) != 0u;
      r.copy(light.group, sizeof(light.group));
      light.group[sizeof(light.group) - 1u] = '\0';
    }
    if (!r.ok) {
      return false;
    }
    *out_inputs = std::move(inputs);
    *out_set_payload_crc = set_crc;
    *out_set_bytes = set_bytes;
    return true;
  } catch (const std::bad_alloc &) {
    return false;
  }
}

namespace {

/* Appends the lamps of `from` that no lamp of `to` matches, each lamp of
   `to` matching once; the one at the same index is tried first. */
void unmatched_lamps(const std::vector<VkrBakeLightmapInputLight> &from,
                     const std::vector<VkrBakeLightmapInputLight> &to,
                     std::vector<const VkrBakeLightmapInputLight *> *out) {
  std::vector<uint8_t> used(to.size(), 0u);
  for (size_t i = 0u; i < from.size(); ++i) {
    if (!from[i].lamp) {
      continue;
    }
    size_t match = to.size();
    if (i < to.size() && vkr_bake_lightmap_light_same(from[i], to[i])) {
      match = i;
    }
    for (size_t j = 0u; match == to.size() && j < to.size(); ++j) {
      if (!used[j] && vkr_bake_lightmap_light_same(from[i], to[j])) {
        match = j;
      }
    }
    if (match == to.size() || used[match]) {
      out->push_back(&from[i]);
    } else {
      used[match] = 1u;
    }
  }
}

} // namespace

bool vkr_bake_lightmap_inputs_match(const VkrBakeLightmapInputs &previous,
                                    const VkrBakeLightmapInputs &current) {
  if (previous.globals != current.globals ||
      previous.instances.size() != current.instances.size()) {
    return false;
  }
  try {
    std::map<std::string, uint32_t> previous_by_key;
    for (uint32_t i = 0u; i < previous.instances.size(); ++i) {
      previous_by_key[key_string(previous.instances[i].key)] = i;
    }
    for (const VkrBakeLightmapInputInstance &now : current.instances) {
      const auto found = previous_by_key.find(key_string(now.key));
      if (found == previous_by_key.end()) {
        return false;
      }
      const VkrBakeLightmapInputInstance &before =
          previous.instances[found->second];
      if (!vkr_bake_lightmap_instance_same(before, now) ||
          before.width != now.width || before.height != now.height ||
          before.direct_width != now.direct_width ||
          before.direct_height != now.direct_height) {
        return false;
      }
    }
    std::vector<const VkrBakeLightmapInputLight *> changed;
    unmatched_lamps(previous.lights, current.lights, &changed);
    unmatched_lamps(current.lights, previous.lights, &changed);
    return changed.empty();
  } catch (const std::bad_alloc &) {
    return false;
  }
}

bool vkr_bake_lightmap_plan_incremental(
    const VkrBakeLightmapInputs &previous, const VkrBakeLightmapInputs &current,
    const std::vector<VkrBakeLightmapPlanLayer> &layers,
    const std::vector<uint8_t> &previous_has_rect,
    const std::vector<uint8_t> &previous_has_direct_rect, float32_t reach,
    VkrBakeLightmapIncrementalPlan *out_plan) {
  if (!out_plan || previous.globals != current.globals ||
      previous_has_rect.size() != current.instances.size() ||
      previous_has_direct_rect.size() != current.instances.size() ||
      !std::isfinite(reach) || reach < 0.0f) {
    return false;
  }
  try {
    VkrBakeLightmapIncrementalPlan plan;
    const size_t count = current.instances.size();
    plan.layer_dirty.assign(layers.size(), std::vector<uint8_t>(count, 0u));
    plan.direct_dirty.assign(layers.size(), std::vector<uint8_t>(count, 0u));

    /* Instances by key in the previous inputs, then what changed: an
       instance's old and new boxes, and the instances relit everywhere. */
    std::map<std::string, uint32_t> previous_by_key;
    for (uint32_t i = 0u; i < previous.instances.size(); ++i) {
      previous_by_key[key_string(previous.instances[i].key)] = i;
    }
    std::vector<ChangedBox> boxes;
    std::vector<uint8_t> matched(previous.instances.size(), 0u);
    std::vector<uint8_t> changed(count, 0u);
    Vec3 scene_min = vec3_new(INFINITY, INFINITY, INFINITY);
    Vec3 scene_max = vec3_new(-INFINITY, -INFINITY, -INFINITY);
    auto grow = [&](Vec3 lo, Vec3 hi) {
      scene_min =
          vec3_new(std::min(scene_min.x, lo.x), std::min(scene_min.y, lo.y),
                   std::min(scene_min.z, lo.z));
      scene_max =
          vec3_new(std::max(scene_max.x, hi.x), std::max(scene_max.y, hi.y),
                   std::max(scene_max.z, hi.z));
    };
    for (uint32_t i = 0u; i < count; ++i) {
      const VkrBakeLightmapInputInstance &now = current.instances[i];
      grow(now.bounds_min, now.bounds_max);
      const auto found = previous_by_key.find(key_string(now.key));
      if (found == previous_by_key.end()) {
        changed[i] = 1u;
        boxes.push_back({now.bounds_min, now.bounds_max});
        continue;
      }
      matched[found->second] = 1u;
      const VkrBakeLightmapInputInstance &before =
          previous.instances[found->second];
      const bool same = vkr_bake_lightmap_instance_same(before, now);
      if (!same) {
        changed[i] = 1u;
        boxes.push_back({before.bounds_min, before.bounds_max});
        boxes.push_back({now.bounds_min, now.bounds_max});
      } else if (before.width != now.width || before.height != now.height ||
                 before.direct_width != now.direct_width ||
                 before.direct_height != now.direct_height) {
        /* Same geometry on a resized rectangle: only its own texels. */
        changed[i] = 1u;
      }
    }
    for (uint32_t i = 0u; i < previous.instances.size(); ++i) {
      if (!matched[i]) {
        boxes.push_back({previous.instances[i].bounds_min,
                         previous.instances[i].bounds_max});
      }
    }
    for (const ChangedBox &box : boxes) {
      grow(box.min, box.max);
    }
    plan.changed_boxes = (uint32_t)boxes.size();
    for (uint32_t i = 0u; i < count; ++i) {
      const bool lightmapped =
          current.instances[i].width != 0u && current.instances[i].height != 0u;
      if (lightmapped && (changed[i] || !previous_has_rect[i])) {
        plan.changed_instances += 1u;
      }
    }

    /* Changed lamps: those of either input set the other has no matching
       lamp for, each with its group. */
    std::vector<const VkrBakeLightmapInputLight *> lamps_changed;
    unmatched_lamps(previous.lights, current.lights, &lamps_changed);
    unmatched_lamps(current.lights, previous.lights, &lamps_changed);
    plan.changed_lights = (uint32_t)lamps_changed.size();

    const Vec3 extent = vec3_sub(scene_max, scene_min);
    const float32_t sweep =
        std::isfinite(extent.x) ? vec3_length(extent) + reach : 0.0f;
    const float32_t reach_squared = reach * reach;
    for (size_t l = 0u; l < layers.size(); ++l) {
      const VkrBakeLightmapPlanLayer &layer = layers[l];
      const bool sun_key = layer.kind == VKR_LIGHT_LAYER_SUN_KEY;
      /* This layer's group name, from its first light, for its changed
         lamps; a lamp layer without lights has none. */
      const char *group = nullptr;
      for (uint32_t light : layer.lights) {
        if (light < current.lights.size() && current.lights[light].lamp) {
          group = current.lights[light].group;
          break;
        }
      }
      std::vector<const VkrBakeLightmapInputLight *> layer_changed;
      for (const VkrBakeLightmapInputLight *light : lamps_changed) {
        if (!sun_key && group && std::strcmp(light->group, group) == 0) {
          layer_changed.push_back(light);
        }
      }
      for (uint32_t i = 0u; i < count; ++i) {
        const VkrBakeLightmapInputInstance &instance = current.instances[i];
        if (instance.width == 0u || instance.height == 0u) {
          continue;
        }
        const Vec3 lo = instance.bounds_min;
        const Vec3 hi = instance.bounds_max;
        if (changed[i] || !previous_has_rect[i]) {
          plan.layer_dirty[l][i] = 1u;
          plan.direct_dirty[l][i] =
              layer.lamp_direct && instance.direct_width != 0u;
          continue;
        }
        /* The lamp-direct rule: a changed lamp reaches the instance, or a
           changed box meets the box spanning a reaching lamp's emitter and
           the instance. */
        auto direct_changes = [&]() {
          for (const VkrBakeLightmapInputLight *light : layer_changed) {
            if (light->unbounded ||
                point_box_distance(light->position, lo, hi) < light->range) {
              return true;
            }
          }
          for (uint32_t light_index : layer.lights) {
            const VkrBakeLightmapInputLight &light =
                current.lights[light_index];
            if (!light.lamp ||
                (!light.unbounded &&
                 point_box_distance(light.position, lo, hi) >= light.range)) {
              continue;
            }
            const float32_t r = light.source_radius;
            const Vec3 span_min =
                vec3_new(std::min(lo.x, light.position.x - r),
                         std::min(lo.y, light.position.y - r),
                         std::min(lo.z, light.position.z - r));
            const Vec3 span_max =
                vec3_new(std::max(hi.x, light.position.x + r),
                         std::max(hi.y, light.position.y + r),
                         std::max(hi.z, light.position.z + r));
            for (const ChangedBox &box : boxes) {
              if (light.unbounded ||
                  box_distance_squared(span_min, span_max, box.min, box.max) ==
                      0.0f) {
                return true;
              }
            }
          }
          return false;
        };
        bool dirty = false;
        if (sun_key) {
          /* A changed box's shadow runs away from the key light; its bounce
             and its sky occlusion stay near it. */
          const Vec3 away = vec3_scale(layer.key_light_direction, -1.0f);
          for (const ChangedBox &box : boxes) {
            const Vec3 center = vec3_scale(vec3_add(box.min, box.max), 0.5f);
            const Vec3 half = vec3_scale(vec3_sub(box.max, box.min), 0.5f);
            const Vec3 grown_min =
                vec3_new(lo.x - half.x - reach, lo.y - half.y - reach,
                         lo.z - half.z - reach);
            const Vec3 grown_max =
                vec3_new(hi.x + half.x + reach, hi.y + half.y + reach,
                         hi.z + half.z + reach);
            if (segment_meets_box(center, away, sweep, grown_min, grown_max)) {
              dirty = true;
              break;
            }
          }
        } else {
          for (const VkrBakeLightmapInputLight *light : layer_changed) {
            if (light->unbounded ||
                point_box_distance(light->position, lo, hi) <
                    light->range + reach) {
              dirty = true;
              break;
            }
          }
          for (size_t b = 0u; !dirty && b < boxes.size(); ++b) {
            const ChangedBox &box = boxes[b];
            if (box_distance_squared(lo, hi, box.min, box.max) <
                reach_squared) {
              dirty = true;
              break;
            }
            /* A box within a lamp's range changes how that lamp's light
               bounces through the lamp's reach. */
            for (uint32_t light_index : layer.lights) {
              const VkrBakeLightmapInputLight &light =
                  current.lights[light_index];
              if (!light.lamp) {
                continue;
              }
              if (light.unbounded ||
                  (point_box_distance(light.position, box.min, box.max) <
                       light.range &&
                   point_box_distance(light.position, lo, hi) <
                       light.range + reach)) {
                dirty = true;
                break;
              }
            }
          }
          dirty = dirty || (layer.texel_direct && direct_changes());
        }
        plan.layer_dirty[l][i] = dirty ? 1u : 0u;
        if (layer.lamp_direct && instance.direct_width != 0u) {
          plan.direct_dirty[l][i] =
              (!previous_has_direct_rect[i] || direct_changes()) ? 1u : 0u;
        }
      }
    }
    *out_plan = std::move(plan);
    return true;
  } catch (const std::bad_alloc &) {
    return false;
  }
}
