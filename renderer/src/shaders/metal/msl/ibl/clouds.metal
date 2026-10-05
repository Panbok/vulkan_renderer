// Volumetric cloud layer (ADR-074): one-time noise synthesis, the
// sun-projected shadow map and the half-resolution trace with temporal
// accumulation. Shared math lives in shared/cloud_kernel.slangh.

constexpr sampler vkr_metal_cloud_noise_sampler(coord::normalized,
                                                address::repeat,
                                                filter::linear,
                                                mip_filter::none);

constexpr sampler vkr_metal_cloud_history_sampler(coord::normalized,
                                                  address::clamp_to_edge,
                                                  filter::linear,
                                                  mip_filter::none);

struct alignas(16) VkrMetalPacketCloudNoiseRoot {
  texture3d<float, access::write> base;
  texture3d<float, access::write> detail;
  texture2d<float, access::write> weather;
  ulong reserved;
};

static_assert(sizeof(VkrMetalPacketCloudNoiseRoot) == 32u,
              "VkrMetalPacketCloudNoiseRoot ABI drift");

kernel void vkr_metal_packet_cloud_base_noise(
    constant VkrMetalPacketCloudNoiseRoot &root [[buffer(0)]],
    uint3 texel [[thread_position_in_grid]]) {
  if (any(texel >= uint3(VKR_CLOUD_BASE_NOISE_SIZE)))
    return;
  float3 uvw = (float3(texel) + 0.5f) / float(VKR_CLOUD_BASE_NOISE_SIZE);
  root.base.write(float4(vkr_cloud_base_noise(uvw), 0.0f, 0.0f, 1.0f), texel);
}

kernel void vkr_metal_packet_cloud_detail_noise(
    constant VkrMetalPacketCloudNoiseRoot &root [[buffer(0)]],
    uint3 texel [[thread_position_in_grid]]) {
  if (any(texel >= uint3(VKR_CLOUD_DETAIL_NOISE_SIZE)))
    return;
  float3 uvw = (float3(texel) + 0.5f) / float(VKR_CLOUD_DETAIL_NOISE_SIZE);
  root.detail.write(float4(vkr_cloud_detail_noise(uvw), 0.0f, 0.0f, 1.0f),
                    texel);
}

kernel void vkr_metal_packet_cloud_weather(
    constant VkrMetalPacketCloudNoiseRoot &root [[buffer(0)]],
    uint2 texel [[thread_position_in_grid]]) {
  if (any(texel >= uint2(VKR_CLOUD_WEATHER_SIZE)))
    return;
  float2 uv = (float2(texel) + 0.5f) / float(VKR_CLOUD_WEATHER_SIZE);
  root.weather.write(float4(vkr_cloud_weather_noise(uv), 0.0f, 0.0f, 1.0f),
                     texel);
}

// Coverage-shaped density without detail erosion. The weather map is read
// first: where it leaves no coverage the base volume is not sampled.
static float vkr_metal_cloud_shape(constant VkrMetalPacketSky &sky,
                                   VkrCloudParams clouds,
                                   VkrCloudSample sample) {
  float weather =
      sky.cloud_weather.sample(vkr_metal_cloud_noise_sampler, sample.weather_uv)
          .r;
  if (vkr_cloud_shape_density(clouds, 1.0f, weather, sample.height_fraction) <=
      0.0f)
    return 0.0f;
  float base =
      sky.cloud_base_noise.sample(vkr_metal_cloud_noise_sampler, sample.base_uvw)
          .r;
  return vkr_cloud_shape_density(clouds, base, weather, sample.height_fraction);
}

struct alignas(16) VkrMetalPacketCloudShadowRoot {
  constant VkrMetalPacketSky *sky;
  texture2d<float, access::write> output;
};

static_assert(sizeof(VkrMetalPacketCloudShadowRoot) == 16u,
              "VkrMetalPacketCloudShadowRoot ABI drift");

