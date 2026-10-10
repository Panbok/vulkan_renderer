#include "vkr_bake_stationary.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <thread>

namespace {

/* Rays between two lamps start and end this far from each, past the lamp's
   own fixture (the bake's punctual occluder clip is at most 5 cm). */
constexpr float32_t kLampClearance = 0.05f;

struct Aabb {
  Vec3 min = {std::numeric_limits<float32_t>::max(),
              std::numeric_limits<float32_t>::max(),
              std::numeric_limits<float32_t>::max()};
  Vec3 max = {-std::numeric_limits<float32_t>::max(),
              -std::numeric_limits<float32_t>::max(),
              -std::numeric_limits<float32_t>::max()};

  bool empty() const { return min.x > max.x; }

  void add(Vec3 point) {
    min = vec3_new(std::min(min.x, point.x), std::min(min.y, point.y),
                   std::min(min.z, point.z));
    max = vec3_new(std::max(max.x, point.x), std::max(max.y, point.y),
                   std::max(max.z, point.z));
  }
};

/* Squared distance from `point` to the box, zero inside it. */
float32_t distance_squared_to(const Aabb &box, Vec3 point) {
  const float32_t dx =
      std::max(std::max(box.min.x - point.x, 0.0f), point.x - box.max.x);
  const float32_t dy =
      std::max(std::max(box.min.y - point.y, 0.0f), point.y - box.max.y);
  const float32_t dz =
      std::max(std::max(box.min.z - point.z, 0.0f), point.z - box.max.z);
  return dx * dx + dy * dy + dz * dz;
}

bool stationary_light(const VkrBakeSceneLight &light) {
  return light.stationary && light.enabled;
}

/* Whether nothing blocks the segment between two lamps. */
bool lamps_see_each_other(const VkrBakeBvh &bvh, Vec3 a, Vec3 b) {
  const Vec3 offset = vec3_sub(b, a);
  const float32_t distance = vec3_length(offset);
  if (!(distance > 2.0f * kLampClearance)) {
    return true;
  }
  const VkrBakeRay ray = {
      .origin = a,
      .direction = vec3_scale(offset, 1.0f / distance),
      .t_min = kLampClearance,
      .t_max = distance - kLampClearance,
  };
  VkrBakeHit hit = {};
  return !vkr_bake_bvh_intersect_closest(&bvh, ray, &hit);
}

} // namespace

uint32_t vkr_bake_stationary_disable_unkeyed(VkrBakeScene *scene) {
  uint32_t disabled = 0u;
  for (size_t i = 0u; i < scene->lights.size(); ++i) {
    VkrBakeSceneLight &light = scene->lights[i];
    if (!stationary_light(light)) {
      continue;
    }
    const bool keyed_kind = light.kind == VkrBakeSceneLightKind::Point ||
                            light.kind == VkrBakeSceneLightKind::Spot;
    if (light.has_document_id && keyed_kind && light.range > 0.0f) {
      continue;
    }
    std::fprintf(stderr,
                 "Stationary light %zu has no %s; it stays out of the bake "
                 "and lights at runtime as a dynamic light\n",
                 i,
                 !light.has_document_id
                     ? "document id"
                     : (!keyed_kind ? "point or spot kind" : "positive range"));
    light.enabled = false_v;
    ++disabled;
  }
  return disabled;
}

