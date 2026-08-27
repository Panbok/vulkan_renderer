/**
 * @file vkr_harness_capture.c
 * @brief Canonicalizes a ready capture batch into comparable artifacts.
 *
 * Comparison always reads the canonical payload, never the preview: color
 * becomes a top-left RGBA8 PNG, depth and identifier channels become tightly
 * packed little-endian 32-bit data beside a PNG a reviewer can look at. Each
 * row is published with a sidecar describing exactly how it was produced.
 */
#include "vkr_harness_runtime.h"

#include <float.h>
#include <stb_image_write.h>

typedef struct VkrHarnessPngBuffer {
  uint8_t *data;
  uint64_t count;
  uint64_t capacity;
  bool8_t failed;
} VkrHarnessPngBuffer;

/* Version 2 embedded the profile struct directly. Keep its exact layout so
 * accepted capture summaries remain readable when the in-memory profile grows.
 */
typedef struct VkrHarnessProfileV2 {
  uint32_t schema_version;
  char manifest_path[VKR_HARNESS_PATH_MAX];
  char manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char id[VKR_HARNESS_ID_MAX];
  char description[VKR_HARNESS_TEXT_MAX];
  bool8_t authoritative;
  bool8_t allow_dirty;
  VkrHarnessTarget target;
  VkrHarnessPresentMode required_present;
  bool8_t require_actual_present;
  bool8_t gpu_timing;
  bool8_t event_subjects;
  uint32_t minimum_repetitions;
  uint32_t warmup_stability_window;
  float64_t warmup_max_drift_ratio;
  bool8_t require_warmup_stability;
  bool8_t require_exclusive_gpu_lane;
  char required_os[64];
  char required_cpu[128];
  char required_gpu[128];
  char required_driver[128];
  uint32_t required_gpu_vendor_id;
  uint32_t required_gpu_device_id;
  char required_power_mode[32];
  char required_thermal_state[32];
  int32_t required_process_priority;
  bool8_t has_required_process_priority;
  char required_metrics[VKR_HARNESS_MAX_REQUIRED_METRICS][128];
  uint32_t required_metric_count;
} VkrHarnessProfileV2;

/* Versions 2 and 3 embedded this case layout before GTAO became authored
 * harness state. Keep it private and byte-exact; stored summaries are an ABI.
 */
typedef struct VkrHarnessRendererConfigV3 {
  bool8_t editor;
  bool8_t skybox;
  bool8_t text_fixture;
  bool8_t taa_enabled;
  bool8_t shadow_pcf_early_out;
  bool8_t shadow_sdsm;
  char backend[16];
  char shadow_preset[32];
  uint32_t shadow_cascades;
  uint32_t shadow_pcf_samples;
  uint32_t shadow_map_size;
  float32_t shadow_split_lambda;
  char render_mode[24];
  char exposure_mode[16];
  float32_t manual_exposure;
  float32_t exposure_compensation_ev;
  uint32_t exposure_reset_frame;
  bool8_t bloom_enabled;
  float32_t bloom_threshold;
  float32_t bloom_knee;
  float32_t bloom_intensity;
  uint32_t shadow_debug_mode;
} VkrHarnessRendererConfigV3;

typedef struct VkrHarnessCaseV3 {
  uint32_t schema_version;
  char manifest_path[VKR_HARNESS_PATH_MAX];
  char manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char id[VKR_HARNESS_ID_MAX];
  char suite[64];
  char description[VKR_HARNESS_TEXT_MAX];
  char scene[VKR_HARNESS_PATH_MAX];
  uint64_t seed;
  uint32_t width;
  uint32_t height;
  bool8_t resize_round_trip;
  uint32_t resize_width;
  uint32_t resize_height;
  VkrHarnessBootProfile boot;
  VkrHarnessTarget target;
  VkrHarnessPresentMode present;
  uint32_t target_image_count;
  VkrHarnessCacheMode cache;
  float64_t fixed_delta_seconds;
  uint32_t warmup_frames;
  uint32_t measure_frames;
  uint32_t repetitions;
  uint32_t repetition_timeout_ms;
  uint32_t asset_ready_timeout_ms;
  VkrHarnessRendererConfigV3 renderer;
  VkrHarnessCamera camera;
  VkrHarnessCapture captures[VKR_HARNESS_MAX_CAPTURES];
  uint32_t capture_count;
  VkrHarnessAssertion assertions[VKR_HARNESS_MAX_ASSERTIONS];
  uint32_t assertion_count;
  VkrHarnessCompareConfig compare;
} VkrHarnessCaseV3;

_Static_assert(offsetof(VkrHarnessCaseV3, renderer) ==
                   offsetof(VkrHarnessCase, renderer),
               "Legacy case prefix drift");
_Static_assert(offsetof(VkrHarnessRendererConfigV3, shadow_debug_mode) ==
                   offsetof(VkrHarnessRendererConfig, gtao_enabled),
               "Legacy renderer prefix drift");

typedef struct VkrHarnessCaptureSummaryHeaderV2 {
  uint8_t magic[8];
  uint32_t version;
  uint32_t capture_count;
  uint32_t artifact_count;
  uint32_t tool;
  uint32_t exit_code;
  bool8_t authoritative;
  bool8_t profile_compatible;
  uint8_t reserved[2];
  char status[24];
  char case_id[VKR_HARNESS_ID_MAX];
  char case_manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char profile_id[VKR_HARNESS_ID_MAX];
  char profile_manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char environment_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char workload_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char policy_fingerprint[VKR_HARNESS_DIGEST_MAX];
  VkrHarnessCaseV3 case_manifest;
  VkrHarnessProfileV2 profile;
  VkrHarnessProvenance provenance;
} VkrHarnessCaptureSummaryHeaderV2;