// Transmittance of the whole layer along the sun ray that enters its base at
// the texel's world position. The planar march ignores curvature over the
// map's few kilometres.
kernel void vkr_metal_packet_cloud_shadow(
    constant VkrMetalPacketCloudShadowRoot &root [[buffer(0)]],
    uint2 texel [[thread_position_in_grid]]) {
  if (any(texel >= uint2(VKR_CLOUD_SHADOW_SIZE)))
    return;
  constant VkrMetalPacketSky &sky = *root.sky;
  VkrSkyParams params = sky.params;
  VkrCloudParams clouds = params.clouds;
  float2 uv = (float2(texel) + 0.5f) / float(VKR_CLOUD_SHADOW_SIZE);
  float2 entry_km = vkr_cloud_shadow_entry_km(clouds, uv);
  float2 camera_km =
      float2(params.camera_position.x, params.camera_position.z) *
      params.camera_position.w;
  float2 slope = vkr_cloud_shadow_slope(params);
  float climb_step =
      (clouds.layer.y - clouds.layer.x) / float(VKR_CLOUD_SHADOW_STEPS);
  float path_step = climb_step * sqrt(1.0f + dot(slope, slope));
  float optical_depth = 0.0f;
  for (uint step = 0u; step < VKR_CLOUD_SHADOW_STEPS; ++step) {
    float climb = (float(step) + 0.5f) * climb_step;
    VkrCloudSample sample = vkr_cloud_sample(
        params, entry_km + slope * climb - camera_km, clouds.layer.x + climb);
    optical_depth +=
        vkr_metal_cloud_shape(sky, clouds, sample) * clouds.layer.z * path_step;
  }
  root.output.write(float4(exp(-optical_depth)), texel);
}

struct alignas(16) VkrMetalPacketCloudTraceRoot {
  float4x4 previous_view_projection;
  constant VkrMetalPacketSky *sky;
  constant VkrMetalPacketFrameRoot *frame;
  texture2d<float, access::read> depth;
  texture2d<float, access::sample> history;
  texture2d<float, access::write> output;
  uint2 extent;
  uint2 depth_extent;
  uint frame_index;
  uint history_valid;
  // Converts history radiance to this frame's pre-exposure.
  float history_pre_exposure_scale;
};

static_assert(sizeof(VkrMetalPacketCloudTraceRoot) == 144u,
              "VkrMetalPacketCloudTraceRoot ABI drift");

// Optical depth toward the sun through the base shape, with doubling steps.
static float vkr_metal_cloud_light_depth(constant VkrMetalPacketSky &sky,
                                         VkrSkyParams params, float3 position,
                                         float3 sun) {
  VkrCloudParams clouds = params.clouds;
  float optical_depth = 0.0f;
  float travelled = 0.0f;
  float step = VKR_CLOUD_LIGHT_FIRST_STEP_KM;
  for (uint index = 0u; index < VKR_CLOUD_LIGHT_STEPS; ++index) {
    float3 sample_position = position + sun * (travelled + 0.5f * step);
    float radius = length(sample_position);
    if (radius > clouds.layer.y)
      break;
    VkrCloudSample sample =
        vkr_cloud_sample(params, sample_position.xz, radius);
    optical_depth +=
        vkr_metal_cloud_shape(sky, clouds, sample) * clouds.layer.z * step;
    travelled += step;
    step *= 2.0f;
  }
  return optical_depth;
}

// Clear-sky ambient for the layer's own lighting, scaled like diffuse IBL:
// the published source's SH, never the cloud-lit sky light that the layer
// feeds, so a frame's clouds do not light the next frame's clouds.
static float3
vkr_metal_cloud_clear_ambient(constant VkrMetalPacketSky &sky,
                              constant VkrMetalPacketFrameRoot *frame) {
  if ((frame->flags & 2u) == 0u)
    return float3(0.0f);
  return max(vkr_sh_l2_average(frame->sh_coefficients[sky.clear_sh_slot]),
             float3(0.0f)) *
         frame->ibl_controls.x * frame->ibl_controls.y;
}

