/* The tiled pipeline's opaque render pass (ADR-087): forward shading of the
 * culled opaque draws after a depth pre-pass, and the clear background sky,
 * in one multisampled render pass resolved on chip. The cloud trace needs the
 * resolved depth, so a later full-screen draw lays the cloud layer and the
 * discs it lets through over the sky, and aerial perspective and fog over
 * every pixel.
 *
 * Static surfaces take diffuse light from their baked lightmap (ADR-088):
 * the frame's active layers, the two sun keys nearest the sun and every lamp
 * group, each scaled by its weight. The runtime adds the sun's direct light
 * with its cascades and cloud shadow, the dynamic local lights with their
 * shadows, and specular light from the global environment. Surfaces without a
 * lightmap take diffuse light from the baked diffuse volume, else the global
 * environment. */

// The forward and depth pre-pass vertex. It transforms like the Slang
// vkr_metal_packet_vertex, whose draw root sits at the same buffer for the
// GPU-encoded commands, and adds the draw's lightmap coordinates: an
// instance's lightmap slot travels as an exact float (ADR-088) and its
// lightmap UVs span its rectangle on its page.
vertex VkrMetalTiledVertexOutput
vkr_metal_tiled_vertex(uint vertex_id [[vertex_id]],
                       uint instance_id [[instance_id]],
                       constant VkrMetalPacketDrawRoot *root [[buffer(0)]]) {
  constant VkrMetalPacketFrameRoot *frame = root->frame;
  const bool gpu_draw = (root->flags & 1u) != 0u;
  const uint visible_row_index =
      gpu_draw ? instance_id : root->visible_row_index;
  const device VkrGpuVisibleDrawRow &visible =
      root->visible_rows[visible_row_index];
  const device VkrGpuGeometryRow &geometry =
      root->geometry_rows[visible.geometry_index];
  const device VkrPackedStaticVertex *vertices =
      (const device VkrPackedStaticVertex *)geometry.vertex_address +
      geometry.first_vertex;
  const device VkrGpuGeometryDecodeRecord &decode =
      ((const device VkrGpuGeometryDecodeRecord *)
           geometry.decode_address)[visible.decode_index];
  VkrGpuDecodedVertex data =
      vkr_decode_packed_vertex(vertices[vertex_id], decode);
  const device VkrMetalPacketInstance &instance =
      frame->instances[visible.instance_index + (gpu_draw ? 0u : instance_id)];
  data = vkr_apply_deformation(data, instance.deformation_address, vertex_id);
  // Terrain tiles morph toward their next level (ADR-084).
  float previous_height;
  data = vkr_gpu_terrain_morph(data, vertices, decode, vertex_id,
                               visible.state_flags, previous_height);
  float4 world = instance.model * float4(data.position, 1.0f);
  VkrMetalTiledVertexOutput output;
  // Frame roots store the transposed matrix used by Slang's row-vector
  // lowering.
  output.position = world * frame->view_projection;
  output.texcoord = data.texcoord;
  output.color = data.color;
  output.world_position = world.xyz;
  output.world_normal = normalize(vkr_instance_transform_normal(
      instance.normal_column0.xyz, instance.normal_column1.xyz,
      instance.normal_column2.xyz, data.normal));
  output.world_tangent =
      float4(normalize((instance.model * float4(data.tangent.xyz, 0.0f)).xyz),
             data.tangent.w * instance.normal_column0.w);
  output.visible_row_index = visible_row_index;
  output.lightmap_uv = float2(0.0f);
  output.lightmap_page = ~0u;
  const uint lightmap_slot = uint(instance.normal_column2.w);
  constant VkrMetalPacketLightmap &lightmap = *frame->lightmap;
  if (lightmap_slot != 0u && lightmap_slot <= lightmap.rect_count &&
      (decode.flags & VKR_GPU_GEOMETRY_DECODE_LIGHTMAP_UV) != 0u) {
    const device VkrLightmapRect &rect = lightmap.rects[lightmap_slot - 1u];
    float2 origin =
        float2(float(rect.origin & 0xffffu), float(rect.origin >> 16u));
    float2 extent =
        float2(float(rect.extent & 0xffffu), float(rect.extent >> 16u));
    output.lightmap_uv =
        (origin + vkr_decode_packed_lightmap_uv(vertices[vertex_id]) * extent) *
        lightmap.inverse_page_size;
    output.lightmap_page = rect.page;
  }
  return output;
}

// The sky draws: the frame and the sky inputs of
// vkr_metal_packet_sky_background. The atmosphere draw also reads the
// resolved depth. Mirrors VkrMetalTiledSkyRoot.
struct VkrMetalTiledSkyRoot {
  constant VkrMetalPacketFrameRoot *frame;
  texture2d<float, access::sample> sky_view;
  float4x4 inverse_view_projection;
  uint2 extent;
  // VKR_SKY_MODE_*: fallback colour, uniform radiance or the atmosphere.
  uint sky_mode;
  uint clouds_enabled;
  // Uniform background radiance for the constant sky mode.
  float4 sky_radiance;
  depth2d<float, access::read> depth;
};