typedef struct VkrHarnessCaptureSummaryHeaderV3 {
  uint8_t magic[8];
  uint32_t version;
  uint32_t capture_count;
  uint32_t artifact_count;
  uint32_t tool;
  uint32_t exit_code;
  bool8_t authoritative;
  bool8_t profile_compatible;
  uint8_t reserved[2];
  char status[24];
  char case_id[VKR_HARNESS_ID_MAX];
  char case_manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char profile_id[VKR_HARNESS_ID_MAX];
  char profile_manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char environment_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char workload_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char policy_fingerprint[VKR_HARNESS_DIGEST_MAX];
  VkrHarnessCaseV3 case_manifest;
  VkrHarnessProfile profile;
  VkrHarnessProvenance provenance;
} VkrHarnessCaptureSummaryHeaderV3;

typedef struct VkrHarnessCaptureSummaryHeaderV4 {
  uint8_t magic[8];
  uint32_t version;
  uint32_t capture_count;
  uint32_t artifact_count;
  uint32_t tool;
  uint32_t exit_code;
  bool8_t authoritative;
  bool8_t profile_compatible;
  uint8_t reserved[2];
  char status[24];
  char case_id[VKR_HARNESS_ID_MAX];
  char case_manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char profile_id[VKR_HARNESS_ID_MAX];
  char profile_manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char environment_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char workload_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char policy_fingerprint[VKR_HARNESS_DIGEST_MAX];
  VkrHarnessCase case_manifest;
  VkrHarnessProfile profile;
  VkrHarnessProvenance provenance;
} VkrHarnessCaptureSummaryHeaderV4;

_Static_assert(offsetof(VkrHarnessCaptureSummaryHeaderV2, case_manifest) ==
                       offsetof(VkrHarnessCaptureSummaryHeaderV3,
                                case_manifest) &&
                   offsetof(VkrHarnessCaptureSummaryHeaderV3, case_manifest) ==
                       offsetof(VkrHarnessCaptureSummaryHeaderV4,
                                case_manifest),
               "Capture summary common prefix drift");

static const uint8_t s_capture_summary_magic[8] = {'V', 'K', 'R', 'C',
                                                   'A', 'P', '0', '1'};

static void vkr_harness_profile_from_v2(const VkrHarnessProfileV2 *source,
                                        VkrHarnessProfile *destination) {
  MemZero(destination, sizeof(*destination));
  destination->schema_version = source->schema_version;
  string_format(destination->manifest_path, sizeof(destination->manifest_path),
                "%s", source->manifest_path);
  string_format(destination->manifest_sha256,
                sizeof(destination->manifest_sha256), "%s",
                source->manifest_sha256);
  string_format(destination->id, sizeof(destination->id), "%s", source->id);
  string_format(destination->description, sizeof(destination->description),
                "%s", source->description);
  destination->authoritative = source->authoritative;
  destination->allow_dirty = source->allow_dirty;
  destination->target = source->target;
  destination->required_present = source->required_present;
  destination->require_actual_present = source->require_actual_present;
  destination->gpu_timing = source->gpu_timing;
  destination->event_subjects = source->event_subjects;
  destination->minimum_repetitions = source->minimum_repetitions;
  destination->warmup_stability_window = source->warmup_stability_window;
  string_format(destination->warmup_stability_metric,
                sizeof(destination->warmup_stability_metric), "%s",
                "cpu.render_submit");
  destination->warmup_max_drift_ratio = source->warmup_max_drift_ratio;
  destination->require_warmup_stability = source->require_warmup_stability;
  destination->require_exclusive_gpu_lane = source->require_exclusive_gpu_lane;
  string_format(destination->required_os, sizeof(destination->required_os),
                "%s", source->required_os);
  string_format(destination->required_cpu, sizeof(destination->required_cpu),
                "%s", source->required_cpu);
  string_format(destination->required_gpu, sizeof(destination->required_gpu),
                "%s", source->required_gpu);
  string_format(destination->required_driver,
                sizeof(destination->required_driver), "%s",
                source->required_driver);
  destination->required_gpu_vendor_id = source->required_gpu_vendor_id;
  destination->required_gpu_device_id = source->required_gpu_device_id;
  string_format(destination->required_power_mode,
                sizeof(destination->required_power_mode), "%s",
                source->required_power_mode);
  string_format(destination->required_thermal_state,
                sizeof(destination->required_thermal_state), "%s",
                source->required_thermal_state);
  destination->required_process_priority = source->required_process_priority;
  destination->has_required_process_priority =
      source->has_required_process_priority;
  MemCopy(destination->required_metrics, source->required_metrics,
          sizeof(destination->required_metrics));
  destination->required_metric_count = source->required_metric_count;
}

static void vkr_harness_case_from_v3(const VkrHarnessCaseV3 *source,
                                     VkrHarnessCase *destination) {
  MemZero(destination, sizeof(*destination));
  MemCopy(destination, source, offsetof(VkrHarnessCaseV3, renderer));
  MemCopy(&destination->renderer, &source->renderer,
          offsetof(VkrHarnessRendererConfigV3, shadow_debug_mode));
  destination->renderer.gtao_enabled = false_v;
  destination->renderer.gtao_radius = VKR_GTAO_DEFAULT_RADIUS;
  destination->renderer.gtao_power = VKR_GTAO_DEFAULT_POWER;
  destination->renderer.shadow_debug_mode = source->renderer.shadow_debug_mode;
  destination->camera = source->camera;
  MemCopy(destination->captures, source->captures,
          sizeof(destination->captures));
  destination->capture_count = source->capture_count;
  MemCopy(destination->assertions, source->assertions,
          sizeof(destination->assertions));
  destination->assertion_count = source->assertion_count;
  destination->compare = source->compare;
}

