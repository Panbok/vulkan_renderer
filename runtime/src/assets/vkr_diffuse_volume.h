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
#include "vkr_ibl_math.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VKR_DIFFUSE_VOLUME_MAGIC 0x4c4f5644u /* "DVOL" in little-endian. */
#define VKR_DIFFUSE_VOLUME_VERSION 1u
#define VKR_DIFFUSE_VOLUME_ENDIAN_TAG 0x01020304u
#define VKR_DIFFUSE_VOLUME_HEADER_BYTES 112u
#define VKR_DIFFUSE_VOLUME_PROBE_BYTES 116u
#define VKR_DIFFUSE_VOLUME_CELL_BYTES 4u
#define VKR_DIFFUSE_VOLUME_MAX_PROBES 256u
#define VKR_DIFFUSE_VOLUME_MAX_REGION_ID 0x00ffffffu

/** A probe's position is derived from the containing volume's origin and
 * spacing. */
typedef struct VkrDiffuseVolumeProbe {
  uint32_t region_id;
  VkrShL2Packed sh;
} VkrDiffuseVolumeProbe;

/**
 * Decoded diffuse-volume source data. `probes` and `cell_region_ids` are owned
 * by the Arena supplied to `vkr_diffuse_volume_decode`; the caller releases
 * them by resetting or destroying that Arena.
 */
typedef struct VkrDiffuseVolume {
  Vec3 origin;
  Vec3 spacing;
  uint32_t dimensions[3];
  VkrDiffuseVolumeProbe *probes;
  uint32_t probe_count;
  /** Zero is an invalid cell. A nonzero value matches all eight cell corners.
   */
  uint32_t *cell_region_ids;
  uint32_t cell_count;
} VkrDiffuseVolume;

/**
 * Serializes a validated volume into caller-Arena bytes. The portable DVOL v1
 * file writes every scalar explicitly in little-endian order; it never writes
 * native structs.
 */
bool8_t vkr_diffuse_volume_encode(const VkrDiffuseVolume *volume, Arena *arena,
                                  const uint8_t **out_bytes,
                                  uint64_t *out_size);

/**
 * Validates and decodes a complete DVOL v1 byte sequence. It rejects malformed
 * layout, checksum failures, non-finite values, invalid regions, and any
 * inconsistent interpolation cell before publishing output storage.
 */
bool8_t vkr_diffuse_volume_decode(const uint8_t *bytes, uint64_t size,
                                  Arena *arena, VkrDiffuseVolume *out_volume);

#ifdef __cplusplus
}
#endif