struct VkrMetalTiledSkyOutput {
  float4 position [[position]];
};

// One triangle covering the target at the far plane, so the depth test keeps
// the sky behind every surface.
vertex VkrMetalTiledSkyOutput vkr_metal_tiled_sky_vertex(uint vertex_id
                                                         [[vertex_id]]) {
  float2 corner = float2(float((vertex_id << 1u) & 2u), float(vertex_id & 2u));
  VkrMetalTiledSkyOutput output;
  output.position = float4(corner * 2.0f - 1.0f, 1.0f, 1.0f);
  return output;
}

// The clear sky behind the opaque surfaces; the discs too unless the
// atmosphere draw lays them over the clouds. Indirect-diffuse capture keeps it
// black, as deferred lighting does.
fragment float4 vkr_metal_tiled_sky_fragment(
    VkrMetalTiledSkyOutput input [[stage_in]],
    constant VkrMetalTiledSkyRoot &root [[buffer(1)]]) {
  if (root.frame->render_mode == 9u || root.frame->render_mode == 12u)
    return float4(0.0f, 0.0f, 0.0f, 1.0f);
  float3 disc;
  float3 radiance = vkr_metal_packet_sky_clear(
      root.frame, root.sky_view, root.inverse_view_projection, root.extent,
      root.sky_mode, root.sky_radiance, uint2(input.position.xy), disc);
  return float4(radiance + (root.clouds_enabled != 0u ? 0.0f : disc), 1.0f);
}

// The media over the resolved opaque image, as the desktop pipeline's
// Fog.Apply lays them over its lit image, blended as vkr_cloud_composite does:
// destination times the light it keeps plus the light the media add. Sky
// pixels take the cloud layer with the discs it lets through, then the fog;
// surfaces take aerial perspective, the farther medium, then the fog. Drawing
// them here rather than in the forward shading keeps their sampling out of
// the opaque pass, whose registers every pixel pays for. A pixel's resolved
// depth is its nearest sample's, so edge pixels take the nearer surface's
// media, as Fog.Apply does.
fragment float4 vkr_metal_tiled_atmosphere_fragment(
    VkrMetalTiledSkyOutput input [[stage_in]],
    constant VkrMetalTiledSkyRoot &root [[buffer(1)]]) {
  constant VkrMetalPacketFrameRoot *frame = root.frame;
  uint2 pixel = uint2(input.position.xy);
  if (frame->render_mode == 9u || frame->render_mode == 12u)
    discard_fragment();
  const float depth = root.depth.read(pixel);
  const bool fog = frame->fog->color_density.w > 0.0f;
  float3 added = float3(0.0f);
  float kept = 1.0f;
  float3 direction;
  if (depth >= 1.0f) {
    direction = vkr_metal_packet_sky_direction(
        frame, root.inverse_view_projection, root.extent, float2(pixel) + 0.5);
    if (root.clouds_enabled != 0u) {
      constant VkrMetalPacketSky &sky = *frame->sky;
      float3 disc = float3(0.0f);
      if (!vkr_sky_view_hits_ground(sky.params.atmosphere, direction.y))
        disc = vkr_metal_packet_sky_discs(
            sky, direction,
            vkr_metal_packet_sky_view_transmittance(sky, direction));
      float2 uv = (float2(pixel) + 0.5) / float2(root.extent);
      float4 cloud = sky.cloud_radiance.sample(vkr_metal_packet_sky_sampler, uv);
      added = cloud.rgb + disc * vkr_cloud_disc_visibility(cloud.a);
      kept = saturate(cloud.a);
    }
    // Fog lies in front of the clouds and the sky behind them, so it scales
    // both what the clouds add and the sky they keep.
    if (fog) {
      VkrFogSample sample =
          vkr_fog_sky_sample(*frame->fog, frame->view_position.xyz, direction);
      added = vkr_fog_apply_sample(
          added,
          vkr_fog_inscatter(*frame->fog, vkr_metal_packet_fog_lighting(frame),
                            vkr_fog_direction(direction)),
          sample);
      kept *= sample.transmittance;
    }
  } else {
    float4 world_h = root.inverse_view_projection *
                     float4(vkr_metal_packet_resolve_ndc(float2(pixel) + 0.5,
                                                         root.extent),
                            depth, 1.0f);
    float3 world = world_h.xyz / max(abs(world_h.w), 1e-7f) * sign(world_h.w);
    if (vkr_metal_packet_aerial_enabled(frame)) {
      VkrMetalPacketAerialSample aerial = vkr_metal_packet_aerial_sample(
          frame->sky, frame->sky->aerial_perspective, world);
      kept = 1.0f - aerial.weight * (1.0f - saturate(aerial.packed.a));
      added = aerial.packed.rgb * aerial.weight;
    }
    if (fog) {
      VkrFogSample sample = vkr_fog_surface_sample(
          *frame->fog, frame->view_position.xyz, world);
      added = vkr_fog_apply_sample(
          added,
          vkr_fog_inscatter(*frame->fog, vkr_metal_packet_fog_lighting(frame),
                            vkr_fog_direction(world -
                                              frame->view_position.xyz)),
          sample);
      kept *= sample.transmittance;
    }
  }
  return float4(added, kept);
}

