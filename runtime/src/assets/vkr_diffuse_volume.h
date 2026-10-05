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
#include "vkr_ibl_math.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VKR_DIFFUSE_VOLUME_MAGIC 0x4c4f5644u /* "DVOL" in little-endian. */
/* DVOL v2 (ADR-054, ADR-088): a 112-byte header, the layer table of
   VKR_LIGHT_LAYER_RECORD_BYTES records, one record per probe (its region,
   then one SH per layer) and one region per cell. Version 1 held one SH per
   probe of all light at once and is refused. */
#define VKR_DIFFUSE_VOLUME_VERSION 2u
#define VKR_DIFFUSE_VOLUME_ENDIAN_TAG 0x01020304u
#define VKR_DIFFUSE_VOLUME_HEADER_BYTES 112u
#define VKR_DIFFUSE_VOLUME_SH_BYTES 112u
#define VKR_DIFFUSE_VOLUME_CELL_BYTES 4u
#define VKR_DIFFUSE_VOLUME_MAX_PROBES 256u
#define VKR_DIFFUSE_VOLUME_MAX_LAYERS 16u
#define VKR_DIFFUSE_VOLUME_MAX_REGION_ID 0x00ffffffu

/**
 * Decoded diffuse-volume source data. Probe positions follow from the origin
 * and spacing. Each probe holds one SH per light layer: probe p's layer l is
 * `probe_sh[p * layer_count + l]`, and the runtime weights the layers by the
 * current sun and light groups. Arrays are owned by the Arena supplied to
 * `vkr_diffuse_volume_decode`; the caller releases them by resetting or
 * destroying that Arena.
 */
typedef struct VkrDiffuseVolume {
  Vec3 origin;
  Vec3 spacing;
  uint32_t dimensions[3];
  /** Each probe's room region; zero is an invalid probe. */
  uint32_t *probe_region_ids;
  VkrShL2Packed *probe_sh;
  uint32_t probe_count;
  /** A valid layer table (vkr_light_layers_valid) of at least one layer. */
  VkrLightLayer *layers;
  uint32_t layer_count;
  /** Zero is an invalid cell. A nonzero value matches all eight cell corners.
   */
  uint32_t *cell_region_ids;
  uint32_t cell_count;
} VkrDiffuseVolume;

/**
 * Serializes a validated volume into caller-Arena bytes. The portable DVOL v2
 * file writes every scalar explicitly in little-endian order; it never writes
 * native structs.
 */
bool8_t vkr_diffuse_volume_encode(const VkrDiffuseVolume *volume, Arena *arena,
                                  const uint8_t **out_bytes,
                                  uint64_t *out_size);

/**
 * Validates and decodes a complete DVOL v2 byte sequence. It rejects malformed
 * layout, checksum failures, an invalid layer table, non-finite values,
 * invalid regions, and any inconsistent interpolation cell before publishing
 * output storage.
 */
bool8_t vkr_diffuse_volume_decode(const uint8_t *bytes, uint64_t size,
                                  Arena *arena, VkrDiffuseVolume *out_volume);

#ifdef __cplusplus
}
#endif
