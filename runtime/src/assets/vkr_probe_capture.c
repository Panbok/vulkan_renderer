#include "assets/vkr_probe_capture.h"

#include "containers/str.h"
#include "core/vkr_byte_io.h"

/* KTX2 file identifier and the header offsets of its key/value data. */
static const uint8_t vkr_probe_capture_ktx2_identifier[12] = {
    0xab, 'K', 'T', 'X', ' ', '2', '0', 0xbb, '\r', '\n', 0x1a, '\n'};
#define VKR_PROBE_CAPTURE_KTX2_HEADER_BYTES 80u
#define VKR_PROBE_CAPTURE_KTX2_KVD_OFFSET 56u
#define VKR_PROBE_CAPTURE_KTX2_KVD_LENGTH 60u

static bool8_t vkr_probe_capture_finite_vec3(Vec3 value) {
  return vkr_is_finite_f32(value.x) && vkr_is_finite_f32(value.y) &&
         vkr_is_finite_f32(value.z);
}

float32_t vkr_probe_capture_light(const VkrProbeCapture *capture,
                                  const float32_t *weights) {
  float32_t light = 0.0f;
  for (uint32_t layer = 0u; layer < capture->layer_count; ++layer) {
    light += weights[layer] * capture->luminance[layer];
  }
  return light;
}

bool8_t vkr_probe_capture_valid(const VkrProbeCapture *capture) {
  if (!capture || !vkr_probe_capture_finite_vec3(capture->origin) ||
      capture->layer_count > VKR_DIFFUSE_VOLUME_MAX_LAYERS ||
      !vkr_light_layers_valid(capture->layers, capture->layer_count)) {
    return false_v;
  }
  for (uint32_t layer = 0u; layer < capture->layer_count; ++layer) {
    const float32_t luminance = capture->luminance[layer];
    const float32_t weight = capture->weights[layer];
    if (!vkr_is_finite_f32(luminance) || luminance < 0.0f ||
        !vkr_is_finite_f32(weight) || weight < 0.0f) {
      return false_v;
    }
  }
  /* A ratio needs light to divide by. */
  return capture->layer_count == 0u ||
         vkr_probe_capture_light(capture, capture->weights) > 0.0f;
}

uint32_t vkr_probe_capture_encode(const VkrProbeCapture *capture,
                                  uint8_t out[VKR_PROBE_CAPTURE_MAX_BYTES]) {
  if (!out || !vkr_f32_is_binary32() || !vkr_probe_capture_valid(capture)) {
    return 0u;
  }
  MemZero(out, VKR_PROBE_CAPTURE_MAX_BYTES);
  vkr_store_le_u32(out, VKR_PROBE_CAPTURE_VERSION);
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    vkr_store_le_f32(out + 4u + axis * 4u, capture->origin.elements[axis]);
  }
  vkr_store_le_u32(out + 16u, capture->layer_count);

  uint8_t *cursor = out + VKR_PROBE_CAPTURE_HEADER_BYTES;
  for (uint32_t layer = 0u; layer < capture->layer_count; ++layer) {
    vkr_light_layer_write(cursor, &capture->layers[layer]);
    vkr_store_le_f32(cursor + VKR_LIGHT_LAYER_RECORD_BYTES,
                     capture->luminance[layer]);
    vkr_store_le_f32(cursor + VKR_LIGHT_LAYER_RECORD_BYTES + 4u,
                     capture->weights[layer]);
    cursor += VKR_PROBE_CAPTURE_LAYER_BYTES;
  }
  return (uint32_t)(cursor - out);
}

bool8_t vkr_probe_capture_decode(const uint8_t *bytes, uint64_t size,
                                 VkrProbeCapture *out_capture) {
  if (!bytes || !out_capture || !vkr_f32_is_binary32() ||
      size < VKR_PROBE_CAPTURE_HEADER_BYTES ||
      vkr_load_le_u32(bytes) != VKR_PROBE_CAPTURE_VERSION) {
    return false_v;
  }
  VkrProbeCapture capture = {0};
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    capture.origin.elements[axis] = vkr_load_le_f32(bytes + 4u + axis * 4u);
  }
  capture.layer_count = vkr_load_le_u32(bytes + 16u);
  if (capture.layer_count > VKR_DIFFUSE_VOLUME_MAX_LAYERS ||
      size !=
          VKR_PROBE_CAPTURE_HEADER_BYTES +
              (uint64_t)capture.layer_count * VKR_PROBE_CAPTURE_LAYER_BYTES) {
    return false_v;
  }

  const uint8_t *cursor = bytes + VKR_PROBE_CAPTURE_HEADER_BYTES;
  for (uint32_t layer = 0u; layer < capture.layer_count; ++layer) {
    if (!vkr_light_layer_read(cursor, &capture.layers[layer])) {
      return false_v;
    }
    capture.luminance[layer] =
        vkr_load_le_f32(cursor + VKR_LIGHT_LAYER_RECORD_BYTES);
    capture.weights[layer] =
        vkr_load_le_f32(cursor + VKR_LIGHT_LAYER_RECORD_BYTES + 4u);
    cursor += VKR_PROBE_CAPTURE_LAYER_BYTES;
  }
  if (!vkr_probe_capture_valid(&capture)) {
    return false_v;
  }
  *out_capture = capture;
  return true_v;
}

