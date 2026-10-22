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

/* One source instance: its key, the digest of its world-space triangles
   (corners, normals, texture and lightmap UVs, colors, tangents, flags) and
   of their materials with their textures' content, its world bounds, and
   for a lightmapped instance the size of its rectangles on the set's pages
   and its lamp-direct pages (zero when it has none). */
struct VkrBakeLightmapInputInstance {
  VkrBakeSceneInstance key;
  VkrBakeDigest digest = {};
  Vec3 bounds_min = {};
  Vec3 bounds_max = {};
  uint32_t width = 0u;
  uint32_t height = 0u;
  uint32_t direct_width = 0u;
  uint32_t direct_height = 0u;
};

/* One lamp (a point, spot or rectangle light; directional lights are
   global): the digest of every field the bake reads, and where it lights
   directly, the sphere of its range about its position, unbounded for a
   rectangle or a lamp without a range. */
struct VkrBakeLightmapInputLight {
  VkrBakeDigest digest = {};
  Vec3 position = {};
  float32_t range = 0.0f;
  float32_t source_radius = 0.0f;
  bool unbounded = false;
  bool lamp = false;
  char group[VKR_LIGHT_GROUP_NAME_BYTES] = {};
};

struct VkrBakeLightmapInputs {
  VkrBakeDigest globals = {};
  /* By source instance index. */
  std::vector<VkrBakeLightmapInputInstance> instances;
  /* By scene light index; directional lights are in `globals`. */
  std::vector<VkrBakeLightmapInputLight> lights;
  /* Of everything above: equal digests bake equal sets. */
  VkrBakeDigest digest = {};
};

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

/* Lowercase hex of a digest. */
std::string vkr_bake_digest_hex(const VkrBakeDigest &digest);

/*
 * The input index file (VKLI): the inputs and the payload checksum and size
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
 * - a lamp group's irradiance relights the instances within the range plus
 *   `reach` of a changed lamp of its group, within `reach` of a changed box,
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