static void vkr_harness_png_write(void *context, void *data, int size) {
  VkrHarnessPngBuffer *buffer = context;
  if (!buffer || size < 0 ||
      buffer->count + (uint64_t)size > buffer->capacity) {
    if (buffer) {
      buffer->failed = true_v;
    }
    return;
  }
  MemCopy(buffer->data + buffer->count, data, (uint64_t)size);
  buffer->count += (uint64_t)size;
}

static const char *vkr_harness_capture_format_name(VkrTextureFormat format) {
  switch (format) {
  case VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM:
    return "R8G8B8A8_UNORM";
  case VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB:
    return "R8G8B8A8_SRGB";
  case VKR_TEXTURE_FORMAT_B8G8R8A8_UNORM:
    return "B8G8R8A8_UNORM";
  case VKR_TEXTURE_FORMAT_B8G8R8A8_SRGB:
    return "B8G8R8A8_SRGB";
  case VKR_TEXTURE_FORMAT_R8_UNORM:
    return "R8_UNORM";
  case VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT:
    return "R16G16B16A16_SFLOAT";
  case VKR_TEXTURE_FORMAT_R16_SFLOAT:
    return "R16_SFLOAT";
  case VKR_TEXTURE_FORMAT_D32_SFLOAT:
    return "D32_SFLOAT";
  case VKR_TEXTURE_FORMAT_D16_UNORM:
    return "D16_UNORM";
  case VKR_TEXTURE_FORMAT_R32_UINT:
    return "R32_UINT";
  case VKR_TEXTURE_FORMAT_R32G32_UINT:
    return "R32G32_UINT";
  case VKR_TEXTURE_FORMAT_R16G16_SNORM:
    return "R16G16_SNORM";
  default:
    return "UNSUPPORTED";
  }
}

static const char *
vkr_harness_capture_value_name(VkrCaptureValueKind value_kind) {
  return value_kind == VKR_CAPTURE_VALUE_COLOR   ? "color"
         : value_kind == VKR_CAPTURE_VALUE_DEPTH ? "depth"
                                                 : "uint";
}

static const char *
vkr_harness_capture_color_space_name(VkrCaptureColorSpace color_space) {
  return color_space == VKR_CAPTURE_COLOR_SPACE_SRGB ? "srgb" : "none";
}

/** Canonical output is top-left; a bottom-left source is flipped on read. */
static const uint8_t *vkr_harness_capture_row(const VkrCaptureItemResult *item,
                                              uint32_t y) {
  const uint32_t source_y = item->origin == VKR_CAPTURE_ORIGIN_BOTTOM_LEFT
                                ? item->height - 1u - y
                                : y;
  return (const uint8_t *)item->data + source_y * item->row_pitch;
}

static uint16_t vkr_harness_capture_read_u16(const uint8_t *bytes) {
  uint16_t value = 0u;
  MemCopy(&value, bytes, sizeof(value));
  return value;
}

static uint32_t vkr_harness_capture_read_u32(const uint8_t *bytes) {
  uint32_t value = 0u;
  MemCopy(&value, bytes, sizeof(value));
  return value;
}

static float32_t vkr_harness_capture_read_f32(const uint8_t *bytes) {
  const uint32_t bits = vkr_harness_capture_read_u32(bytes);
  float32_t value = 0.0f;
  MemCopy(&value, &bits, sizeof(value));
  return value;
}

static void vkr_harness_capture_write_u32_le(uint8_t *bytes, uint32_t value) {
  bytes[0] = (uint8_t)value;
  bytes[1] = (uint8_t)(value >> 8u);
  bytes[2] = (uint8_t)(value >> 16u);
  bytes[3] = (uint8_t)(value >> 24u);
}

bool8_t vkr_harness_capture_png_write(const char *path, const uint8_t *rgba,
                                      uint32_t width, uint32_t height,
                                      const VkrHarnessArenas *arenas,
                                      VkrHarnessError *error) {
  const uint64_t capacity = (uint64_t)width * height * 5u + KB(64);
  Scratch scratch = scratch_create(arenas->transient);
  VkrHarnessPngBuffer output = {
      .data = arena_alloc(arenas->transient, capacity, ARENA_MEMORY_TAG_ARRAY),
      .capacity = capacity,
  };
  bool8_t ok =
      output.data &&
      stbi_write_png_to_func(vkr_harness_png_write, &output, (int)width,
                             (int)height, 4, rgba, (int)(width * 4u)) != 0 &&
      !output.failed &&
      vkr_harness_atomic_write(path, output.data, output.count, error);
  scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
  return ok;
}

static float32_t vkr_harness_capture_half_to_float(uint16_t half) {
  const uint32_t sign = half >> 15u;
  const uint32_t exponent = (half >> 10u) & 0x1fu;
  const uint32_t mantissa = half & 0x3ffu;
  float32_t value = 0.0f;
  if (exponent == 0u) {
    value = (float32_t)mantissa * (1.0f / 16777216.0f);
  } else if (exponent < 31u) {
    value = (1.0f + (float32_t)mantissa * (1.0f / 1024.0f)) *
            vkr_pow_f32(2.0f, (float32_t)((int32_t)exponent - 15));
  }
  return sign ? -value : value;
}

static float32_t vkr_harness_capture_aces(float32_t value) {
  const float32_t nonnegative = Max(value, 0.0f);
  return Clamp((nonnegative * (2.51f * nonnegative + 0.03f)) /
                   (nonnegative * (2.43f * nonnegative + 0.59f) + 0.14f),
               0.0f, 1.0f);
}