VkrProbeCaptureStatus
vkr_probe_capture_find_ktx2(const uint8_t *bytes, uint64_t size,
                            VkrProbeCapture *out_capture) {
  if (!bytes || !out_capture || size < VKR_PROBE_CAPTURE_KTX2_HEADER_BYTES ||
      MemCompare(bytes, vkr_probe_capture_ktx2_identifier,
                 sizeof(vkr_probe_capture_ktx2_identifier)) != 0) {
    return VKR_PROBE_CAPTURE_INVALID;
  }
  const uint64_t offset =
      vkr_load_le_u32(bytes + VKR_PROBE_CAPTURE_KTX2_KVD_OFFSET);
  const uint64_t length =
      vkr_load_le_u32(bytes + VKR_PROBE_CAPTURE_KTX2_KVD_LENGTH);
  if (offset > size || length > size - offset) {
    return VKR_PROBE_CAPTURE_INVALID;
  }

  /* Each entry: uint32 byte length, the key, a zero byte, the value, then
     padding to four bytes (KTX2 section 3.11). */
  const uint64_t key_length = sizeof(VKR_PROBE_CAPTURE_KEY);
  uint64_t cursor = offset;
  const uint64_t end = offset + length;
  while (end - cursor >= 4u) {
    const uint64_t entry_length = vkr_load_le_u32(bytes + cursor);
    cursor += 4u;
    if (entry_length > end - cursor) {
      return VKR_PROBE_CAPTURE_INVALID;
    }
    if (entry_length >= key_length &&
        MemCompare(bytes + cursor, VKR_PROBE_CAPTURE_KEY, key_length) == 0) {
      return vkr_probe_capture_decode(bytes + cursor + key_length,
                                      entry_length - key_length, out_capture)
                 ? VKR_PROBE_CAPTURE_FOUND
                 : VKR_PROBE_CAPTURE_INVALID;
    }
    cursor = vkr_align_up_u64(cursor + entry_length, 4u);
    if (cursor > end) {
      return VKR_PROBE_CAPTURE_INVALID;
    }
  }
  return VKR_PROBE_CAPTURE_ABSENT;
}

bool8_t vkr_probe_capture_matches(const VkrProbeCapture *capture,
                                  const VkrLightLayer *layers,
                                  uint32_t layer_count) {
  if (!capture || capture->layer_count != layer_count ||
      (layer_count != 0u && !layers)) {
    return false_v;
  }
  for (uint32_t layer = 0u; layer < layer_count; ++layer) {
    const VkrLightLayer *recorded = &capture->layers[layer];
    const VkrLightLayer *current = &layers[layer];
    if (recorded->kind != current->kind || recorded->index != current->index ||
        MemCompare(&recorded->sun_direction, &current->sun_direction,
                   sizeof(recorded->sun_direction)) != 0 ||
        !string_n_equals(recorded->name, current->name,
                         VKR_LIGHT_GROUP_NAME_BYTES)) {
      return false_v;
    }
  }
  return true_v;
}

float32_t vkr_probe_capture_ratio(const VkrProbeCapture *capture,
                                  const float32_t *weights) {
  if (!capture || capture->layer_count == 0u || !weights) {
    return 1.0f;
  }
  const float32_t captured = vkr_probe_capture_light(capture, capture->weights);
  if (!(captured > 0.0f)) {
    return 1.0f;
  }
  const float32_t ratio = vkr_probe_capture_light(capture, weights) / captured;
  if (!vkr_is_finite_f32(ratio)) {
    return VKR_PROBE_CAPTURE_RATIO_MAX;
  }
  return Clamp(ratio, VKR_PROBE_CAPTURE_RATIO_MIN, VKR_PROBE_CAPTURE_RATIO_MAX);
}
