struct VkrVulkanAtmosphereRoot {
  VkrAtmosphereParams params;
  uint transmittance_sample;
  uint transmittance_storage;
  uint multiple_scattering_sample;
  uint multiple_scattering_storage;
  uint source_storage;
  uint sampler;
  uint2 extent;
  uint face_size;
  uint reserved;
};

struct VkrVulkanAtmospherePush {
  VkrVulkanAtmosphereRoot *root;
  uint2 reserved;
};
[[vk::push_constant]] ConstantBuffer<VkrVulkanAtmospherePush> g_atmosphere_push;

float3 vk_atmosphere_transmittance(VkrVulkanAtmosphereRoot root,
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
float3 vk_atmosphere_lookup_transmittance(uint texture, uint sampler,
                                          VkrAtmosphereParams params,
                                          float3 p, float3 d) {
  float3 up = vkr_atmosphere_safe_normalize(p, float3(0, 1, 0));
  return g_textures[texture]
      .SampleLevel(g_samplers[sampler],
                   vkr_atmosphere_transmittance_uv(params, length(p),
                                                   dot(up, d)),
                   0)
      .rgb;
}

float3 vk_atmosphere_lookup_multiple(uint texture, uint sampler,
                                     VkrAtmosphereParams params, float3 p,
                                     float3 d) {
  float3 up = vkr_atmosphere_safe_normalize(p, float3(0, 1, 0));
  return g_textures[texture]
      .SampleLevel(g_samplers[sampler],
                   vkr_atmosphere_multi_scattering_uv(params, length(p),
                                                      dot(up, d)),
                   0)
      .rgb;
}

float3 vk_atmosphere_trans_sample(VkrVulkanAtmosphereRoot root, float3 p,
                                  float3 d) {
  return vk_atmosphere_lookup_transmittance(root.transmittance_sample,
                                            root.sampler, root.params, p, d);
}

// One atmosphere light seen along a view ray: the direction toward it, its
// top-of-atmosphere irradiance and the ray's phase terms. A light without
// irradiance is inactive and contributes nothing.
struct VkrVkAtmosphereLight {
  float3 direction;
  float3 irradiance;
  float rayleigh_phase;
  float mie_phase;
  bool active;
};

VkrVkAtmosphereLight vk_atmosphere_light(VkrAtmosphereParams params,
                                         float3 view_direction,
                                         float3 direction, float4 irradiance) {
  VkrVkAtmosphereLight light;
  float phase_cosine = dot(view_direction, direction);
  light.direction = direction;
  light.irradiance = irradiance.rgb;
  light.rayleigh_phase = vkr_atmosphere_rayleigh_phase(phase_cosine);
  light.mie_phase = vkr_atmosphere_mie_phase(params.planet.w, phase_cosine);
  light.active = !vkr_atmosphere_light_dark(irradiance);
  return light;
}

VkrVkAtmosphereLight vk_atmosphere_no_light() {
  VkrVkAtmosphereLight light;
  light.direction = float3(0, 1, 0);
  light.irradiance = 0;
  light.rayleigh_phase = 0;
  light.mie_phase = 0;
  light.active = false;
  return light;
}

// Single scattering from one attenuated light plus the multiple-scattering
// lookup, per unit length, at one sample of a view ray.
float3 vk_atmosphere_source(uint transmittance, uint multiple_scattering,
                            uint sampler, VkrAtmosphereParams params,
                            VkrAtmosphereMedium m, float3 p,
                            VkrVkAtmosphereLight light) {
  if (!light.active)
    return 0;
  float3 ts = vkr_atmosphere_sun_occluded(params, p, light.direction)
                  ? 0
                  : vk_atmosphere_lookup_transmittance(transmittance, sampler,
                                                       params, p,
                                                       light.direction);
  float3 ms = vk_atmosphere_lookup_multiple(multiple_scattering, sampler,
                                            params, p, light.direction);
  return light.irradiance *
         (ts * (m.rayleigh_scattering * light.rayleigh_phase +
                m.mie_scattering * light.mie_phase) +
          ms * m.scattering);
}

// One light's first-order Lambertian ground radiance at `surface`.
float3 vk_atmosphere_ground(uint transmittance, uint sampler,
                            VkrAtmosphereParams params, float3 surface,
                            float3 up, VkrVkAtmosphereLight light) {
  if (!light.active)
    return 0;
  float3 ts = vkr_atmosphere_sun_occluded(params, surface, light.direction)
                  ? 0
                  : vk_atmosphere_lookup_transmittance(
                        transmittance, sampler, params, surface,
                        light.direction);
  return light.irradiance * ts * params.ground.rgb *
         max(dot(up, light.direction), 0) / VKR_ATMOSPHERE_PI;
}

// Radiance reaching `o` along `d`: atmospheric in-scatter of both lights and,
// below the horizon, the lit Lambertian ground through the path's
// transmittance, the same first-order term the multiple-scattering lookup
// uses. The source cube and each sky-view table store this integral.
float3 vk_atmosphere_scattered_radiance(uint transmittance,
                                        uint multiple_scattering, uint sampler,
                                        VkrAtmosphereParams params, float3 o,
                                        float3 d, VkrVkAtmosphereLight first,
                                        VkrVkAtmosphereLight second) {
  bool ground = false;
  float distance = vkr_atmosphere_segment_limit(params, o, d, ground);
  float step = distance / float(VKR_ATMOSPHERE_SOURCE_SAMPLES);
  float3 t = 1, l = 0;
  for (uint i = 0; i < VKR_ATMOSPHERE_SOURCE_SAMPLES; ++i) {
    float3 p = o + d * ((float(i) + .5f) * step);
    VkrAtmosphereMedium m = vkr_atmosphere_medium(params, p);
    float3 source = vk_atmosphere_source(transmittance, multiple_scattering,
                                         sampler, params, m, p, first) +
                    vk_atmosphere_source(transmittance, multiple_scattering,
                                         sampler, params, m, p, second);
    l += t * vkr_atmosphere_segment_source_integral(source, m.extinction, step);
    t *= exp(-min(m.extinction * step, VKR_ATMOSPHERE_MAX_OPTICAL_DEPTH));
  }
  if (ground) {
    float3 gp = o + d * distance,
           up = vkr_atmosphere_safe_normalize(gp, float3(0, 1, 0));
    float3 surface = gp + up * 1e-3f;
    l += t * (vk_atmosphere_ground(transmittance, sampler, params, surface,
                                   up, first) +
              vk_atmosphere_ground(transmittance, sampler, params, surface,
                                   up, second));
  }
  return l;
}
[shader("compute")][numthreads(8, 8, 1)] void
atmosphere_transmittance_compute(uint3 id : SV_DispatchThreadID) {
  VkrVulkanAtmosphereRoot root = vkr_vk_uniform_root(*g_atmosphere_push.root);
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
  VkrVulkanAtmosphereRoot root = vkr_vk_uniform_root(*g_atmosphere_push.root);
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
  VkrVulkanAtmosphereRoot root = vkr_vk_uniform_root(*g_atmosphere_push.root);
  if (id.x >= root.face_size || id.y >= root.face_size || id.z >= 6)
    return;
  float3 o = float3(0, root.params.planet.x + root.params.planet.z, 0),
         d = vkr_atmosphere_safe_normalize(
             vkr_atmosphere_cube_direction(id.z, (float2(id.xy) + .5f) /
                                                     float(root.face_size)),
             float3(0, 1, 0));
  float3 l = vk_atmosphere_scattered_radiance(
      root.transmittance_sample, root.multiple_scattering_sample, root.sampler,
      root.params, o, d,
      vk_atmosphere_light(root.params, d, root.params.sun.xyz,
                          root.params.solar),
      vk_atmosphere_light(root.params, d, root.params.moon.xyz,
                          root.params.lunar));
  // The source excludes the sun and moon discs; the visible sky draws them
  // analytically.
  g_storage_images[root.source_storage][id] =
      float4(vkr_atmosphere_source_radiance(l), 0);
}

// Per-frame builders read the published generation's lookup textures through
// the frame-slot sky record.
struct VkrVulkanSkyBuildRoot {
  VkrVulkanSky *sky;
  uint output_texture;
  uint reserved;
};

struct VkrVulkanSkyBuildPush {
  VkrVulkanSkyBuildRoot *root;
  uint2 reserved;
};
[[vk::push_constant]] ConstantBuffer<VkrVulkanSkyBuildPush> g_sky_build_push;

// One texel per sky-view direction around the current camera altitude. The
// left table is lit by the sun and the right by the moon, each around its own
// light's azimuth; a light without irradiance leaves its table black.
[shader("compute")][numthreads(8, 8, 1)] void
sky_view_compute(uint3 id : SV_DispatchThreadID) {
  VkrVulkanSkyBuildRoot root = vkr_vk_uniform_root(*g_sky_build_push.root);
  if (id.x >= VKR_ATMOSPHERE_SKY_VIEW_WIDTH * VKR_ATMOSPHERE_SKY_VIEW_TABLES ||
      id.y >= VKR_ATMOSPHERE_SKY_VIEW_HEIGHT)
    return;
  VkrVulkanSky *sky = root.sky;
  VkrAtmosphereParams params = sky.params.atmosphere;
  uint table = id.x / VKR_ATMOSPHERE_SKY_VIEW_WIDTH;
  float4 light =
      table == VKR_ATMOSPHERE_SKY_VIEW_MOON_TABLE ? params.moon : params.sun;
  float4 irradiance = table == VKR_ATMOSPHERE_SKY_VIEW_MOON_TABLE
                          ? params.lunar
                          : params.solar;
  if (vkr_atmosphere_light_dark(irradiance)) {
    g_storage_2d[root.output_texture][id.xy] = float4(0.0f, 0.0f, 0.0f, 1.0f);
    return;
  }
  uint2 texel = uint2(id.x % VKR_ATMOSPHERE_SKY_VIEW_WIDTH, id.y);
  float2 unit = float2(texel) / float2(VKR_ATMOSPHERE_SKY_VIEW_WIDTH - 1u,
                                       VKR_ATMOSPHERE_SKY_VIEW_HEIGHT - 1u);
  float view_zenith_cosine, light_view_cosine;
  vkr_sky_view_angles(params, unit, view_zenith_cosine, light_view_cosine);
  float light_zenith_cosine = clamp(light.y, -1.0f, 1.0f);
  float3 local_light = float3(
      sqrt(max(1.0f - light_zenith_cosine * light_zenith_cosine, 0.0f)),
      light_zenith_cosine, 0.0f);
  float3 origin = float3(0.0f, params.planet.x + params.planet.z, 0.0f);
  float3 direction =
      vkr_sky_view_direction(view_zenith_cosine, light_view_cosine);
  float3 radiance = vk_atmosphere_scattered_radiance(
      sky.transmittance_texture, sky.multiple_scattering_texture, sky.sampler,
      params, origin, direction,
      vk_atmosphere_light(params, direction, local_light, irradiance),
      vk_atmosphere_no_light());
  g_storage_2d[root.output_texture][id.xy] =
      float4(vkr_atmosphere_source_radiance(radiance), 1.0f);
}

// One thread marches a camera froxel column and writes the cumulative
// in-scatter and mean transmittance at every slice depth.
[shader("compute")][numthreads(8, 8, 1)] void
aerial_perspective_compute(uint3 id : SV_DispatchThreadID) {
  VkrVulkanSkyBuildRoot root = vkr_vk_uniform_root(*g_sky_build_push.root);
  if (any(id.xy >= uint2(VKR_ATMOSPHERE_AERIAL_SIZE)))
    return;
  VkrVulkanSky *sky = root.sky;
  VkrSkyParams frame = sky.params;
  VkrAtmosphereParams params = frame.atmosphere;
  float2 ndc =
      (float2(id.xy) + 0.5f) / float(VKR_ATMOSPHERE_AERIAL_SIZE) * 2.0f -
      1.0f;
  float4 ray_point =
      mul(frame.inverse_view_projection, float4(ndc, 0.5f, 1.0f));
  float3 direction = vkr_atmosphere_safe_normalize(
      ray_point.xyz / ray_point.w - frame.camera_position.xyz,
      float3(0.0f, 0.0f, -1.0f));
  float3 origin = float3(0.0f, params.planet.x + params.planet.z, 0.0f);
  VkrVkAtmosphereLight sun =
      vk_atmosphere_light(params, direction, params.sun.xyz, params.solar);
  VkrVkAtmosphereLight moon =
      vk_atmosphere_light(params, direction, params.moon.xyz, params.lunar);
  bool hits_ground = false;
  float limit =
      vkr_atmosphere_segment_limit(params, origin, direction, hits_ground);
  float3 throughput = 1.0f, radiance = 0.0f;
  float travelled = 0.0f;
  for (uint slice = 0u; slice < VKR_ATMOSPHERE_AERIAL_SIZE; ++slice) {
    float target = min(vkr_sky_aerial_slice_distance(frame, slice), limit);
    float step = max(target - travelled, 0.0f) /
                 float(VKR_ATMOSPHERE_AERIAL_STEPS_PER_SLICE);
    for (uint index = 0u; index < VKR_ATMOSPHERE_AERIAL_STEPS_PER_SLICE;
         ++index) {
      float3 p =
          origin + direction * (travelled + (float(index) + 0.5f) * step);
      VkrAtmosphereMedium medium = vkr_atmosphere_medium(params, p);
      float3 source =
          vk_atmosphere_source(sky.transmittance_texture,
                               sky.multiple_scattering_texture, sky.sampler,
                               params, medium, p, sun) +
          vk_atmosphere_source(sky.transmittance_texture,
                               sky.multiple_scattering_texture, sky.sampler,
                               params, medium, p, moon);
      radiance += throughput * vkr_atmosphere_segment_source_integral(
                                   source, medium.extinction, step);
      throughput *= exp(
          -min(medium.extinction * step, VKR_ATMOSPHERE_MAX_OPTICAL_DEPTH));
    }
    travelled = max(travelled, target);
    g_storage_3d[root.output_texture][uint3(id.xy, slice)] =
        float4(vkr_atmosphere_source_radiance(radiance),
               dot(throughput, float3(1.0f / 3.0f)));
  }
}