// Irradiance of a lightmapped surface at `uv` on `page`: the frame's active
// layers, each scaled by its weight.
static float3 vkr_metal_tiled_lightmap_irradiance(
    constant VkrMetalPacketLightmap &lightmap, float2 uv, uint page) {
  constexpr sampler lightmap_sampler(coord::normalized, address::clamp_to_edge,
                                     filter::linear);
  float3 irradiance = float3(0.0f);
  const uint slice_base = page * lightmap.layer_count;
  for (uint i = 0u; i < lightmap.active_layer_count; ++i) {
    irradiance += lightmap.texture
                      .sample(lightmap_sampler, uv,
                              slice_base + lightmap.active_layers[i])
                      .rgb *
                  lightmap.active_weights[i];
  }
  return irradiance;
}

// A forward-drawn surface's material inputs before lighting. A terrain
// material reads the vertex colour as its layer weights, not as a tint, and
// blends its layers as the G-buffer resolve does (ADR-084).
struct VkrMetalTiledSurface {
  float4 base;
  // Tangent-space normal, applied when `normal_mapped`.
  float3 tangent_normal;
  bool normal_mapped;
  float metallic;
  float roughness;
  float occlusion;
};

static VkrMetalTiledSurface
vkr_metal_tiled_surface(thread const VkrMetalTiledVertexOutput &input,
                        constant VkrMetalPacketFrameRoot *frame,
                        const device VkrMetalPacketMaterial &material,
                        uint material_index) {
  // A draw's material decides `terrain`, so the samples under it keep their
  // implicit derivatives.
  const bool terrain = (material.flags & 2048u) != 0u;
  VkrMetalTiledSurface surface;
  surface.base = material.base_color_texture.sample(
                     material.base_color_sampler, input.texcoord) *
                 material.tint * (terrain ? float4(1.0f) : input.color);
  surface.normal_mapped = terrain || (material.flags & 1u) != 0u;
  surface.tangent_normal = float3(0.0f, 0.0f, 1.0f);
  surface.metallic = saturate(material.material_surface.x);
  surface.roughness = clamp(material.material_surface.y, 0.04f, 1.0f);
  surface.occlusion = saturate(material.material_surface.w);
  float3 orm = float3(1.0f);
  if ((material.flags & 2u) != 0u)
    orm = material.orm_texture.sample(material.orm_sampler, input.texcoord)
              .rgb;
  if ((material.flags & 1u) != 0u)
    surface.tangent_normal = vkr_normal_map_decode(
        material.normal_texture.sample(material.normal_sampler, input.texcoord)
            .xyz,
        material.material_surface.z);
  if (terrain) {
    // Explicit gradients keep the extra layers' samples defined under the
    // branches on their weights.
    VkrTerrainSurface layered = vkr_metal_packet_terrain_surface(
        vkr_terrain_layer(surface.base, orm, surface.tangent_normal,
                          float4(1.0f), material.material_surface),
        frame->terrain_materials[material_index],
        vkr_terrain_weights(input.color), input.texcoord,
        gradient2d(dfdx(input.texcoord), dfdy(input.texcoord)), true);
    surface.base = layered.base;
    surface.tangent_normal = layered.tangent_normal;
    surface.metallic = layered.metallic;
    surface.roughness = layered.roughness;
    surface.occlusion = layered.occlusion;
  } else if ((material.flags & 2u) != 0u) {
    surface.occlusion *= orm.r;
    surface.roughness = clamp(surface.roughness * orm.g, 0.04f, 1.0f);
    surface.metallic = saturate(surface.metallic * orm.b);
  }
  return surface;
}

// The media between the camera and a blended surface, as the desktop
// pipeline's Fog.Apply lays them over its lit image: aerial perspective, the
// farther medium, then analytic height fog. Blended surfaces draw after the
// atmosphere draw, so each attenuates only its own light: `feedback_weight`
// is the share of the already-fogged light behind it that it keeps, per
// channel.
static float3
vkr_metal_tiled_atmosphere(constant VkrMetalPacketFrameRoot *frame,
                           float3 radiance, float3 world_position,
                           float3 feedback_weight) {
  if (vkr_metal_packet_aerial_enabled(frame)) {
    VkrMetalPacketAerialSample aerial = vkr_metal_packet_aerial_sample(
        frame->sky, frame->sky->aerial_perspective, world_position);
    radiance = vkr_sky_apply_aerial_over_feedback(
        radiance, feedback_weight, aerial.packed, aerial.weight);
  }
  if (frame->fog->color_density.w > 0.0f) {
    float3 inscatter = vkr_fog_inscatter(
        *frame->fog, vkr_metal_packet_fog_lighting(frame),
        vkr_fog_direction(world_position - frame->view_position.xyz));
    radiance = vkr_fog_apply_local_over_feedback(
        radiance, feedback_weight, inscatter,
        vkr_fog_surface_sample(*frame->fog, frame->view_position.xyz,
                               world_position));
  }
  return radiance;
}

