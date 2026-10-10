#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
#include "memory/arena.h"
#ifdef __cplusplus
}
#endif

#include "assets/vkr_light_layers.h"
#include "math/vec.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Baked lightmap set (ADR-087, ADR-088, ADR-104): the
 * scene's lightmap pages in one or more planes per light layer, the layers'
 * meanings and each lightmapped instance's page rectangle.
 *
 * A plane is one layer's irradiance or dominant direction in one encoding.
 * A bake gives every layer an ASTC 4x4 HDR irradiance plane, which the tiled
 * pipeline samples, and each lamp-group layer planes in encodings desktop
 * GPUs sample (RGB9E5 or BC6H irradiance, RGBA8 or BC7 direction).
 * Packaging may leave out the planes a platform does not sample; a runtime
 * publishes only the planes its pipeline class samples.
 *
 * VKLM v6 writes every scalar explicitly in little-endian order: a fixed
 * header, the layer table (vkr_light_layers.h records), the plane table, the
 * instance table, the lamp-direct rectangles when the set has lamp-direct
 * planes, padding to VKR_LIGHTMAP_SET_PAYLOAD_ALIGNMENT, then for each page
 * one image per plane other than lamp direct in table order, then for each
 * lamp-direct page one image per lamp-direct plane in table order. Producers
 * stream the payload and write the prefix last, so a set never needs to be
 * held in memory whole. Version 6 marks brush and blockout charts laid
 * with their long side along u; version 5, the same file with the former
 * charts, and version 4, without lamp-direct planes, still decode, without
 * lighting brushes and shapes; version 3 (ASTC irradiance only) is refused.
 */

#define VKR_LIGHTMAP_SET_MAGIC 0x4d4c4b56u /* "VKLM" in little-endian. */
#define VKR_LIGHTMAP_SET_VERSION 6u
#define VKR_LIGHTMAP_SET_VERSION_V5 5u
#define VKR_LIGHTMAP_SET_VERSION_V4 4u
#define VKR_LIGHTMAP_SET_ENDIAN_TAG 0x01020304u
#define VKR_LIGHTMAP_SET_HEADER_BYTES 128u
#define VKR_LIGHTMAP_SET_PLANE_BYTES 16u
#define VKR_LIGHTMAP_SET_INSTANCE_BYTES 44u
#define VKR_LIGHTMAP_SET_DIRECT_RECT_BYTES 20u
#define VKR_LIGHTMAP_SET_PAYLOAD_ALIGNMENT 256u
#define VKR_LIGHTMAP_SET_BLOCK_BYTES 16u
#define VKR_LIGHTMAP_SET_MAX_PAGE_SIZE 8192u
#define VKR_LIGHTMAP_SET_MAX_PAGES 64u
#define VKR_LIGHTMAP_SET_MAX_LAYERS 32u
#define VKR_LIGHTMAP_SET_MAX_PLANES 128u

/* What a plane holds. Irradiance is RGB irradiance with ambient visibility
   in alpha where the encoding has alpha; direction is the luminance-weighted
   mean incident direction, encoded as 0.5 * d + 0.5 in RGB with its length,
   the directionality, in alpha. Lamp direct is a lamp group's direct
   irradiance at the lamp-direct pages' density (ADR-108); a lamp group with
   one holds only its bounce, sky and emission in its irradiance plane. */
typedef enum VkrLightmapPlaneKind {
  VKR_LIGHTMAP_PLANE_IRRADIANCE = 0,
  VKR_LIGHTMAP_PLANE_DIRECTION = 1,
  VKR_LIGHTMAP_PLANE_LAMP_DIRECT = 2,
} VkrLightmapPlaneKind;

/* Plane encodings. ASTC planes are the tiled pipeline's; the others are the
   desktop pipeline's. Block encodings store 16-byte 4x4 blocks, the others
   4 bytes per texel. */
typedef enum VkrLightmapPlaneFormat {
  VKR_LIGHTMAP_FORMAT_ASTC_4X4_HDR = 1,
  VKR_LIGHTMAP_FORMAT_RGB9E5 = 2,
  VKR_LIGHTMAP_FORMAT_BC6H = 3,
  VKR_LIGHTMAP_FORMAT_ASTC_4X4_LDR = 4,
  VKR_LIGHTMAP_FORMAT_RGBA8 = 5,
  VKR_LIGHTMAP_FORMAT_BC7 = 6,
} VkrLightmapPlaneFormat;

typedef struct VkrLightmapPlane {
  uint32_t layer;
  VkrLightmapPlaneKind kind;
  VkrLightmapPlaneFormat format;
} VkrLightmapPlane;