// Cloud in-scatter, with aerial perspective at the layer's apparent depth when
// `aerial` (the volume covers only the view), and the layer's gray
// transmittance. The segment through the layer takes `steps` primary steps;
// `offset` places samples within each.
static float4 vkr_metal_cloud_march(constant VkrMetalPacketSky *sky_record,
                                    constant VkrMetalPacketFrameRoot *frame,
                                    float3 direction, uint steps, float offset,
                                    bool aerial) {
  constant VkrMetalPacketSky &sky = *sky_record;
  VkrSkyParams params = sky.params;
  VkrCloudParams clouds = params.clouds;
  VkrCloudSegment segment = vkr_cloud_segment(params, direction);
  if (segment.end_km <= segment.start_km)
    return float4(0.0f, 0.0f, 0.0f, 1.0f);

  float camera_radius = params.atmosphere.planet.x + params.atmosphere.planet.z;
  float3 origin = float3(0.0f, camera_radius, 0.0f);
  float3 sun = params.atmosphere.sun.xyz;
  float cosine = dot(direction, sun);
  // The moon lights the layer while it is the key light; a sun below the
  // observer's horizon may still light the layer above, at twilight.
  float3 moon = params.atmosphere.moon.xyz;
  float moon_cosine = dot(direction, moon);
  bool sun_lights = !vkr_atmosphere_light_dark(params.atmosphere.solar);
  bool moon_lights = params.key_light.w > 0.0f &&
                     !vkr_atmosphere_light_dark(params.atmosphere.lunar);
  float3 ambient = vkr_metal_cloud_clear_ambient(sky, frame);
  float step_km = (segment.end_km - segment.start_km) / float(steps);
  VkrCloudIntegration state = vkr_cloud_integration_begin();
  for (uint index = 0u; index < steps; ++index) {
    float distance = segment.start_km + (float(index) + offset) * step_km;
    float3 position = origin + direction * distance;
    float radius = length(position);
    VkrCloudSample sample = vkr_cloud_sample(params, position.xz, radius);
    float shape = vkr_metal_cloud_shape(sky, clouds, sample);
    if (shape <= 0.0f)
      continue;
    float detail =
        sky.cloud_detail_noise
            .sample(vkr_metal_cloud_noise_sampler, sample.detail_uvw)
            .r;
    float density =
        vkr_cloud_eroded_density(shape, detail, sample.height_fraction) *
        vkr_cloud_distance_fade(clouds, distance);
    if (density <= 0.0f)
      continue;
    // An occluded light contributes nothing, so its light march is skipped.
    float3 source = float3(0.0f);
    if (sun_lights &&
        !vkr_atmosphere_sun_occluded(params.atmosphere, position, sun)) {
      float3 sun_irradiance =
          params.atmosphere.solar.rgb *
          vkr_metal_atmosphere_lookup_transmittance(
              sky.transmittance, params.atmosphere, position, sun);
      source = vkr_cloud_sun_scattering(
          sun_irradiance, cosine,
          vkr_metal_cloud_light_depth(sky, params, position, sun));
    }
    if (moon_lights &&
        !vkr_atmosphere_sun_occluded(params.atmosphere, position, moon)) {
      float3 moon_irradiance =
          params.atmosphere.lunar.rgb *
          vkr_metal_atmosphere_lookup_transmittance(
              sky.transmittance, params.atmosphere, position, moon);
      source += vkr_cloud_sun_scattering(
          moon_irradiance, moon_cosine,
          vkr_metal_cloud_light_depth(sky, params, position, moon));
    }
    source += vkr_cloud_ambient(ambient, sample.height_fraction);
    state = vkr_cloud_integrate_step(state, density * clouds.layer.z, step_km,
                                     source, distance);
    if (state.transmittance < VKR_CLOUD_TRANSMITTANCE_CUTOFF)
      break;
  }

  float3 radiance = state.radiance;
  if (aerial && params.aerial.w > 0.0f && state.transmittance < 1.0f) {
    float depth_km = vkr_cloud_integration_depth(state, segment.start_km);
    float3 world = params.camera_position.xyz +
                   direction * (depth_km / params.camera_position.w);
    VkrMetalPacketAerialSample aerial =
        vkr_metal_packet_aerial_sample(sky_record, sky.aerial_perspective,
                                       world);
    radiance = vkr_sky_apply_aerial_over_feedback(
        radiance, float3(state.transmittance), aerial.packed, aerial.weight);
  }
  return float4(radiance, state.transmittance);
}