// One surface's light under the shared material model, split into the lobes
// transmission composes separately (vkr_transmission_compose).
struct VkrMetalTiledSurfaceLight {
  float3 diffuse;
  float3 specular;
  float3 emissive;
  float3 base;
  float3 reflectance;
  float metallic;
};

// The shading variants by the dynamic lights a frame has (ADR-087); mirrors
// VkrMetalTiledLighting. Each leaves out the code its frames do not need,
// whose registers cost the opaque pass even when no light is in range: on
// Bistro 0.65 ms median for the light loop, 1.2 ms for the desktop local
// shadow filter and 1.1 ms for rectangle lights.
enum VkrMetalTiledLighting : uint {
  VKR_METAL_TILED_LIGHTING_NONE = 0u,
  // Point and spot lights, none of them shadowed.
  VKR_METAL_TILED_LIGHTING_PUNCTUAL = 1u,
  // Point and spot lights with their local shadows.
  VKR_METAL_TILED_LIGHTING_SHADOWED = 2u,
  // Rectangle lights besides shadowed point and spot lights.
  VKR_METAL_TILED_LIGHTING_ALL = 3u,
  // Every light, and the editor's inspection modes (VkrRenderMode): unlit,
  // detail lighting, lighting only and wireframe.
  VKR_METAL_TILED_LIGHTING_INSPECT = 4u,
};

// The tiled pipeline's local shadows: one hardware-filtered tap on the opaque
// atlas, without a soft penumbra and without the refractive layers its glass
// never casts into.
struct VkrMetalTiledLocalShadow {
  float3 operator()(constant VkrMetalPacketFrameRoot *frame,
                    uint first_view_encoded, uint, uint kind,
                    float3 world_position, float3 normal) const {
    return vkr_metal_packet_local_shadow_sample<false, false>(
        frame, first_view_encoded, kind, world_position, normal);
  }

  bool contribution_cutoff() const { return false; }
  void measure(uint, float) const {}
};

// Point and spot lights take no shadow in the variant without shadowed
// lights.
struct VkrMetalTiledNoLocalShadow {
  float3 operator()(constant VkrMetalPacketFrameRoot *, uint, uint, uint,
                    float3, float3) const {
    return float3(1.0f);
  }

  bool contribution_cutoff() const { return false; }
  void measure(uint, float) const {}
};

// The reflection probe a blended surface shades with, as frame probe index
// + 1: the probe with the largest influence at the pixel, when that reaches
// one half. Opaque draws take the one their encode chose for the whole draw
// (vkr_metal_packet_draw_probe).
static uint vkr_metal_tiled_pixel_probe(constant VkrMetalPacketFrameRoot *frame,
                                        float3 world_position) {
  const uint count = min(frame->ibl_probe_count, VKR_GPU_DRAW_PROBE_MASK - 1u);
  if (frame->ibl_probes == nullptr)
    return 0u;
  uint best = 0u;
  float best_weight = 0.5f;
  for (uint i = 0u; i < count; ++i) {
    float weight =
        vkr_metal_packet_probe_influence(frame->ibl_probes[i], world_position);
    if (weight > best_weight) {
      best = i + 1u;
      best_weight = weight;
    }
  }
  return best;
}