/*
 * The rectangle in texels of one instance of a scene entity: the entity's
 * document id (all zero for an entity without one), its index in the baked
 * entity array (the scene document's entities, then the project World's,
 * then editor-created ones), and the instance's source-node index in the
 * entity's cooked model (zero for a model without source nodes and for a
 * brush). The runtime matches an instance by document id when it has one and
 * by index otherwise. Its lightmap UVs span the rectangle. Rectangles lie on
 * whole 4x4 blocks.
 */
typedef struct VkrLightmapInstance {
  uint8_t document_id[16];
  uint32_t entity_index;
  uint32_t instance_index;
  uint32_t page;
  uint32_t x;
  uint32_t y;
  uint32_t width;
  uint32_t height;
} VkrLightmapInstance;

/* An instance's rectangle in texels on one lamp-direct page; all zero for an
   instance no lamp reaches. */
typedef struct VkrLightmapDirectRect {
  uint32_t page;
  uint32_t x;
  uint32_t y;
  uint32_t width;
  uint32_t height;
} VkrLightmapDirectRect;

/*
 * A set's tables and payload. Producers fill everything but `payload`.
 * Instances are sorted by entity, then instance, without duplicates.
 * Decoding allocates `layers` and `instances` from the caller's Arena and
 * points `payload` into the decoded bytes, which must outlive the set.
 */
typedef struct VkrLightmapSet {
  uint32_t page_size;
  uint32_t page_count;
  uint32_t layer_count;
  uint32_t plane_count;
  uint32_t instance_count;
  float32_t texels_per_unit;
  const VkrLightLayer *layers;
  /** Sorted by layer, then kind, then format, without duplicates. */
  const VkrLightmapPlane *planes;
  const VkrLightmapInstance *instances;
  /** Lamp-direct pages (ADR-108), of `page_size` texels at
      `direct_texels_per_unit`, and one rectangle per instance on them;
      zero pages and no rectangles without lamp-direct planes. */
  uint32_t direct_page_count;
  float32_t direct_texels_per_unit;
  const VkrLightmapDirectRect *direct_rects;
  /** Version 6 and later: brush and blockout charts lie with their long side
      along u (vkr_brush_lightmap_chart). An older set's rectangles hold the
      former charts, so brushes and shapes do not sample it. */
  bool8_t shape_charts_current;
  const uint8_t *payload;
} VkrLightmapSet;

/* Whether the desktop pipeline samples a plane encoding; the tiled pipeline
   samples the others. */
bool8_t vkr_lightmap_format_desktop(VkrLightmapPlaneFormat format);

/* Bytes of one page image of a plane encoding, zero for an unknown one. */
uint64_t vkr_lightmap_set_plane_bytes(VkrLightmapPlaneFormat format,
                                      uint32_t page_size);

/* Bytes of one page's images of every plane but lamp direct. */
uint64_t vkr_lightmap_set_page_stride(const VkrLightmapSet *set);

/* Bytes of one lamp-direct page's images of every lamp-direct plane. */
uint64_t vkr_lightmap_set_direct_page_stride(const VkrLightmapSet *set);

/* Offset in the payload of one page's image of one plane; `page` counts
   lamp-direct pages for a lamp-direct plane. */
uint64_t vkr_lightmap_set_plane_offset(const VkrLightmapSet *set, uint32_t page,
                                       uint32_t plane);

/* Index of the layer's plane of `kind` in the desktop encodings (`desktop`)
   or the tiled ones, or UINT32_MAX without one. */
uint32_t vkr_lightmap_set_find_plane(const VkrLightmapSet *set, uint32_t layer,
                                     VkrLightmapPlaneKind kind,
                                     bool8_t desktop);

/*
 * Validates a producer's set (everything but `payload`) and reports the
 * payload offset, which is also the prefix size, and the file size.
 */
bool8_t vkr_lightmap_set_layout(const VkrLightmapSet *set,
                                uint64_t *out_payload_offset,
                                uint64_t *out_file_size);

/*
 * Writes the header and tables of a validated set into `out_prefix`, which
 * holds exactly the payload offset bytes (padding zeroed). `payload_crc` is
 * the CRC-32 of the whole payload in file order.
 */
bool8_t vkr_lightmap_set_write_prefix(const VkrLightmapSet *set,
                                      uint32_t payload_crc, uint8_t *out_prefix,
                                      uint64_t prefix_size);

/*
 * Validates and decodes a complete VKLM v4 or v5 file: header, table and
 * payload checksums, sizes and offsets, layer meanings and names, the plane
 * table (known encodings of a kind they can hold, canonical order, desktop
 * planes only on lamp groups, lamp-direct planes only on lamp groups and
 * only with lamp-direct pages), and every rectangle, lamp-direct ones
 * included, lying on whole blocks within its page.
 */
bool8_t vkr_lightmap_set_decode(const uint8_t *bytes, uint64_t size,
                                Arena *arena, VkrLightmapSet *out_set);

#ifdef __cplusplus
}
#endif