static uint8_t vkr_harness_capture_linear_to_srgb8(float32_t value,
                                                   float32_t exposure) {
  const float32_t linear = vkr_harness_capture_aces(value * exposure);
  const float32_t srgb =
      linear <= 0.0031308f ? linear * 12.92f
                           : 1.055f * vkr_pow_f32(linear, 1.0f / 2.4f) - 0.055f;
  return (uint8_t)(Clamp(srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
}

static void vkr_harness_capture_color_rgba(const VkrCaptureItemResult *item,
                                           uint8_t *rgba) {
  const bool8_t bgra = item->format == VKR_TEXTURE_FORMAT_B8G8R8A8_UNORM ||
                       item->format == VKR_TEXTURE_FORMAT_B8G8R8A8_SRGB;
  const bool8_t hdr = item->format == VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT;
  const bool8_t oct_normal = item->format == VKR_TEXTURE_FORMAT_R16G16_SNORM;
  const bool8_t grayscale = item->format == VKR_TEXTURE_FORMAT_R8_UNORM;
  for (uint32_t y = 0; y < item->height; ++y) {
    const uint8_t *source = vkr_harness_capture_row(item, y);
    uint8_t *target = rgba + (uint64_t)y * item->width * 4u;
    for (uint32_t x = 0; x < item->width; ++x) {
      if (grayscale) {
        const uint8_t value = source[x];
        target[x * 4u + 0u] = value;
        target[x * 4u + 1u] = value;
        target[x * 4u + 2u] = value;
        target[x * 4u + 3u] = 255u;
        continue;
      }
      if (oct_normal) {
        const uint8_t *texel = source + (uint64_t)x * 4u;
        const int16_t encoded_x =
            (int16_t)vkr_harness_capture_read_u16(texel + 0u);
        const int16_t encoded_y =
            (int16_t)vkr_harness_capture_read_u16(texel + 2u);
        float32_t nx = Clamp((float32_t)encoded_x / 32767.0f, -1.0f, 1.0f);
        float32_t ny = Clamp((float32_t)encoded_y / 32767.0f, -1.0f, 1.0f);
        float32_t nz = 1.0f - vkr_abs_f32(nx) - vkr_abs_f32(ny);
        const float32_t fold = Clamp(-nz, 0.0f, 1.0f);
        nx += nx >= 0.0f ? -fold : fold;
        ny += ny >= 0.0f ? -fold : fold;
        const float32_t length =
            vkr_sqrt_f32(Max(nx * nx + ny * ny + nz * nz, 1e-12f));
        nx /= length;
        ny /= length;
        nz /= length;
        target[x * 4u + 0u] =
            (uint8_t)(Clamp(nx * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
        target[x * 4u + 1u] =
            (uint8_t)(Clamp(ny * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
        target[x * 4u + 2u] =
            (uint8_t)(Clamp(nz * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
        target[x * 4u + 3u] = 255u;
        continue;
      }
      if (hdr) {
        const uint8_t *texel = source + (uint64_t)x * 8u;
        target[x * 4u + 0u] = vkr_harness_capture_linear_to_srgb8(
            vkr_harness_capture_half_to_float(
                vkr_harness_capture_read_u16(texel + 0u)),
            item->display_exposure);
        target[x * 4u + 1u] = vkr_harness_capture_linear_to_srgb8(
            vkr_harness_capture_half_to_float(
                vkr_harness_capture_read_u16(texel + 2u)),
            item->display_exposure);
        target[x * 4u + 2u] = vkr_harness_capture_linear_to_srgb8(
            vkr_harness_capture_half_to_float(
                vkr_harness_capture_read_u16(texel + 4u)),
            item->display_exposure);
        target[x * 4u + 3u] =
            (uint8_t)(Clamp(vkr_harness_capture_half_to_float(
                                vkr_harness_capture_read_u16(texel + 6u)),
                            0.0f, 1.0f) *
                          255.0f +
                      0.5f);
        continue;
      }
      target[x * 4u + 0u] = source[x * 4u + (bgra ? 2u : 0u)];
      target[x * 4u + 1u] = source[x * 4u + 1u];
      target[x * 4u + 2u] = source[x * 4u + (bgra ? 0u : 2u)];
      target[x * 4u + 3u] = source[x * 4u + 3u];
    }
  }
}

/**
 * One texel of a depth source as a floating-point depth value. The accepted
 * source formats are the only ones capture initialization permits, so this is
 * the single place each encoding is interpreted.
 */
static float32_t vkr_harness_capture_depth_at(const VkrCaptureItemResult *item,
                                              const uint8_t *row, uint32_t x) {
  if (item->format == VKR_TEXTURE_FORMAT_D16_UNORM) {
    return (float32_t)vkr_harness_capture_read_u16(row + (uint64_t)x * 2u) /
           65535.0f;
  }
  if (item->format == VKR_TEXTURE_FORMAT_R16_SFLOAT) {
    return vkr_harness_capture_half_to_float(
        vkr_harness_capture_read_u16(row + (uint64_t)x * 2u));
  }
  return vkr_harness_capture_read_f32(row + (uint64_t)x * 4u);
}

/**
 * Depth previews normalize over the observed range, which the sidecar records:
 * a fixed [0,1] ramp would render almost every scene as flat white. The
 * canonical payload is unnormalized, so comparison is unaffected.
 */
static void vkr_harness_capture_depth_preview(const VkrCaptureItemResult *item,
                                              uint8_t *rgba, float32_t *out_min,
                                              float32_t *out_max) {
  const bool8_t view_depth = item->format == VKR_TEXTURE_FORMAT_R16_SFLOAT;
  float32_t min_value = view_depth ? FLT_MAX : 1.0f;
  float32_t max_value = view_depth ? -FLT_MAX : 0.0f;
  bool8_t observed_value = false_v;
  for (uint32_t y = 0; y < item->height; ++y) {
    const uint8_t *row = vkr_harness_capture_row(item, y);
    for (uint32_t x = 0; x < item->width; ++x) {
      const float32_t depth = vkr_harness_capture_depth_at(item, row, x);
      /* NaN compares unequal to itself and must not poison the range. */
      if (depth == depth) {
        min_value = Min(min_value, depth);
        max_value = Max(max_value, depth);
        observed_value = true_v;
      }
    }
  }
  if (view_depth && !observed_value) {
    min_value = 0.0f;
    max_value = 0.0f;
  }
  const float32_t range = max_value > min_value ? max_value - min_value : 1.0f;
  for (uint32_t y = 0; y < item->height; ++y) {
    const uint8_t *row = vkr_harness_capture_row(item, y);
    uint8_t *target = rgba + (uint64_t)y * item->width * 4u;
    for (uint32_t x = 0; x < item->width; ++x) {
      const float32_t depth = vkr_harness_capture_depth_at(item, row, x);
      const float32_t normalized =
          Clamp((depth - min_value) / range, 0.0f, 1.0f);
      const uint8_t value = (uint8_t)(normalized * 255.0f + 0.5f);
      target[x * 4u + 0u] = value;
      target[x * 4u + 1u] = value;
      target[x * 4u + 2u] = value;
      target[x * 4u + 3u] = 255u;
    }
  }
  if (out_min) {
    *out_min = min_value;
  }
  if (out_max) {
    *out_max = max_value;
  }
}

/**
 * Identifiers have no meaningful ordering, so the preview hashes each one into
 * a stable colour. Zero stays black so "no object" reads as background.
 */
static void vkr_harness_capture_uint_preview(const VkrCaptureItemResult *item,
                                             uint32_t component,
                                             uint32_t value_mask,
                                             uint8_t *rgba) {
  const uint64_t texel_stride =
      item->format == VKR_TEXTURE_FORMAT_R32G32_UINT ? 8u : 4u;
  for (uint32_t y = 0; y < item->height; ++y) {
    const uint8_t *row = vkr_harness_capture_row(item, y);
    uint8_t *target = rgba + (uint64_t)y * item->width * 4u;
    for (uint32_t x = 0; x < item->width; ++x) {
      const uint32_t source_id =
          vkr_harness_capture_read_u32(row + (uint64_t)x * texel_stride +
                                       (uint64_t)component * 4u) &
          value_mask;
      uint32_t id = source_id;
      id ^= id >> 16;
      id *= 0x7feb352du;
      id ^= id >> 15;
      id *= 0x846ca68bu;
      id ^= id >> 16;
      target[x * 4u + 0u] = source_id ? (uint8_t)id : 0u;
      target[x * 4u + 1u] = source_id ? (uint8_t)(id >> 8) : 0u;
      target[x * 4u + 2u] = source_id ? (uint8_t)(id >> 16) : 0u;
      target[x * 4u + 3u] = 255u;
    }
  }
}

/**
 * Writes the tightly packed little-endian 32-bit payload that comparison
 * actually reads: depth becomes float bits, identifiers keep their exact
 * value. `D16_UNORM` and `R16_SFLOAT` are widened here so one canonical
 * encoding covers every depth source.
 */
static void vkr_harness_capture_canonical_u32(const VkrCaptureItemResult *item,
                                              uint32_t component,
                                              uint32_t value_mask,
                                              uint8_t *tight) {
  const bool8_t depth = item->value_kind == VKR_CAPTURE_VALUE_DEPTH;
  const uint64_t texel_stride =
      item->format == VKR_TEXTURE_FORMAT_R32G32_UINT ? 8u : 4u;
  for (uint32_t y = 0; y < item->height; ++y) {
    const uint8_t *source = vkr_harness_capture_row(item, y);
    uint8_t *target = tight + (uint64_t)y * item->width * 4u;
    for (uint32_t x = 0; x < item->width; ++x) {
      uint32_t canonical;
      if (depth && (item->format == VKR_TEXTURE_FORMAT_D16_UNORM ||
                    item->format == VKR_TEXTURE_FORMAT_R16_SFLOAT)) {
        const float32_t value = vkr_harness_capture_depth_at(item, source, x);
        MemCopy(&canonical, &value, sizeof(canonical));
      } else {
        canonical =
            vkr_harness_capture_read_u32(source + (uint64_t)x * texel_stride +
                                         (uint64_t)component * 4u) &
            value_mask;
      }
      vkr_harness_capture_write_u32_le(target + (uint64_t)x * 4u, canonical);
    }
  }
}

/**
 * Emits the sidecar describing how one canonical payload was produced. Written
 * through the shared JSON writer so escaping, bounds, and atomic publication
 * are the writer's problem rather than a format string's.
 */
static bool8_t vkr_harness_capture_write_metadata(
    const char *path, const VkrHarnessCaptureResult *capture,
    float32_t display_exposure, float32_t preview_min, float32_t preview_max) {
  VkrJsonFileWriter file = {0};
  if (!vkr_json_file_writer_begin(
          &file, string8_create_from_cstr((const uint8_t *)path,
                                          string_length(path)))) {
    return false_v;
  }
  VkrJsonWriter *writer = &file.writer;
  const bool8_t depth = string_equals(capture->value_kind, "depth");
  const bool8_t ok =
      vkr_json_writer_begin_object(writer) &&
      vkr_harness_json_emit_u64(writer, "schema_version",
                                VKR_HARNESS_SCHEMA_VERSION) &&
      vkr_harness_json_emit_string(writer, "channel", capture->channel) &&
      vkr_harness_json_emit_u64(writer, "capture_version",
                                capture->capture_version) &&
      vkr_harness_json_emit_string(writer, "producer_resource",
                                   capture->producer_resource) &&
      vkr_harness_json_emit_string(writer, "value_kind", capture->value_kind) &&
      vkr_harness_json_emit_string(writer, "color_space",
                                   capture->color_space) &&
      vkr_harness_json_emit_string(writer, "source_format",
                                   capture->source_format) &&
      vkr_harness_json_emit_string(writer, "canonical_encoding",
                                   capture->canonical_encoding) &&
      vkr_harness_json_emit_string(writer, "origin", capture->origin) &&
      vkr_harness_json_emit_u64(writer, "width", capture->width) &&
      vkr_harness_json_emit_u64(writer, "height", capture->height) &&
      vkr_harness_json_emit_u64(writer, "source_row_pitch",
                                capture->source_row_pitch) &&
      vkr_harness_json_emit_u64(writer, "mip", capture->mip) &&
      vkr_harness_json_emit_u64(writer, "layer", capture->layer) &&
      vkr_harness_json_emit_f64(writer, "display_exposure", display_exposure) &&
      vkr_harness_json_emit_u64(writer, "source_frame_index",
                                capture->source_frame_index) &&
      vkr_harness_json_emit_u64(writer, "submit_serial",
                                capture->submit_serial) &&
      vkr_harness_json_emit_string(writer, "data_path", capture->data_path) &&
      vkr_harness_json_emit_string(writer, "data_sha256",
                                   capture->data_sha256) &&
      vkr_harness_json_emit_string(writer, "preview_path",
                                   capture->preview_path) &&
      vkr_harness_json_emit_string(writer, "preview_sha256",
                                   capture->preview_sha256) &&
      vkr_harness_json_emit_name(writer, "preview_normalization") &&
      vkr_json_writer_begin_object(writer) &&
      vkr_harness_json_emit_string(writer, "mode",
                                   depth ? "observed_range" : "identity") &&
      vkr_harness_json_emit_f64(writer, "min", (float64_t)preview_min) &&
      vkr_harness_json_emit_f64(writer, "max", (float64_t)preview_max) &&
      vkr_json_writer_end_object(writer) && vkr_json_writer_end_object(writer);
  if (!ok) {
    vkr_json_file_writer_abort(&file);
    return false_v;
  }
  return vkr_json_file_writer_commit(&file);
}

/** Fills the report row for one item, leaving only the digests to compute. */
static void vkr_harness_capture_describe(
    VkrHarnessCaptureResult *capture, const VkrCaptureItemResult *item,
    const VkrCaptureChannelDescription *channel,
    const VkrHarnessCaptureChannelDescription *logical_channel,
    const VkrCapturePollResult *poll, uint32_t checkpoint_frame,
    const char *stem, bool8_t color) {
  capture->checkpoint_frame = checkpoint_frame;
  /* The payload version belongs to the backend channel that produced it: a
     logical name only chooses renderer state, never the canonical encoding.
     Recording the backend's version is what invalidates a stored baseline when
     that encoding changes. */
  capture->capture_version = channel->version;
  string_format(capture->channel, sizeof(capture->channel), "%s",
                logical_channel->name);
  /* The backend reports which resource it actually copied; the catalog name is
     only a fallback for a producer that did not name itself. */
  string_format(capture->producer_resource, sizeof(capture->producer_resource),
                "%s",
                item->producer_resource[0] ? item->producer_resource
                                           : channel->source_name);
  string_format(capture->source_format, sizeof(capture->source_format), "%s",
                vkr_harness_capture_format_name(item->format));
  string_format(capture->canonical_encoding,
                sizeof(capture->canonical_encoding), "%s",
                channel->canonical_encoding);
  string_format(capture->value_kind, sizeof(capture->value_kind), "%s",
                vkr_harness_capture_value_name(item->value_kind));
  string_format(capture->color_space, sizeof(capture->color_space), "%s",
                vkr_harness_capture_color_space_name(item->color_space));
  string_format(capture->origin, sizeof(capture->origin), "top_left");
  capture->width = item->width;
  capture->height = item->height;
  capture->source_row_pitch = item->row_pitch;
  capture->mip = item->mip;
  capture->layer = item->layer;
  capture->source_frame_index = poll->source_frame_index;
  capture->submit_serial = poll->submit_serial;
  capture->comparison.outcome = VKR_HARNESS_COMPARISON_NOT_RUN;
  string_format(capture->comparison_status, sizeof(capture->comparison_status),
                "not_run");
  string_format(capture->data_path, sizeof(capture->data_path), "captures/%s%s",
                stem, color ? ".png" : ".raw");
  string_format(capture->preview_path, sizeof(capture->preview_path),
                "captures/%s.png", stem);
  string_format(capture->metadata_path, sizeof(capture->metadata_path),
                "captures/%s.json", stem);
}

bool8_t vkr_harness_capture_publish(
    const char *run_dir, uint32_t checkpoint_frame,
    const VkrCapturePollResult *poll, const char logical_channels[][64],
    uint32_t logical_channel_count, const VkrHarnessArenas *arenas,
    VkrHarnessReport *report, VkrHarnessError *error) {
  if (!run_dir || !poll || poll->status != VKR_CAPTURE_STATUS_READY ||
      !poll->items || !logical_channels ||
      logical_channel_count != poll->item_count || !arenas || !report ||
      !report->captures) {
    return false_v;
  }
  char capture_dir[VKR_HARNESS_PATH_MAX];
  string_format(capture_dir, sizeof(capture_dir), "%s/captures", run_dir);
  FilePath directory = vkr_harness_file_path(capture_dir);
  if (!file_create_directory(&directory)) {
    vkr_harness_error_set(error, "capture.directory", "captures",
                          "Unable to create '%s'", capture_dir);
    return false_v;
  }

  for (uint32_t i = 0; i < poll->item_count; ++i) {
    const VkrCaptureItemResult *item = &poll->items[i];
    const VkrCaptureChannelDescription *channel =
        vkr_renderer_capture_channel_get(item->channel);
    const VkrHarnessCaptureChannelDescription *logical_channel =
        vkr_harness_capture_channel_description(logical_channels[i]);
    const uint32_t uint_component =
        string_equals(logical_channels[i], "visibility_primitives") ||
                string_equals(logical_channels[i],
                              "transmission_visibility_primitives")
            ? 1u
            : 0u;
    const uint32_t uint_mask = uint_component == 1u ? 0x7fffffffu : UINT32_MAX;
    if (report->capture_count >= report->capture_capacity || !channel ||
        !logical_channel || !item->data) {
      vkr_harness_error_set(error, "capture.publish", "captures",
                            "Capture item %u cannot be published", i);
      return false_v;
    }
    /* Color is its own canonical payload, so its PNG is both data and preview
       and no separate raw file exists. */
    const bool8_t color = item->value_kind == VKR_CAPTURE_VALUE_COLOR;
    char stem[160];
    char png_path[VKR_HARNESS_PATH_MAX];
    char data_path[VKR_HARNESS_PATH_MAX];
    char metadata_path[VKR_HARNESS_PATH_MAX];
    string_format(stem, sizeof(stem), "frame_%06u_%s", checkpoint_frame,
                  logical_channel->name);
    string_format(png_path, sizeof(png_path), "%s/%s.png", capture_dir, stem);
    string_format(metadata_path, sizeof(metadata_path), "%s/%s.json",
                  capture_dir, stem);
    string_format(data_path, sizeof(data_path),
                  color ? "%s/%s.png" : "%s/%s.raw", capture_dir, stem);

    Scratch scratch = scratch_create(arenas->transient);
    const uint64_t pixel_bytes = (uint64_t)item->width * item->height * 4u;
    uint8_t *rgba =
        arena_alloc(arenas->transient, pixel_bytes, ARENA_MEMORY_TAG_ARRAY);
    uint8_t *tight = color ? rgba
                           : arena_alloc(arenas->transient, pixel_bytes,
                                         ARENA_MEMORY_TAG_ARRAY);
    float32_t preview_min = 0.0f;
    float32_t preview_max = 1.0f;
    bool8_t ok = rgba != NULL && tight != NULL;
    if (ok) {
      if (color) {
        vkr_harness_capture_color_rgba(item, rgba);
      } else if (item->value_kind == VKR_CAPTURE_VALUE_DEPTH) {
        vkr_harness_capture_depth_preview(item, rgba, &preview_min,
                                          &preview_max);
      } else {
        vkr_harness_capture_uint_preview(item, uint_component, uint_mask, rgba);
      }
    }

    VkrHarnessCaptureResult *capture = &report->captures[report->capture_count];
    if (ok && !color) {
      vkr_harness_capture_canonical_u32(item, uint_component, uint_mask, tight);
      ok = vkr_harness_atomic_write(data_path, tight, pixel_bytes, error);
    }
    ok = ok && vkr_harness_capture_png_write(png_path, rgba, item->width,
                                             item->height, arenas, error);
    if (ok) {
      vkr_harness_capture_describe(capture, item, channel, logical_channel,
                                   poll, checkpoint_frame, stem, color);
      /* Digests of the payloads must exist before the sidecar quotes them. */
      ok = vkr_harness_sha256_file(data_path, capture->data_sha256) &&
           vkr_harness_sha256_file(png_path, capture->preview_sha256) &&
           vkr_harness_capture_write_metadata(metadata_path, capture,
                                              item->display_exposure,
                                              preview_min, preview_max) &&
           vkr_harness_sha256_file(metadata_path, capture->metadata_sha256) &&
           vkr_harness_report_add_artifact(
               report, color ? "capture.color" : "capture.raw",
               capture->data_path,
               color ? "image/png" : "application/octet-stream", data_path) &&
           (color || vkr_harness_report_add_artifact(report, "capture.preview",
                                                     capture->preview_path,
                                                     "image/png", png_path)) &&
           vkr_harness_report_add_artifact(report, "capture.metadata",
                                           capture->metadata_path,
                                           "application/json", metadata_path);
    }
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    if (!ok) {
      /* Digest, sidecar, and artifact failures carry no reason of their own. */
      if (error && error->code[0] == '\0') {
        vkr_harness_error_set(error, "capture.publish", "captures",
                              "Unable to publish '%s'", stem);
      }
      return false_v;
    }
    report->capture_count++;
  }
  return true_v;
}

bool8_t vkr_harness_capture_summary_write(const char *path,
                                          const VkrHarnessReport *report,
                                          Arena *transient,
                                          VkrHarnessError *error) {
  if (!path || !report || !transient ||
      (report->capture_count && !report->captures) ||
      (report->artifact_count && !report->artifacts) ||
      report->capture_count > VKR_HARNESS_MAX_CAPTURE_RESULTS ||
      report->artifact_count > VKR_HARNESS_MAX_ARTIFACTS) {
    return false_v;
  }
  const uint64_t capture_bytes =
      (uint64_t)report->capture_count * sizeof(VkrHarnessCaptureResult);
  const uint64_t artifact_bytes =
      (uint64_t)report->artifact_count * sizeof(VkrHarnessArtifact);
  const uint64_t size =
      sizeof(VkrHarnessCaptureSummaryHeaderV4) + capture_bytes + artifact_bytes;
  Scratch scratch = scratch_create(transient);
  uint8_t *bytes = arena_alloc(transient, size, ARENA_MEMORY_TAG_ARRAY);
  if (!bytes) {
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    return false_v;
  }
  MemZero(bytes, size);
  VkrHarnessCaptureSummaryHeaderV4 *header =
      (VkrHarnessCaptureSummaryHeaderV4 *)bytes;
  MemCopy(header->magic, s_capture_summary_magic, sizeof(header->magic));
  header->version = 4u;
  header->capture_count = report->capture_count;
  header->artifact_count = report->artifact_count;
  header->tool = (uint32_t)report->tool;
  header->exit_code = (uint32_t)report->exit_code;
  header->authoritative = report->authoritative;
  header->profile_compatible = report->profile_compatible;
  string_format(header->status, sizeof(header->status), "%s", report->status);
  string_format(header->case_id, sizeof(header->case_id), "%s",
                report->case_manifest.id);
  string_format(header->case_manifest_sha256,
                sizeof(header->case_manifest_sha256), "%s",
                report->case_manifest.manifest_sha256);
  string_format(header->profile_id, sizeof(header->profile_id), "%s",
                report->profile.id);
  string_format(header->profile_manifest_sha256,
                sizeof(header->profile_manifest_sha256), "%s",
                report->profile.manifest_sha256);
  string_format(header->environment_fingerprint,
                sizeof(header->environment_fingerprint), "%s",
                report->environment_fingerprint);
  string_format(header->workload_fingerprint,
                sizeof(header->workload_fingerprint), "%s",
                report->workload_fingerprint);
  string_format(header->policy_fingerprint, sizeof(header->policy_fingerprint),
                "%s", report->policy_fingerprint);
  header->case_manifest = report->case_manifest;
  header->profile = report->profile;
  header->provenance = report->provenance;
  if (capture_bytes) {
    MemCopy(bytes + sizeof(*header), report->captures, capture_bytes);
  }
  if (artifact_bytes) {
    MemCopy(bytes + sizeof(*header) + capture_bytes, report->artifacts,
            artifact_bytes);
  }
  const bool8_t ok = vkr_harness_atomic_write(path, bytes, size, error);
  scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
  return ok;
}

bool8_t
vkr_harness_capture_summary_read(const char *path, Arena *arena,
                                 VkrHarnessCaptureSummary *out_summary) {
  if (!path || !arena || !out_summary) {
    return false_v;
  }
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (!vkr_harness_read_file(path, arena, &bytes, &size) ||
      size < offsetof(VkrHarnessCaptureSummaryHeaderV2, case_manifest)) {
    return false_v;
  }
  const VkrHarnessCaptureSummaryHeaderV2 *common =
      (const VkrHarnessCaptureSummaryHeaderV2 *)bytes;
  if (MemCompare(common->magic, s_capture_summary_magic,
                 sizeof(common->magic)) != 0 ||
      (common->version != 2u && common->version != 3u &&
       common->version != 4u) ||
      common->tool > VKR_HARNESS_TOOL_COMPARE ||
      common->exit_code > VKR_HARNESS_EXIT_ERROR ||
      common->capture_count > VKR_HARNESS_MAX_CAPTURE_RESULTS ||
      common->artifact_count > VKR_HARNESS_MAX_ARTIFACTS) {
    return false_v;
  }
  const uint64_t header_size =
      common->version == 2u   ? sizeof(VkrHarnessCaptureSummaryHeaderV2)
      : common->version == 3u ? sizeof(VkrHarnessCaptureSummaryHeaderV3)
                              : sizeof(VkrHarnessCaptureSummaryHeaderV4);
  const uint64_t capture_bytes =
      (uint64_t)common->capture_count * sizeof(VkrHarnessCaptureResult);
  const uint64_t artifact_bytes =
      (uint64_t)common->artifact_count * sizeof(VkrHarnessArtifact);
  if (size != header_size + capture_bytes + artifact_bytes) {
    return false_v;
  }
  out_summary->captures =
      (const VkrHarnessCaptureResult *)(bytes + header_size);
  out_summary->tool = (VkrHarnessTool)common->tool;
  out_summary->exit_code = (VkrHarnessExitCode)common->exit_code;
  out_summary->authoritative = common->authoritative;
  out_summary->profile_compatible = common->profile_compatible;
  string_format(out_summary->status, sizeof(out_summary->status), "%s",
                common->status);
  string_format(out_summary->case_id, sizeof(out_summary->case_id), "%s",
                common->case_id);
  string_format(out_summary->case_manifest_sha256,
                sizeof(out_summary->case_manifest_sha256), "%s",
                common->case_manifest_sha256);
  string_format(out_summary->profile_id, sizeof(out_summary->profile_id), "%s",
                common->profile_id);
  string_format(out_summary->profile_manifest_sha256,
                sizeof(out_summary->profile_manifest_sha256), "%s",
                common->profile_manifest_sha256);
  string_format(out_summary->environment_fingerprint,
                sizeof(out_summary->environment_fingerprint), "%s",
                common->environment_fingerprint);
  string_format(out_summary->workload_fingerprint,
                sizeof(out_summary->workload_fingerprint), "%s",
                common->workload_fingerprint);
  string_format(out_summary->policy_fingerprint,
                sizeof(out_summary->policy_fingerprint), "%s",
                common->policy_fingerprint);
  if (common->version == 2u) {
    vkr_harness_case_from_v3(&common->case_manifest,
                             &out_summary->case_manifest);
    vkr_harness_profile_from_v2(&common->profile, &out_summary->profile);
    out_summary->provenance = common->provenance;
  } else if (common->version == 3u) {
    const VkrHarnessCaptureSummaryHeaderV3 *header =
        (const VkrHarnessCaptureSummaryHeaderV3 *)bytes;
    vkr_harness_case_from_v3(&header->case_manifest,
                             &out_summary->case_manifest);
    out_summary->profile = header->profile;
    out_summary->provenance = header->provenance;
  } else {
    const VkrHarnessCaptureSummaryHeaderV4 *header =
        (const VkrHarnessCaptureSummaryHeaderV4 *)bytes;
    out_summary->case_manifest = header->case_manifest;
    out_summary->profile = header->profile;
    out_summary->provenance = header->provenance;
  }
  out_summary->capture_count = common->capture_count;
  out_summary->artifacts =
      (const VkrHarnessArtifact *)(bytes + header_size + capture_bytes);
  out_summary->artifact_count = common->artifact_count;
  return true_v;
}
