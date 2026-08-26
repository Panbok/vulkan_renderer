struct VkrMetalPacketTonemapRoot {
  texture2d<float, access::sample> source;
  float exposure;
  uint3 reserved;
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

static float4 vkr_metal_packet_post_sample(
    texture2d<float, access::sample> source, sampler source_sampler, float2 uv,
    float exposure, bool tonemap) {
  float4 hdr = source.sample(source_sampler, uv);
  float3 color = max(hdr.rgb * exposure, 0.0);
  hdr.rgb = tonemap ? vkr_metal_packet_aces_fitted(color)
                    : clamp(color, 0.0, 1.0);
  return hdr;
}

static float vkr_metal_packet_fxaa_luminance(float3 color) {
  return dot(sqrt(max(color, 0.0)), float3(0.2126, 0.7152, 0.0722));
}

static float4 vkr_metal_packet_fxaa(
    texture2d<float, access::sample> source, sampler source_sampler, float2 uv,
    float2 inverse_extent, float exposure, bool tonemap) {
  float4 center = vkr_metal_packet_post_sample(
      source, source_sampler, uv, exposure, tonemap);
  float4 north = vkr_metal_packet_post_sample(
      source, source_sampler, uv - float2(0.0, inverse_extent.y), exposure,
      tonemap);
  float4 south = vkr_metal_packet_post_sample(
      source, source_sampler, uv + float2(0.0, inverse_extent.y), exposure,
      tonemap);
  float4 west = vkr_metal_packet_post_sample(
      source, source_sampler, uv - float2(inverse_extent.x, 0.0), exposure,
      tonemap);
  float4 east = vkr_metal_packet_post_sample(
      source, source_sampler, uv + float2(inverse_extent.x, 0.0), exposure,
      tonemap);
  float4 northwest = vkr_metal_packet_post_sample(
      source, source_sampler, uv + float2(-1.0, -1.0) * inverse_extent,
      exposure, tonemap);
  float4 northeast = vkr_metal_packet_post_sample(
      source, source_sampler, uv + float2(1.0, -1.0) * inverse_extent, exposure,
      tonemap);
  float4 southwest = vkr_metal_packet_post_sample(
      source, source_sampler, uv + float2(-1.0, 1.0) * inverse_extent, exposure,
      tonemap);
  float4 southeast = vkr_metal_packet_post_sample(
      source, source_sampler, uv + inverse_extent, exposure, tonemap);
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
  if (luma_max - luma_min < max(0.0312, luma_max * 0.125))
    return center;

  float2 direction;
  direction.x = -((luma_northwest + luma_northeast) -
                  (luma_southwest + luma_southeast));
  direction.y = (luma_northwest + luma_southwest) -
                (luma_northeast + luma_southeast);
  float direction_reduce =
      max((luma_northwest + luma_northeast + luma_southwest +
           luma_southeast) *
              0.03125,
          0.0078125);
  float inverse_direction_min =
      1.0 / (min(abs(direction.x), abs(direction.y)) + direction_reduce);
  direction =
      clamp(direction * inverse_direction_min, -8.0, 8.0) * inverse_extent;
  float4 result_a =
      0.5 *
      (vkr_metal_packet_post_sample(
           source, source_sampler, uv + direction * (1.0 / 3.0 - 0.5),
           exposure, tonemap) +
       vkr_metal_packet_post_sample(
           source, source_sampler, uv + direction * (2.0 / 3.0 - 0.5),
           exposure, tonemap));
  float4 result_b =
      result_a * 0.5 +
      0.25 *
          (vkr_metal_packet_post_sample(source, source_sampler,
                                        uv + direction * -0.5, exposure,
                                        tonemap) +
           vkr_metal_packet_post_sample(source, source_sampler,
                                        uv + direction * 0.5, exposure,
                                        tonemap));
  float result_b_luma = vkr_metal_packet_fxaa_luminance(result_b.rgb);
  float4 result =
      result_b_luma < luma_min || result_b_luma > luma_max ? result_a
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
  result.a = center.a;
  return result;
}

vertex VkrMetalPacketTonemapOutput
vkr_metal_packet_tonemap_vertex(uint vertex_id [[vertex_id]]) {
  const float2 positions[3] = {float2(-1.0, -1.0), float2(3.0, -1.0),
                               float2(-1.0, 3.0)};
  VkrMetalPacketTonemapOutput output;
  output.position = float4(positions[vertex_id], 0.0, 1.0);
  return output;
}

fragment float4 vkr_metal_packet_tonemap_fragment(
    VkrMetalPacketTonemapOutput input [[stage_in]],
    constant VkrMetalPacketTonemapRoot *root [[buffer(1)]]) {
  uint2 extent(root->source.get_width(), root->source.get_height());
  uint2 pixel = min(uint2(input.position.xy), extent - 1u);
  constexpr sampler source_sampler(coord::normalized, address::clamp_to_edge,
                                   filter::linear);
  float2 uv = (float2(pixel) + 0.5) / float2(extent);
  if (root->reserved.y == 0u)
    return vkr_metal_packet_post_sample(
        root->source, source_sampler, uv, root->exposure,
        root->reserved.x != 0u);
  return vkr_metal_packet_fxaa(root->source, source_sampler, uv,
                               1.0 / float2(extent), root->exposure,
                               root->reserved.x != 0u);
}