// Shades a forward-drawn surface: the sun with its cascades and cloud shadow,
// the dynamic local lights that Lighting includes, diffuse light from its
// lightmap, the diffuse volume or the environment, and environment specular.
// The environment is `probe` (frame probe index + 1) or, for zero, the global
// environment: one per surface, where the desktop pipeline blends its probes
// per pixel.
template <VkrMetalTiledLighting Lighting>
static VkrMetalTiledSurfaceLight
vkr_metal_tiled_shade(thread const VkrMetalTiledVertexOutput &input,
                      constant VkrMetalPacketFrameRoot *frame,
                      const device VkrMetalPacketMaterial &material,
                      thread const VkrMetalTiledSurface &surface,
                      bool front_facing, uint probe) {
  // Detail lighting and lighting only shade a neutral grey dielectric
  // without emission; lighting only also leaves out normal maps.
  const bool neutral = Lighting == VKR_METAL_TILED_LIGHTING_INSPECT &&
                       vkr_editor_neutral_lighting(frame->render_mode);
  float3 geometric_normal =
      normalize(input.world_normal) * (front_facing ? 1.0f : -1.0f);
  float3 normal = geometric_normal;
  if (surface.normal_mapped &&
      !(neutral && vkr_editor_skip_normal_map(frame->render_mode))) {
    float3 sampled = surface.tangent_normal;
    float3 tangent = normalize(input.world_tangent.xyz);
    tangent = normalize(tangent - dot(tangent, normal) * normal);
    float3 bitangent =
        normalize(cross(normal, tangent)) * input.world_tangent.w;
    normal = normalize(tangent * sampled.x + bitangent * sampled.y +
                       normal * sampled.z);
  }
  float3 view = vkr_metal_packet_view_direction(frame, input.world_position);
  float no_v = max(dot(normal, view), 0.0f);
  const float3 base = neutral ? float3(0.5f) : surface.base.rgb;
  float metallic = neutral ? 0.0f : surface.metallic;
  float roughness = neutral ? 0.5f : surface.roughness;
  float ao = neutral ? 1.0f : surface.occlusion;
  float3 emissive = neutral ? float3(0.0f) : material.material_emissive.rgb;
  // Specular anti-aliasing widens the lobe by the normal's screen variance,
  // as the forward and G-buffer paths do.
  float3 normal_dx = dfdx(normal);
  float3 normal_dy = dfdy(normal);
  roughness = vkr_ggx_filter_roughness(
      roughness,
      0.25f * (dot(normal_dx, normal_dx) + dot(normal_dy, normal_dy)));
  if ((material.flags & 4u) != 0u)
    emissive *= material.emissive_texture
                    .sample(material.emissive_sampler, input.texcoord)
                    .rgb;
  emissive *= frame->pre_exposure;

  // The shared material model: clearcoat, sheen, anisotropy and diffuse
  // transmission are not modeled yet (ADR-087).
  float3 f0 = mix(neutral ? float3(0.04f)
                         : saturate(material.material_dielectric_specular.rgb),
                  base, metallic);
  VkrGgxMaterialEnergy energy =
      vkr_metal_packet_prepare_brdf(frame, no_v, roughness, f0);
  VkrMetalTiledSurfaceLight light;
  light.diffuse = float3(0.0f);
  light.specular = float3(0.0f);
  light.emissive = emissive;
  light.base = base;
  light.reflectance = energy.reflectance;
  light.metallic = metallic;

  // The sun, shadowed by its cascades and the cloud layer.
  if (frame->directional_direction_enabled.w > 0.5f) {
    float3 sun = normalize(-frame->directional_direction_enabled.xyz);
    float shadow =
        dot(normal, sun) > 0.0f
            ? vkr_metal_packet_directional_shadow_sample(
                  frame, input.world_position, normal)
                      .factor *
                  vkr_metal_packet_cloud_shadow(frame, input.world_position)
            : 0.0f;
    VkrMetalPacketDirectResult direct = vkr_metal_packet_direct(
        normal, view, sun,
        frame->directional_color_intensity.rgb *
            frame->directional_color_intensity.w * shadow,
        base, metallic, roughness, f0, energy);
    light.diffuse += direct.diffuse;
    light.specular += direct.specular;
  }

  // The dynamic local lights, the bounded set the runtime keeps, through the
  // world-space light grid with their shadows sampled inline. Static lights
  // are baked (ADR-088).
  if (Lighting != VKR_METAL_TILED_LIGHTING_NONE) {
    VkrClearcoatLayer no_coat = {};
    VkrSheenLayer no_sheen = {};
    float3 punctual_irradiance = float3(0.0f);
    float3 coat_unused = float3(0.0f);
    float3 sheen_unused = float3(0.0f);
    if (Lighting == VKR_METAL_TILED_LIGHTING_PUNCTUAL) {
      vkr_metal_packet_punctual_layered<true>(
          frame, input.world_position, normal, view, base, metallic,
          roughness, f0, energy, false, no_coat, false, no_sheen, 0.0f,
          punctual_irradiance, light.diffuse, light.specular, coat_unused,
          sheen_unused, VkrMetalTiledNoLocalShadow{});
    } else {
      vkr_metal_packet_punctual_layered<true>(
          frame, input.world_position, normal, view, base, metallic,
          roughness, f0, energy, false, no_coat, false, no_sheen, 0.0f,
          punctual_irradiance, light.diffuse, light.specular, coat_unused,
          sheen_unused, VkrMetalTiledLocalShadow{});
    }
    if (Lighting == VKR_METAL_TILED_LIGHTING_ALL ||
        Lighting == VKR_METAL_TILED_LIGHTING_INSPECT) {
      VkrMetalPacketDirectResult rectangles =
          vkr_metal_packet_layered_rectangle_lights<true, true>(
              frame, input.world_position, normal, view, base, metallic,
              roughness, f0, energy, false, no_coat, false, no_sheen,
              coat_unused, sheen_unused);
      light.diffuse += rectangles.diffuse;
      light.specular += rectangles.specular;
    }
  }

  // Diffuse light: the lightmap stores irradiance, the volume and the global
  // environment the Lambertian response; each is pre-exposed here.
  float3 diffuse_light = float3(0.0f);
  bool environment_diffuse = false;
  if (input.lightmap_page != ~0u) {
    diffuse_light = vkr_metal_tiled_lightmap_irradiance(
                        *frame->lightmap, input.lightmap_uv,
                        input.lightmap_page) *
                    (frame->pre_exposure / M_PI_F);
  } else {
    float4 volume =
        vkr_metal_packet_diffuse_volume(frame, input.world_position, normal);
    diffuse_light = volume.rgb;
    environment_diffuse = volume.w == 0.0f;
  }
  if ((frame->flags & 2u) != 0u) {
    constexpr sampler environment_sampler(coord::normalized,
                                          address::clamp_to_edge,
                                          filter::linear, mip_filter::linear);
    const float lod =
        roughness * float(max(frame->prefilter_mip_count, 1u) - 1u);
    float3 reflection = reflect(-view, normal);
    // One environment per surface: its probe, box-projected, or the global
    // environment with the cloud layer. The two paths share no live values,
    // so the probe costs the pass few registers.
    float3 prefiltered;
    uint sh_slot = frame->sh_global_slot;
    float diffuse_scale = 1.0f;
    if (probe != 0u) {
      const device VkrMetalPacketIblProbe &local =
          frame->ibl_probes[probe - 1u];
      float3 direction =
          local.intensity_box.w > 0.5f
              ? vkr_metal_packet_box_project(
                    reflection, input.world_position, local.center_blend.xyz,
                    max(local.extents_weight.xyz, 0.0f))
              : reflection;
      prefiltered =
          local.prefilter.sample(environment_sampler, direction, level(lod))
              .rgb *
          (local.intensity_box.x * local.intensity_box.z);
      sh_slot = local.sh_slot;
      diffuse_scale = local.intensity_box.x * local.intensity_box.y;
    } else {
      prefiltered = vkr_metal_packet_global_prefiltered(
          frame, environment_sampler, reflection, lod);
    }
    if (environment_diffuse)
      diffuse_light = vkr_sh_l2_evaluate(frame->sh_coefficients[sh_slot],
                                         vkr_sh_l2_prepare_evaluation(normal)) *
                      diffuse_scale * frame->ibl_controls.y *
                      frame->ibl_controls.x;
    float horizon = saturate(1.0f + dot(reflection, geometric_normal));
    float specular_visibility =
        horizon * horizon * vkr_metal_packet_specular_ao(ao, no_v, roughness);
    light.specular += prefiltered * energy.reflectance * specular_visibility *
                      frame->ibl_controls.z * frame->ibl_controls.x;
  } else if (environment_diffuse) {
    diffuse_light = frame->ambient_color.rgb;
  }
  light.diffuse +=
      energy.diffuse_weight * (1.0f - metallic) * base * diffuse_light * ao;
  return light;
}

