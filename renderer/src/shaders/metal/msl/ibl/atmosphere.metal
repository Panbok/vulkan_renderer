struct alignas(16) VkrMetalPacketAtmosphereRoot {
  VkrAtmosphereParams params;
  texture2d<float, access::sample> transmittance_sample;
  texture2d<float, access::write> transmittance_storage;
  texture2d<float, access::sample> multiple_scattering_sample;
  texture2d<float, access::write> multiple_scattering_storage;
  texturecube<float, access::write> source_storage;
  device float4 *sun_output;
  uint2 extent;
  uint face_size;
  uint reserved;
};

// Metal resource references occupy eight bytes. Six references follow the
// 128-byte parameter record, so the scalar tail starts at byte 176 and the
// root rounds to 192 bytes.
static_assert(sizeof(VkrMetalPacketAtmosphereRoot) == 192u,
              "Metal atmosphere root ABI");

static float3
vkr_metal_atmosphere_transmittance(constant VkrMetalPacketAtmosphereRoot &root,
                                   float3 position, float3 direction) {
  bool hits_ground = false;
  float distance = vkr_atmosphere_segment_limit(root.params, position,
                                                direction, hits_ground);
  if (distance <= 0.0f || hits_ground)
    return float3(0.0f);
  float step = distance / float(VKR_ATMOSPHERE_TRANSMITTANCE_SAMPLES);
  float3 optical_depth = float3(0.0f);
  for (uint index = 0u; index < VKR_ATMOSPHERE_TRANSMITTANCE_SAMPLES; ++index) {
    float3 sample_position =
        position + direction * ((float(index) + 0.5f) * step);
    optical_depth +=
        vkr_atmosphere_medium(root.params, sample_position).extinction * step;
  }
  return exp(-min(optical_depth, VKR_ATMOSPHERE_MAX_OPTICAL_DEPTH));
}

static float3 vkr_metal_atmosphere_sample_transmittance(
    constant VkrMetalPacketAtmosphereRoot &root, float3 position,
    float3 direction) {
  float radius = length(position);
  float3 up = vkr_atmosphere_safe_normalize(position, float3(0.0f, 1.0f, 0.0f));
  float2 uv =
      vkr_atmosphere_transmittance_uv(root.params, radius, dot(up, direction));
  constexpr sampler linear_sampler(coord::normalized, address::clamp_to_edge,
                                   filter::linear);
  return root.transmittance_sample.sample(linear_sampler, uv).rgb;
}

static float3 vkr_metal_atmosphere_sample_multiple(
    constant VkrMetalPacketAtmosphereRoot &root, float3 position,
    float3 sun_direction) {
  float radius = length(position);
  float3 up = vkr_atmosphere_safe_normalize(position, float3(0.0f, 1.0f, 0.0f));
  float2 uv = vkr_atmosphere_multi_scattering_uv(root.params, radius,
                                                 dot(up, sun_direction));
  constexpr sampler linear_sampler(coord::normalized, address::clamp_to_edge,
                                   filter::linear);
  return root.multiple_scattering_sample.sample(linear_sampler, uv).rgb;
}

kernel void vkr_metal_packet_atmosphere_transmittance_compute(
    constant VkrMetalPacketAtmosphereRoot &root [[buffer(0)]],
    uint2 pixel [[thread_position_in_grid]]) {
  if (any(pixel >= root.extent))
    return;
  float2 uv = (float2(pixel) + 0.5f) / float2(root.extent);
  float radius, mu;
  vkr_atmosphere_transmittance_params(root.params, uv, radius, mu);
  float3 position = float3(0.0f, radius, 0.0f);
  float3 direction = float3(sqrt(max(1.0f - mu * mu, 0.0f)), mu, 0.0f);
  root.transmittance_storage.write(
      float4(vkr_metal_atmosphere_transmittance(root, position, direction),
             1.0f),
      pixel);
}