bool vkr_bake_plan_stationary(const VkrBakeScene &scene, const VkrBakeBvh &bvh,
                              uint32_t source_instance_count,
                              VkrBakeStationaryPlan *out_plan) {
  try {
    VkrBakeStationaryPlan plan;
    plan.lamp_by_light.assign(scene.lights.size(), UINT32_MAX);
    for (uint32_t i = 0u; i < scene.lights.size(); ++i) {
      const VkrBakeSceneLight &light = scene.lights[i];
      if (!stationary_light(light)) {
        continue;
      }
      VkrLightmapStationaryLamp lamp = {};
      std::memcpy(lamp.document_id, light.document_id.data(),
                  sizeof(lamp.document_id));
      lamp.position = light.position;
      lamp.range = light.range;
      lamp.direction = vec3_normalize(light.direction);
      lamp.weight = (0.2126f * light.color.x + 0.7152f * light.color.y +
                     0.0722f * light.color.z) *
                    std::max(light.intensity, 0.0f);
      lamp.cos_inner = std::cos(light.inner_cone_angle);
      lamp.cos_outer = std::cos(light.outer_cone_angle);
      lamp.kind = light.kind == VkrBakeSceneLightKind::Spot ? 2u : 1u;
      lamp.channel = VKR_LIGHTMAP_STATIONARY_NO_CHANNEL;
      plan.lamp_by_light[i] = (uint32_t)plan.lamps.size();
      plan.lamps.push_back(lamp);
      plan.lights.push_back(i);
    }
    if (plan.lamps.size() > VKR_LIGHTMAP_SET_MAX_STATIONARY) {
      std::fprintf(stderr,
                   "The scene has %zu stationary lights; a set holds %u\n",
                   plan.lamps.size(), VKR_LIGHTMAP_SET_MAX_STATIONARY);
      return false;
    }
    plan.ranges_by_source.assign(source_instance_count,
                                 VkrLightmapStationaryRange{});
    if (plan.lamps.empty()) {
      *out_plan = std::move(plan);
      return true;
    }

    /* World bounds of each lightmapped instance. */
    std::vector<Aabb> bounds(source_instance_count);
    for (const VkrBakeTriangle &triangle : scene.triangles) {
      if (triangle.source_instance_index >= source_instance_count) {
        continue;
      }
      Aabb &box = bounds[triangle.source_instance_index];
      for (const VkrBakeVertex &vertex : triangle.vertex) {
        box.add(vertex.position);
      }
    }

    /* Each instance's reach: the lamps whose range reaches its bounds.
       Lamps that share an instance may conflict. */
    const size_t lamp_count = plan.lamps.size();
    std::vector<uint8_t> share(lamp_count * lamp_count, 0u);
    std::vector<uint16_t> reaching;
    plan.reach_by_source.assign(source_instance_count,
                                VkrLightmapStationaryRange{});
    plan.ranges_by_source.assign(source_instance_count,
                                 VkrLightmapStationaryRange{});
    plan.block_by_source.assign(source_instance_count, 0u);
    uint32_t block_total = 0u;
    for (uint32_t s = 0u; s < source_instance_count; ++s) {
      if (bounds[s].empty()) {
        continue;
      }
      reaching.clear();
      for (uint32_t l = 0u; l < lamp_count; ++l) {
        const VkrLightmapStationaryLamp &lamp = plan.lamps[l];
        if (distance_squared_to(bounds[s], lamp.position) <
            lamp.range * lamp.range) {
          reaching.push_back((uint16_t)l);
        }
      }
      plan.reach_by_source[s] = {(uint32_t)plan.reach.size(),
                                 (uint32_t)reaching.size()};
      plan.reach.insert(plan.reach.end(), reaching.begin(), reaching.end());
      plan.block_by_source[s] = block_total;
      plan.ranges_by_source[s] = {block_total, 0u};
      block_total += std::min<uint32_t>((uint32_t)reaching.size(),
                                        VKR_LIGHTMAP_SET_MAX_CANDIDATES);
      for (size_t a = 0u; a < reaching.size(); ++a) {
        for (size_t b = a + 1u; b < reaching.size(); ++b) {
          share[(size_t)reaching[a] * lamp_count + reaching[b]] = 1u;
        }
      }
    }
    plan.candidates.assign(block_total, 0u);

    /* Conflicts among the lamps that cast shadows, which need a channel. */
    std::vector<std::vector<uint32_t>> conflicts(lamp_count);
    for (uint32_t a = 0u; a < lamp_count; ++a) {
      const VkrBakeSceneLight &light_a = scene.lights[plan.lights[a]];
      for (uint32_t b = a + 1u; b < lamp_count; ++b) {
        const VkrBakeSceneLight &light_b = scene.lights[plan.lights[b]];
        if (!light_a.casts_shadow || !light_b.casts_shadow ||
            !share[(size_t)a * lamp_count + b]) {
          continue;
        }
        const VkrLightmapStationaryLamp &lamp_a = plan.lamps[a];
        const VkrLightmapStationaryLamp &lamp_b = plan.lamps[b];
        const float32_t distance =
            vec3_length(vec3_sub(lamp_a.position, lamp_b.position));
        if (distance < lamp_a.range + lamp_b.range &&
            lamps_see_each_other(bvh, lamp_a.position, lamp_b.position)) {
          conflicts[a].push_back(b);
          conflicts[b].push_back(a);
        }
      }
    }

    /* Largest weight first; ties keep scene order. */
    std::vector<uint32_t> order;
    for (uint32_t l = 0u; l < lamp_count; ++l) {
      if (scene.lights[plan.lights[l]].casts_shadow) {
        order.push_back(l);
      }
    }
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
      return plan.lamps[a].weight > plan.lamps[b].weight;
    });
    for (uint32_t l : order) {
      float32_t cost[VKR_LIGHTMAP_STATIONARY_CHANNELS] = {};
      bool held[VKR_LIGHTMAP_STATIONARY_CHANNELS] = {};
      for (uint32_t other : conflicts[l]) {
        const uint32_t channel = plan.lamps[other].channel;
        if (channel >= VKR_LIGHTMAP_STATIONARY_CHANNELS) {
          continue;
        }
        const Vec3 offset =
            vec3_sub(plan.lamps[l].position, plan.lamps[other].position);
        held[channel] = true;
        cost[channel] +=
            std::min(plan.lamps[l].weight, plan.lamps[other].weight) /
            std::max(vec3_dot(offset, offset), 1.0f);
      }
      uint32_t channel = VKR_LIGHTMAP_STATIONARY_CHANNELS;
      for (uint32_t c = 0u; c < VKR_LIGHTMAP_STATIONARY_CHANNELS; ++c) {
        if (!held[c]) {
          channel = c;
          break;
        }
      }
      if (channel == VKR_LIGHTMAP_STATIONARY_CHANNELS) {
        channel = 0u;
        for (uint32_t c = 1u; c < VKR_LIGHTMAP_STATIONARY_CHANNELS; ++c) {
          if (cost[c] < cost[channel]) {
            channel = c;
          }
        }
        ++plan.shared_channel_lamps;
      }
      plan.lamps[l].channel = channel;
    }
    *out_plan = std::move(plan);
    return true;
  } catch (const std::bad_alloc &) {
    std::fprintf(stderr, "Planning stationary lights ran out of memory\n");
    return false;
  }
}

