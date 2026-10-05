#pragma once

#include <array>
#include <cstdint>
#include <vector>

extern "C" {
#include "math/mat.h"
#include "vkr_bake_geometry.h"
}

/*
 * Scene lightmap layout and texel generation (ADR-087). Each lightmapped
 * instance takes one rectangle on a square page; its source mesh's normalized
 * lightmap UVs span that rectangle. Pages are baked one at a time, so a page's
 * texel records are the only per-texel state alive during a bake.
 */

/* One lightmapped instance: its chart atlas in texels at texels_per_unit
   local units, its world transform, and its key: the entity's document id
   when it has one, its index in the baked entity array, and the instance's
   source-node index in the entity's cooked model (zero for a brush). */
struct VkrBakeLightmapInstance {
  uint32_t source_instance_index = 0u;
  std::array<uint8_t, 16> document_id = {};
  bool has_document_id = false;
  uint32_t entity_index = 0u;
  uint32_t source_node_index = 0u;
  Mat4 world = {};
  uint32_t atlas_width = 0u;
  uint32_t atlas_height = 0u;
  float32_t texels_per_unit = 0.0f;
};

/* An instance's rectangle in texels on one page. */
struct VkrBakeLightmapRect {
  uint32_t source_instance_index = 0u;
  uint32_t page = 0u;
  uint32_t x = 0u;
  uint32_t y = 0u;
  uint32_t width = 0u;
  uint32_t height = 0u;
};

struct VkrBakeLightmapLayout {
  uint32_t page_size = 0u;
  uint32_t page_count = 0u;
  std::vector<VkrBakeLightmapRect> rects;
  /* Rect index per source instance index; UINT32_MAX when unlit. */
  std::vector<uint32_t> rect_by_instance;
};

/* One texel center that lies inside a lightmapped triangle on a page. */
struct VkrBakeLightmapTexel {
  uint32_t x = 0u;
  uint32_t y = 0u;
  Vec3 position = {};
  /* Interpolated shading normal, unit length. */
  Vec3 normal = {};
  uint32_t triangle_index = 0u;
};

/*
 * Sizes each instance's rectangle for texels_per_world_unit: its atlas times
 * the instance's uniform world scale times the ratio of that density to the
 * atlas's local density, in whole 4x4 blocks, and shelf-packs the
 * rectangles tallest first onto page_size pages. A rectangle larger than a
 * page is scaled down to fit. Instances without an atlas get no rectangle.
 * page_size is a positive multiple of four.
 */
bool vkr_bake_lightmap_pack(
    const std::vector<VkrBakeLightmapInstance> &instances, uint32_t page_size,
    float32_t texels_per_world_unit, VkrBakeLightmapLayout *out_layout);

/*
 * Packs as vkr_bake_lightmap_pack does, but when every rectangle fits on one
 * page smaller than page_size without being scaled down, uses the smallest
 * such page from 256 texels upward in powers of two, so a small scene does not
 * store a mostly empty page.
 */
bool vkr_bake_lightmap_pack_fitted(
    const std::vector<VkrBakeLightmapInstance> &instances, uint32_t page_size,
    float32_t texels_per_world_unit, VkrBakeLightmapLayout *out_layout);

/*
 * Collects every texel center of `page` that lies inside a triangle of a
 * lightmapped instance, in row-major order. A texel claimed by one triangle
 * is not claimed again by a neighbor sharing its edge.
 */
bool vkr_bake_lightmap_rasterize_page(
    const VkrBakeTriangle *triangles, uint32_t triangle_count,
    const VkrBakeLightmapLayout &layout, uint32_t page,
    std::vector<VkrBakeLightmapTexel> *out_texels);

/*
 * Writes a page's texel values into `out_rgba`, page_size x page_size RGBA
 * floats in row-major order with alpha one, and fills the texels of each
 * rectangle that no triangle covers. Up to `dilation_passes` rings around the
 * covered texels take the mean of their filled 8-neighbors in the same
 * rectangle, so bilinear filtering and 4x4 blocks at chart edges see chart
 * values instead of black; the rest of the rectangle takes the mean of its
 * covered texels. Texels outside every rectangle are zero. `values` holds one
 * finite, non-negative value per texel of `texels`.
 */
bool vkr_bake_lightmap_compose_page(
    const VkrBakeLightmapLayout &layout, uint32_t page,
    const std::vector<VkrBakeLightmapTexel> &texels,
    const std::vector<Vec3> &values, uint32_t dilation_passes,
    std::vector<float32_t> *out_rgba);

/*
 * Encodes a composed page to ASTC 4x4 blocks in the HDR RGB, LDR alpha
 * profile at astcenc `effort` (0 to 100) on `threads` workers. Writes 16
 * bytes per 4x4 block in row-major block order. `size` is a multiple of four.
 */
bool vkr_bake_lightmap_encode_astc_hdr(const std::vector<float32_t> &rgba,
                                       uint32_t size, float32_t effort,
                                       uint32_t threads,
                                       std::vector<uint8_t> *out_blocks);

/* Decodes blocks written by vkr_bake_lightmap_encode_astc_hdr to RGBA floats,
   to measure the encoding error. */
bool vkr_bake_lightmap_decode_astc_hdr(const std::vector<uint8_t> &blocks,
                                       uint32_t size,
                                       std::vector<float32_t> *out_rgba);
