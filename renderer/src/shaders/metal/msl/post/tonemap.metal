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
};

static constant uint VKR_METAL_PACKET_TONEMAP_FLAG_ALREADY_OUTPUT_ENCODED =
    1u << 3u;
static constant uint VKR_METAL_PACKET_TONEMAP_FLAG_OPAQUE_ALPHA = 1u << 4u;
static constant uint VKR_METAL_PACKET_TONEMAP_FLAG_SOURCE_DISPLAY_LINEAR = 1u << 5u;
static constant uint VKR_METAL_PACKET_TONEMAP_FLAG_PREPARE_DISPLAY_LINEAR = 1u << 6u;

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
                             VkrDisplayOutputParams display_output) {
  float4 hdr = source.sample(source_sampler, uv);
  // The editor Scene composite samples presentation-linear pixels from the
  // retained scene image. They already carry the physical-output scale, so a
  // second grade or SDR clamp would destroy extended-linear highlights.
  if ((flags & (VKR_METAL_PACKET_TONEMAP_FLAG_ALREADY_OUTPUT_ENCODED |
                VKR_METAL_PACKET_TONEMAP_FLAG_SOURCE_DISPLAY_LINEAR)) != 0u) {
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
  // Post-MetalFX alpha carries private history age, never scene opacity.
  if ((flags & VKR_METAL_PACKET_TONEMAP_FLAG_OPAQUE_ALPHA) != 0u)
    color.a = 1.0f;
  return color;
}

static float vkr_metal_packet_fxaa_luminance(float3 color) {
  return dot(sqrt(max(color, 0.0)), float3(0.2126, 0.7152, 0.0722));
}

static float4 vkr_metal_packet_fxaa(texture2d<float, access::sample> source,
                                    sampler source_sampler, float2 uv,
                                    float2 inverse_extent, float exposure,
                                    uint flags, float sharpness,
                                    VkrColorGrading grading,
                                    VkrDisplayOutputParams display_output) {
  float4 center = vkr_metal_packet_post_sample(source, source_sampler, uv,
                                               exposure, flags, grading,
                                               display_output);
  float4 north = vkr_metal_packet_post_sample(
      source, source_sampler, uv - float2(0.0, inverse_extent.y), exposure,
      flags, grading, display_output);
  float4 south = vkr_metal_packet_post_sample(
      source, source_sampler, uv + float2(0.0, inverse_extent.y), exposure,
      flags, grading, display_output);
  float4 west = vkr_metal_packet_post_sample(source, source_sampler,
                                             uv - float2(inverse_extent.x, 0.0),
                                             exposure, flags, grading,
                                             display_output);
  float4 east = vkr_metal_packet_post_sample(source, source_sampler,
                                             uv + float2(inverse_extent.x, 0.0),
                                             exposure, flags, grading,
                                             display_output);
  float4 northwest = vkr_metal_packet_post_sample(
      source, source_sampler, uv + float2(-1.0, -1.0) * inverse_extent,
      exposure, flags, grading, display_output);
  float4 northeast = vkr_metal_packet_post_sample(
      source, source_sampler, uv + float2(1.0, -1.0) * inverse_extent, exposure,
      flags, grading, display_output);
  float4 southwest = vkr_metal_packet_post_sample(
      source, source_sampler, uv + float2(-1.0, 1.0) * inverse_extent, exposure,
      flags, grading, display_output);
  float4 southeast = vkr_metal_packet_post_sample(
      source, source_sampler, uv + inverse_extent, exposure, flags, grading,
      display_output);
  float luma_center = vkr_metal_packet_fxaa_luminance(center.rgb);
  float luma_northwest = vkr_metal_packet_fxaa_luminance(northwest.rgb);
  float luma_northeast = vkr_metal_packet_fxaa_luminance(northeast.rgb);
  float luma_southwest = vkr_metal_packet_fxaa_luminance(southwest.rgb);
  float luma_southeast = vkr_metal_packet_fxaa_luminance(southeast.rgb);
  float luma_north = vkr_metal_packet_fxaa_luminance(north.rgb);
  float luma_south = vkr_metal_packet_fxaa_luminance(south.rgb);
  float luma_west = vkr_metal_packet_fxaa_luminance(west.rgb);
  float luma_east = vkr_metal_packet_fxaa_luminance(east.rgb);
  float luma_min =
      min(luma_center,
          min(min(min(luma_north, luma_south), min(luma_west, luma_east)),
              min(min(luma_northwest, luma_northeast),
                  min(luma_southwest, luma_southeast))));
  float luma_max =
      max(luma_center,
          max(max(max(luma_north, luma_south), max(luma_west, luma_east)),
              max(max(luma_northwest, luma_northeast),
                  max(luma_southwest, luma_southeast))));
  if (luma_max - luma_min < max(0.0312, luma_max * 0.125)) {
    if (sharpness > 0.0)
      center.rgb = vkr_sharpen_color(
          center.rgb, 0.25 * (north.rgb + south.rgb + west.rgb + east.rgb),
          min(min(min(north.rgb, south.rgb), min(west.rgb, east.rgb)),
              min(min(northwest.rgb, northeast.rgb), min(southwest.rgb, southeast.rgb))),
          max(max(max(north.rgb, south.rgb), max(west.rgb, east.rgb)),
              max(max(northwest.rgb, northeast.rgb), max(southwest.rgb, southeast.rgb))),
          sharpness);
    return center;
  }

  float2 direction;
  direction.x =
      -((luma_northwest + luma_northeast) - (luma_southwest + luma_southeast));
  direction.y =
      (luma_northwest + luma_southwest) - (luma_northeast + luma_southeast);
  float direction_reduce =
      max((luma_northwest + luma_northeast + luma_southwest + luma_southeast) *
              0.03125,
          0.0078125);
  float inverse_direction_min =
      1.0 / (min(abs(direction.x), abs(direction.y)) + direction_reduce);
  direction =
      clamp(direction * inverse_direction_min, -8.0, 8.0) * inverse_extent;
  float4 result_a =
      0.5 * (vkr_metal_packet_post_sample(source, source_sampler,
                                          uv + direction * (1.0 / 3.0 - 0.5),
                                          exposure, flags, grading,
                                          display_output) +
             vkr_metal_packet_post_sample(source, source_sampler,
                                          uv + direction * (2.0 / 3.0 - 0.5),
                                          exposure, flags, grading,
                                          display_output));
  float4 result_b = result_a * 0.5 +
                    0.25 * (vkr_metal_packet_post_sample(source, source_sampler,
                                                         uv + direction * -0.5,
                                                         exposure, flags, grading,
                                                         display_output) +
                            vkr_metal_packet_post_sample(source, source_sampler,
                                                         uv + direction * 0.5,
                                                         exposure, flags, grading,
                                                         display_output));
  float result_b_luma = vkr_metal_packet_fxaa_luminance(result_b.rgb);
  float4 result = result_b_luma < luma_min || result_b_luma > luma_max
                      ? result_a
                      : result_b;
  float luma_range = luma_max - luma_min;
  float luma_average =
      (2.0 * (luma_north + luma_south + luma_west + luma_east) +
       luma_northwest + luma_northeast + luma_southwest + luma_southeast) /
      12.0;
  float subpixel =
      clamp(abs(luma_average - luma_center) / luma_range, 0.0, 1.0);
  subpixel = subpixel * subpixel * (3.0 - 2.0 * subpixel);
  subpixel = subpixel * subpixel * 0.75;
  result = mix(result, 0.25 * (north + south + west + east), subpixel);
  if (sharpness > 0.0)
    result.rgb = vkr_sharpen_color(
        result.rgb, 0.25 * (north.rgb + south.rgb + west.rgb + east.rgb),
        min(min(min(north.rgb, south.rgb), min(west.rgb, east.rgb)),
            min(min(northwest.rgb, northeast.rgb), min(southwest.rgb, southeast.rgb))),
        max(max(max(north.rgb, south.rgb), max(west.rgb, east.rgb)),
            max(max(northwest.rgb, northeast.rgb), max(southwest.rgb, southeast.rgb))),
        sharpness * (1.0 - subpixel));
  result.a = center.a;
  return result;
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
  float exposure = root->exposure_state->exposure_multiplier;
  uint flags = root->flags;
  float sharpness = root->image_sharpness;
  VkrDisplayOutputParams display_output = *root->display_output;
  if ((root->flags & 2u) == 0u) {
    float4 result = vkr_metal_packet_post_sample(root->source, source_sampler, uv,
                                                exposure, flags, *root->color_grading,
                                                display_output);
    if (sharpness > 0.0) {
      float2 step = 1.0 / float2(root->output_extent);
      float3 north = vkr_metal_packet_post_sample(root->source, source_sampler,
          uv - float2(0.0, step.y), exposure, flags, *root->color_grading,
          display_output).rgb;
      float3 south = vkr_metal_packet_post_sample(root->source, source_sampler,
          uv + float2(0.0, step.y), exposure, flags, *root->color_grading,
          display_output).rgb;
      float3 west = vkr_metal_packet_post_sample(root->source, source_sampler,
          uv - float2(step.x, 0.0), exposure, flags, *root->color_grading,
          display_output).rgb;
      float3 east = vkr_metal_packet_post_sample(root->source, source_sampler,
          uv + float2(step.x, 0.0), exposure, flags, *root->color_grading,
          display_output).rgb;
      result.rgb = vkr_sharpen_color(result.rgb,
          0.25 * (north + south + west + east),
          min(min(north, south), min(west, east)),
          max(max(north, south), max(west, east)), sharpness);
    }
    return vkr_metal_packet_finish_output(result, flags, display_output);
  }
  return vkr_metal_packet_finish_output(
      vkr_metal_packet_fxaa(root->source, source_sampler, uv,
                            1.0 / float2(root->output_extent), exposure,
                            flags, sharpness, *root->color_grading,
                            display_output),
      flags, display_output);
}

static_assert(sizeof(VkrMetalPacketTonemapRoot) == 48,
              "Tonemap root ABI must remain 48 bytes");
