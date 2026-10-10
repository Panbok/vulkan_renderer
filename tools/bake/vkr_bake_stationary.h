#pragma once

#include <cstdint>
#include <vector>

#include "assets/vkr_lightmap_set.h"
#include "vkr_bake_bvh.h"
#include "vkr_bake_scene.h"

/*
 * Stationary lamps of a lightmap bake (ADR-107). A stationary point or spot
 * lamp bakes its bounce into its group's layer; its direct light stays at
 * runtime, shadowed by a runtime shadow map near the camera and elsewhere by
 * the set's shadow mask, whose four channels the lamps share.
 *
 * Two lamps conflict when their ranges overlap, both reach a common
 * lightmapped instance and nothing blocks the segment between them: they
 * light one room. Lamps take channels largest weight first, each the first
 * channel no conflicting lamp holds, else the channel whose holders cost
 * least (weights over squared distance). Where same-channel lamps still meet,
 * the ownership metric (vkr_lightmap_stationary_metric) gives each point to
 * one of them and the other bakes there as a static lamp.
 *
 * The lamps whose range reaches an instance's world bounds may light it.
 * Its candidates are those of them that own a channel, or have none and
 * light, at one of its texels at least, ranked among all of them by the
 * metric; past VKR_LIGHTMAP_SET_MAX_CANDIDATES, those owning the most
 * texels. A lamp that reaches an instance without being one of its
 * candidates bakes as a static lamp on it. Ranking among the candidates
 * alone gives every texel the owners the full ranking gives, unless the cap
 * cut one; the bake and the runtime rank among the candidates either way.
 */
struct VkrBakeStationaryPlan {
  /* The set's lamp records, in scene light order. */
  std::vector<VkrLightmapStationaryLamp> lamps;
  /* Each record's scene light index. */
  std::vector<uint32_t> lights;
  /* Each record's index per scene light, UINT32_MAX for other lights. */
  std::vector<uint32_t> lamp_by_light;
  /* The lamps that reach each instance, by source instance index: ranges
     into `reach`, ascending lamp indices. */
  std::vector<VkrLightmapStationaryRange> reach_by_source;
  std::vector<uint16_t> reach;
  /* Candidate ranges by source instance index, into `candidates`, which
     holds a block of min(reach count, VKR_LIGHTMAP_SET_MAX_CANDIDATES)
     entries per instance at `block_by_source`; a range covers the start of
     its block. Empty until vkr_bake_stationary_choose_candidates runs on the
     instance's page; unused entries are zero. */
  std::vector<VkrLightmapStationaryRange> ranges_by_source;
  std::vector<uint32_t> block_by_source;
  std::vector<uint16_t> candidates;
  /* Lamps that hold a channel with a conflicting lamp, and instances whose
     candidates were cut at VKR_LIGHTMAP_SET_MAX_CANDIDATES. */
  uint32_t shared_channel_lamps = 0u;
  uint32_t capped_instances = 0u;
};

/*
 * Disables each enabled stationary light that a set cannot key: one without
 * a document id, a positive range or a point or spot kind. The runtime then
 * finds no record and lights it as a dynamic light, which the bake also
 * leaves out. Returns how many it disabled; each is reported on stderr.
 */
uint32_t vkr_bake_stationary_disable_unkeyed(VkrBakeScene *scene);

/*
 * Plans the enabled stationary lights of `scene` over the lightmapped
 * instances [0, source_instance_count): records, reach, channels and empty
 * candidate blocks. `bvh` covers the scene's triangles. A scene without
 * stationary lights plans no lamps and no candidates. False on allocation
 * failure or more than VKR_LIGHTMAP_SET_MAX_STATIONARY lamps, with the
 * reason on stderr.
 */
bool vkr_bake_plan_stationary(const VkrBakeScene &scene, const VkrBakeBvh &bvh,
                              uint32_t source_instance_count,
                              VkrBakeStationaryPlan *out_plan);

/*
 * Chooses the candidates of the instances whose texels a page holds, from
 * the owners at those texels' centers, on `threads` workers. Run once per
 * page before its gather. False on allocation failure.
 */
bool vkr_bake_stationary_choose_candidates(
    VkrBakeStationaryPlan *plan,
    const std::vector<VkrBakeLightmapTexel> &texels, uint32_t threads);
