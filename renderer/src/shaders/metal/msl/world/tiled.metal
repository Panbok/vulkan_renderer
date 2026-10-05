/* The tiled pipeline's opaque render pass (ADR-087): forward shading of the
 * culled opaque draws after a depth pre-pass, and the clear background sky,
 * in one multisampled render pass resolved on chip. The cloud trace needs the
 * resolved depth, so a later full-screen draw lays the cloud layer and the
 * discs it lets through over the sky.
 *
 * Static surfaces take diffuse light from their baked lightmap (ADR-088):
 * the frame's active layers, the two sun keys nearest the sun and every lamp
 * group, each scaled by its weight. The runtime adds the sun's direct light
 * with its cascades and cloud shadow, and specular light from the global
 * environment. Surfaces without a lightmap take diffuse light from the baked
 * diffuse volume, else the global environment. */

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

// The sky draws: the frame and the deferred-lighting sky inputs
// (vkr_metal_packet_sky_background). The cloud draw also reads the resolved
// depth. Mirrors VkrMetalTiledSkyRoot.
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

// The clear sky behind the opaque surfaces; the discs too unless the cloud
// draw lays them over the clouds. Indirect-diffuse capture keeps it black, as
// deferred lighting does.
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

// The cloud layer over the sky pixels of the resolved image, blended as
// vkr_cloud_composite does (destination times transmittance plus cloud
// radiance), with the discs the clouds let through.
fragment float4 vkr_metal_tiled_clouds_fragment(
    VkrMetalTiledSkyOutput input [[stage_in]],
    constant VkrMetalTiledSkyRoot &root [[buffer(1)]]) {
  uint2 pixel = uint2(input.position.xy);
  if (root.depth.read(pixel) < 1.0f || root.frame->render_mode == 9u ||
      root.frame->render_mode == 12u)
    discard_fragment();
  constant VkrMetalPacketSky &sky = *root.frame->sky;
  float3 direction = vkr_metal_packet_sky_direction(
      root.frame, root.inverse_view_projection, root.extent,
      float2(pixel) + 0.5);
  float3 disc = float3(0.0f);
  if (!vkr_sky_view_hits_ground(sky.params.atmosphere, direction.y))
    disc = vkr_metal_packet_sky_discs(
        sky, direction, vkr_metal_packet_sky_view_transmittance(sky, direction));
  float2 uv = (float2(pixel) + 0.5) / float2(root.extent);
  float4 cloud = sky.cloud_radiance.sample(vkr_metal_packet_sky_sampler, uv);
  return float4(cloud.rgb + disc * vkr_cloud_disc_visibility(cloud.a),
                saturate(cloud.a));
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

// Shades a forward-drawn surface: the sun with its cascades and cloud shadow,
// diffuse light from its lightmap, the diffuse volume or the global
// environment, and environment specular.
static VkrMetalTiledSurfaceLight
vkr_metal_tiled_shade(thread const VkrMetalTiledVertexOutput &input,
                      constant VkrMetalPacketFrameRoot *frame,
                      const device VkrMetalPacketMaterial &material,
                      float3 base, bool front_facing) {
  float3 geometric_normal =
      normalize(input.world_normal) * (front_facing ? 1.0f : -1.0f);
  float3 normal = geometric_normal;
  if ((material.flags & 1u) != 0u) {
    float3 sampled = vkr_normal_map_decode(
        material.normal_texture.sample(material.normal_sampler, input.texcoord)
            .xyz,
        material.material_surface.z);
    float3 tangent = normalize(input.world_tangent.xyz);
    tangent = normalize(tangent - dot(tangent, normal) * normal);
    float3 bitangent =
        normalize(cross(normal, tangent)) * input.world_tangent.w;
    normal = normalize(tangent * sampled.x + bitangent * sampled.y +
                       normal * sampled.z);
  }
  float3 view = vkr_metal_packet_view_direction(frame, input.world_position);
  float no_v = max(dot(normal, view), 0.0f);
  float metallic = saturate(material.material_surface.x);
  float roughness = clamp(material.material_surface.y, 0.04f, 1.0f);
  float ao = saturate(material.material_surface.w);
  float3 emissive = material.material_emissive.rgb;
  if ((material.flags & 2u) != 0u) {
    float3 orm =
        material.orm_texture.sample(material.orm_sampler, input.texcoord).rgb;
    ao *= orm.r;
    roughness = clamp(roughness * orm.g, 0.04f, 1.0f);
    metallic = saturate(metallic * orm.b);
  }
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
  float3 f0 =
      mix(saturate(material.material_dielectric_specular.rgb), base, metallic);
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
    if (environment_diffuse)
      diffuse_light = vkr_sh_l2_evaluate(
                          frame->sh_coefficients[frame->sh_global_slot],
                          vkr_sh_l2_prepare_evaluation(normal)) *
                      frame->ibl_controls.y * frame->ibl_controls.x;
    float3 reflection = reflect(-view, normal);
    float horizon = saturate(1.0f + dot(reflection, geometric_normal));
    float specular_visibility =
        horizon * horizon * vkr_metal_packet_specular_ao(ao, no_v, roughness);
    float3 prefiltered = vkr_metal_packet_global_prefiltered(
        frame, environment_sampler, reflection,
        roughness * float(max(frame->prefilter_mip_count, 1u) - 1u));
    light.specular += prefiltered * energy.reflectance * specular_visibility *
                      frame->ibl_controls.z * frame->ibl_controls.x;
  } else if (environment_diffuse) {
    diffuse_light = frame->ambient_color.rgb;
  }
  light.diffuse +=
      energy.diffuse_weight * (1.0f - metallic) * base * diffuse_light * ao;
  return light;
}

fragment float4 vkr_metal_tiled_forward_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  constant VkrMetalPacketFrameRoot *frame = root->frame;
  const device VkrGpuVisibleDrawRow &visible =
      root->visible_rows[input.visible_row_index];
  const device VkrMetalPacketMaterial &material =
      frame->materials[visible.material_index];
  float4 base = material.base_color_texture.sample(material.base_color_sampler,
                                                   input.texcoord) *
                material.tint * input.color;
  if (material.alpha_mode == 1u && base.a < material.material_alpha.x)
    discard_fragment();
  VkrMetalTiledSurfaceLight light =
      vkr_metal_tiled_shade(input, frame, material, base.rgb, front_facing);
  return float4(light.diffuse + light.specular + light.emissive, 1.0f);
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
fragment VkrMetalTiledBlendOutput vkr_metal_tiled_blend_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  constant VkrMetalPacketFrameRoot *frame = root->frame;
  const device VkrGpuVisibleDrawRow &visible =
      root->visible_rows[input.visible_row_index];
  const device VkrMetalPacketMaterial &material =
      frame->materials[visible.material_index];
  float4 base = material.base_color_texture.sample(material.base_color_sampler,
                                                   input.texcoord) *
                material.tint * input.color;
  float transmission = saturate(material.material_alpha.y);
  float alpha = transmission > 0.0f ? 1.0f : base.a;
  if (alpha <= 1e-4f)
    discard_fragment();
  VkrMetalTiledSurfaceLight light =
      vkr_metal_tiled_shade(input, frame, material, base.rgb, front_facing);
  VkrMetalTiledBlendOutput output;
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
  return output;
}
