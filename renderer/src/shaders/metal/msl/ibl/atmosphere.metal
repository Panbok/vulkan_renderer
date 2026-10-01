struct alignas(16) VkrMetalPacketAtmosphereRoot {
  VkrAtmosphereParams params;
  texture2d<float, access::sample> transmittance_sample;
  texture2d<float, access::write> transmittance_storage;
  texture2d<float, access::sample> multiple_scattering_sample;
  texture2d<float, access::write> multiple_scattering_storage;
  texturecube<float, access::write> source_storage;
  uint2 extent;
  uint face_size;
  uint reserved;
};

// Metal resource references occupy eight bytes. Five references follow the
// 160-byte parameter record, so the scalar tail starts at byte 200 and the
// root rounds to 224 bytes.
static_assert(sizeof(VkrMetalPacketAtmosphereRoot) == 224u,
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

constexpr sampler vkr_metal_atmosphere_linear_sampler(coord::normalized,
                                                      address::clamp_to_edge,
                                                      filter::linear);

static float3 vkr_metal_atmosphere_lookup_transmittance(
    texture2d<float, access::sample> transmittance, VkrAtmosphereParams params,
    float3 position, float3 direction) {
  float radius = length(position);
  float3 up = vkr_atmosphere_safe_normalize(position, float3(0.0f, 1.0f, 0.0f));
  float2 uv =
      vkr_atmosphere_transmittance_uv(params, radius, dot(up, direction));
  return transmittance.sample(vkr_metal_atmosphere_linear_sampler, uv).rgb;
}

static float3 vkr_metal_atmosphere_lookup_multiple(
    texture2d<float, access::sample> multiple_scattering,
    VkrAtmosphereParams params, float3 position, float3 sun_direction) {
  float radius = length(position);
  float3 up = vkr_atmosphere_safe_normalize(position, float3(0.0f, 1.0f, 0.0f));
  float2 uv = vkr_atmosphere_multi_scattering_uv(params, radius,
                                                 dot(up, sun_direction));
  return multiple_scattering.sample(vkr_metal_atmosphere_linear_sampler, uv)
      .rgb;
}

// One atmosphere light seen along a view ray: the direction toward it, its
// top-of-atmosphere irradiance and the ray's phase terms. A light without
// irradiance is inactive and contributes nothing.
struct VkrMetalAtmosphereLight {
  float3 direction;
  float3 irradiance;
  float rayleigh_phase;
  float mie_phase;
  bool active;
};

static VkrMetalAtmosphereLight
vkr_metal_atmosphere_light(VkrAtmosphereParams params, float3 view_direction,
                           float3 direction, float4 irradiance) {
  VkrMetalAtmosphereLight light;
  float phase_cosine = dot(view_direction, direction);
  light.direction = direction;
  light.irradiance = irradiance.rgb;
  light.rayleigh_phase = vkr_atmosphere_rayleigh_phase(phase_cosine);
  light.mie_phase = vkr_atmosphere_mie_phase(params.planet.w, phase_cosine);
  light.active = !vkr_atmosphere_light_dark(irradiance);
  return light;
}

static VkrMetalAtmosphereLight vkr_metal_atmosphere_no_light() {
  VkrMetalAtmosphereLight light;
  light.direction = float3(0.0f, 1.0f, 0.0f);
  light.irradiance = float3(0.0f);
  light.rayleigh_phase = 0.0f;
  light.mie_phase = 0.0f;
  light.active = false;
  return light;
}

// Single scattering from one attenuated light plus the multiple-scattering
// lookup, per unit length, at one sample of a view ray.
static float3 vkr_metal_atmosphere_source(
    texture2d<float, access::sample> transmittance,
    texture2d<float, access::sample> multiple_scattering,
    VkrAtmosphereParams params, VkrAtmosphereMedium medium, float3 position,
    VkrMetalAtmosphereLight light) {
  if (!light.active)
    return float3(0.0f);
  float3 light_transmittance =
      vkr_atmosphere_sun_occluded(params, position, light.direction)
          ? float3(0.0f)
          : vkr_metal_atmosphere_lookup_transmittance(
                transmittance, params, position, light.direction);
  float3 multi = vkr_metal_atmosphere_lookup_multiple(
      multiple_scattering, params, position, light.direction);
  return light.irradiance *
         (light_transmittance *
              (medium.rayleigh_scattering * light.rayleigh_phase +
               medium.mie_scattering * light.mie_phase) +
          multi * medium.scattering);
}

// One light's first-order Lambertian ground radiance at `surface`.
static float3 vkr_metal_atmosphere_ground(
    texture2d<float, access::sample> transmittance,
    VkrAtmosphereParams params, float3 surface, float3 ground_up,
    VkrMetalAtmosphereLight light) {
  if (!light.active)
    return float3(0.0f);
  float3 to_light =
      vkr_atmosphere_sun_occluded(params, surface, light.direction)
          ? float3(0.0f)
          : vkr_metal_atmosphere_lookup_transmittance(transmittance, params,
                                                      surface,
                                                      light.direction);
  return light.irradiance * to_light * params.ground.rgb *
         max(dot(ground_up, light.direction), 0.0f) / VKR_ATMOSPHERE_PI;
}

// Radiance reaching `origin` along `direction`: atmospheric in-scatter of
// both lights and, below the horizon, the lit Lambertian ground through the
// path's transmittance, the same first-order term the multiple-scattering
// lookup uses. The source cube and each sky-view table store this integral.
static float3 vkr_metal_atmosphere_scattered_radiance(
    texture2d<float, access::sample> transmittance,
    texture2d<float, access::sample> multiple_scattering,
    VkrAtmosphereParams params, float3 origin, float3 direction,
    VkrMetalAtmosphereLight first, VkrMetalAtmosphereLight second) {
  bool hits_ground = false;
  float distance =
      vkr_atmosphere_segment_limit(params, origin, direction, hits_ground);
  float step = distance / float(VKR_ATMOSPHERE_SOURCE_SAMPLES);
  float3 throughput = float3(1.0f), radiance = float3(0.0f);
  for (uint index = 0u; index < VKR_ATMOSPHERE_SOURCE_SAMPLES; ++index) {
    float3 p = origin + direction * ((float(index) + 0.5f) * step);
    VkrAtmosphereMedium medium = vkr_atmosphere_medium(params, p);
    float3 source = vkr_metal_atmosphere_source(transmittance,
                                                multiple_scattering, params,
                                                medium, p, first) +
                    vkr_metal_atmosphere_source(transmittance,
                                                multiple_scattering, params,
                                                medium, p, second);
    radiance += throughput * vkr_atmosphere_segment_source_integral(
                                 source, medium.extinction, step);
    throughput *=
        exp(-min(medium.extinction * step, VKR_ATMOSPHERE_MAX_OPTICAL_DEPTH));
  }
  if (hits_ground) {
    float3 ground_position = origin + direction * distance;
    float3 ground_up = vkr_atmosphere_safe_normalize(ground_position,
                                                     float3(0.0f, 1.0f, 0.0f));
    float3 surface = ground_position + ground_up * 1e-3f;
    radiance += throughput * (vkr_metal_atmosphere_ground(
                                  transmittance, params, surface, ground_up,
                                  first) +
                              vkr_metal_atmosphere_ground(
                                  transmittance, params, surface, ground_up,
                                  second));
  }
  return radiance;
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
            : vkr_metal_atmosphere_lookup_transmittance(
                  root.transmittance_sample, root.params, p, sun_direction);
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
            : vkr_metal_atmosphere_lookup_transmittance(
                  root.transmittance_sample, root.params,
                  ground_position + ground_up * 1e-3f, sun_direction);
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
  float3 radiance = vkr_metal_atmosphere_scattered_radiance(
      root.transmittance_sample, root.multiple_scattering_sample, root.params,
      origin, direction,
      vkr_metal_atmosphere_light(root.params, direction, root.params.sun.xyz,
                                 root.params.solar),
      vkr_metal_atmosphere_light(root.params, direction, root.params.moon.xyz,
                                 root.params.lunar));
  // The source excludes the sun and moon discs; the visible sky draws them
  // analytically.
  root.source_storage.write(
      float4(vkr_atmosphere_source_radiance(radiance), 0.0f), position_id.xy,
      position_id.z);
}

struct alignas(16) VkrMetalPacketSkyViewRoot {
  constant VkrMetalPacketSky *sky;
  texture2d<float, access::write> output;
};

static_assert(sizeof(VkrMetalPacketSkyViewRoot) == 16u,
              "Metal sky-view root ABI");

// One texel per sky-view direction around the current camera altitude. The
// left table is lit by the sun and the right by the moon, each around its own
// light's azimuth; a light without irradiance leaves its table black.
kernel void vkr_metal_packet_sky_view_compute(
    constant VkrMetalPacketSkyViewRoot &root [[buffer(0)]],
    uint2 pixel [[thread_position_in_grid]]) {
  if (pixel.x >= VKR_ATMOSPHERE_SKY_VIEW_WIDTH * VKR_ATMOSPHERE_SKY_VIEW_TABLES ||
      pixel.y >= VKR_ATMOSPHERE_SKY_VIEW_HEIGHT)
    return;
  constant VkrMetalPacketSky &sky = *root.sky;
  VkrAtmosphereParams params = sky.params.atmosphere;
  uint table = pixel.x / VKR_ATMOSPHERE_SKY_VIEW_WIDTH;
  float4 light = table == VKR_ATMOSPHERE_SKY_VIEW_MOON_TABLE ? params.moon
                                                             : params.sun;
  float4 irradiance = table == VKR_ATMOSPHERE_SKY_VIEW_MOON_TABLE
                          ? params.lunar
                          : params.solar;
  if (vkr_atmosphere_light_dark(irradiance)) {
    root.output.write(float4(0.0f, 0.0f, 0.0f, 1.0f), pixel);
    return;
  }
  uint2 texel = uint2(pixel.x % VKR_ATMOSPHERE_SKY_VIEW_WIDTH, pixel.y);
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
  float3 radiance = vkr_metal_atmosphere_scattered_radiance(
      sky.transmittance, sky.multiple_scattering, params, origin, direction,
      vkr_metal_atmosphere_light(params, direction, local_light, irradiance),
      vkr_metal_atmosphere_no_light());
  root.output.write(float4(vkr_atmosphere_source_radiance(radiance), 1.0f),
                    pixel);
}

struct alignas(16) VkrMetalPacketAerialPerspectiveRoot {
  constant VkrMetalPacketSky *sky;
  texture3d<float, access::write> output;
};

static_assert(sizeof(VkrMetalPacketAerialPerspectiveRoot) == 16u,
              "Metal aerial-perspective root ABI");

// One thread marches a camera froxel column and writes the cumulative
// in-scatter and mean transmittance at every slice depth.
kernel void vkr_metal_packet_aerial_perspective_compute(
    constant VkrMetalPacketAerialPerspectiveRoot &root [[buffer(0)]],
    uint2 column [[thread_position_in_grid]]) {
  if (any(column >= uint2(VKR_ATMOSPHERE_AERIAL_SIZE)))
    return;
  constant VkrMetalPacketSky &sky = *root.sky;
  VkrSkyParams frame = sky.params;
  VkrAtmosphereParams params = frame.atmosphere;
  float2 ndc = (float2(column) + 0.5f) / float(VKR_ATMOSPHERE_AERIAL_SIZE) *
                   2.0f -
               1.0f;
  float4 ray_point = frame.inverse_view_projection * float4(ndc, 0.5f, 1.0f);
  float3 direction = vkr_atmosphere_safe_normalize(
      ray_point.xyz / ray_point.w - frame.camera_position.xyz,
      float3(0.0f, 0.0f, -1.0f));
  float3 origin = float3(0.0f, params.planet.x + params.planet.z, 0.0f);
  VkrMetalAtmosphereLight sun = vkr_metal_atmosphere_light(
      params, direction, params.sun.xyz, params.solar);
  VkrMetalAtmosphereLight moon = vkr_metal_atmosphere_light(
      params, direction, params.moon.xyz, params.lunar);
  bool hits_ground = false;
  float limit =
      vkr_atmosphere_segment_limit(params, origin, direction, hits_ground);
  float3 throughput = float3(1.0f), radiance = float3(0.0f);
  float travelled = 0.0f;
  for (uint slice = 0u; slice < VKR_ATMOSPHERE_AERIAL_SIZE; ++slice) {
    float target = min(vkr_sky_aerial_slice_distance(frame, slice), limit);
    float step = max(target - travelled, 0.0f) /
                 float(VKR_ATMOSPHERE_AERIAL_STEPS_PER_SLICE);
    for (uint index = 0u; index < VKR_ATMOSPHERE_AERIAL_STEPS_PER_SLICE;
         ++index) {
      float3 p = origin + direction * (travelled + (float(index) + 0.5f) * step);
      VkrAtmosphereMedium medium = vkr_atmosphere_medium(params, p);
      float3 source =
          vkr_metal_atmosphere_source(sky.transmittance,
                                      sky.multiple_scattering, params, medium,
                                      p, sun) +
          vkr_metal_atmosphere_source(sky.transmittance,
                                      sky.multiple_scattering, params, medium,
                                      p, moon);
      radiance += throughput * vkr_atmosphere_segment_source_integral(
                                   source, medium.extinction, step);
      throughput *= exp(
          -min(medium.extinction * step, VKR_ATMOSPHERE_MAX_OPTICAL_DEPTH));
    }
    travelled = max(travelled, target);
    root.output.write(
        float4(vkr_atmosphere_source_radiance(radiance),
               dot(throughput, float3(1.0f / 3.0f))),
        uint3(column, slice));
  }
}
