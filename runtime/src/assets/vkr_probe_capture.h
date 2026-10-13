#pragma once

#include <stdint.h>

#include "assets/vkr_diffuse_volume.h"
#include "assets/vkr_light_layers.h"
#include "math/vec.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Capture record of a baked reflection probe (ADR-019): where its six faces
 * were rendered and the baked light at that point when they were. The
 * record travels inside the probe's KTX2 cubemap as the key/value entry
 * VKR_PROBE_CAPTURE_KEY, so the cube carries it through project lowering
 * and packaging; `vkr_bakery bake probe` writes it and the scene loader
 * reads it.
 *
 * The baked light is one value per layer of the scene's diffuse volume:
 * the volume's luminance of that layer at the capture origin
 * (vkr_diffuse_volume_layer_luminance), and the weight the volume composed
 * it with during the capture. At runtime the probe's radiance follows the
 * time of day by the ratio of the light the current weights compose over
 * the light the capture weights composed (vkr_probe_capture_ratio).
 *
 * Encoding, little-endian: uint32 version, float32 origin x, y, z, uint32
 * layer count (0 when the scene had no diffuse volume), then per layer its
 * VKR_LIGHT_LAYER_RECORD_BYTES record, float32 luminance and float32 weight.
 */
#define VKR_PROBE_CAPTURE_KEY "vkr.probe_capture"
#define VKR_PROBE_CAPTURE_VERSION 1u
#define VKR_PROBE_CAPTURE_HEADER_BYTES 20u
#define VKR_PROBE_CAPTURE_LAYER_BYTES (VKR_LIGHT_LAYER_RECORD_BYTES + 8u)
#define VKR_PROBE_CAPTURE_MAX_BYTES                                            \
  (VKR_PROBE_CAPTURE_HEADER_BYTES +                                            \
   VKR_DIFFUSE_VOLUME_MAX_LAYERS * VKR_PROBE_CAPTURE_LAYER_BYTES)

/*
 * Bounds of the time-of-day ratio. The ratio is one luminance at one point,
 * so it cannot follow a change in colour or in where the light comes from;
 * the further it strays from one, the more it extrapolates. Darkening is the
 * benign error: up to 6 stops lets a noon capture fall into deep dusk, where
 * lamps and the volume's own lamp layers take over. Brightening amplifies
 * the capture's noise and its stale sky, so it stops at 4 stops, which
 * still carries a dawn capture to noon.
 */
#define VKR_PROBE_CAPTURE_RATIO_MIN (1.0f / 64.0f)
#define VKR_PROBE_CAPTURE_RATIO_MAX 16.0f

/* How far a probe's center may sit from its capture origin before its box
   projection counts as wrong and the probe as needing a recapture. */
#define VKR_PROBE_CAPTURE_ORIGIN_TOLERANCE 0.01f

typedef struct VkrProbeCapture {
  /* World position the six faces were rendered from. */
  Vec3 origin;
  /* Layers of the scene's diffuse volume at capture; zero without one. */
  uint32_t layer_count;
  VkrLightLayer layers[VKR_DIFFUSE_VOLUME_MAX_LAYERS];
  /* Each layer's baked luminance at `origin`. */
  float32_t luminance[VKR_DIFFUSE_VOLUME_MAX_LAYERS];
  /* Each layer's composed weight while the faces were rendered. */
  float32_t weights[VKR_DIFFUSE_VOLUME_MAX_LAYERS];
} VkrProbeCapture;

typedef enum VkrProbeCaptureStatus {
  VKR_PROBE_CAPTURE_ABSENT = 0,
  VKR_PROBE_CAPTURE_FOUND = 1,
  VKR_PROBE_CAPTURE_INVALID = 2,
} VkrProbeCaptureStatus;

/* Whether `capture` holds a finite origin, a valid layer table, and finite,
   non-negative luminances and weights whose capture light is positive when
   it has layers. */
bool8_t vkr_probe_capture_valid(const VkrProbeCapture *capture);

/* Encodes a valid record into `out`; returns its size, or zero when the
   record is invalid. */
uint32_t vkr_probe_capture_encode(const VkrProbeCapture *capture,
                                  uint8_t out[VKR_PROBE_CAPTURE_MAX_BYTES]);

/* Decodes and validates one encoded record of exactly `size` bytes. */
bool8_t vkr_probe_capture_decode(const uint8_t *bytes, uint64_t size,
                                 VkrProbeCapture *out_capture);

/* Finds and decodes the VKR_PROBE_CAPTURE_KEY entry of a KTX2 file's
   key/value data. Reads only the header and that section, so `bytes` may be
   a mapping of the whole file. */
VkrProbeCaptureStatus vkr_probe_capture_find_ktx2(const uint8_t *bytes,
                                                  uint64_t size,
                                                  VkrProbeCapture *out_capture);

/* Whether the record's layers are exactly `layers`, so its luminances and
   weights line up with a volume's layer weights. */
bool8_t vkr_probe_capture_matches(const VkrProbeCapture *capture,
                                  const VkrLightLayer *layers,
                                  uint32_t layer_count);

/* The factor on the probe's radiance for the volume's current layer
   `weights` (layer_count of the matching table): the light they compose at
   the capture origin over the light the capture weights composed, clamped
   to [VKR_PROBE_CAPTURE_RATIO_MIN, VKR_PROBE_CAPTURE_RATIO_MAX]. One for a
   record without layers. */
float32_t vkr_probe_capture_ratio(const VkrProbeCapture *capture,
                                  const float32_t *weights);

#ifdef __cplusplus
}
#endif
