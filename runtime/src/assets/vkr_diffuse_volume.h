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
/* DVOL v3 (sparse diffuse volumes): a 128-byte header, the layer table of
   VKR_LIGHT_LAYER_RECORD_BYTES records, then the indirection entries, the
   bricks, each probe's offset and validity, each probe's distance moments
   and each layer's L1 SH per probe, every section aligned to
   VKR_DIFFUSE_VOLUME_SECTION_ALIGNMENT. Versions 1 and 2 (one uniform grid
   of L2 SH with room regions) are refused. */
#define VKR_DIFFUSE_VOLUME_VERSION 3u
#define VKR_DIFFUSE_VOLUME_ENDIAN_TAG 0x01020304u
#define VKR_DIFFUSE_VOLUME_HEADER_BYTES 128u
#define VKR_DIFFUSE_VOLUME_SECTION_ALIGNMENT 16u
#define VKR_DIFFUSE_VOLUME_MAX_LAYERS 16u

/* A brick holds 4 x 4 x 4 probes; probe i of a brick is x + 4 * (y + 4 * z).
   Level 0 has the finest spacing and each level triples it, so a level-k
   brick spans 3^k indirection entries per axis. */
#define VKR_DIFFUSE_VOLUME_BRICK_SIZE 4u
#define VKR_DIFFUSE_VOLUME_BRICK_PROBES 64u
#define VKR_DIFFUSE_VOLUME_MAX_LEVELS 3u
#define VKR_DIFFUSE_VOLUME_MAX_ENTRIES 4194304u
/* The runtime's moment atlas, 512 tiles wide and at most 16384 texels
   high, holds 1638 rows of 512 probes. */
#define VKR_DIFFUSE_VOLUME_MAX_BRICKS 13104u

/* An indirection entry names the brick that covers it and that brick's
   level, or is empty: no probe covers it and lighting keeps the environment
   path. */
#define VKR_DIFFUSE_VOLUME_ENTRY_LEVEL_SHIFT 30u
#define VKR_DIFFUSE_VOLUME_ENTRY_BRICK_MASK 0x3fffffffu
#define VKR_DIFFUSE_VOLUME_ENTRY_EMPTY 0xffffffffu

/* Per probe: the relocation offset and validity as four halves; each
   layer's L1 SH as three half4 rows (red, green, blue: linear x, y, z, then
   the constant term, as Sloan-packed E/pi without the quadratic band),
   divided by the volume's power-of-two `sh_scale` so it fits a half. */
#define VKR_DIFFUSE_VOLUME_AUX_BYTES 8u
#define VKR_DIFFUSE_VOLUME_SH_BYTES 24u

/* Distance moments per probe: an octahedral 10 x 10 RG16F tile, 8 x 8
   texels with a one-texel border that repeats the opposite edge, so bilinear
   filtering wraps across the octahedron's seams. With 6 x 6 texels a probe
   just past a thin roof or wall saw its hits and its escapes in one texel,
   and the Chebyshev test let its light through. */
#define VKR_DIFFUSE_VOLUME_MOMENT_SIZE 10u
#define VKR_DIFFUSE_VOLUME_MOMENT_INTERIOR 8u
#define VKR_DIFFUSE_VOLUME_MOMENT_TEXELS 100u
#define VKR_DIFFUSE_VOLUME_MOMENT_BYTES 400u

/** One brick: its lowest indirection entry, a multiple of 3^level per axis,
    and its level. */
typedef struct VkrDiffuseVolumeBrick {
  uint32_t entry[3];
  uint32_t level;
} VkrDiffuseVolumeBrick;

/**
 * Decoded DVOL v3. Probe i of brick b is probe `b * 64 + i`; its lattice
 * position is `origin + spacing * (3 * entry + 3^level * (x, y, z))`.
 *
 * `entries`, `probe_aux`, `moments` and `layer_sh` are little-endian byte
 * views laid out as the GPU reads them: entries as uint32 x + nx * (y + ny *
 * z); probe_aux as VKR_DIFFUSE_VOLUME_AUX_BYTES per probe; moments as
 * VKR_DIFFUSE_VOLUME_MOMENT_BYTES per probe; layer_sh band-major, band b's
 * probe p at `(b * probe_count + p) * VKR_DIFFUSE_VOLUME_SH_BYTES`. Bands
 * 0 to layer_count - 1 are the layers; with lamp direct bands, band
 * layer_count + i holds the direct light of the i-th lamp-group layer in
 * layer order, which its layer leaves out (vkr_diffuse_volume_sh_band_count).
 * After
 * decoding they point into the caller's bytes and `bricks` and `layers` into
 * the caller's Arena; both must outlive the volume.
 */
typedef struct VkrDiffuseVolume {
  Vec3 origin;
  float32_t spacing;
  /** Power of two the stored SH is multiplied by. */
  float32_t sh_scale;
  uint32_t level_count;
  uint32_t dimensions[3];
  const uint8_t *entries;
  uint32_t entry_count;
  const VkrDiffuseVolumeBrick *bricks;
  uint32_t brick_count;
  uint32_t probe_count;
  const uint8_t *probe_aux;
  const uint8_t *moments;
  const uint8_t *layer_sh;
  /** A valid layer table (vkr_light_layers_valid) of at least one layer. */
  const VkrLightLayer *layers;
  uint32_t layer_count;
  /**
   * Zero, or the number of lamp-group layers: then each lamp group's direct
   * light at the probes, which no gathered path reaches, follows the layers
   * as its own band. A frame that samples the lamps' baked lightmaps adds it
   * for receivers without one; with runtime lamps it stays out (desktop
   * baked lamps proposal).
   */
  uint32_t lamp_direct_count;
} VkrDiffuseVolume;

/** SH bands a volume stores: its layers, then its lamp direct bands. */
uint32_t vkr_diffuse_volume_sh_band_count(const VkrDiffuseVolume *volume);

/** Entry `index` of a volume's indirection grid. */
uint32_t vkr_diffuse_volume_entry(const VkrDiffuseVolume *volume,
                                  uint32_t index);

/**
 * Serializes a validated volume into caller-Arena bytes. Every scalar is
 * written explicitly in little-endian order; native structs are never
 * written.
 */
bool8_t vkr_diffuse_volume_encode(const VkrDiffuseVolume *volume, Arena *arena,
                                  const uint8_t **out_bytes,
                                  uint64_t *out_size);

/**
 * Validates and decodes a complete DVOL v3 byte sequence. It rejects
 * malformed layout, checksum failures, an invalid layer table, entries that
 * name a missing brick or another level, bricks off the grid or off their
 * level's alignment, probe offsets beyond half their spacing, validity other
 * than zero or one, and non-finite halves before publishing output.
 */
bool8_t vkr_diffuse_volume_decode(const uint8_t *bytes, uint64_t size,
                                  Arena *arena, VkrDiffuseVolume *out_volume);

#ifdef __cplusplus
}
#endif
