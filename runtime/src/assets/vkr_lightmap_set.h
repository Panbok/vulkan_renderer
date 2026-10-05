#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
#include "memory/arena.h"
#ifdef __cplusplus
}
#endif

#include "math/vec.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Baked lightmap set (ADR-087, docs/proposals/tiled-pipeline.md): every
 * layer of a scene's lightmap pages as ASTC 4x4 blocks (HDR RGB, LDR alpha),
 * the layers' meanings and each lightmapped instance's page rectangle.
 *
 * VKLM v3 writes every scalar explicitly in little-endian order: a fixed
 * header, the layer table, the instance table, padding to
 * VKR_LIGHTMAP_SET_PAYLOAD_ALIGNMENT, then one page image per page and layer,
 * page-major. Producers stream the payload and write the prefix last, so a
 * set never needs to be held in memory whole.
 */

#define VKR_LIGHTMAP_SET_MAGIC 0x4d4c4b56u /* "VKLM" in little-endian. */
#define VKR_LIGHTMAP_SET_VERSION 3u
#define VKR_LIGHTMAP_SET_ENDIAN_TAG 0x01020304u
#define VKR_LIGHTMAP_SET_HEADER_BYTES 128u
#define VKR_LIGHTMAP_SET_LAYER_BYTES 64u
#define VKR_LIGHTMAP_SET_INSTANCE_BYTES 44u
#define VKR_LIGHTMAP_SET_PAYLOAD_ALIGNMENT 256u
#define VKR_LIGHTMAP_SET_BLOCK_BYTES 16u
/* ASTC 4x4 blocks in the HDR RGB, LDR alpha profile. */
#define VKR_LIGHTMAP_SET_FORMAT_ASTC_4X4_HDR 1u
#define VKR_LIGHTMAP_SET_MAX_PAGE_SIZE 8192u
#define VKR_LIGHTMAP_SET_MAX_PAGES 64u
#define VKR_LIGHTMAP_SET_MAX_LAYERS 32u
/* Lamp-group layers of one set; a scene with more static groups fails to
   bake. */
#define VKR_LIGHTMAP_SET_MAX_LAMP_GROUPS 4u

/*
 * Light group names (ADR-088): at most VKR_LIGHTMAP_GROUP_NAME_BYTES - 1
 * letters, digits, '_' or '-'. Scene lights and lamp-group layers share the
 * rule; a scene light's empty name is the group VKR_LIGHTMAP_DEFAULT_GROUP.
 */
#define VKR_LIGHTMAP_GROUP_NAME_BYTES 32u
#define VKR_LIGHTMAP_DEFAULT_GROUP "default"

/* Whether `name` is a light group name, empty included. */
bool8_t vkr_lightmap_group_name_valid(const char *name, uint64_t length);

typedef enum VkrLightmapLayerKind {
  /* Sky light and sun bounce for one sun direction, without the sun's direct
     term, which the runtime adds. */
  VKR_LIGHTMAP_LAYER_SUN_KEY = 0,
  /* A lamp group's direct and bounce light and its surfaces' emission. */
  VKR_LIGHTMAP_LAYER_LAMP_GROUP = 1,
} VkrLightmapLayerKind;

typedef struct VkrLightmapLayer {
  uint32_t kind;
  /* Sun-key or lamp-group number; a kind's numbers are unique. */
  uint32_t index;
  /* Unit direction toward the sun for a sun key; zero for a lamp group. */
  Vec3 sun_direction;
  /* A lamp group's light group name, unique among the set's lamp groups;
     empty for a sun key. Null-terminated. */
  char name[VKR_LIGHTMAP_GROUP_NAME_BYTES];
} VkrLightmapLayer;

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
  uint32_t instance_count;
  float32_t texels_per_unit;
  const VkrLightmapLayer *layers;
  const VkrLightmapInstance *instances;
  const uint8_t *payload;
} VkrLightmapSet;

/* Bytes of one page image: (page_size / 4)^2 blocks. */
uint64_t vkr_lightmap_set_page_bytes(uint32_t page_size);

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
 * Validates and decodes a complete VKLM v3 file: header, table and payload
 * checksums, sizes and offsets, layer meanings and names, and every rectangle
 * lying on whole blocks within its page.
 */
bool8_t vkr_lightmap_set_decode(const uint8_t *bytes, uint64_t size,
                                Arena *arena, VkrLightmapSet *out_set);

#ifdef __cplusplus
}
#endif