// The editor's unlit and wireframe modes, which replace shading; false for
// the modes that shade. Unlit shows the base colour and the pre-exposed
// emission; wireframe draws one-pixel edges over a dark fill, as the
// visibility-buffer resolve does.
static bool vkr_metal_tiled_inspect(
    thread const VkrMetalTiledVertexOutput &input,
    constant VkrMetalPacketFrameRoot *frame,
    const device VkrMetalPacketMaterial &material,
    thread const VkrMetalTiledSurface &surface, float3 barycentric,
    thread float3 &out_color) {
  if (frame->render_mode == 12u) {
    out_color = vkr_editor_wire_color(barycentric, dfdx(barycentric),
                                      dfdy(barycentric));
    return true;
  }
  if (frame->render_mode == 3u) {
    float3 emissive = material.material_emissive.rgb;
    if ((material.flags & 4u) != 0u)
      emissive *= material.emissive_texture
                      .sample(material.emissive_sampler, input.texcoord)
                      .rgb;
    out_color = surface.base.rgb + emissive * frame->pre_exposure;
    return true;
  }
  return false;
}

// Shades the opaque pass. Opaque draws never discard, so hidden-surface
// removal keeps its fast path. With Coverage, alpha-tested draws return their
// alpha sharpened about the material's cut-off to a transition about one
// pixel wide; alpha to coverage turns it into the samples they cover, so their
// edges resolve like geometry edges (ADR-087). Fully uncovered fragments
// discard before shading.
template <VkrMetalTiledLighting Lighting, bool Coverage, bool Probes>
static float4
vkr_metal_tiled_forward(thread const VkrMetalTiledVertexOutput &input,
                        constant VkrMetalPacketDrawRoot *root,
                        bool front_facing, float3 barycentric) {
  constant VkrMetalPacketFrameRoot *frame = root->frame;
  const device VkrGpuVisibleDrawRow &visible =
      root->visible_rows[input.visible_row_index];
  const device VkrMetalPacketMaterial &material =
      frame->materials[visible.material_index];
  VkrMetalTiledSurface surface =
      vkr_metal_tiled_surface(input, frame, material, visible.material_index);
  float coverage = 1.0f;
  if (Coverage && material.alpha_mode == 1u) {
    coverage = saturate((surface.base.a - material.material_alpha.x) /
                            max(fwidth(surface.base.a), 1e-4f) +
                        0.5f);
    if (coverage <= 0.0f)
      discard_fragment();
  }
  if (Lighting == VKR_METAL_TILED_LIGHTING_INSPECT) {
    float3 inspected;
    if (vkr_metal_tiled_inspect(input, frame, material, surface, barycentric,
                                inspected))
      return float4(inspected, coverage);
  }
  VkrMetalTiledSurfaceLight light = vkr_metal_tiled_shade<Lighting>(
      input, frame, material, surface, front_facing,
      Probes ? (visible.state_flags >> VKR_GPU_DRAW_PROBE_SHIFT) &
                   VKR_GPU_DRAW_PROBE_MASK
             : 0u);
  return float4(light.diffuse + light.specular + light.emissive, coverage);
}

