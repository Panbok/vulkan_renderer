struct VkrVulkanAtmosphereRoot {
  VkrAtmosphereParams params;
  uint transmittance_sample;
  uint transmittance_storage;
  uint multiple_scattering_sample;
  uint multiple_scattering_storage;
  uint source_storage;
  uint sampler;
  uint64_t sun_output;
  uint2 extent;
  uint face_size;
  uint reserved;
};

struct VkrVulkanAtmospherePush {
  VkrVulkanAtmosphereRoot *root;
  uint2 reserved;
};
[[vk::push_constant]] ConstantBuffer<VkrVulkanAtmospherePush> g_atmosphere_push;

float3 vk_atmosphere_transmittance(VkrVulkanAtmosphereRoot *root,
                                   float3 position, float3 direction) {
  bool ground = false;
  float distance =
      vkr_atmosphere_segment_limit(root.params, position, direction, ground);
  if (distance <= 0.0f || ground)
    return 0.0f;
  float step = distance / float(VKR_ATMOSPHERE_TRANSMITTANCE_SAMPLES);
  float3 optical = 0.0f;
  for (uint i = 0u; i < VKR_ATMOSPHERE_TRANSMITTANCE_SAMPLES; ++i)
    optical +=
        vkr_atmosphere_medium(root.params,
                              position + direction * ((float(i) + .5f) * step))
            .extinction *
        step;
  return exp(-min(optical, VKR_ATMOSPHERE_MAX_OPTICAL_DEPTH));
}
float3 vk_atmosphere_trans_sample(VkrVulkanAtmosphereRoot *root, float3 p,
                                  float3 d) {
  float3 up = vkr_atmosphere_safe_normalize(p, float3(0, 1, 0));
  return g_textures[root.transmittance_sample]
      .SampleLevel(
          g_samplers[root.sampler],
          vkr_atmosphere_transmittance_uv(root.params, length(p), dot(up, d)),
          0)
      .rgb;
}
float3 vk_atmosphere_multi_sample(VkrVulkanAtmosphereRoot *root, float3 p,
                                  float3 d) {
  float3 up = vkr_atmosphere_safe_normalize(p, float3(0, 1, 0));
  return g_textures[root.multiple_scattering_sample]
      .SampleLevel(g_samplers[root.sampler],
                   vkr_atmosphere_multi_scattering_uv(root.params, length(p),
                                                      dot(up, d)),
                   0)
      .rgb;
}
[shader("compute")][numthreads(8, 8, 1)] void
atmosphere_transmittance_compute(uint3 id : SV_DispatchThreadID) {
  VkrVulkanAtmosphereRoot *root = g_atmosphere_push.root;
  if (any(id.xy >= root.extent))
    return;
  float radius, mu;
  vkr_atmosphere_transmittance_params(
      root.params, (float2(id.xy) + .5f) / float2(root.extent), radius, mu);
  g_storage_2d[root.transmittance_storage][id.xy] = float4(
      vk_atmosphere_transmittance(root, float3(0, radius, 0),
                                  float3(sqrt(max(1 - mu * mu, 0)), mu, 0)),
      1);
} groupshared float3 vk_multi_a[64];
groupshared float3 vk_multi_l[64];
[shader("compute")][numthreads(1, 1, 64)] void
atmosphere_multiple_scattering_compute(uint3 gid : SV_GroupID,
                                       uint lane : SV_GroupIndex) {
  VkrVulkanAtmosphereRoot *root = g_atmosphere_push.root;
  if (any(gid.xy >= root.extent))
    return;
  float2 uv = (float2(gid.xy) + .5f) / float2(root.extent);
  float radius = root.params.planet.x +
                 uv.y * (root.params.planet.y - root.params.planet.x);
  float mu = uv.x * 2 - 1;
  float3 sun = float3(0, sqrt(max(1 - mu * mu, 0)), mu);
  float z = 1 - 2 * ((float(lane) + .5f) / 64);
  float phi = 2 * VKR_ATMOSPHERE_PI * fract(float(lane) * .61803398875f);
  float3 d = float3(cos(phi) * sqrt(max(1 - z * z, 0)),
                    sin(phi) * sqrt(max(1 - z * z, 0)), z),
         p0 = float3(0, 0, radius);
  bool ground = false;
  float distance = vkr_atmosphere_segment_limit(root.params, p0, d, ground),
        step = distance / float(VKR_ATMOSPHERE_MULTI_SAMPLES);
  float3 t = 1, a = 0, l = 0;
  for (uint i = 0; i < VKR_ATMOSPHERE_MULTI_SAMPLES; ++i) {
    float3 p = p0 + d * ((float(i) + .5f) * step);
    VkrAtmosphereMedium m = vkr_atmosphere_medium(root.params, p);
    float3 tr =
        exp(-min(m.extinction * step, VKR_ATMOSPHERE_MAX_OPTICAL_DEPTH));
    a += t * vkr_atmosphere_segment_source_integral(m.scattering, m.extinction,
                                                    step);
    float3 ts = vkr_atmosphere_sun_occluded(root.params, p, sun)
                    ? 0
                    : vk_atmosphere_trans_sample(root, p, sun);
    l += t * vkr_atmosphere_segment_source_integral(
                 ts * m.scattering * (1 / (4 * VKR_ATMOSPHERE_PI)),
                 m.extinction, step);
    t *= tr;
  }
  if (ground) {
    float3 gp = p0 + d * distance,
           up = vkr_atmosphere_safe_normalize(gp, float3(0, 1, 0));
    float3 ts = vkr_atmosphere_sun_occluded(root.params, gp + up * 1e-3f, sun)
                    ? 0
                    : vk_atmosphere_trans_sample(root, gp + up * 1e-3f, sun);
    l += t * ts * root.params.ground.rgb * max(dot(up, sun), 0) /
         VKR_ATMOSPHERE_PI;
  }
  vk_multi_a[lane] = a;
  vk_multi_l[lane] = l;
  GroupMemoryBarrierWithGroupSync();
  for (uint s = 32; s > 0; s >>= 1) {
    if (lane < s) {
      vk_multi_a[lane] += vk_multi_a[lane + s];
      vk_multi_l[lane] += vk_multi_l[lane + s];
    }
    GroupMemoryBarrierWithGroupSync();
  }
  if (lane == 0) {
    float3 f = vk_multi_a[0] / 64;
    g_storage_2d[root.multiple_scattering_storage][gid.xy] =
        float4((vk_multi_l[0] / 64) / max(1 - f, float3(.001f)), 1);
  }
}[shader("compute")][numthreads(
    8, 8, 1)] void atmosphere_source_compute(uint3 id : SV_DispatchThreadID) {
  VkrVulkanAtmosphereRoot *root = g_atmosphere_push.root;
  if (id.x >= root.face_size || id.y >= root.face_size || id.z >= 6)
    return;
  float3 o = float3(0, root.params.planet.x + root.params.planet.z, 0),
         d = vkr_atmosphere_safe_normalize(
             vkr_atmosphere_cube_direction(id.z, (float2(id.xy) + .5f) /
                                                     float(root.face_size)),
             float3(0, 1, 0));
  bool ground = false;
  float distance = vkr_atmosphere_segment_limit(root.params, o, d, ground),
        step = distance / float(VKR_ATMOSPHERE_SOURCE_SAMPLES),
        c = dot(d, root.params.sun.xyz), pr = vkr_atmosphere_rayleigh_phase(c),
        pm = vkr_atmosphere_mie_phase(root.params.planet.w, c);
  float3 t = 1, l = 0;
  for (uint i = 0; i < VKR_ATMOSPHERE_SOURCE_SAMPLES; ++i) {
    float3 p = o + d * ((float(i) + .5f) * step);
    VkrAtmosphereMedium m = vkr_atmosphere_medium(root.params, p);
    float3 ts = vkr_atmosphere_sun_occluded(root.params, p, root.params.sun.xyz)
                    ? 0
                    : vk_atmosphere_trans_sample(root, p, root.params.sun.xyz);
    float3 ms = vk_atmosphere_multi_sample(root, p, root.params.sun.xyz);
    float3 source = root.params.solar.rgb *
                    (ts * (m.rayleigh_scattering * pr + m.mie_scattering * pm) +
                     ms * m.scattering);
    l += t * vkr_atmosphere_segment_source_integral(source, m.extinction, step);
    t *= exp(-min(m.extinction * step, VKR_ATMOSPHERE_MAX_OPTICAL_DEPTH));
  }
  float coverage =
      vkr_atmosphere_sun_occluded(root.params, o, root.params.sun.xyz)
          ? 0
          : vkr_atmosphere_solar_disc_coverage(id.z, id.xy, root.face_size,
                                               root.params.sun.xyz,
                                               root.params.sun.w);
  g_storage_images[root.source_storage][id] =
      float4(vkr_atmosphere_source_radiance(l), coverage);
}
[shader("compute")][numthreads(1, 1, 1)] void
atmosphere_sun_compute(uint3 id : SV_DispatchThreadID) {
  VkrVulkanAtmosphereRoot *root = g_atmosphere_push.root;
  float3 o = float3(0, root.params.planet.x + root.params.planet.z, 0);
  float3 e = vkr_atmosphere_sun_occluded(root.params, o, root.params.sun.xyz)
                 ? 0
                 : root.params.solar.rgb *
                       vk_atmosphere_trans_sample(root, o, root.params.sun.xyz);
  ((float4 *)root.sun_output)[0] = float4(e, 0);
}