// One thread per half-resolution texel. Composition bilinearly samples texels
// up to one full-resolution pixel beyond each 2x2 footprint, so a texel traces
// when any pixel of its 4x4 neighbourhood shows sky; others write the
// untraced marker.
kernel void vkr_metal_packet_cloud_trace(
    constant VkrMetalPacketCloudTraceRoot &root [[buffer(0)]],
    uint2 texel [[thread_position_in_grid]]) {
  if (any(texel >= root.extent))
    return;
  int2 origin = int2(texel) * 2 - 1;
  int2 limit = int2(root.depth_extent) - 1;
  bool sky_visible = false;
  for (int y = 0; y < 4 && !sky_visible; ++y) {
    for (int x = 0; x < 4; ++x) {
      uint2 pixel = uint2(clamp(origin + int2(x, y), int2(0), limit));
      if (root.depth.read(pixel).x >= 1.0f) {
        sky_visible = true;
        break;
      }
    }
  }
  if (!sky_visible) {
    root.output.write(float4(0.0f, 0.0f, 0.0f, VKR_CLOUD_UNTRACED_ALPHA),
                      texel);
    return;
  }

  VkrSkyParams params = root.sky->params;
  float2 texel_size = 1.0f / float2(root.extent);
  float2 centre_uv = (float2(texel) + 0.5f) * texel_size;
  float3 direction = vkr_cloud_view_direction(
      params, centre_uv + vkr_cloud_ray_jitter(root.frame_index) * texel_size);
  float4 current = vkr_metal_cloud_march(
      root.sky, root.frame, direction, VKR_CLOUD_PRIMARY_STEPS,
      vkr_cloud_march_offset(texel, root.frame_index), true);

  // Clouds lie kilometres away, so the previous camera saw this texel's
  // centre along the same direction.
  float3 centre_direction = vkr_cloud_view_direction(params, centre_uv);
  float4 previous_clip =
      root.previous_view_projection * float4(centre_direction, 0.0f);
  float2 previous_uv =
      previous_clip.xy / max(previous_clip.w, 1e-6f) * 0.5f + 0.5f;
  bool history_valid = root.history_valid != 0u && previous_clip.w > 0.0f &&
                       all(previous_uv >= 0.0f) && all(previous_uv <= 1.0f);
  float4 history =
      history_valid
          ? root.history.sample(vkr_metal_cloud_history_sampler, previous_uv)
          : current;
  if (history_valid)
    history.rgb *= root.history_pre_exposure_scale;
  root.output.write(vkr_cloud_history_blend(current, history, history_valid),
                    texel);
}

// ADR-074 cloud-lit sky light, shared by its march, projection and chain
// kernels: the clear source cube and its scale, this frame's cloud chain and
// this frame slot's SH destination.
struct alignas(16) VkrMetalPacketCloudSkyLightRoot {
  constant VkrMetalPacketSky *sky;
  constant VkrMetalPacketFrameRoot *frame;
  device float4 *radiance;
  device VkrShL2Packed *destination;
  texturecube<float, access::read> source;
  uint source_mip;
  uint face_size;
  // Pre-exposed cloud radiance into the source's radiance_stops scale.
  float cloud_to_source_scale;
  float window_band_0;
  float window_band_1;
  float window_band_2;
  uint2 reserved;
};