fragment float4 vkr_metal_tiled_forward_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_NONE, false, false>(
      input, root, front_facing, float3(0.0f));
}

fragment float4 vkr_metal_tiled_forward_punctual_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_PUNCTUAL, false, false>(
      input, root, front_facing, float3(0.0f));
}

fragment float4 vkr_metal_tiled_forward_shadowed_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_SHADOWED, false, false>(
      input, root, front_facing, float3(0.0f));
}

fragment float4 vkr_metal_tiled_forward_all_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_ALL, false, false>(
      input, root, front_facing, float3(0.0f));
}

fragment float4 vkr_metal_tiled_forward_inspect_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]],
    float3 barycentric [[barycentric_coord]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_INSPECT, false, true>(
      input, root, front_facing, barycentric);
}

fragment float4 vkr_metal_tiled_forward_coverage_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_NONE, true, false>(
      input, root, front_facing, float3(0.0f));
}

fragment float4 vkr_metal_tiled_forward_punctual_coverage_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_PUNCTUAL, true, false>(
      input, root, front_facing, float3(0.0f));
}

fragment float4 vkr_metal_tiled_forward_shadowed_coverage_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_SHADOWED, true, false>(
      input, root, front_facing, float3(0.0f));
}

fragment float4 vkr_metal_tiled_forward_all_coverage_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_ALL, true, false>(
      input, root, front_facing, float3(0.0f));
}

fragment float4 vkr_metal_tiled_forward_inspect_coverage_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]],
    float3 barycentric [[barycentric_coord]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_INSPECT, true, true>(
      input, root, front_facing, barycentric);
}

// The same variants for frames whose camera sees a reflection probe volume
// (ADR-087); probe shading costs the opaque pass registers on every pixel, so
// frames without a visible probe keep the variants above.

fragment float4 vkr_metal_tiled_forward_probes_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_NONE, false, true>(
      input, root, front_facing, float3(0.0f));
}

fragment float4 vkr_metal_tiled_forward_punctual_probes_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_PUNCTUAL, false, true>(
      input, root, front_facing, float3(0.0f));
}

fragment float4 vkr_metal_tiled_forward_shadowed_probes_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_SHADOWED, false, true>(
      input, root, front_facing, float3(0.0f));
}

fragment float4 vkr_metal_tiled_forward_all_probes_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_ALL, false, true>(
      input, root, front_facing, float3(0.0f));
}

fragment float4 vkr_metal_tiled_forward_coverage_probes_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_NONE, true, true>(
      input, root, front_facing, float3(0.0f));
}

fragment float4 vkr_metal_tiled_forward_punctual_coverage_probes_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_PUNCTUAL, true, true>(
      input, root, front_facing, float3(0.0f));
}

fragment float4 vkr_metal_tiled_forward_shadowed_coverage_probes_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_SHADOWED, true, true>(
      input, root, front_facing, float3(0.0f));
}

fragment float4 vkr_metal_tiled_forward_all_coverage_probes_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_ALL, true, true>(
      input, root, front_facing, float3(0.0f));
}

// The opaque pass's multisample resolve (ADR-087). A hardware resolve
// averages scene-linear samples, so a bright sample dominates its pixel and
// edges against light sources keep their steps. This tile kernel replaces a
// pixel's samples with their average weighted by 1 / (1 + largest channel),
// a tone-mapped resolve, and the pass's resolve then stores that value.
struct VkrMetalTiledResolvePixel {
  half4 color [[color(0)]];
};

kernel void vkr_metal_tiled_resolve_tile(
    imageblock<VkrMetalTiledResolvePixel, imageblock_layout_implicit> block,
    ushort2 coord [[thread_position_in_threadgroup]]) {
  ushort sample_count = block.get_num_samples();
  float4 sum = float4(0.0f);
  float weight_sum = 0.0f;
  for (ushort sample = 0; sample < sample_count; ++sample) {
    float4 color = float4(
        block.read(coord, sample, imageblock_data_rate::sample).color);
    float weight = 1.0f / (1.0f + max(color.r, max(color.g, color.b)));
    sum += color * weight;
    weight_sum += weight;
  }
  VkrMetalTiledResolvePixel pixel;
  pixel.color = half4(sum / weight_sum);
  block.write(pixel, coord, ushort((1u << sample_count) - 1u));
}