bool vkr_bake_stationary_choose_candidates(
    VkrBakeStationaryPlan *plan,
    const std::vector<VkrBakeLightmapTexel> &texels, uint32_t threads) {
  if (plan->lamps.empty() || texels.empty()) {
    return true;
  }
  try {
    /* Texels each reaching lamp owns, per worker, in `reach` order. */
    const size_t slots = plan->reach.size();
    const uint32_t workers = std::max(
        1u,
        std::min<uint32_t>(threads, (uint32_t)(texels.size() / 4096u + 1u)));
    std::vector<std::vector<uint32_t>> owned(workers,
                                             std::vector<uint32_t>(slots, 0u));
    std::vector<uint8_t> touched(plan->reach_by_source.size(), 0u);
    auto count = [&](uint32_t worker) {
      std::vector<uint32_t> &counts = owned[worker];
      const size_t begin = texels.size() * worker / workers;
      const size_t end = texels.size() * (worker + 1u) / workers;
      for (size_t t = begin; t < end; ++t) {
        const VkrBakeLightmapTexel &texel = texels[t];
        const uint32_t source = texel.source_instance_index;
        if (source >= plan->reach_by_source.size()) {
          continue;
        }
        touched[source] = 1u;
        const VkrLightmapStationaryRange reach = plan->reach_by_source[source];
        uint32_t owner[VKR_LIGHTMAP_STATIONARY_CHANNELS];
        float32_t best[VKR_LIGHTMAP_STATIONARY_CHANNELS] = {};
        for (uint32_t c = 0u; c < VKR_LIGHTMAP_STATIONARY_CHANNELS; ++c) {
          owner[c] = UINT32_MAX;
        }
        for (uint32_t i = 0u; i < reach.count; ++i) {
          const VkrLightmapStationaryLamp &lamp =
              plan->lamps[plan->reach[reach.first + i]];
          const float32_t metric =
              vkr_lightmap_stationary_metric(&lamp, texel.position);
          if (!(metric > 0.0f)) {
            continue;
          }
          if (lamp.channel >= VKR_LIGHTMAP_STATIONARY_CHANNELS) {
            ++counts[reach.first + i];
          } else if (metric > best[lamp.channel]) {
            best[lamp.channel] = metric;
            owner[lamp.channel] = reach.first + i;
          }
        }
        for (uint32_t c = 0u; c < VKR_LIGHTMAP_STATIONARY_CHANNELS; ++c) {
          if (owner[c] != UINT32_MAX) {
            ++counts[owner[c]];
          }
        }
      }
    };
    std::vector<std::thread> pool;
    for (uint32_t w = 1u; w < workers; ++w) {
      pool.emplace_back(count, w);
    }
    count(0u);
    for (std::thread &worker : pool) {
      worker.join();
    }

    /* The most-owning lamps of each touched instance, in lamp order. */
    std::vector<std::pair<uint32_t, uint16_t>> ranked;
    for (uint32_t s = 0u; s < touched.size(); ++s) {
      if (!touched[s]) {
        continue;
      }
      const VkrLightmapStationaryRange reach = plan->reach_by_source[s];
      ranked.clear();
      for (uint32_t i = 0u; i < reach.count; ++i) {
        uint32_t total = 0u;
        for (uint32_t w = 0u; w < workers; ++w) {
          total += owned[w][reach.first + i];
        }
        if (total != 0u) {
          ranked.emplace_back(total, plan->reach[reach.first + i]);
        }
      }
      std::sort(ranked.begin(), ranked.end(),
                [](const std::pair<uint32_t, uint16_t> &a,
                   const std::pair<uint32_t, uint16_t> &b) {
                  return a.first != b.first ? a.first > b.first
                                            : a.second < b.second;
                });
      if (ranked.size() > VKR_LIGHTMAP_SET_MAX_CANDIDATES) {
        ranked.resize(VKR_LIGHTMAP_SET_MAX_CANDIDATES);
        ++plan->capped_instances;
      }
      std::sort(ranked.begin(), ranked.end(),
                [](const std::pair<uint32_t, uint16_t> &a,
                   const std::pair<uint32_t, uint16_t> &b) {
                  return a.second < b.second;
                });
      const uint32_t first = plan->block_by_source[s];
      for (size_t i = 0u; i < ranked.size(); ++i) {
        plan->candidates[first + i] = ranked[i].second;
      }
      plan->ranges_by_source[s] = {first, (uint32_t)ranked.size()};
    }
    return true;
  } catch (const std::bad_alloc &) {
    std::fprintf(stderr, "Choosing stationary candidates ran out of memory\n");
    return false;
  } catch (const std::system_error &) {
    std::fprintf(stderr, "Choosing stationary candidates could not start "
                         "its workers\n");
    return false;
  }
}