static_assert(sizeof(VkrMetalPacketCloudSkyLightRoot) == 80u,
              "VkrMetalPacketCloudSkyLightRoot ABI drift");

// One thread per texel of the clear sky source's 16-texel mip: the layer
// along the texel's direction from the camera, in the source's scale, as the
// chain's first level (ADR-074).
kernel void vkr_metal_packet_cloud_sky_light(
    constant VkrMetalPacketCloudSkyLightRoot &root [[buffer(0)]],
    uint3 id [[thread_position_in_grid]]) {
  uint extent = root.face_size;
  if (any(id.xy >= uint2(extent)) || id.z >= 6u)
    return;
  float2 uv = (float2(id.xy) + 0.5f) / float(extent);
  float3 direction = normalize(vkr_metal_packet_cube_direction(id.z, uv));
  float4 cloud = vkr_metal_cloud_march(
      root.sky, root.frame, direction, VKR_CLOUD_SKY_LIGHT_PRIMARY_STEPS,
      VKR_CLOUD_SKY_LIGHT_MARCH_OFFSET, false);
  root.radiance[vkr_cloud_sky_light_index(extent, 0u, id.z, id.xy)] =
      float4(cloud.rgb * root.cloud_to_source_scale, cloud.a);
}

// Projects the clear texels seen through the layer into this frame slot's
// SH, as the clear source's own projection does.
kernel void vkr_metal_packet_cloud_sky_light_sh(
    constant VkrMetalPacketCloudSkyLightRoot &root [[buffer(0)]],
    uint lane [[thread_position_in_threadgroup]]) {
  threadgroup float3 partial[64 * 9];
  threadgroup float partial_weight[64];
  vkr_metal_sh_project(lane, root.face_size, root.source, root.source_mip,
                       root.radiance, true,
                       float3(root.window_band_0, root.window_band_1,
                              root.window_band_2),
                       root.destination, partial, partial_weight);
}

// One threadgroup per face averages the chain's first level down to one
// texel, so global reflections read the layer at their roughness.
kernel void vkr_metal_packet_cloud_sky_light_mips(
    constant VkrMetalPacketCloudSkyLightRoot &root [[buffer(0)]],
    uint2 texel [[thread_position_in_threadgroup]],
    uint3 group [[threadgroup_position_in_grid]]) {
  threadgroup float4 tile[VKR_CLOUD_SKY_LIGHT_TILE * VKR_CLOUD_SKY_LIGHT_TILE];
  uint face_size = root.face_size;
  uint face = group.z;
  uint cell = texel.y * VKR_CLOUD_SKY_LIGHT_TILE + texel.x;
  if (all(texel < uint2(face_size)))
    tile[cell] =
        root.radiance[vkr_cloud_sky_light_index(face_size, 0u, face, texel)];
  threadgroup_barrier(mem_flags::mem_threadgroup);
  uint levels = vkr_cloud_sky_light_levels(face_size);
  for (uint level = 1u; level < levels; ++level) {
    bool writes = all(texel < uint2(face_size >> level));
    float4 value = float4(0.0f);
    if (writes) {
      uint corner = 2u * texel.y * VKR_CLOUD_SKY_LIGHT_TILE + 2u * texel.x;
      value = 0.25f * (tile[corner] + tile[corner + 1u] +
                       tile[corner + VKR_CLOUD_SKY_LIGHT_TILE] +
                       tile[corner + VKR_CLOUD_SKY_LIGHT_TILE + 1u]);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (writes) {
      tile[cell] = value;
      root.radiance[vkr_cloud_sky_light_index(face_size, level, face, texel)] =
          value;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
  }
}