// The blend pass's two outputs: the surface's own light, and the factor the
// light behind it keeps, per channel (dual-source blending: destination
// times the second output plus the first).
struct VkrMetalTiledBlendOutput {
  float4 color [[color(0), index(0)]];
  float4 behind [[color(0), index(1)]];
};

// Glass and alpha-blended surfaces, drawn back to front over the resolved
// image. Glass composes as the transmission passes do
// (vkr_transmission_compose): the light behind it arrives through the
// hardware blend, so stacked panes compose in draw order. The tiled pipeline
// draws all glass as thin and smooth: it takes no refraction offset, rough
// blur, volume attenuation or transmission textures (ADR-087).
template <VkrMetalTiledLighting Lighting, bool Probes>
static VkrMetalTiledBlendOutput
vkr_metal_tiled_blend(thread const VkrMetalTiledVertexOutput &input,
                      constant VkrMetalPacketDrawRoot *root,
                      bool front_facing, float3 barycentric) {
  constant VkrMetalPacketFrameRoot *frame = root->frame;
  const device VkrGpuVisibleDrawRow &visible =
      root->visible_rows[input.visible_row_index];
  const device VkrMetalPacketMaterial &material =
      frame->materials[visible.material_index];
  VkrMetalTiledSurface surface =
      vkr_metal_tiled_surface(input, frame, material, visible.material_index);
  float transmission = saturate(material.material_alpha.y);
  float alpha = transmission > 0.0f ? 1.0f : surface.base.a;
  if (alpha <= 1e-4f)
    discard_fragment();
  VkrMetalTiledBlendOutput output;
  if (Lighting == VKR_METAL_TILED_LIGHTING_INSPECT) {
    float3 inspected;
    if (vkr_metal_tiled_inspect(input, frame, material, surface, barycentric,
                                inspected)) {
      output.color = float4(inspected * alpha, 0.0f);
      output.behind = float4(1.0f - alpha);
      return output;
    }
  }
  VkrMetalTiledSurfaceLight light = vkr_metal_tiled_shade<Lighting>(
      input, frame, material, surface, front_facing,
      Probes ? vkr_metal_tiled_pixel_probe(frame, input.world_position) : 0u);
  if (transmission > 0.0f) {
    VkrTransmissionLobes lobes = {light.diffuse, light.specular,
                                  light.emissive};
    VkrTransmissionLobes no_lobes = {float3(0.0f), float3(0.0f),
                                     float3(0.0f)};
    output.color = float4(vkr_transmission_compose(
                              lobes, float3(0.0f), light.base,
                              light.reflectance, transmission, light.metallic),
                          0.0f);
    output.behind = float4(vkr_transmission_compose(
                               no_lobes, float3(1.0f), light.base,
                               light.reflectance, transmission, light.metallic),
                           1.0f);
  } else {
    output.color =
        float4((light.diffuse + light.specular + light.emissive) * alpha, 0.0f);
    output.behind = float4(1.0f - alpha);
  }
  output.color.rgb = vkr_metal_tiled_atmosphere(
      frame, output.color.rgb, input.world_position, output.behind.rgb);
  return output;
}

fragment VkrMetalTiledBlendOutput vkr_metal_tiled_blend_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_blend<VKR_METAL_TILED_LIGHTING_NONE, false>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledBlendOutput vkr_metal_tiled_blend_punctual_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_blend<VKR_METAL_TILED_LIGHTING_PUNCTUAL, false>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledBlendOutput vkr_metal_tiled_blend_shadowed_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_blend<VKR_METAL_TILED_LIGHTING_SHADOWED, false>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledBlendOutput vkr_metal_tiled_blend_all_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_blend<VKR_METAL_TILED_LIGHTING_ALL, false>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledBlendOutput vkr_metal_tiled_blend_inspect_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]],
    float3 barycentric [[barycentric_coord]]) {
  return vkr_metal_tiled_blend<VKR_METAL_TILED_LIGHTING_INSPECT, true>(
      input, root, front_facing, barycentric);
}

fragment VkrMetalTiledBlendOutput vkr_metal_tiled_blend_probes_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_blend<VKR_METAL_TILED_LIGHTING_NONE, true>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledBlendOutput vkr_metal_tiled_blend_punctual_probes_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_blend<VKR_METAL_TILED_LIGHTING_PUNCTUAL, true>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledBlendOutput vkr_metal_tiled_blend_shadowed_probes_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_blend<VKR_METAL_TILED_LIGHTING_SHADOWED, true>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledBlendOutput vkr_metal_tiled_blend_all_probes_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_blend<VKR_METAL_TILED_LIGHTING_ALL, true>(
      input, root, front_facing, float3(0.0f));
}
