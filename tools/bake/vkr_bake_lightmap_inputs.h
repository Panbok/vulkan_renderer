#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "vkr_bake_lightmap.h"
#include "vkr_bake_scene.h"

extern "C" {
#include "assets/vkr_lightmap_set.h"
}

/*
 * Inputs of a lightmap bake and the incremental plan they give
 * (ADR-088, owner decision 2026-10-23: incremental rebakes by default).
 *
 * A bake describes what it read as an input index written beside its set:
 * one digest of everything that is the same wherever it lies (the bake's
 * settings, the atmosphere, the sky, the sun keys, the directional lights,
 * the layer table and the subsurface profiles), and per source instance and
 * per lamp what an edit changes. A later bake compares its own inputs with
 * the previous set's index. Any global difference bakes the whole set.
 * Otherwise each layer relights only the lightmapped instances an edit can
 * reach (vkr_bake_lightmap_plan_incremental) and copies every other
 * instance's encoded blocks from the previous set, wherever the packer
 * places its rectangle now. Each texel's samples follow its place in its
 * instance (vkr_bake_lightmap_texel_seed) and its cleanup reads its own
 * instance alone, so a relit instance matches a full bake of the same
 * inputs; a copied one keeps what the light an edit moved beyond the plan's
 * reach did to it, the incremental bake's only error.
 */

using VkrBakeDigest = std::array<uint8_t, 32>;

/*
 * Floating-point inputs compare within a tolerance, not by digest: an editor
 * save rewrites a rotation it derives from angles, so a brush or prop that
 * did not move comes back a few ulps off and its world-space corners with it
 * (73 of 6,384 Level Design Test instances after one save, 2026-10-23). A
 * value matches when it differs by at most kVkrBakeInputTolerance plus
 * kVkrBakeInputRelativeTolerance of its magnitude: 0.1 mm at the origin,
 * about 1.3 mm 600 m away, far below a texel and far above float drift.
 */
constexpr float32_t kVkrBakeInputTolerance = 1.0e-4f;
constexpr float32_t kVkrBakeInputRelativeTolerance = 2.0e-6f;

/* How many of its ranges from a changed lamp the lamp group's gathered
   light relights: the lamp's light first bounces within its range, and a
   receiver farther than two more ranges from that sphere gets at most the
   reflected share of the lamp's change spread over that distance. With
   one range plus the reach, a doubled lamp on a 48 m street left a 3.3%
   luminance-weighted error in the lamp group plane (16% for one wall,
   2026-10-23). */
constexpr float32_t kVkrBakeLampBounceRanges = 3.0f;

/* Area-weighted moments of an instance's world-space triangles: area,
   centroid, covariance, and the means of the corners' normals, texture
   UVs and their squares, lightmap UVs and their squares, colors and
   tangents. A move, rotation, reshape or remap changes some of them. */
constexpr uint32_t kVkrBakeInstanceMoments = 28u;

/* One source instance: its key, the digest of what it is made of exactly
   (its triangle count, each triangle's flags and material, and the
   materials with their textures' content), the moments of its geometry,
   its world bounds, and for a lightmapped instance the size of its
   rectangles on the set's pages and its lamp-direct pages (zero when it
   has none). */
struct VkrBakeLightmapInputInstance {
  VkrBakeSceneInstance key;
  VkrBakeDigest digest = {};
  float32_t moments[kVkrBakeInstanceMoments] = {};
  Vec3 bounds_min = {};
  Vec3 bounds_max = {};
  uint32_t width = 0u;
  uint32_t height = 0u;
  uint32_t direct_width = 0u;
  uint32_t direct_height = 0u;
};

/* Light fields compared within the tolerance: position, direction, right,
   up, color, intensity, range, attenuation, cones, rectangle size,
   radiance and emitter radius. */
constexpr uint32_t kVkrBakeLightFields = 26u;

/* One lamp (a point, spot or rectangle light; directional lights are
   global): the digest of its exact fields (kind, group, shadow and enabled
   flags), its other fields, and where it lights directly, the sphere of
   its range about its position, unbounded for a rectangle or a lamp
   without a range. */
struct VkrBakeLightmapInputLight {
  VkrBakeDigest digest = {};
  float32_t fields[kVkrBakeLightFields] = {};
  Vec3 position = {};
  float32_t range = 0.0f;
  float32_t source_radius = 0.0f;
  bool unbounded = false;
  bool lamp = false;
  char group[VKR_LIGHT_GROUP_NAME_BYTES] = {};
};

struct VkrBakeLightmapInputs {
  /* Of the settings, page size, atmosphere, environment, subsurface
     profiles, sun keys, directional lights and layer table, their floats
     rounded to 1e-5. */
  VkrBakeDigest globals = {};
  /* By source instance index. */
  std::vector<VkrBakeLightmapInputInstance> instances;
  /* By scene light index; directional lights are in `globals`. */
  std::vector<VkrBakeLightmapInputLight> lights;
};

/* Whether two instances' geometry matches within the tolerance. */
bool vkr_bake_lightmap_instance_same(const VkrBakeLightmapInputInstance &a,
                                     const VkrBakeLightmapInputInstance &b);

/* Whether two lamps match within the tolerance. */
bool vkr_bake_lightmap_light_same(const VkrBakeLightmapInputLight &a,
                                  const VkrBakeLightmapInputLight &b);

/* Whether `current` matches `previous` everywhere: equal globals, the same
   instances by key, each matching with its rectangle sizes, and the same
   lamps; a bake of `current` would publish the previous set again. */
bool vkr_bake_lightmap_inputs_match(const VkrBakeLightmapInputs &previous,
                                    const VkrBakeLightmapInputs &current);