kernel void vkr_metal_packet_atmosphere_multiple_scattering_compute(
    constant VkrMetalPacketAtmosphereRoot &root [[buffer(0)]],
    uint3 group_position [[threadgroup_position_in_grid]],
    uint lane [[thread_index_in_threadgroup]]) {
  threadgroup float3
      vkr_metal_atmosphere_multi_a[VKR_ATMOSPHERE_MULTI_DIRECTIONS];
  threadgroup float3
      vkr_metal_atmosphere_multi_l[VKR_ATMOSPHERE_MULTI_DIRECTIONS];
  if (any(group_position.xy >= root.extent) ||
      lane >= VKR_ATMOSPHERE_MULTI_DIRECTIONS)
    return;
  float2 uv = (float2(group_position.xy) + 0.5f) / float2(root.extent);
  float radius = root.params.planet.x +
                 uv.y * (root.params.planet.y - root.params.planet.x);
  float mu_s = uv.x * 2.0f - 1.0f;
  float3 sun_direction =
      float3(0.0f, sqrt(max(1.0f - mu_s * mu_s, 0.0f)), mu_s);
  float z = 1.0f - 2.0f * ((float(lane) + 0.5f) /
                           float(VKR_ATMOSPHERE_MULTI_DIRECTIONS));
  float phi =
      2.0f * VKR_ATMOSPHERE_PI * fract(float(lane) * 0.6180339887498948f);
  float3 direction = float3(cos(phi) * sqrt(max(1.0f - z * z, 0.0f)),
                            sin(phi) * sqrt(max(1.0f - z * z, 0.0f)), z);
  float3 position = float3(0.0f, 0.0f, radius);
  bool hits_ground = false;
  float distance = vkr_atmosphere_segment_limit(root.params, position,
                                                direction, hits_ground);
  float step = distance / float(VKR_ATMOSPHERE_MULTI_SAMPLES);
  float3 throughput = float3(1.0f), a = float3(0.0f), l = float3(0.0f);
  for (uint index = 0u; index < VKR_ATMOSPHERE_MULTI_SAMPLES; ++index) {
    float3 p = position + direction * ((float(index) + 0.5f) * step);
    VkrAtmosphereMedium medium = vkr_atmosphere_medium(root.params, p);
    float3 segment_transmittance =
        exp(-min(medium.extinction * step, VKR_ATMOSPHERE_MAX_OPTICAL_DEPTH));
    a += throughput * vkr_atmosphere_segment_source_integral(
                          medium.scattering, medium.extinction, step);
    float3 to_sun =
        vkr_atmosphere_sun_occluded(root.params, p, sun_direction)
            ? float3(0.0f)
            : vkr_metal_atmosphere_sample_transmittance(root, p, sun_direction);
    float3 source =
        to_sun * medium.scattering * (1.0f / (4.0f * VKR_ATMOSPHERE_PI));
    l += throughput * vkr_atmosphere_segment_source_integral(
                          source, medium.extinction, step);
    throughput *= segment_transmittance;
  }
  if (hits_ground) {
    float3 ground_position = position + direction * distance;
    float3 ground_up = vkr_atmosphere_safe_normalize(ground_position,
                                                     float3(0.0f, 1.0f, 0.0f));
    float3 to_sun =
        vkr_atmosphere_sun_occluded(
            root.params, ground_position + ground_up * 1e-3f, sun_direction)
            ? float3(0.0f)
            : vkr_metal_atmosphere_sample_transmittance(
                  root, ground_position + ground_up * 1e-3f, sun_direction);
    l += throughput * to_sun * root.params.ground.rgb *
         max(dot(ground_up, sun_direction), 0.0f) / VKR_ATMOSPHERE_PI;
  }
  vkr_metal_atmosphere_multi_a[lane] = a;
  vkr_metal_atmosphere_multi_l[lane] = l;
  threadgroup_barrier(mem_flags::mem_threadgroup);
  for (uint stride = VKR_ATMOSPHERE_MULTI_DIRECTIONS / 2u; stride > 0u;
       stride >>= 1u) {
    if (lane < stride) {
      vkr_metal_atmosphere_multi_a[lane] +=
          vkr_metal_atmosphere_multi_a[lane + stride];
      vkr_metal_atmosphere_multi_l[lane] +=
          vkr_metal_atmosphere_multi_l[lane + stride];
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
  }
  if (lane != 0u)
    return;
  float3 f_ms =
      vkr_metal_atmosphere_multi_a[0] / float(VKR_ATMOSPHERE_MULTI_DIRECTIONS);
  float3 l_ms =
      vkr_metal_atmosphere_multi_l[0] / float(VKR_ATMOSPHERE_MULTI_DIRECTIONS);
  root.multiple_scattering_storage.write(
      float4(l_ms / max(1.0f - f_ms, float3(1e-3f)), 1.0f), group_position.xy);
}

kernel void vkr_metal_packet_atmosphere_source_compute(
    constant VkrMetalPacketAtmosphereRoot &root [[buffer(0)]],
    uint3 position_id [[thread_position_in_grid]]) {
  if (position_id.x >= root.face_size || position_id.y >= root.face_size ||
      position_id.z >= 6u)
    return;
  float3 origin =
      float3(0.0f, root.params.planet.x + root.params.planet.z, 0.0f);
  float3 direction = vkr_atmosphere_safe_normalize(
      vkr_atmosphere_cube_direction(position_id.z,
                                    (float2(position_id.xy) + 0.5f) /
                                        float(root.face_size)),
      float3(0.0f, 1.0f, 0.0f));
  bool hits_ground = false;
  float distance =
      vkr_atmosphere_segment_limit(root.params, origin, direction, hits_ground);
  float step = distance / float(VKR_ATMOSPHERE_SOURCE_SAMPLES);
  float3 throughput = float3(1.0f), radiance = float3(0.0f);
  float phase_cosine = dot(direction, root.params.sun.xyz);
  float rayleigh_phase = vkr_atmosphere_rayleigh_phase(phase_cosine);
  float mie_phase =
      vkr_atmosphere_mie_phase(root.params.planet.w, phase_cosine);
  for (uint index = 0u; index < VKR_ATMOSPHERE_SOURCE_SAMPLES; ++index) {
    float3 p = origin + direction * ((float(index) + 0.5f) * step);
    VkrAtmosphereMedium medium = vkr_atmosphere_medium(root.params, p);
    float3 sun_transmittance =
        vkr_atmosphere_sun_occluded(root.params, p, root.params.sun.xyz)
            ? float3(0.0f)
            : vkr_metal_atmosphere_sample_transmittance(root, p,
                                                        root.params.sun.xyz);
    float3 multi =
        vkr_metal_atmosphere_sample_multiple(root, p, root.params.sun.xyz);
    float3 source =
        root.params.solar.rgb *
        (sun_transmittance * (medium.rayleigh_scattering * rayleigh_phase +
                              medium.mie_scattering * mie_phase) +
         multi * medium.scattering);
    radiance += throughput * vkr_atmosphere_segment_source_integral(
                                 source, medium.extinction, step);
    throughput *=
        exp(-min(medium.extinction * step, VKR_ATMOSPHERE_MAX_OPTICAL_DEPTH));
  }
  float coverage =
      vkr_atmosphere_sun_occluded(root.params, origin, root.params.sun.xyz)
          ? 0.0f
          : vkr_atmosphere_solar_disc_coverage(
                position_id.z, position_id.xy, root.face_size,
                root.params.sun.xyz, root.params.sun.w);
  root.source_storage.write(
      float4(vkr_atmosphere_source_radiance(radiance), coverage),
      position_id.xy, position_id.z);
}

kernel void vkr_metal_packet_atmosphere_sun_compute(
    constant VkrMetalPacketAtmosphereRoot &root [[buffer(0)]],
    uint index [[thread_position_in_grid]]) {
  if (index != 0u)
    return;
  float radius = root.params.planet.x + root.params.planet.z;
  float3 origin = float3(0.0f, radius, 0.0f);
  float3 observed =
      vkr_atmosphere_sun_occluded(root.params, origin, root.params.sun.xyz)
          ? float3(0.0f)
          : root.params.solar.rgb * vkr_metal_atmosphere_sample_transmittance(
                                        root, origin, root.params.sun.xyz);
  root.sun_output[0] = float4(observed, 0.0f);
}
