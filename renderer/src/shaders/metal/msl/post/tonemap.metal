struct alignas(16) VkrMetalPacketTonemapRoot {
  texture2d<float, access::sample> source;
  uint flags;
  float image_sharpness;
  // Always valid. Manual frames bind a frame-upload fallback record and
  // automatic frames bind the graph resolve output.
  device const VkrExposureState *exposure_state;
  uint2 output_extent;
  device const VkrColorGrading *color_grading;
  constant VkrDisplayOutputParams *display_output;
  // Returns pre-exposed radiance to scene-linear before exposure.
  float inverse_pre_exposure;
  // With VKR_METAL_PACKET_TONEMAP_FLAG_BLOOM, the bloom chain's first level,
  // added to the sampled scene at this intensity.
  float bloom_intensity;
  texture2d<float, access::sample> bloom;
};

static constant uint VKR_METAL_PACKET_TONEMAP_FLAG_ALREADY_OUTPUT_ENCODED =
    1u << 3u;
static constant uint VKR_METAL_PACKET_TONEMAP_FLAG_SOURCE_DISPLAY_LINEAR = 1u << 5u;
static constant uint VKR_METAL_PACKET_TONEMAP_FLAG_PREPARE_DISPLAY_LINEAR = 1u << 6u;
static constant uint VKR_METAL_PACKET_TONEMAP_FLAG_SCENE_BLUR = 1u << 7u;
// The pass adds bloom to the scene-linear samples itself, as Post.Bloom.Combine
// does in graphs that have it (ADR-087).
static constant uint VKR_METAL_PACKET_TONEMAP_FLAG_BLOOM = 1u << 8u;
// The exposed scene shows in false colour by stops (ADR-099).
static constant uint VKR_METAL_PACKET_TONEMAP_FLAG_FALSE_COLOR = 1u << 9u;

// The bloom a tonemap tap adds to its scene-linear sample.
struct VkrMetalPacketTonemapBloom {
  texture2d<float, access::sample> texture;
  float intensity;
};

static float3 vkr_metal_packet_aces_fitted(float3 color) {
  const float a = 2.51;
  const float b = 0.03;
  const float c = 2.43;
  const float d = 0.59;
  const float e = 0.14;
  return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0,
               1.0);
}

static float4
vkr_metal_packet_post_sample(texture2d<float, access::sample> source,
                             sampler source_sampler, float2 uv, float exposure,
                             uint flags, VkrColorGrading grading,
                             VkrDisplayOutputParams display_output,
                             VkrMetalPacketTonemapBloom bloom) {
  float4 hdr = source.sample(source_sampler, uv);
  // The editor Scene composite samples presentation-linear pixels from the
  // retained scene image. They already carry the physical-output scale, so a
  // second grade or SDR clamp would destroy extended-linear highlights.
  if ((flags & (VKR_METAL_PACKET_TONEMAP_FLAG_ALREADY_OUTPUT_ENCODED |
                VKR_METAL_PACKET_TONEMAP_FLAG_SOURCE_DISPLAY_LINEAR)) != 0u) {
    return hdr;
  }
  // The scene's alpha may carry the tiled opaque pass's surface share
  // (vkr_metal_tiled_resolve_tile); the image it presents is opaque.
  hdr.a = 1.0;
  if ((flags & VKR_METAL_PACKET_TONEMAP_FLAG_BLOOM) != 0u) {
    // The combine pass stored its sum in RGBA16F.
    float3 bloomed = hdr.rgb + bloom.texture.sample(source_sampler, uv).rgb *
                                   bloom.intensity;
    hdr.rgb = float3(half3(bloomed));
  }
  if ((flags & VKR_METAL_PACKET_TONEMAP_FLAG_FALSE_COLOR) != 0u) {
    const float luminance =
        dot(max(hdr.rgb * exposure, 0.0), float3(0.2126, 0.7152, 0.0722));
    hdr.rgb = vkr_editor_ev_color(log2(max(luminance, 1e-6) / 0.18));
    return hdr;
  }
  float3 color = vkr_color_grade(max(hdr.rgb * exposure, 0.0), grading);
  float3 display_linear =
      (flags & 1u) != 0u
          ? ((flags & 4u) != 0u ? vkr_metal_packet_aces_fitted(color)
                                : vkr_agx_tonemap(color))
          : clamp(color, 0.0, 1.0);
  hdr.rgb = (flags & 1u) != 0u
                ? vkr_display_output_scene_relative(color, display_linear,
                                                    display_output)
                : display_linear;
  return hdr;
}

