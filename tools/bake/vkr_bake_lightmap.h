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
  /* World offsets of one texel step along the page's x and y on the
     texel's triangle: the texel's footprint on the surface. */
  Vec3 step_x = {};
  Vec3 step_y = {};
  uint32_t triangle_index = 0u;
  /* The lightmapped instance whose rectangle holds the texel. */
  uint32_t source_instance_index = 0u;
};

/*
 * Sizes each instance's rectangle for texels_per_world_unit: its atlas times
 * the instance's uniform world scale times the ratio of that density to the
 * atlas's local density, in whole 4x4 blocks, and packs the rectangles
 * tallest first onto page_size pages, each at the lowest, then leftmost,
 * place on the first page whose skyline holds it. A rectangle larger than a
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
 * floats in row-major order, and fills the texels of each rectangle that no
 * triangle covers. Alpha is the texel's ambient occlusion from `occlusion`
 * (one visibility in [0, 1] per texel of `texels`), or one without it. Up to
 * `dilation_passes` rings around the covered texels take the mean of their
 * filled 8-neighbors in the same rectangle, so bilinear filtering and 4x4
 * blocks at chart edges see chart values instead of black; the rest of the
 * rectangle takes the mean of its covered texels. Texels outside every
 * rectangle are zero with alpha one. `values` holds one finite, non-negative
 * value per texel of `texels`.
 */
bool vkr_bake_lightmap_compose_page(
    const VkrBakeLightmapLayout &layout, uint32_t page,
    const std::vector<VkrBakeLightmapTexel> &texels,
    const std::vector<Vec3> &values, const std::vector<float32_t> *occlusion,
    uint32_t dilation_passes, std::vector<float32_t> *out_rgba);

/*
 * Writes a lamp layer's direction page (ADR-104) into
 * `out_rgba` as vkr_bake_lightmap_compose_page lays a page out: each texel's
 * world-space mean incident direction d from `directions` becomes
 * rgb = 0.5 * normalize(d) + 0.5 and alpha = min(|d|, 1), its
 * directionality. A texel with no direction (|d| near zero or not finite) is
 * the neutral (0.5, 0.5, 0.5, 0). Uncovered rectangle texels dilate and fill
 * as in compose_page; texels outside every rectangle, and rectangles no
 * texel covers, are neutral.
 */
bool vkr_bake_lightmap_compose_direction_page(
    const VkrBakeLightmapLayout &layout, uint32_t page,
    const std::vector<VkrBakeLightmapTexel> &texels,
    const std::vector<Vec3> &directions, uint32_t dilation_passes,
    std::vector<float32_t> *out_rgba);

/*
 * Same-surface neighbors of a page's texels: for texel i, the entries
 * [first[i], first[i + 1]) of `indices` name the covered texels among its
 * eight around it on the page whose positions lie within `max_distance`
 * world units of its own and whose normals lie within about 37 degrees of
 * its own, so a chart seam or an unrelated chart next to it in the atlas is
 * not a neighbor.
 */
struct VkrBakeLightmapNeighbors {
  std::vector<uint32_t> first;
  std::vector<uint32_t> indices;
};

bool vkr_bake_lightmap_neighbors(
    uint32_t page_size, const std::vector<VkrBakeLightmapTexel> &texels,
    float32_t max_distance, VkrBakeLightmapNeighbors *out_neighbors);

/* Texels a layer's outlier rejection flagged, and their share of the
   layer's summed luminance before rejection. */
struct VkrBakeLightmapOutliers {
  uint64_t texels = 0u;
  float64_t energy_fraction = 0.0;
};

/*
 * Rejects firefly texels: a texel with at least `min_neighbors` same-surface
 * neighbors whose luminance exceeds `ratio` times the brightest of them is a
 * rare high-energy path the sample count cannot average out, and takes the
 * mean of its neighbors when `replace`. Decisions read the values before any
 * replacement, so the result does not depend on texel order.
 */
VkrBakeLightmapOutliers
vkr_bake_lightmap_reject_outliers(const VkrBakeLightmapNeighbors &neighbors,
                                  float32_t ratio, uint32_t min_neighbors,
                                  bool replace, std::vector<Vec3> *values);

