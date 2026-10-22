/* Screen-space sun shafts on the tiled pipeline (ADR-132), after GPU Gems 3
 * chapter 13. Two dispatches at half the internal resolution:
 *
 * - The source: each texel's share of sky, reversed depth 0 (ADR-109), over
 *   its 2x2 block of the resolved depth, scaled by its nearness to the sun's
 *   place on the screen. Sky counts fully at the sun and less with distance
 *   from it, so the light streams out from the sun rather than lifting the
 *   whole sky.
 * - The march: each texel samples the source with bilinear filtering toward
 *   the sun and keeps the weighted mean. Samples nearer the texel weigh
 *   more, so a surface between the texel and the sun casts a dark shaft that
 *   fades away from it. Frames that MetalFX accumulates start each march at
 *   another offset, which the upscaler averages. The texel then takes the
 *   fog's Henyey-Greenstein phase toward its view ray and the cloud shadow
 *   over the camera, so the atmosphere draw only scales it by the sun's
 *   light and the share of it the fog scatters (world/tiled.metal). */

constant uint VKR_SUN_SHAFTS_SAMPLES = 24u;
// The longest march, in view heights: a texel farther from the sun looks
// only this far toward it.
constant float VKR_SUN_SHAFTS_LENGTH = 0.5f;
// Each sample's weight over the one before it, from the texel toward the
// sun: the last of the 24 weighs about a fifth of the first.
constant float VKR_SUN_SHAFTS_DECAY = 0.935f;
// The distance from the sun, in view heights, at which sky stops counting;
// it counts as the square of the remaining fraction of this radius.
constant float VKR_SUN_SHAFTS_SOURCE_RADIUS = 0.5f;

// Mirrors VkrMetalTiledSunShaftsRoot. The source is one texture, written
// through `source_output` by the first dispatch and sampled through `source`
// by the second.
struct VkrMetalTiledSunShaftsRoot {
  depth2d<float, access::sample> depth;
  texture2d<float, access::write> output;
  texture2d<float, access::write> source_output;
  texture2d<float, access::sample> source;
  // Null without a published sky.
  constant VkrMetalPacketSky *sky;
  uint noise_index;
  // The depth's width over its height.
  float aspect;
  // The camera-relative view directions (vkr_atmosphere_view_rays).
  float4x4 view_rays;
  // xyz the direction to the sun, w the fog's anisotropy.
  float4 sun_direction_anisotropy;
  // xyz the camera's position.
  float4 camera_position;
  // The sun's place in texture coordinates, y down.
  float2 sun_uv;
  // One over the depth texture's size.
  float2 depth_texel;
  uint2 extent;
  uint reserved[2];
};

static_assert(sizeof(VkrMetalTiledSunShaftsRoot) == 176,
              "Metal tiled sun shafts root ABI drift");

kernel void
vkr_metal_tiled_sun_shafts_source(constant VkrMetalTiledSunShaftsRoot &root
                                  [[buffer(0)]],
                                  uint2 texel [[thread_position_in_grid]]) {
  if (any(texel >= root.extent)) {
    return;
  }
  // One gather reads the texel's 2x2 block of the resolved depth; past the
  // depth's edge it reads the edge.
  constexpr sampler block_sampler(coord::normalized, address::clamp_to_edge,
                                  filter::nearest);
  const float4 depths = root.depth.gather(
      block_sampler, float2(texel * 2u + 1u) * root.depth_texel);
  const float sky =
      dot(select(float4(0.0f), float4(0.25f), depths <= 0.0f), float4(1.0f));
  const float2 uv = (float2(texel) + 0.5f) / float2(root.extent);
  const float sun_distance =
      length((root.sun_uv - uv) * float2(root.aspect, 1.0f));
  const float nearness =
      saturate(1.0f - sun_distance / VKR_SUN_SHAFTS_SOURCE_RADIUS);
  root.source_output.write(float4(sky * nearness * nearness), texel);
}

kernel void vkr_metal_tiled_sun_shafts(constant VkrMetalTiledSunShaftsRoot &root
                                       [[buffer(0)]],
                                       uint2 texel [[thread_position_in_grid]]) {
  if (any(texel >= root.extent)) {
    return;
  }
  const float2 pixel_center = float2(texel) + 0.5f;
  const float2 uv = pixel_center / float2(root.extent);

  // The way to the sun, shortened to the longest march, measured in view
  // heights so that it spans the same distance along both axes.
  float2 span = root.sun_uv - uv;
  const float sun_distance = length(span * float2(root.aspect, 1.0f));
  span *= min(1.0f, VKR_SUN_SHAFTS_LENGTH / max(sun_distance, 1e-4f));
  const float2 stride = span / float(VKR_SUN_SHAFTS_SAMPLES);
  const float noise = vkr_local_shadow_contact_noise(texel, root.noise_index);

  // Every sample reads without waiting for the last, so the reads overlap.
  constexpr sampler source_sampler(coord::normalized, address::clamp_to_edge,
                                   filter::linear);
  float lit = 0.0f;
  float weight = 1.0f;
  float weight_sum = 0.0f;
  for (uint i = 0u; i < VKR_SUN_SHAFTS_SAMPLES; ++i) {
    const float2 sample_uv = uv + stride * (float(i) + noise);
    lit += weight *
           root.source.sample(source_sampler, sample_uv, level(0.0f)).r;
    weight_sum += weight;
    weight *= VKR_SUN_SHAFTS_DECAY;
  }

  const float3 direction =
      vkr_metal_packet_sky_direction(root.view_rays, root.extent, pixel_center);
  const float phase =
      vkr_fog_phase_hg(root.sun_direction_anisotropy.w,
                       dot(direction, root.sun_direction_anisotropy.xyz));
  const float cloud_shadow = vkr_metal_packet_sky_cloud_shadow(
      root.sky, root.camera_position.xyz);
  root.output.write(float4(lit / weight_sum * phase * cloud_shadow), texel);
}