static float4 vkr_metal_packet_finish_output(
    float4 color, uint flags, VkrDisplayOutputParams display_output) {
  if ((flags & (VKR_METAL_PACKET_TONEMAP_FLAG_ALREADY_OUTPUT_ENCODED |
                VKR_METAL_PACKET_TONEMAP_FLAG_PREPARE_DISPLAY_LINEAR)) == 0u) {
    color.rgb = vkr_display_output_scale(color.rgb, display_output);
  }
  return color;
}

vertex VkrMetalPacketTonemapOutput
vkr_metal_packet_tonemap_vertex(uint vertex_id [[vertex_id]]) {
  const float2 positions[3] = {float2(-1.0, -1.0), float2(3.0, -1.0),
                               float2(-1.0, 3.0)};
  VkrMetalPacketTonemapOutput output;
  output.position = float4(positions[vertex_id], 0.0, 1.0);
  output.texcoord = float2(positions[vertex_id].x * 0.5 + 0.5,
                           0.5 - positions[vertex_id].y * 0.5);
  return output;
}

fragment float4 vkr_metal_packet_tonemap_fragment(
    VkrMetalPacketTonemapOutput input [[stage_in]],
    constant VkrMetalPacketTonemapRoot *root [[buffer(1)]]) {
  constexpr sampler source_sampler(coord::normalized, address::clamp_to_edge,
                                   filter::linear);
  float2 uv = input.texcoord;
  float exposure =
      root->exposure_state->exposure_multiplier * root->inverse_pre_exposure;
  uint flags = root->flags;
  float sharpness = root->image_sharpness;
  VkrDisplayOutputParams display_output = *root->display_output;
  VkrMetalPacketTonemapBloom bloom = {root->bloom, root->bloom_intensity};
  if ((flags & VKR_METAL_PACKET_TONEMAP_FLAG_SCENE_BLUR) != 0u) {
    float4 blurred = float4(0.0);
    for (int y = -2; y <= 2; ++y) {
      for (int x = -2; x <= 2; ++x) {
        blurred += root->source.sample(source_sampler,
            uv + vkr_scene_blur_offset(x, y, root->output_extent)) *
            vkr_scene_blur_weight(x) * vkr_scene_blur_weight(y);
      }
    }
    return vkr_metal_packet_finish_output(blurred, flags, display_output);
  }
  float4 result = vkr_metal_packet_post_sample(root->source, source_sampler, uv,
                                              exposure, flags, *root->color_grading,
                                              display_output, bloom);
  if (sharpness > 0.0) {
    float2 step = 1.0 / float2(root->output_extent);
    float3 north = vkr_metal_packet_post_sample(root->source, source_sampler,
        uv - float2(0.0, step.y), exposure, flags, *root->color_grading,
        display_output, bloom).rgb;
    float3 south = vkr_metal_packet_post_sample(root->source, source_sampler,
        uv + float2(0.0, step.y), exposure, flags, *root->color_grading,
        display_output, bloom).rgb;
    float3 west = vkr_metal_packet_post_sample(root->source, source_sampler,
        uv - float2(step.x, 0.0), exposure, flags, *root->color_grading,
        display_output, bloom).rgb;
    float3 east = vkr_metal_packet_post_sample(root->source, source_sampler,
        uv + float2(step.x, 0.0), exposure, flags, *root->color_grading,
        display_output, bloom).rgb;
    result.rgb = vkr_sharpen_color(result.rgb,
        0.25 * (north + south + west + east),
        min(min(north, south), min(west, east)),
        max(max(north, south), max(west, east)), sharpness);
  }
  return vkr_metal_packet_finish_output(result, flags, display_output);
}

static_assert(sizeof(VkrMetalPacketTonemapRoot) == 64,
              "Tonemap root ABI must remain 64 bytes");