/*
 * Smooths a layer's sampling noise in `passes` passes: each texel takes the
 * weighted mean of itself (weight 4), its same-surface edge neighbors
 * (weight 2) and corner neighbors (weight 1), so light does not bleed across
 * chart seams or onto another surface. One pass blurs irradiance by about a
 * texel. Each pass reads the previous pass's values.
 */
bool vkr_bake_lightmap_smooth(const VkrBakeLightmapNeighbors &neighbors,
                              const std::vector<VkrBakeLightmapTexel> &texels,
                              uint32_t passes, std::vector<Vec3> *values);

/*
 * How a page's invalid texels, such as floor texels buried under a wall, take
 * their values from valid texels of the same surface: in rings, each texel
 * of `texels` in order takes the mean of its sources, its same-surface
 * neighbors that are valid or filled in an earlier ring. Invalid texels no
 * ring reaches (a surface buried whole) are counted in `unfilled` and keep
 * their values.
 */
struct VkrBakeLightmapFill {
  std::vector<uint32_t> texels;
  /* Sources of texels[k] are sources[first[k], first[k + 1]). */
  std::vector<uint32_t> first;
  std::vector<uint32_t> sources;
  uint64_t unfilled = 0u;
};

/* Plans the fill of the texels whose `valid` entry is zero, one entry per
   texel of the neighbors' page. */
bool vkr_bake_lightmap_plan_fill(const VkrBakeLightmapNeighbors &neighbors,
                                 const std::vector<uint8_t> &valid,
                                 VkrBakeLightmapFill *out_fill);

void vkr_bake_lightmap_apply_fill(const VkrBakeLightmapFill &fill,
                                  std::vector<Vec3> *values);

void vkr_bake_lightmap_apply_fill(const VkrBakeLightmapFill &fill,
                                  std::vector<float32_t> *values);

/*
 * Edge-aware a-trous denoising of a layer's indirect light (ADR-088). Each
 * of `iterations` passes takes a 5x5 B3-spline tap pattern on the page at a
 * step that doubles per pass (1, 2, 4, 8, ...). A tap counts only when it is
 * a covered texel of the same instance whose normal lies within about 37
 * degrees of the center's, whose world position lies within 1.5 times the
 * distance its page offset implies at the center's texel spacing plus half a
 * texel (so a chart seam or an unrelated chart next to it in the atlas is not
 * mixed in), and that is valid when `valid` is given. Taps are weighted by
 * the normal cosine to the 16th power, by their distance from the center's
 * tangent plane, and, when `variance` is given, by their luminance
 * difference over `luminance_sigma` standard deviations of the center's
 * 3x3-filtered variance, so a real gradient survives while noise averages
 * out. Variance is propagated through the passes. `values` and `variance`
 * hold one entry per texel.
 */
struct VkrBakeLightmapDenoiseSettings {
  uint32_t iterations = 4u;
  /* World density the page was packed at, for texels without a same-surface
     edge neighbor to measure their spacing from. */
  float32_t texels_per_unit = 8.0f;
  float32_t luminance_sigma = 4.0f;
  /* Worker threads; zero uses the hardware concurrency. The result does not
     depend on it. */
  uint32_t threads = 0u;
};

bool vkr_bake_lightmap_denoise(uint32_t page_size,
                               const std::vector<VkrBakeLightmapTexel> &texels,
                               const std::vector<uint8_t> *valid,
                               const std::vector<float32_t> *variance,
                               const VkrBakeLightmapDenoiseSettings &settings,
                               std::vector<Vec3> *values);

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

/* As vkr_bake_lightmap_encode_astc_hdr in the linear LDR profile: every
   channel in [0, 1], for direction pages. */
bool vkr_bake_lightmap_encode_astc_ldr(const std::vector<float32_t> &rgba,
                                       uint32_t size, float32_t effort,
                                       uint32_t threads,
                                       std::vector<uint8_t> *out_blocks);

bool vkr_bake_lightmap_decode_astc_ldr(const std::vector<uint8_t> &blocks,
                                       uint32_t size,
                                       std::vector<float32_t> *out_rgba);