/*
 * Describes `scene`'s inputs. `settings` holds every bake option that
 * changes the set, in a fixed order; `layers` the set's layer table;
 * `layout` and `direct_layout` the packed rectangles (`direct_layout` may
 * have no pages). Hashes the triangles on `threads` workers.
 */
bool vkr_bake_lightmap_inputs_build(const VkrBakeScene &scene,
                                    const std::string &settings,
                                    const std::vector<VkrLightLayer> &layers,
                                    const std::vector<Vec3> &sun_keys,
                                    const VkrBakeLightmapLayout &layout,
                                    const VkrBakeLightmapLayout &direct_layout,
                                    uint32_t threads,
                                    VkrBakeLightmapInputs *out_inputs);

/*
 * The input index file (VKLI v2): the inputs and the payload checksum and size
 * of the set they describe, so a later bake checks that the set beside the
 * index is the one it was written with. Little-endian, CRC-checked.
 */
bool vkr_bake_lightmap_index_write(const char *path,
                                   const VkrBakeLightmapInputs &inputs,
                                   uint32_t set_payload_crc,
                                   uint64_t set_bytes);

bool vkr_bake_lightmap_index_read(const char *path,
                                  VkrBakeLightmapInputs *out_inputs,
                                  uint32_t *out_set_payload_crc,
                                  uint64_t *out_set_bytes);

/* Which lightmapped instances each layer relights. */
struct VkrBakeLightmapIncrementalPlan {
  /* By layer, then by source instance index: nonzero when the layer's
     irradiance plane (and on the desktop class its direction plane)
     relights the instance. */
  std::vector<std::vector<uint8_t>> layer_dirty;
  /* By layer: nonzero when the layer's lamp-direct plane relights it. */
  std::vector<std::vector<uint8_t>> direct_dirty;
  /* Instances with no rectangle in the previous set or a changed key,
     digest, bounds or rectangle size, relit everywhere. */
  uint32_t changed_instances = 0u;
  uint32_t changed_lights = 0u;
  /* Old and new bounds of the instances, lightmapped or not, that changed,
     appeared or went. */
  uint32_t changed_boxes = 0u;
};

/* What the plan needs of one layer: its kind, its lights (scene light
   indices), whether it adds its lights' direct term at the texel, whether
   it has a lamp-direct plane, and for a sun key the unit direction toward
   its key light. */
struct VkrBakeLightmapPlanLayer {
  uint32_t kind = 0u;
  std::vector<uint32_t> lights;
  bool texel_direct = false;
  bool lamp_direct = false;
  Vec3 key_light_direction = {};
};

/*
 * Plans an incremental bake of `current` over `previous`, whose globals are
 * equal. `previous_has_rect[i]` tells whether current source instance i had
 * a rectangle (and, for lamp-direct planes, `previous_has_direct_rect[i]` a
 * lamp-direct one) in the previous set. `reach` is the distance in world
 * units within which a changed instance or lamp changes indirect light.
 *
 * An instance relights everywhere when it changed or the previous set lacks
 * its rectangle. Otherwise, for each changed instance's old and new bounds
 * and each changed lamp's old and new sphere:
 * - a sun key relights the instances within `reach` of a changed box swept
 *   away from its key light across the scene, which its shadow or its
 *   bounce can reach;
 * - a lamp group's irradiance relights the instances within
 *   kVkrBakeLampBounceRanges ranges plus `reach` of a changed lamp of its
 *   group, within `reach` of a changed box,
 *   and within a lamp's range plus `reach` when the changed box lies within
 *   that lamp's range; with its lights' direct term it also follows the
 *   lamp-direct rule;
 * - a lamp-direct plane relights the instances a changed lamp of its group
 *   reaches, and those a lamp of its group reaches when a changed box meets
 *   the box spanning the lamp's emitter and the instance.
 */
bool vkr_bake_lightmap_plan_incremental(
    const VkrBakeLightmapInputs &previous, const VkrBakeLightmapInputs &current,
    const std::vector<VkrBakeLightmapPlanLayer> &layers,
    const std::vector<uint8_t> &previous_has_rect,
    const std::vector<uint8_t> &previous_has_direct_rect, float32_t reach,
    VkrBakeLightmapIncrementalPlan *out_plan);

/* Which receivers each layer relights: by layer, then by receiver,
   nonzero when its gathered light, or its lamp-direct light, changes. */
struct VkrBakeReceiverPlan {
  std::vector<std::vector<uint8_t>> gathered_dirty;
  std::vector<std::vector<uint8_t>> direct_dirty;
  uint32_t changed_boxes = 0u;
  uint32_t changed_lights = 0u;
};

/*
 * Plans which receivers, boxes such as the cells a diffuse volume's probes
 * may move within, an incremental bake of `current` over `previous` (equal
 * globals) relights per layer, by the rules
 * vkr_bake_lightmap_plan_incremental applies to an instance that did not
 * change: a layer's gathered light by its sun-key or lamp-group rule, its
 * lamp-direct light, for a layer with `lamp_direct`, by the lamp-direct
 * rule. A lamp-group layer without lights marks the receivers within
 * `reach` of a changed box. A receiver is never itself a changed instance;
 * the caller relights receivers whose own inputs changed.
 */
bool vkr_bake_plan_receivers(
    const VkrBakeLightmapInputs &previous, const VkrBakeLightmapInputs &current,
    const std::vector<VkrBakeLightmapPlanLayer> &layers,
    const std::vector<Vec3> &receiver_min,
    const std::vector<Vec3> &receiver_max, float32_t reach,
    VkrBakeReceiverPlan *out_plan);
