/* The tiled pipeline's opaque render pass (ADR-087): forward shading of the
 * culled opaque draws, and the clear background sky, in one multisampled
 * render pass resolved on chip. The cloud trace needs the resolved depth, so
 * a later full-screen draw lays the cloud layer and the discs it lets through
 * over the sky, and aerial perspective and fog over every pixel.
 *
 * Static surfaces take diffuse light from their baked lightmap (ADR-088):
 * the frame's active layers, the two sun keys nearest the sun and every lamp
 * group, each scaled by its weight. The runtime adds the sun's direct light
 * with its cascades and cloud shadow, the dynamic local lights with their
 * shadows, and specular light from the global environment. Surfaces without a
 * lightmap take diffuse light from the baked diffuse volume, else the global
 * environment. */

// Sun contact frames (VkrPreparedFrame.tiled_sun_contact) take the opaque
// pass's variants with this constant set: its draws also write the sun's
// light to a second target, which the atmosphere draw reads to remove the
// part a contact march finds occluded. Every pipeline that reaches these
// entries specializes it.
constant bool vkr_tiled_sun_output [[function_constant(1)]];

// The opaque pass's outputs: the lit colour and, in sun contact frames, the
// sun's light within it, after its cascades and cloud shadow.
struct VkrMetalTiledForwardOutput {
  float4 color [[color(0)]];
  half4 sun_light [[color(1), function_constant(vkr_tiled_sun_output)]];
};

// The forward vertex. It transforms like the Slang vkr_metal_packet_vertex,
// whose draw root sits at the same buffer for the GPU-encoded commands, and
// adds the draw's lightmap coordinates: an instance's lightmap slot travels as
// an exact float (ADR-088) and its lightmap UVs span its rectangle on its
// page.
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
  output.lightmap_direct_uv = float2(0.0f);
  output.lightmap_direct_page = ~0u;
  const uint lightmap_slot = uint(instance.normal_column2.w);
  constant VkrMetalPacketLightmap &lightmap = *frame->lightmap;
  if (lightmap_slot != 0u && lightmap_slot <= lightmap.rect_count &&
      (decode.flags & VKR_GPU_GEOMETRY_DECODE_LIGHTMAP_UV) != 0u) {
    const device VkrLightmapRect &rect = lightmap.rects[lightmap_slot - 1u];
    float2 origin =
        float2(float(rect.origin & 0xffffu), float(rect.origin >> 16u));
    float2 extent =
        float2(float(rect.extent & 0xffffu), float(rect.extent >> 16u));
    const float2 chart_uv = vkr_decode_packed_lightmap_uv(vertices[vertex_id]);
    output.lightmap_uv = (origin + chart_uv * extent) * lightmap.inverse_page_size;
    output.lightmap_page = rect.page;
    // The same chart on the lamp-direct pages (ADR-108); page ~0u for an
    // instance no lamp reaches.
    if (lightmap.direct_layer_count != 0u &&
        lightmap.direct_rects[lightmap_slot - 1u].page != ~0u) {
      const device VkrLightmapRect &direct =
          lightmap.direct_rects[lightmap_slot - 1u];
      float2 direct_origin =
          float2(float(direct.origin & 0xffffu), float(direct.origin >> 16u));
      float2 direct_extent =
          float2(float(direct.extent & 0xffffu), float(direct.extent >> 16u));
      output.lightmap_direct_uv = (direct_origin + chart_uv * direct_extent) *
                                  lightmap.inverse_page_size;
      output.lightmap_direct_page = direct.page;
    }
  }
  return output;
}

// The sky draws: the frame and the sky inputs of
// vkr_metal_packet_sky_background. The atmosphere draw also reads the
// resolved depth, and in sun contact frames the opaque pass's sun light and
// the contact march's noise index. Mirrors VkrMetalTiledSkyRoot.
struct VkrMetalTiledSkyRoot {
  constant VkrMetalPacketFrameRoot *frame;
  texture2d<float, access::sample> sky_view;
  // Places the resolved surfaces in the world.
  float4x4 inverse_view_projection;
  // The sky's camera-relative view directions (vkr_atmosphere_view_rays).
  float4x4 view_rays;
  uint2 extent;
  // VKR_SKY_MODE_*: fallback colour, uniform radiance or the atmosphere.
  uint sky_mode;
  uint clouds_enabled;
  // Uniform background radiance for the constant sky mode.
  float4 sky_radiance;
  depth2d<float, access::read> depth;
  texture2d<float, access::read> sun_light;
  uint contact_noise_index;
  // A resolved depth's distance along the camera axis:
  // depth_scale / (depth + depth_offset).
  float depth_offset;
  float depth_scale;
  uint reserved;
};

struct VkrMetalTiledSkyOutput {
  float4 position [[position]];
};

// One triangle covering the target at the far plane, reversed depth 0, so
// the depth test keeps the sky behind every surface (ADR-109).
vertex VkrMetalTiledSkyOutput vkr_metal_tiled_sky_vertex(uint vertex_id
                                                         [[vertex_id]]) {
  float2 corner = float2(float((vertex_id << 1u) & 2u), float(vertex_id & 2u));
  VkrMetalTiledSkyOutput output;
  output.position = float4(corner * 2.0f - 1.0f, 0.0f, 1.0f);
  return output;
}

// The clear sky at `pixel`; the discs too unless the atmosphere draw lays
// them over the clouds. The sky draw writes it and the atmosphere draw
// recomputes it to split edge pixels, so both take it from here.
static float3
vkr_metal_tiled_sky_background(constant VkrMetalTiledSkyRoot &root,
                               uint2 pixel) {
  float3 disc;
  float3 radiance = vkr_metal_packet_sky_clear(
      root.frame, root.sky_view, root.view_rays, root.extent, root.sky_mode,
      root.sky_radiance, pixel, disc);
  return radiance + (root.clouds_enabled != 0u ? 0.0f : disc);
}

// The clear sky behind the opaque surfaces. Its alpha is zero, the surfaces'
// one, so the resolve can record the share of each pixel that is surface.
// Indirect-diffuse capture keeps it black, as deferred lighting does. The sun
// lights no sky pixel.
fragment VkrMetalTiledForwardOutput vkr_metal_tiled_sky_fragment(
    VkrMetalTiledSkyOutput input [[stage_in]],
    constant VkrMetalTiledSkyRoot &root [[buffer(1)]]) {
  if (root.frame->render_mode == 9u || root.frame->render_mode == 12u ||
      vkr_editor_data_view(root.frame->render_mode))
    return {float4(0.0f), half4(0.0h)};
  return {float4(vkr_metal_tiled_sky_background(root, uint2(input.position.xy)),
                 0.0f),
          half4(0.0h)};
}

// The media over the resolved opaque image, as the desktop pipeline's
// Fog.Apply lays them over its lit image: destination times the light it
// keeps plus the light the media add. Sky pixels take the cloud layer with the
// discs it lets through, then the fog; surfaces take aerial perspective, the
// farther medium, then the fog. Drawing them here rather than in the forward
// shading keeps their sampling out of the opaque pass, whose registers every
// pixel pays for. The destination's alpha, read through programmable
// blending, is the share of the pixel's resolve weight that surfaces hold
// (vkr_metal_tiled_resolve_tile). An edge pixel mixing sky and surface
// samples splits into its clear sky, recomputed here, and the rest, and each
// part takes its own media; the resolved depth is the nearest surface's.
// The light a medium adds and the share of the light behind it it keeps.
struct VkrMetalTiledMedia {
  float3 added;
  float kept;
};

// The world position of the resolved surface at `pixel_center` with
// reversed depth `depth`.
static float3 vkr_metal_tiled_resolved_world(constant VkrMetalTiledSkyRoot &root,
                                             float2 pixel_center, float depth) {
  float4 world_h =
      root.inverse_view_projection *
      float4(vkr_metal_packet_resolve_ndc(pixel_center, root.extent), depth,
             1.0f);
  return world_h.xyz / max(abs(world_h.w), 1e-7f) * sign(world_h.w);
}

static VkrMetalTiledMedia
vkr_metal_tiled_sky_media(constant VkrMetalTiledSkyRoot &root, uint2 pixel) {
  constant VkrMetalPacketFrameRoot *frame = root.frame;
  VkrMetalTiledMedia media = {float3(0.0f), 1.0f};
  const float3 direction = vkr_metal_packet_sky_direction(
      root.view_rays, root.extent, float2(pixel) + 0.5);
  if (root.clouds_enabled != 0u) {
    constant VkrMetalPacketSky &sky = *frame->sky;
    float3 disc = float3(0.0f);
    if (!vkr_sky_view_hits_ground(sky.params.atmosphere, direction.y))
      disc = vkr_metal_packet_sky_discs(
          sky, direction,
          vkr_metal_packet_sky_view_transmittance(sky, direction));
    float2 uv = (float2(pixel) + 0.5) / float2(root.extent);
    float4 cloud = sky.cloud_radiance.sample(vkr_metal_packet_sky_sampler, uv);
    media.added = cloud.rgb + disc * vkr_cloud_disc_visibility(cloud.a);
    media.kept = saturate(cloud.a);
  }
  // Fog lies in front of the clouds and the sky behind them, so it scales
  // both what the clouds add and the sky they keep.
  if (frame->fog->color_density.w > 0.0f) {
    VkrFogSample sample =
        vkr_fog_sky_sample(*frame->fog, frame->view_position.xyz, direction);
    media.added = vkr_fog_apply_sample(
        media.added,
        vkr_fog_inscatter(*frame->fog, vkr_metal_packet_fog_lighting(frame),
                          vkr_fog_direction(direction)),
        sample);
    media.kept *= sample.transmittance;
  }
  return media;
}

static VkrMetalTiledMedia
vkr_metal_tiled_surface_media(constant VkrMetalTiledSkyRoot &root, uint2 pixel,
                              float depth) {
  constant VkrMetalPacketFrameRoot *frame = root.frame;
  VkrMetalTiledMedia media = {float3(0.0f), 1.0f};
  float3 world =
      vkr_metal_tiled_resolved_world(root, float2(pixel) + 0.5f, depth);
  if (vkr_metal_packet_aerial_enabled(frame)) {
    VkrMetalPacketAerialSample aerial = vkr_metal_packet_aerial_sample(
        frame->sky, frame->sky->aerial_perspective, world);
    media.kept = 1.0f - aerial.weight * (1.0f - saturate(aerial.packed.a));
    media.added = aerial.packed.rgb * aerial.weight;
  }
  if (frame->fog->color_density.w > 0.0f) {
    VkrFogSample sample =
        vkr_fog_surface_sample(*frame->fog, frame->view_position.xyz, world);
    media.added = vkr_fog_apply_sample(
        media.added,
        vkr_fog_inscatter(
            *frame->fog, vkr_metal_packet_fog_lighting(frame),
            vkr_fog_direction(world - frame->view_position.xyz)),
        sample);
    media.kept *= sample.transmittance;
  }
  return media;
}

// Sun contact visibility of the resolved surface `world` at `pixel`: marches
// the resolved depth toward the sun and stops at the first sample behind a
// nearby surface (shadow_kernel.slangh). Depths compare along the camera
// axis, where a sample's distance is its clip w, so a step reads one depth
// and places no world position. The march leaves the screen without
// occlusion. The sun shades only surfaces that face it, so the start offset
// follows the sun rather than a normal.
static float vkr_metal_tiled_sun_contact(constant VkrMetalTiledSkyRoot &root,
                                         uint2 pixel, float3 world) {
  constant VkrMetalPacketFrameRoot *frame = root.frame;
  const float camera_distance = distance(world, frame->view_position.xyz);
  const float strength = vkr_sun_contact_strength(camera_distance);
  if (strength <= 0.0f)
    return 1.0f;
  const float3 sun = normalize(-frame->directional_direction_enabled.xyz);
  const float3 origin =
      world + sun * vkr_local_shadow_contact_start_offset(camera_distance);
  const float march_length = vkr_sun_contact_length(camera_distance);
  const float noise =
      vkr_local_shadow_contact_noise(pixel, root.contact_noise_index);
  // Frame roots store the transposed matrix used by Slang's row-vector
  // lowering.
  const float4 clip_origin = float4(origin, 1.0f) * frame->view_projection;
  const float4 clip_span =
      float4(sun * march_length, 0.0f) * frame->view_projection;
  for (uint step = 0u; step < VKR_SUN_CONTACT_STEPS; ++step) {
    const float t = (float(step) + noise) / float(VKR_SUN_CONTACT_STEPS);
    const float4 clip = clip_origin + t * clip_span;
    if (clip.w <= 1e-4f)
      break;
    const float2 ndc = clip.xy / clip.w;
    if (any(abs(ndc) >= 1.0f))
      break;
    // The inverse of vkr_metal_packet_resolve_ndc: y runs down in pixels.
    const float2 sample_center =
        float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f) * float2(root.extent);
    const uint2 sample_pixel = min(uint2(sample_center), root.extent - 1u);
    const float sample_depth = root.depth.read(sample_pixel);
    if (sample_depth <= 0.0f)
      continue;
    const float surface_in_front =
        clip.w - root.depth_scale / (sample_depth + root.depth_offset);
    if (vkr_local_shadow_contact_occluded(surface_in_front, clip.w))
      return 1.0f - strength * (1.0f - vkr_local_shadow_contact_visibility(t));
  }
  return 1.0f;
}

fragment float4 vkr_metal_tiled_atmosphere_fragment(
    VkrMetalTiledSkyOutput input [[stage_in]],
    constant VkrMetalTiledSkyRoot &root [[buffer(1)]],
    float4 destination [[color(0)]]) {
  constant VkrMetalPacketFrameRoot *frame = root.frame;
  uint2 pixel = uint2(input.position.xy);
  if (frame->render_mode == 9u || frame->render_mode == 12u ||
      vkr_editor_data_view(frame->render_mode))
    discard_fragment();
  const float depth = root.depth.read(pixel);
  // Reversed depth 0 is the far plane: only sky reached the pixel.
  const float surface = depth <= 0.0f ? 0.0f : saturate(destination.a);
  if (surface <= 0.0f) {
    const VkrMetalTiledMedia sky = vkr_metal_tiled_sky_media(root, pixel);
    return float4(destination.rgb * sky.kept + sky.added, 1.0f);
  }
  // In sun contact frames the sun's light that the march finds occluded
  // leaves the surfaces' light first, before the media scale it: the sun's
  // term is linear in its visibility. The sky's samples hold no sun light,
  // and an edge pixel's sun light is its samples' mean, which the nearest
  // sample's visibility scales.
  float3 lit = destination.rgb;
  if (vkr_tiled_sun_output) {
    const float3 sun_light = root.sun_light.read(pixel).rgb;
    if (any(sun_light > 0.0f)) {
      const float3 world =
          vkr_metal_tiled_resolved_world(root, float2(pixel) + 0.5f, depth);
      const float visibility = vkr_metal_tiled_sun_contact(root, pixel, world);
      lit = max(lit - sun_light * (1.0f - visibility), 0.0f);
    }
  }
  const VkrMetalTiledMedia near =
      vkr_metal_tiled_surface_media(root, pixel, depth);
  if (surface >= 1.0f)
    return float4(lit * near.kept + near.added, 1.0f);
  // The resolve averaged the sky's samples, all one clear-sky colour, with
  // the surfaces' by weight, so the surfaces' part is what remains.
  const VkrMetalTiledMedia sky = vkr_metal_tiled_sky_media(root, pixel);
  const float3 clear_sky = vkr_metal_tiled_sky_background(root, pixel);
  const float3 surfaces = max(lit - (1.0f - surface) * clear_sky, 0.0f);
  return float4(surfaces * near.kept + surface * near.added +
                    (1.0f - surface) * (clear_sky * sky.kept + sky.added),
                1.0f);
}

// A lamp-direct page's light at `uv` (ADR-108): the cubic B-spline over the
// 4x4 texels around it, from four bilinear taps, so a shadow edge baked at
// a few centimetres per texel shows no texel steps.
static float3 vkr_metal_tiled_lamp_direct(
    constant VkrMetalPacketLightmap &lightmap, sampler lightmap_sampler,
    float2 uv, uint slice) {
  const float texels = 1.0f / lightmap.inverse_page_size;
  const float2 position = uv * texels - 0.5f;
  const float2 base = floor(position);
  const float2 f = position - base;
  const float2 f2 = f * f;
  const float2 f3 = f2 * f;
  const float2 w0 = (-f3 + 3.0f * f2 - 3.0f * f + 1.0f) / 6.0f;
  const float2 w1 = (3.0f * f3 - 6.0f * f2 + 4.0f) / 6.0f;
  const float2 w2 = (-3.0f * f3 + 3.0f * f2 + 3.0f * f + 1.0f) / 6.0f;
  const float2 w3 = f3 / 6.0f;
  const float2 g0 = w0 + w1;
  const float2 g1 = w2 + w3;
  // Each bilinear tap lands between two texel centres in its pair's ratio.
  const float2 near = (base - 0.5f + w1 / g0) * lightmap.inverse_page_size;
  const float2 far = (base + 1.5f + w3 / g1) * lightmap.inverse_page_size;
  const float3 s00 =
      lightmap.lamp_direct.sample(lightmap_sampler, near, slice).rgb;
  const float3 s10 =
      lightmap.lamp_direct.sample(lightmap_sampler, float2(far.x, near.y), slice)
          .rgb;
  const float3 s01 =
      lightmap.lamp_direct.sample(lightmap_sampler, float2(near.x, far.y), slice)
          .rgb;
  const float3 s11 =
      lightmap.lamp_direct.sample(lightmap_sampler, far, slice).rgb;
  return g0.y * (g0.x * s00 + g1.x * s10) + g1.y * (g0.x * s01 + g1.x * s11);
}

// Irradiance of a lightmapped surface at `uv` on `page`, the frame's active
// layers each scaled by its weight, a lamp group's lamp-direct page at
// `direct_uv` on `direct_page` included (ADR-108), and in alpha the baked
// ambient visibility every layer's alpha carries (ADR-088; one for sets
// baked without it or with no active layer). `sky_luminance` receives the
// luminance of the sun-key layers' part, the sky light and sun bounce
// (ADR-054).
static float4 vkr_metal_tiled_lightmap_irradiance(
    constant VkrMetalPacketLightmap &lightmap, float2 uv, uint page,
    float2 direct_uv, uint direct_page, thread float &sky_luminance) {
  constexpr sampler lightmap_sampler(coord::normalized, address::clamp_to_edge,
                                     filter::linear);
  float4 irradiance = float4(0.0f, 0.0f, 0.0f, 1.0f);
  sky_luminance = 0.0f;
  const uint slice_base = page * lightmap.layer_count;
  const uint direct_base = direct_page * lightmap.direct_layer_count;
  for (uint i = 0u; i < lightmap.active_layer_count; ++i) {
    float4 layer = lightmap.texture.sample(
        lightmap_sampler, uv, slice_base + lightmap.active_layers[i]);
    if (direct_page != ~0u && lightmap.active_direct[i] != ~0u)
      layer.rgb += vkr_metal_tiled_lamp_direct(
          lightmap, lightmap_sampler, direct_uv,
          direct_base + lightmap.active_direct[i]);
    irradiance.rgb += layer.rgb * lightmap.active_weights[i];
    if ((lightmap.active_lamp_mask & (1u << i)) == 0u)
      sky_luminance += dot(layer.rgb, float3(0.2126f, 0.7152f, 0.0722f)) *
                       lightmap.active_weights[i];
    if (i == 0u)
      irradiance.a = saturate(layer.a);
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
  // The roughness before the shading floor, which picks the level of the
  // refraction source a volume blurs with (ADR-087).
  float feedback_roughness;
  float occlusion;
  // Emitted light before pre-exposure.
  float3 emissive;
};

// A forward-drawn surface's base colour and alpha: its texture times the
// material's tint and, except on terrain, the vertex colour. Alpha-tested
// draws read it alone first, so fragments that cover no sample discard
// before the surface's other textures.
// Under the temporal upscaler the frame's texture LOD bias picks the mips the
// output resolution would, which the jittered history then resolves.
static float4
vkr_metal_tiled_base(thread const VkrMetalTiledVertexOutput &input,
                     constant VkrMetalPacketFrameRoot *frame,
                     const device VkrMetalPacketMaterial &material) {
  const bool terrain = (material.flags & 2048u) != 0u;
  return material.base_color_texture.sample(material.base_color_sampler,
                                            input.texcoord,
                                            bias(frame->texture_lod_bias)) *
         material.tint * (terrain ? float4(1.0f) : input.color);
}

static VkrMetalTiledSurface
vkr_metal_tiled_surface(thread const VkrMetalTiledVertexOutput &input,
                        constant VkrMetalPacketFrameRoot *frame,
                        const device VkrMetalPacketMaterial &material,
                        uint material_index, float4 base) {
  // A draw's material decides `terrain`, so the samples under it keep their
  // implicit derivatives.
  const bool terrain = (material.flags & 2048u) != 0u;
  VkrMetalTiledSurface surface;
  surface.base = base;
  surface.normal_mapped = terrain || (material.flags & 1u) != 0u;
  surface.tangent_normal = float3(0.0f, 0.0f, 1.0f);
  surface.metallic = saturate(material.material_surface.x);
  surface.feedback_roughness = saturate(material.material_surface.y);
  surface.occlusion = saturate(material.material_surface.w);
  float3 orm = float3(1.0f);
  if ((material.flags & 2u) != 0u)
    orm = material.orm_texture
              .sample(material.orm_sampler, input.texcoord,
                      bias(frame->texture_lod_bias))
              .rgb;
  if ((material.flags & 1u) != 0u)
    surface.tangent_normal = vkr_normal_map_decode(
        material.normal_texture
            .sample(material.normal_sampler, input.texcoord,
                    bias(frame->texture_lod_bias))
            .xyz,
        material.material_surface.z);
  if (terrain) {
    // Explicit gradients keep the extra layers' samples defined under the
    // branches on their weights.
    const device VkrMetalPacketTerrainMaterial &layers =
        frame->terrain_materials[material_index];
    const float gradient_scale = exp2(frame->texture_lod_bias);
    const gradient2d gradients =
        gradient2d(dfdx(input.texcoord) * gradient_scale,
                   dfdy(input.texcoord) * gradient_scale);
    float4 mask = float4(0.0f);
    if (layers.mask_source == VKR_LAYER_MASK_TEXTURE)
      mask = layers.mask_texture.sample(layers.mask_sampler, input.texcoord,
                                        gradients);
    const float4 weights = vkr_layer_mask_weights(
        layers.mask_source, input.color, mask,
        normalize(input.world_normal).y, input.world_position.y,
        layers.mask_range.xy);
    VkrTerrainSurface layered = vkr_metal_packet_terrain_surface(
        vkr_terrain_layer(surface.base, orm, surface.tangent_normal,
                          float4(1.0f), material.material_surface),
        layers, weights, input.texcoord, gradients, true);
    surface.base = layered.base;
    surface.tangent_normal = layered.tangent_normal;
    surface.metallic = layered.metallic;
    surface.feedback_roughness = layered.roughness;
    surface.occlusion = layered.occlusion;
  } else if ((material.flags & 2u) != 0u) {
    surface.occlusion *= orm.r;
    surface.feedback_roughness = saturate(surface.feedback_roughness * orm.g);
    surface.metallic = saturate(surface.metallic * orm.b);
  }
  surface.roughness = clamp(surface.feedback_roughness, 0.04f, 1.0f);
  surface.emissive = material.material_emissive.rgb;
  if ((material.flags & 4u) != 0u)
    surface.emissive *=
        material.emissive_texture.sample(material.emissive_sampler,
                                         input.texcoord,
                                         bias(frame->texture_lod_bias))
            .rgb;
  return surface;
}

// The frame's decals over a surface's base colour, before lighting
// (ADR-092). A decal's box projects along its -Y onto the surfaces facing its
// +Y. Seen from its +Y with its -Z up, its texture reads upright: u runs along
// +X and, as VKR images keep their bottom row at v = 0, v along -Z. Its
// material's base colour times its
// tint covers the surface by the texture's alpha, the decal's opacity, the
// fade by the angle between `normal` and the box's +Y and the fade toward
// the box's near and far faces; later decals lie on top. The SIMD group
// iterates the union of its lanes' cell masks, so each decal row loads once
// for the group; a lane outside a decal's box skips it, and a lane inside it
// has it in its own cell. The decal's UV gradients come from the world
// position's derivatives, taken before the loop, so its samples stay defined
// in the lanes' divergent branches.
static float3 vkr_metal_tiled_decals(constant VkrMetalPacketFrameRoot *frame,
                                     float3 world_position, float3 normal,
                                     float3 base) {
  constant VkrMetalPacketDecals &decals = *frame->decals;
  const float3 world_dx = dfdx(world_position);
  const float3 world_dy = dfdy(world_position);
  const uint3 dimensions = decals.grid_dimensions_count.xyz;
  const int3 cell =
      int3(floor((world_position - decals.grid_origin_cell_size.xyz) /
                 decals.grid_origin_cell_size.w));
  uint2 mask = uint2(0u);
  if (all(cell >= 0) && all(uint3(cell) < dimensions)) {
    mask = decals.masks[uint(cell.x) +
                        dimensions.x *
                            (uint(cell.y) + dimensions.y * uint(cell.z))];
  }
  const float4 position = float4(world_position, 1.0f);
  for (uint word = 0u; word < 2u; ++word) {
    uint remaining = simd_or(mask[word]);
    while (remaining != 0u) {
      const uint index = word * 32u + ctz(remaining);
      remaining &= remaining - 1u;
      const device VkrMetalPacketDecal &decal = decals.rows[index];
      const float3 box = float3(dot(decal.world_to_box[0], position),
                                dot(decal.world_to_box[1], position),
                                dot(decal.world_to_box[2], position));
      if (any(abs(box) > 0.5f))
        continue;
      const float weight =
          decal.fade.w *
          saturate(dot(normal, decal.world_to_box[1].xyz) * decal.fade.x +
                   decal.fade.y) *
          saturate((0.5f - abs(box.y)) * decal.fade.z);
      if (weight <= 0.0f)
        continue;
      const float2 uv_dx = float2(dot(decal.world_to_box[0].xyz, world_dx),
                                  -dot(decal.world_to_box[2].xyz, world_dx));
      const float2 uv_dy = float2(dot(decal.world_to_box[0].xyz, world_dy),
                                  -dot(decal.world_to_box[2].xyz, world_dy));
      const device VkrMetalPacketMaterial &material =
          frame->materials[decal.material_index];
      const float4 colour =
          material.base_color_texture.sample(material.base_color_sampler,
                                             float2(box.x + 0.5f,
                                                    0.5f - box.z),
                                             gradient2d(uv_dx, uv_dy)) *
          material.tint;
      base = mix(base, colour.rgb, saturate(colour.a * weight));
    }
  }
  return base;
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
  // The sun's part of diffuse and specular.
  half3 sun;
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
  // Moving casters' shadows of the baked lamps besides shadowed point and
  // spot lights (ADR-108).
  VKR_METAL_TILED_LIGHTING_CASTERS = 5u,
  // Moving casters' shadows besides every dynamic light, rectangle lights
  // included. Frames without rectangle lights take CASTERS: the rectangle
  // code's registers cost every pixel of the opaque pass.
  VKR_METAL_TILED_LIGHTING_CASTERS_ALL = 6u,
};

// The tiled pipeline's local shadows: four hardware-filtered taps on the
// opaque atlas, a 3x3 tent that softens texel edges without the desktop
// penumbra, and without the refractive layers its glass never casts into.
// The normal offset follows the geometric normal, as the cascades' does: a
// normal-mapped one pushed each pixel its own way and left speckle on bumpy
// surfaces near lamps.
struct VkrMetalTiledLocalShadow {
  float3 geometric_normal;

  float3 operator()(constant VkrMetalPacketFrameRoot *frame,
                    uint first_view_encoded, uint, uint kind,
                    float3 world_position, float3) const {
    return vkr_metal_packet_local_shadow_sample<false, false, true>(
        frame, first_view_encoded, kind, world_position, geometric_normal);
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

// The reflection probe a blended surface, or an opaque draw that straddles a
// probe boundary, shades with at a pixel, as frame probe index + 1: the first
// unbound probe, in the host's smaller-box-first order, whose influence at
// the pixel reaches one half, as the desktop pipeline's priority blend would
// give it most of the pixel. Other opaque draws take the one their encode
// chose for the whole draw (vkr_metal_packet_draw_probe).
static uint vkr_metal_tiled_pixel_probe(constant VkrMetalPacketFrameRoot *frame,
                                        float3 world_position) {
  const uint count = min(frame->ibl_probe_count, VKR_GPU_DRAW_PROBE_MASK - 1u);
  if (frame->ibl_probes == nullptr)
    return 0u;
  for (uint i = 0u; i < count; ++i) {
    // A probe bound to a planar reflector serves that reflector's draws only
    // (vkr_metal_packet_draw_probe).
    if (frame->ibl_probes[i].owner_object_id == 0u &&
        vkr_metal_packet_probe_influence(frame->ibl_probes[i],
                                         world_position) >= 0.5f)
      return i + 1u;
  }
  return 0u;
}

// Where a draw takes its baked diffuse light and its reflection probe: its
// mover's lighting origin (ADR-120), else the shaded point. A mover's draws
// keep their own normals, so only the sampled position is shared.
static float3
vkr_metal_tiled_lighting_position(constant VkrMetalPacketFrameRoot *frame,
                                  uint instance_index, float3 world_position) {
  const float4 origin = frame->instances[instance_index].lighting_origin;
  return origin.w != 0.0f ? origin.xyz : world_position;
}

// The irradiance the moving casters take from a lightmapped surface
// (ADR-104, ADR-108): per selected baked lamp, its light at the surface
// times the share its static shadow passes and its composite shadow stops,
// through the same filter taps. The light is pre-exposed.
static float3 vkr_metal_tiled_moving_shadow(
    constant VkrMetalPacketFrameRoot *frame, float3 world_position,
    float3 normal, float3 geometric_normal) {
  constant VkrMetalPacketLightmap &lightmap = *frame->lightmap;
  float3 removed = float3(0.0f);
  const uint count = min(lightmap.baked_lamp_count, 2u);
  for (uint i = 0u; i < count; ++i) {
    constant VkrMetalPacketBakedLamp &lamp = lightmap.baked_lamps[i];
    const VkrPunctualLightTerm term = vkr_punctual_light_term(
        lamp.light.p0, lamp.light.p1, lamp.light.p2, lamp.light.p3,
        world_position);
    const float facing = dot(normal, term.direction);
    if (!term.in_range || term.cone <= 0.0f || facing <= 0.0f)
      continue;
    // A face without a dynamic square holds no moving caster: its static and
    // composite views name one square, so the difference is zero and neither
    // is read. A moving caster that shadows the pixel lies on its ray, in the
    // same face.
    // The face the sampler reads: the one its normal-offset receiver lies
    // in. Choosing it from the unoffset position skipped the dynamic square
    // in a thin band along cube edges.
    uint face = 0u;
    if (term.kind != 2u) {
      const float3 from_light = world_position - lamp.light.p0.xyz;
      const device VkrLocalShadowView &source =
          frame->local_shadow_views[lamp.static_first_view +
                                    vkr_local_shadow_face(from_light)];
      const float footprint = 2.0f *
                              dot(from_light, source.light_direction_far.xyz) *
                              source.projection_params.x *
                              source.projection_params.y;
      face = vkr_local_shadow_face(
          from_light +
          geometric_normal * (source.projection_params.w * footprint) -
          normalize(from_light) * (source.projection_params.z * footprint));
    }
    if (all(frame->local_shadow_views[lamp.static_first_view + face]
                .atlas_rect ==
            frame->local_shadow_views[lamp.composite_first_view + face]
                .atlas_rect))
      continue;
    const float passed = vkr_metal_packet_local_shadow_sample<false, false, true>(
                             frame, lamp.static_first_view + 1u, term.kind,
                             world_position, geometric_normal)
                             .x;
    if (passed <= 0.0f)
      continue;
    const float kept = vkr_metal_packet_local_shadow_sample<false, false, true>(
                           frame, lamp.composite_first_view + 1u, term.kind,
                           world_position, geometric_normal)
                           .x;
    removed += lamp.light.p1.rgb * lamp.light.p2.x *
               (term.attenuation * term.cone * facing * max(passed - kept, 0.0f));
  }
  return removed;
}

// The frame's BRDF extension variant (ADR-113), set by the pipelines the
// renderer creates when a scene first publishes an extended material. A
// pipeline without the constant, such as a Custom graph's, shades the base
// model, and the extension code compiles out of it.
constant bool vkr_tiled_brdf_extension_constant [[function_constant(0)]];
constant bool vkr_tiled_brdf_extension =
    is_function_constant_defined(vkr_tiled_brdf_extension_constant) &&
    vkr_tiled_brdf_extension_constant;

// The sun's visibility at a forward-drawn surface: its cascades and the
// cloud layer, zero where the surface faces away. The cascades take the
// geometric normal: the receiver plane, normal offset and slope bias describe
// the surface the map rasterized, not its normal map.
static float vkr_metal_tiled_sun_visibility(
    thread const VkrMetalTiledVertexOutput &input,
    constant VkrMetalPacketFrameRoot *frame, float3 normal,
    float3 geometric_normal) {
  const float3 sun = normalize(-frame->directional_direction_enabled.xyz);
  return dot(normal, sun) > 0.0f
             ? vkr_metal_packet_directional_shadow_sample(
                   frame, input.world_position, geometric_normal)
                       .factor *
                   vkr_metal_packet_cloud_shadow(frame, input.world_position)
             : 0.0f;
}

// Adds the sun's light at `visibility` to `light`; sun contact frames also
// keep it as `light.sun`.
static void vkr_metal_tiled_light_sun(
    constant VkrMetalPacketFrameRoot *frame, float visibility, float3 normal,
    float3 view, float3 base, float metallic, float roughness, float3 f0,
    thread const VkrGgxMaterialEnergy &energy,
    thread VkrMetalTiledSurfaceLight &light) {
  VkrMetalPacketDirectResult direct = vkr_metal_packet_direct(
      normal, view, normalize(-frame->directional_direction_enabled.xyz),
      frame->directional_color_intensity.rgb *
          frame->directional_color_intensity.w * visibility,
      base, metallic, roughness, f0, energy);
  light.diffuse += direct.diffuse;
  light.specular += direct.specular;
  if (vkr_tiled_sun_output)
    light.sun = half3(direct.diffuse + direct.specular);
}

// Shades a forward-drawn surface: the sun with its cascades and cloud shadow,
// the dynamic local lights that Lighting includes, diffuse light from its
// lightmap, the diffuse volume or the environment, and environment specular.
// The environment is `probe` (frame probe index + 1) or, for zero, the global
// environment: one per surface, where the desktop pipeline blends its probes
// per pixel. `instance_index` names the draw's instance, whose lighting
// origin places the volume lookup and the probe's box projection.
template <VkrMetalTiledLighting Lighting>
static VkrMetalTiledSurfaceLight
vkr_metal_tiled_shade(thread const VkrMetalTiledVertexOutput &input,
                      constant VkrMetalPacketFrameRoot *frame,
                      const device VkrMetalPacketMaterial &material,
                      thread const VkrMetalTiledSurface &surface,
                      bool front_facing, uint probe, uint instance_index) {
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
  float3 emissive = neutral ? float3(0.0f) : surface.emissive;
  // Specular anti-aliasing widens the lobe by the normal's screen variance,
  // as the forward and G-buffer paths do.
  float3 normal_dx = dfdx(normal);
  float3 normal_dy = dfdy(normal);
  roughness = vkr_ggx_filter_roughness(
      roughness,
      0.25f * (dot(normal_dx, normal_dx) + dot(normal_dy, normal_dy)));
  emissive *= frame->pre_exposure;

  // The shared material model: clearcoat, sheen, anisotropy and diffuse
  // transmission are not modeled yet (ADR-087).
  float3 f0 = mix(neutral ? float3(0.04f)
                         : saturate(material.material_dielectric_specular.rgb),
                  base, metallic);
  VkrGgxMaterialEnergy energy =
      vkr_metal_packet_prepare_brdf(frame, no_v, roughness, f0);
  // The material's BRDF extensions (ADR-113) in the extension variant; a
  // neutral inspection keeps the base model.
  if (vkr_tiled_brdf_extension && !neutral) {
    VkrBrdfExtension extension =
        vkr_metal_packet_material_brdf_extension(material);
    energy = vkr_metal_packet_apply_specular_extension(frame, energy,
                                                       extension, roughness, f0);
    energy = vkr_brdf_apply_diffuse_extension(energy, extension,
                                              base * (1.0f - metallic));
  }
  VkrMetalTiledSurfaceLight light;
  light.diffuse = float3(0.0f);
  light.specular = float3(0.0f);
  light.emissive = emissive;
  light.sun = half3(0.0h);
  light.base = base;
  light.reflectance = energy.reflectance;
  light.metallic = metallic;

  // The sun, shadowed by its cascades and the cloud layer. Sun contact
  // frames return its light beside the colour: kept live through the local
  // lights' loops, those three values cost the opaque pass about 1.5 ms on
  // the Bistro plaza (M1 Pro, 1440p at 0.6 scale), so those frames keep only
  // its visibility through the loops and light it after them (+0.7 ms).
  const bool sun_shines = frame->directional_direction_enabled.w > 0.5f;
  const float sun_visibility =
      sun_shines ? vkr_metal_tiled_sun_visibility(input, frame, normal,
                                                  geometric_normal)
                 : 0.0f;
  if (sun_shines && !vkr_tiled_sun_output)
    vkr_metal_tiled_light_sun(frame, sun_visibility, normal, view, base,
                              metallic, roughness, f0, energy, light);

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
          sheen_unused, VkrMetalTiledLocalShadow{geometric_normal});
    }
    if (Lighting == VKR_METAL_TILED_LIGHTING_ALL ||
        Lighting == VKR_METAL_TILED_LIGHTING_INSPECT ||
        Lighting == VKR_METAL_TILED_LIGHTING_CASTERS_ALL) {
      VkrMetalPacketDirectResult rectangles =
          vkr_metal_packet_layered_rectangle_lights<true, true>(
              frame, input.world_position, normal, view, base, metallic,
              roughness, f0, energy, false, no_coat, false, no_sheen,
              coat_unused, sheen_unused);
      light.diffuse += rectangles.diffuse;
      light.specular += rectangles.specular;
    }
  }

  if (sun_shines && vkr_tiled_sun_output)
    vkr_metal_tiled_light_sun(frame, sun_visibility, normal, view, base,
                              metallic, roughness, f0, energy, light);

  // Diffuse light: the lightmap stores irradiance, the volume and the global
  // environment the Lambertian response; each is pre-exposed here.
  float3 diffuse_light = float3(0.0f);
  // The baked sky light's part of it, the sun-key layers' light.
  float3 baked_sky = float3(0.0f);
  bool environment_diffuse = false;
  // The lightmap's diffuse light already holds its occlusion; its baked
  // ambient visibility only occludes environment specular.
  float baked_occlusion = 1.0f;
  // Read after the direct light, so the light loops keep no more live state.
  const float3 lighting_position = vkr_metal_tiled_lighting_position(
      frame, instance_index, input.world_position);
  if (input.lightmap_page != ~0u) {
    float sky_luminance;
    float4 lightmap = vkr_metal_tiled_lightmap_irradiance(
        *frame->lightmap, input.lightmap_uv, input.lightmap_page,
        input.lightmap_direct_uv, input.lightmap_direct_page, sky_luminance);
    diffuse_light = lightmap.rgb * (frame->pre_exposure / M_PI_F);
    // A grey of that luminance: Rec. 709 weights sum to one.
    baked_sky = float3(sky_luminance * (frame->pre_exposure / M_PI_F));
    baked_occlusion = lightmap.a;
    // Moving casters shadow the baked lamps that light them most (ADR-108).
    if (Lighting == VKR_METAL_TILED_LIGHTING_CASTERS ||
        Lighting == VKR_METAL_TILED_LIGHTING_CASTERS_ALL ||
        Lighting == VKR_METAL_TILED_LIGHTING_INSPECT)
      diffuse_light = max(diffuse_light -
                              vkr_metal_tiled_moving_shadow(
                                  frame, input.world_position, normal,
                                  geometric_normal) /
                                  M_PI_F,
                          float3(0.0f));
  } else {
    float sky_luminance;
    float4 volume =
        vkr_metal_packet_diffuse_volume(frame, lighting_position, normal,
                                        view, sky_luminance);
    diffuse_light = volume.rgb;
    // A grey of that luminance: Rec. 709 weights sum to one.
    baked_sky = float3(sky_luminance);
    environment_diffuse = volume.w == 0.0f;
  }
  if ((frame->flags & 2u) != 0u) {
    constexpr sampler environment_sampler(coord::normalized,
                                          address::clamp_to_edge,
                                          filter::linear, mip_filter::linear);
    const float lod =
        roughness * float(max(frame->prefilter_mip_count, 1u) - 1u);
    float3 reflection = reflect(-view, normal);
    const float alpha = roughness * roughness;
    const float3 dominant =
        vkr_specular_dominant_direction(normal, reflection, alpha);
    // One environment per surface: its probe, box-projected, or the global
    // environment with the cloud layer. The two paths share no live values,
    // so the probe costs the pass few registers.
    float3 prefiltered;
    uint sh_slot = frame->sh_global_slot;
    // The environment's pre-exposed radiance scale and its diffuse and
    // specular gains: a probe's own, or the global environment's.
    float radiance_scale = frame->ibl_controls.x;
    float diffuse_gain = frame->ibl_controls.y;
    float specular_gain = frame->ibl_controls.z;
    if (probe != 0u) {
      const device VkrMetalPacketIblProbe &local =
          frame->ibl_probes[probe - 1u];
      float3 direction =
          local.intensity_box.w > 0.5f
              ? vkr_reflection_probe_direction(
                    dominant, lighting_position, local.center_blend.xyz,
                    max(local.extents_weight.xyz, 0.0f),
                    local.capture_origin.xyz, alpha)
              : dominant;
      prefiltered =
          local.prefilter.sample(environment_sampler, direction, level(lod))
              .rgb;
      sh_slot = local.sh_slot;
      radiance_scale = local.intensity_box.x;
      diffuse_gain = local.intensity_box.y;
      specular_gain = local.intensity_box.z;
    } else {
      prefiltered = vkr_metal_packet_global_prefiltered(
          frame, environment_sampler, dominant, lod);
    }
    // The environment's radiance and the diffuse response it gives at the
    // normal, pre-exposed.
    float3 incoming = prefiltered * radiance_scale;
    const float3 environment_response =
        vkr_sh_l2_evaluate(frame->sh_coefficients[sh_slot],
                           vkr_sh_l2_prepare_evaluation(normal)) *
        radiance_scale;
    // A surface without baked light takes its diffuse light from the
    // environment. One with baked light reflects the global environment in
    // the share its baked sky light supports and its surroundings in the
    // rest, so a room or tunnel does not reflect the sky (ADR-054). A probe's
    // capture already holds what its room sees, so it is not scaled again.
    if (environment_diffuse)
      diffuse_light = environment_response * diffuse_gain;
    else if (probe == 0u)
      incoming = vkr_sh_l2_occluded_radiance(
          incoming, diffuse_light,
          vkr_sh_l2_sky_visibility(baked_sky, environment_response));
    float horizon = saturate(1.0f + dot(reflection, geometric_normal));
    float specular_visibility =
        horizon * horizon *
        vkr_specular_occlusion(ao * baked_occlusion, no_v, alpha);
    light.specular += incoming * energy.primary_reflectance *
                      specular_visibility * specular_gain;
    // The dual lobe (ADR-113) reads the same environment at its roughness,
    // about its own dominant direction.
    if (energy.dual_opacity > 0.0f) {
      const float dual_lod = energy.dual_roughness *
                             float(max(frame->prefilter_mip_count, 1u) - 1u);
      const float dual_alpha = energy.dual_roughness * energy.dual_roughness;
      const float3 dual_dominant =
          vkr_specular_dominant_direction(normal, reflection, dual_alpha);
      float3 dual_incoming;
      if (probe != 0u) {
        const device VkrMetalPacketIblProbe &local =
            frame->ibl_probes[probe - 1u];
        float3 direction =
            local.intensity_box.w > 0.5f
                ? vkr_reflection_probe_direction(
                      dual_dominant, lighting_position,
                      local.center_blend.xyz,
                      max(local.extents_weight.xyz, 0.0f),
                      local.capture_origin.xyz, dual_alpha)
                : dual_dominant;
        dual_incoming = local.prefilter
                            .sample(environment_sampler, direction,
                                    level(dual_lod))
                            .rgb *
                        radiance_scale;
      } else {
        dual_incoming = vkr_metal_packet_global_prefiltered(
                            frame, environment_sampler, dual_dominant,
                            dual_lod) *
                        radiance_scale;
        if (!environment_diffuse)
          dual_incoming = vkr_sh_l2_occluded_radiance(
              dual_incoming, diffuse_light,
              vkr_sh_l2_sky_visibility(baked_sky, environment_response));
      }
      light.specular +=
          dual_incoming * energy.dual_reflectance * horizon * horizon *
          vkr_specular_occlusion(ao * baked_occlusion, no_v, dual_alpha) *
          specular_gain;
    }
  } else if (environment_diffuse) {
    diffuse_light = frame->ambient_color.rgb;
  }
  light.diffuse +=
      energy.diffuse_weight * (1.0f - metallic) * base * diffuse_light * ao;
  return light;
}

// An alpha-tested surface's coverage: its alpha sharpened about the
// material's cut-off to a transition about one pixel wide, which alpha to
// coverage turns into the samples it covers. Fragments that cover no sample
// discard.
static float
vkr_metal_tiled_coverage(const device VkrMetalPacketMaterial &material,
                         float alpha) {
  if (material.alpha_mode != 1u)
    return 1.0f;
  const float coverage = saturate(
      (alpha - material.material_alpha.x) / max(fwidth(alpha), 1e-4f) + 0.5f);
  if (coverage <= 0.0f)
    discard_fragment();
  return coverage;
}

// How the forward pass gets a surface. The Standard tier reads the material
// row; alpha-tested draws read the base colour alone first, so fragments that
// cover no sample discard before the other textures.
struct VkrMetalStandardSurface {
  template <bool Coverage>
  static VkrMetalTiledSurface
  shade(thread const VkrMetalTiledVertexOutput &input,
        constant VkrMetalPacketFrameRoot *frame,
        const device VkrMetalPacketMaterial &material, uint material_index,
        thread float &coverage) {
    const float4 base = vkr_metal_tiled_base(input, frame, material);
    if (Coverage)
      coverage = vkr_metal_tiled_coverage(material, base.a);
    return vkr_metal_tiled_surface(input, frame, material, material_index,
                                   base);
  }
};

// A Custom graph's surface function (ADR-096) takes the shaded point and
// gives the core material outputs; vkr_material_codegen.c generates it.
struct VkrMetalCustomInput {
  float2 texcoord;
  float4 color;
  float3 world_position;
  float3 world_normal;
  float time;
  float camera_distance;
};

struct VkrMetalCustomOutput {
  float3 base_color;
  float opacity;
  float metallic;
  float roughness;
  float occlusion;
  float3 emissive;
  float3 tangent_normal;
  bool normal_mapped;
};

// The outputs a graph leaves unconnected.
static VkrMetalCustomOutput vkr_metal_custom_output() {
  VkrMetalCustomOutput out;
  out.base_color = float3(1.0f);
  out.opacity = 1.0f;
  out.metallic = 0.0f;
  out.roughness = 1.0f;
  out.occlusion = 1.0f;
  out.emissive = float3(0.0f);
  out.tangent_normal = float3(0.0f, 0.0f, 1.0f);
  out.normal_mapped = false;
  return out;
}

// A over B, zero where B is zero.
static float vkr_custom_divide(float a, float b) {
  return b != 0.0f ? a / b : 0.0f;
}

static float2 vkr_custom_divide(float2 a, float2 b) {
  return select(float2(0.0f), a / b, b != float2(0.0f));
}

static float3 vkr_custom_divide(float3 a, float3 b) {
  return select(float3(0.0f), a / b, b != float3(0.0f));
}

// X to the exponent, X below zero taken as zero.
static float vkr_custom_power(float x, float e) {
  return powr(max(x, 0.0f), e);
}

static float2 vkr_custom_power(float2 x, float e) {
  return powr(max(x, float2(0.0f)), float2(e));
}

static float3 vkr_custom_power(float3 x, float e) {
  return powr(max(x, float3(0.0f)), float3(e));
}

// A Custom graph's surface: its function evaluated once, its opacity tested
// for coverage on alpha-tested draws.
template <typename Graph> struct VkrMetalCustomSurface {
  template <bool Coverage>
  static VkrMetalTiledSurface
  shade(thread const VkrMetalTiledVertexOutput &input,
        constant VkrMetalPacketFrameRoot *frame,
        const device VkrMetalPacketMaterial &material, uint material_index,
        thread float &coverage) {
    VkrMetalCustomInput in;
    in.texcoord = input.texcoord;
    in.color = input.color;
    in.world_position = input.world_position;
    in.world_normal = normalize(input.world_normal);
    in.time = frame->custom_time;
    in.camera_distance = length(frame->view_position.xyz - input.world_position);
    const VkrMetalCustomOutput out =
        Graph::evaluate(in, frame->custom_materials[material_index]);
    if (Coverage)
      coverage = vkr_metal_tiled_coverage(material, out.opacity);
    VkrMetalTiledSurface surface;
    surface.base = float4(out.base_color, out.opacity);
    surface.tangent_normal = out.tangent_normal;
    surface.normal_mapped = out.normal_mapped;
    surface.metallic = saturate(out.metallic);
    surface.feedback_roughness = saturate(out.roughness);
    surface.roughness = clamp(surface.feedback_roughness, 0.04f, 1.0f);
    surface.occlusion = saturate(out.occlusion);
    surface.emissive = max(out.emissive, float3(0.0f));
    return surface;
  }
};

// Texture samples a pixel of `material` takes: its base colour, each map
// its flags enable, and a layered or terrain material's layers and mask.
static float vkr_metal_tiled_material_samples(
    const device VkrMetalPacketMaterial &material) {
  return 1.0f + float(popcount(material.flags & 0x7FFu)) +
         ((material.flags & 2048u) != 0u ? 10.0f : 0.0f);
}

// The base colour texture's texels per metre on the surface: its texel
// footprint over the pixel's world footprint, along both screen axes.
static float vkr_metal_tiled_texel_density(
    thread const VkrMetalTiledVertexOutput &input,
    const device VkrMetalPacketMaterial &material) {
  const float2 size = float2(material.base_color_texture.get_width(),
                             material.base_color_texture.get_height());
  const float texels = length(dfdx(input.texcoord) * size) +
                       length(dfdy(input.texcoord) * size);
  const float metres = length(dfdx(input.world_position)) +
                       length(dfdy(input.world_position));
  return metres > 0.0f ? texels / metres : 0.0f;
}

// The editor's modes that replace shading; false for the modes that shade.
// Unlit shows the base colour and the pre-exposed emission; wireframe draws
// one-pixel edges over a dark fill, as the visibility-buffer resolve does.
// The artist data views (ADR-099) write their value pre-exposed, which the
// tonemap shows without exposure or the tone curve: the shading normal,
// base colour, roughness, metallic, the material's texture samples, the
// base colour texture's texels per metre against art.lint's default range,
// and the draw's reach before the camera stops drawing it (ADR-114).
// A Custom graph's pixels show the cost of their fallback, tinted magenta.
static bool vkr_metal_tiled_inspect(
    thread const VkrMetalTiledVertexOutput &input,
    constant VkrMetalPacketFrameRoot *frame,
    const device VkrMetalPacketMaterial &material,
    thread const VkrMetalTiledSurface &surface, float3 barycentric,
    bool front_facing, uint state_flags, thread float3 &out_color) {
  const uint mode = frame->render_mode;
  if (mode == 12u) {
    out_color = vkr_editor_wire_color(barycentric, dfdx(barycentric),
                                      dfdy(barycentric));
    return true;
  }
  if (mode == 3u) {
    out_color = surface.base.rgb + surface.emissive * frame->pre_exposure;
    return true;
  }
  if (!vkr_editor_data_view(mode)) {
    return false;
  }
  float3 value = float3(0.0f);
  if (mode == 2u) {
    float3 normal =
        normalize(input.world_normal) * (front_facing ? 1.0f : -1.0f);
    if (surface.normal_mapped) {
      float3 tangent = normalize(input.world_tangent.xyz);
      tangent = normalize(tangent - dot(tangent, normal) * normal);
      const float3 bitangent =
          normalize(cross(normal, tangent)) * input.world_tangent.w;
      normal = normalize(tangent * surface.tangent_normal.x +
                         bitangent * surface.tangent_normal.y +
                         normal * surface.tangent_normal.z);
    }
    value = normal * 0.5f + 0.5f;
  } else if (mode == 13u) {
    value = surface.base.rgb;
  } else if (mode == 14u) {
    value = float3(surface.feedback_roughness);
  } else if (mode == 15u) {
    value = float3(surface.metallic);
  } else if (mode == 19u) {
    // The encode kernel left the draw's reach code in its probe bits.
    value = vkr_editor_reach_color((state_flags >> VKR_GPU_DRAW_PROBE_SHIFT) &
                                   VKR_GPU_DRAW_PROBE_MASK);
  } else if (mode == 16u) {
    value = vkr_editor_cost_color(vkr_metal_tiled_material_samples(material));
    if (((material.flags >> 24u) & 63u) != 0u) {
      value = 0.5f * value + float3(0.5f, 0.0f, 0.5f);
    }
  } else {
    value = vkr_editor_density_color(
        vkr_metal_tiled_texel_density(input, material), 128.0f, 2048.0f);
  }
  out_color = value * frame->pre_exposure;
  return true;
}

// Shades the opaque pass. Opaque draws never discard, so hidden-surface
// removal keeps its fast path. With Coverage, alpha-tested draws return their
// alpha sharpened about the material's cut-off to a transition about one
// pixel wide; alpha to coverage turns it into the samples they cover, so their
// edges resolve like geometry edges (ADR-087). Fragments that cover no
// sample discard before the other material textures and shading. With Decals,
// the frame's decals cover the surface's base colour before lighting.
template <VkrMetalTiledLighting Lighting, bool Coverage, bool Probes,
          bool Decals, typename Surface = VkrMetalStandardSurface>
static VkrMetalTiledForwardOutput
vkr_metal_tiled_forward(thread const VkrMetalTiledVertexOutput &input,
                        constant VkrMetalPacketDrawRoot *root,
                        bool front_facing, float3 barycentric) {
  constant VkrMetalPacketFrameRoot *frame = root->frame;
  const device VkrGpuVisibleDrawRow &visible =
      root->visible_rows[input.visible_row_index];
  const device VkrMetalPacketMaterial &material =
      frame->materials[visible.material_index];
  float coverage = 1.0f;
  VkrMetalTiledSurface surface = Surface::template shade<Coverage>(
      input, frame, material, visible.material_index, coverage);
  if (Decals) {
    surface.base.rgb = vkr_metal_tiled_decals(
        frame, input.world_position,
        normalize(input.world_normal) * (front_facing ? 1.0f : -1.0f),
        surface.base.rgb);
  }
  if (Lighting == VKR_METAL_TILED_LIGHTING_INSPECT) {
    float3 inspected;
    if (vkr_metal_tiled_inspect(input, frame, material, surface, barycentric,
                                front_facing, visible.state_flags, inspected))
      return {float4(inspected, coverage), half4(0.0h)};
  }
  uint probe = Probes ? (visible.state_flags >> VKR_GPU_DRAW_PROBE_SHIFT) &
                           VKR_GPU_DRAW_PROBE_MASK
                     : 0u;
  // A draw that straddles a probe boundary chooses per pixel
  // (vkr_metal_packet_draw_probe); a mover's draw chose at its lighting
  // origin, so never does.
  if (Probes && probe == VKR_GPU_DRAW_PROBE_PER_PIXEL)
    probe = vkr_metal_tiled_pixel_probe(frame, input.world_position);
  VkrMetalTiledSurfaceLight light = vkr_metal_tiled_shade<Lighting>(
      input, frame, material, surface, front_facing, probe,
      visible.instance_index);
  return {float4(light.diffuse + light.specular + light.emissive, coverage),
          half4(light.sun, 0.0h)};
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_NONE, false, false, false>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_punctual_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_PUNCTUAL, false, false, false>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_shadowed_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_SHADOWED, false, false, false>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_all_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_ALL, false, false, false>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_inspect_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]],
    float3 barycentric [[barycentric_coord]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_INSPECT, false, true, true>(
      input, root, front_facing, barycentric);
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_coverage_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_NONE, true, false, false>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_punctual_coverage_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_PUNCTUAL, true, false, false>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_shadowed_coverage_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_SHADOWED, true, false, false>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_all_coverage_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_ALL, true, false, false>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_inspect_coverage_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]],
    float3 barycentric [[barycentric_coord]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_INSPECT, true, true, true>(
      input, root, front_facing, barycentric);
}

// The same variants for frames whose camera sees a reflection probe volume
// (ADR-087); probe shading costs the opaque pass registers on every pixel, so
// frames without a visible probe keep the variants above.

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_probes_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_NONE, false, true, false>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_punctual_probes_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_PUNCTUAL, false, true, false>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_shadowed_probes_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_SHADOWED, false, true, false>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_all_probes_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_ALL, false, true, false>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_coverage_probes_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_NONE, true, true, false>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_punctual_coverage_probes_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_PUNCTUAL, true, true, false>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_shadowed_coverage_probes_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_SHADOWED, true, true, false>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_all_coverage_probes_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_ALL, true, true, false>(
      input, root, front_facing, float3(0.0f));
}

// The same variants for frames whose camera sees a decal box (ADR-092);
// decal shading costs the opaque pass registers on every pixel, so frames
// without a visible decal keep the variants above. The inspection variants
// always shade decals.

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_decals_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_NONE, false, false, true>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_punctual_decals_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_PUNCTUAL, false, false, true>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_shadowed_decals_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_SHADOWED, false, false, true>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_all_decals_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_ALL, false, false, true>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_coverage_decals_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_NONE, true, false, true>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_punctual_coverage_decals_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_PUNCTUAL, true, false, true>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_shadowed_coverage_decals_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_SHADOWED, true, false, true>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_all_coverage_decals_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_ALL, true, false, true>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_probes_decals_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_NONE, false, true, true>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_punctual_probes_decals_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_PUNCTUAL, false, true, true>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_shadowed_probes_decals_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_SHADOWED, false, true, true>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_all_probes_decals_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_ALL, false, true, true>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_coverage_probes_decals_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_NONE, true, true, true>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_punctual_coverage_probes_decals_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_PUNCTUAL, true, true, true>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_shadowed_coverage_probes_decals_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_SHADOWED, true, true, true>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_all_coverage_probes_decals_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_ALL, true, true, true>(
      input, root, front_facing, float3(0.0f));
}

// The opaque pass's multisample resolve (ADR-087). A hardware resolve
// averages scene-linear samples, so a bright sample dominates its pixel and
// edges against light sources keep their steps. This tile kernel replaces a
// pixel's samples with their average weighted by 1 / (1 + largest channel),
// a tone-mapped resolve, and the pass's resolve then stores that value. Its
// alpha is the share of that weight surfaces hold, since surfaces write alpha
// one and the sky zero; the atmosphere draw splits edge pixels by it, and it
// leaves the atmosphere draw as one.
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
    sum += float4(color.rgb, saturate(color.a)) * weight;
    weight_sum += weight;
  }
  VkrMetalTiledResolvePixel pixel;
  pixel.color = half4(sum / weight_sum);
  block.write(pixel, coord, ushort((1u << sample_count) - 1u));
}

// The MetalFX upscaler's camera motion (VkrMetalTiledMotionRoot): the
// resolved depth's point, reprojected into the previous frame, as the
// previous unjittered texture coordinate minus the current one. Sky pixels
// sit at depth zero, a direction under the reversed infinite projection,
// which the homogeneous reprojection keeps free of the camera's translation.
// Moving objects take the camera's motion.
struct VkrMetalTiledMotionRoot {
  float4x4 reprojection;
  depth2d<float, access::read> depth;
  texture2d<float, access::write> motion;
  uint2 extent;
  float2 jitter_uv;
};

kernel void vkr_metal_tiled_motion(constant VkrMetalTiledMotionRoot &root
                                   [[buffer(0)]],
                                   uint2 pixel [[thread_position_in_grid]]) {
  if (any(pixel >= root.extent))
    return;
  const float2 center = float2(pixel) + 0.5f;
  const float depth = root.depth.read(pixel);
  const float4 previous =
      root.reprojection *
      float4(vkr_metal_packet_resolve_ndc(center, root.extent), depth, 1.0f);
  float2 motion = float2(0.0f);
  if (previous.w > 0.0f) {
    const float2 previous_ndc = previous.xy / previous.w;
    const float2 previous_uv =
        float2(previous_ndc.x * 0.5f + 0.5f, 0.5f - previous_ndc.y * 0.5f);
    const float2 current_uv = center / float2(root.extent) - root.jitter_uv;
    motion = previous_uv - current_uv;
  }
  root.motion.write(float4(motion, 0.0f, 0.0f), pixel);
}

static_assert(sizeof(VkrMetalTiledMotionRoot) == 96,
              "Metal tiled motion root ABI drift");

// The still-camera running mean over MetalFX's output (Tiled.Stabilize).
// With the camera still, the jitter still moves MetalFX's reconstruction a
// little from frame to frame; averaging each still pixel over up to
// VKR_METAL_TILED_STABLE_FRAMES frames holds it steady. Jitter flicker on
// highlights and thin geometry changes a pixel by 10 to 40% of its
// luminance, so the mean is clamped to the range of the current 3x3
// neighbourhood instead of tested against the pixel alone: flicker stays
// inside that range, while a lighting change moves the whole neighbourhood.
// A pixel restarts its mean where an object moves over it or the clamp moves
// the mean by more than VKR_METAL_TILED_STABLE_CLIP of its luminance. The
// count is in alpha.
constant float VKR_METAL_TILED_STABLE_FRAMES = 32.0f;
constant float VKR_METAL_TILED_STABLE_CLIP = 0.05f;
constant float VKR_METAL_TILED_STABLE_MOTION_PIXELS = 0.01f;

struct VkrMetalTiledStabilizeRoot {
  texture2d<float, access::read> color;
  texture2d<float, access::read> motion;
  texture2d<float, access::read> history;
  texture2d<float, access::write> output;
  uint2 extent;
  uint2 motion_extent;
  float history_scale;
  uint history_valid;
  uint reserved[2];
};

kernel void vkr_metal_tiled_stabilize(constant VkrMetalTiledStabilizeRoot &root
                                      [[buffer(0)]],
                                      uint2 pixel [[thread_position_in_grid]]) {
  if (any(pixel >= root.extent))
    return;
  const float3 current = root.color.read(pixel).rgb;
  float4 result = float4(current, 1.0f);
  if (root.history_valid != 0u) {
    const uint2 motion_pixel =
        min(uint2((float2(pixel) + 0.5f) * float2(root.motion_extent) /
                  float2(root.extent)),
            root.motion_extent - 1u);
    const float2 motion =
        root.motion.read(motion_pixel).xy * float2(root.motion_extent);
    const float4 history = root.history.read(pixel);
    const int2 last = int2(root.extent) - 1;
    float3 low = current;
    float3 high = current;
    for (int y = -1; y <= 1; ++y) {
      for (int x = -1; x <= 1; ++x) {
        const uint2 tap = uint2(clamp(int2(pixel) + int2(x, y), int2(0), last));
        const float3 neighbour = root.color.read(tap).rgb;
        low = min(low, neighbour);
        high = max(high, neighbour);
      }
    }
    const float3 history_mean = history.rgb * root.history_scale;
    const float3 mean = clamp(history_mean, low, high);
    const float3 weights = float3(0.2126f, 0.7152f, 0.0722f);
    const float clip = dot(abs(history_mean - mean), weights);
    const bool still = dot(motion, motion) <
                       VKR_METAL_TILED_STABLE_MOTION_PIXELS *
                           VKR_METAL_TILED_STABLE_MOTION_PIXELS;
    const bool steady =
        clip <= max(VKR_METAL_TILED_STABLE_CLIP * dot(mean, weights), 1e-4f);
    if (still && steady) {
      const float count =
          min(history.a + 1.0f, VKR_METAL_TILED_STABLE_FRAMES);
      result = float4(mean + (current - mean) / count, count);
    }
  }
  root.output.write(result, pixel);
}

static_assert(sizeof(VkrMetalTiledStabilizeRoot) == 64,
              "Metal tiled stabilize root ABI drift");

// Mirrors VKR_TEMPORAL_TRANSFORM_CAPACITY and the instance temporal flags'
// owner bit (vkr_gpu_abi.h).
constant uint VKR_METAL_TEMPORAL_TRANSFORM_CAPACITY = 32768u;
constant uint VKR_METAL_INSTANCE_TEMPORAL_OWNER = 1u;

// The MetalFX upscaler's object motion (Tiled.ObjectMotion): the motion
// view's dynamic draws, rasterized with the opaque pass's jittered matrix,
// carry their current and previous unjittered clip positions. A draw without
// the previous frame's transform takes its current one, and a skinned draw
// without the previous frame's vertices its current ones, which leaves the
// camera's motion.
struct VkrMetalTiledObjectMotionVertex {
  float4 position [[position]];
  float4 current_clip;
  float4 previous_clip;
};

vertex VkrMetalTiledObjectMotionVertex
vkr_metal_tiled_object_motion_vertex(uint vertex_id [[vertex_id]],
                                     uint instance_id [[instance_id]],
                                     constant VkrMetalPacketDrawRoot *root
                                     [[buffer(0)]]) {
  constant VkrMetalPacketFrameRoot *frame = root->frame;
  constant VkrMetalPacketTemporalDrawState &temporal =
      *frame->temporal_draw_state;
  const device VkrGpuVisibleDrawRow &visible = root->visible_rows[instance_id];
  const device VkrGpuGeometryRow &geometry =
      root->geometry_rows[visible.geometry_index];
  const device VkrPackedStaticVertex *vertices =
      (const device VkrPackedStaticVertex *)geometry.vertex_address +
      geometry.first_vertex;
  const device VkrGpuGeometryDecodeRecord &decode =
      ((const device VkrGpuGeometryDecodeRecord *)
           geometry.decode_address)[visible.decode_index];
  const VkrGpuDecodedVertex data =
      vkr_decode_packed_vertex(vertices[vertex_id], decode);
  const device VkrMetalPacketInstance &instance =
      frame->instances[visible.instance_index];
  const float3 position =
      vkr_apply_deformation(data, instance.deformation_address, vertex_id)
          .position;
  float4x4 previous_model = instance.model;
  float3 previous_position = position;
  if (temporal.history_valid != 0u &&
      instance.temporal_index < VKR_METAL_TEMPORAL_TRANSFORM_CAPACITY) {
    const device VkrTemporalTransform &previous =
        temporal.previous_transforms[instance.temporal_index];
    if (previous.valid != 0u &&
        previous.generation == instance.temporal_generation &&
        previous.frame_index == temporal.previous_frame_index) {
      previous_model = previous.model;
      if (instance.previous_deformation_address != 0ul)
        previous_position =
            vkr_apply_deformation(data, instance.previous_deformation_address,
                                  vertex_id)
                .position;
    }
  }
  const float4 world = instance.model * float4(position, 1.0f);
  VkrMetalTiledObjectMotionVertex output;
  // Frame and temporal roots store the transposed matrices Slang's row-vector
  // lowering uses.
  output.position = world * frame->view_projection;
  output.current_clip = world * temporal.current_view_projection;
  output.previous_clip = (previous_model * float4(previous_position, 1.0f)) *
                         temporal.previous_view_projection;
  return output;
}

fragment float4 vkr_metal_tiled_object_motion_fragment(
    VkrMetalTiledObjectMotionVertex input [[stage_in]]) {
  if (input.previous_clip.w <= 0.0f || input.current_clip.w <= 0.0f)
    discard_fragment();
  const float2 current_ndc = input.current_clip.xy / input.current_clip.w;
  const float2 previous_ndc = input.previous_clip.xy / input.previous_clip.w;
  const float2 current_uv =
      float2(current_ndc.x * 0.5f + 0.5f, 0.5f - current_ndc.y * 0.5f);
  const float2 previous_uv =
      float2(previous_ndc.x * 0.5f + 0.5f, 0.5f - previous_ndc.y * 0.5f);
  return float4(previous_uv - current_uv, 0.0f, 0.0f);
}

// Temporal.TransformHistory on the tiled pipeline: each owning dynamic
// instance's transform by temporal index, stamped with the frame that wrote
// it. Static instances keep their transform, so the object motion that reads
// the history draws only dynamic ones.
struct VkrMetalPacketTemporalTransformRoot {
  device const VkrMetalPacketInstance *instances;
  device VkrTemporalTransform *transforms;
  uint instance_count;
  uint transform_capacity;
  uint frame_index;
  uint first_instance;
};

kernel void vkr_metal_packet_temporal_transform(
    constant VkrMetalPacketTemporalTransformRoot &root [[buffer(0)]],
    uint index [[thread_position_in_grid]]) {
  if (index >= root.instance_count)
    return;
  const device VkrMetalPacketInstance &instance =
      root.instances[root.first_instance + index];
  if ((instance.temporal_flags & VKR_METAL_INSTANCE_TEMPORAL_OWNER) == 0u ||
      instance.temporal_index >= root.transform_capacity)
    return;
  VkrTemporalTransform transform;
  transform.model = instance.model;
  transform.generation = instance.temporal_generation;
  transform.frame_index = root.frame_index;
  transform.valid = 1u;
  transform.reserved = 0u;
  root.transforms[instance.temporal_index] = transform;
}

static_assert(sizeof(VkrMetalPacketTemporalTransformRoot) == 32,
              "Metal temporal transform root ABI drift");

// The light behind glass from the frame's refraction copy (ADR-087): at the
// projection of the point the light leaves the glass from, and at the level
// its roughness picks.
static float3
vkr_metal_tiled_refracted_light(constant VkrMetalPacketFrameRoot *frame,
                                float3 behind_position, float roughness,
                                float ior) {
  constexpr sampler refraction_sampler(coord::normalized,
                                       address::clamp_to_edge, filter::linear,
                                       mip_filter::linear);
  const float2 uv = saturate(vkr_transmission_project_uv(
      float4(behind_position, 1.0f) * frame->view_projection, -1.0f));
  const float lod = vkr_transmission_rough_lod(
      roughness, ior, frame->transmission_source.get_num_mip_levels());
  return frame->transmission_source.sample(refraction_sampler, uv, level(lod))
      .rgb;
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
// (vkr_transmission_compose). Smooth thin glass takes the light straight
// behind it through the hardware blend, so stacked panes compose in draw
// order. A volume refracts and absorbs the light behind it, and rough glass
// blurs it: on frames with either in view, a half-resolution copy of the
// opaque image, reduced to six levels, stands in for that light, and the
// glass's roughness selects the level (ADR-087). The copy holds no blended
// surface, so such glass hides the panes behind it.
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
      vkr_metal_tiled_surface(input, frame, material, visible.material_index,
                              vkr_metal_tiled_base(input, frame, material));
  float transmission = saturate(material.material_alpha.y);
  float alpha = transmission > 0.0f ? 1.0f : surface.base.a;
  if (alpha <= 1e-4f)
    discard_fragment();
  // The transmission texture scales the material's factor (red), and the
  // thickness texture its volume (green), as the desktop transmission does.
  float thickness = 0.0f;
  if (transmission > 0.0f) {
    const device VkrMetalPacketTransmissionMaterial &volume =
        frame->transmission_materials[visible.material_index];
    if ((material.flags & 8u) != 0u)
      transmission *=
          volume.transmission_texture
              .sample(volume.transmission_sampler, input.texcoord)
              .r;
    thickness = max(material.material_alpha.w, 0.0f);
    if (thickness > 0.0f && (material.flags & 16u) != 0u)
      thickness *=
          volume.thickness_texture.sample(volume.thickness_sampler,
                                          input.texcoord)
              .g;
  }
  VkrMetalTiledBlendOutput output;
  if (Lighting == VKR_METAL_TILED_LIGHTING_INSPECT) {
    float3 inspected;
    if (vkr_metal_tiled_inspect(input, frame, material, surface, barycentric,
                                front_facing, visible.state_flags, inspected)) {
      output.color = float4(inspected * alpha, 0.0f);
      output.behind = float4(1.0f - alpha);
      return output;
    }
  }
  VkrMetalTiledSurfaceLight light = vkr_metal_tiled_shade<Lighting>(
      input, frame, material, surface, front_facing,
      Probes ? vkr_metal_tiled_pixel_probe(
                   frame, vkr_metal_tiled_lighting_position(
                              frame, visible.instance_index,
                              input.world_position))
             : 0u,
      visible.instance_index);
  // The light behind a volume or rough glass, when this frame has the copy.
  bool refracted = false;
  float3 refracted_light = float3(0.0f);
  if (transmission > 0.0f) {
    VkrTransmissionLobes lobes = {light.diffuse, light.specular,
                                  light.emissive};
    VkrTransmissionLobes no_lobes = {float3(0.0f), float3(0.0f),
                                     float3(0.0f)};
    output.color = float4(vkr_transmission_compose(
                              lobes, float3(0.0f), light.base,
                              light.reflectance, transmission, light.metallic),
                          0.0f);
    // A volume bends the view ray into the pane and absorbs the light behind
    // it along that path through its thickness (Beer-Lambert). Rough thin
    // glass (VKR_METAL_PACKET_MATERIAL_ROUGH) blurs the light straight behind
    // it.
    float3 absorption = float3(1.0f);
    const bool copy = !is_null_texture(frame->transmission_source);
    if (thickness > 0.0f) {
      const device VkrMetalPacketInstance &instance =
          frame->instances[visible.instance_index];
      VkrTransmissionExit exit = vkr_transmission_exit_point(
          input.world_position,
          input.world_position +
              vkr_metal_packet_view_direction(frame, input.world_position),
          normalize(input.world_normal) * (front_facing ? 1.0f : -1.0f),
          instance.model[0].xyz, instance.model[1].xyz, instance.model[2].xyz,
          material.material_alpha.z, thickness);
      const float4 attenuation = material.material_attenuation_color;
      if (attenuation.w > 1e-4f)
        absorption = pow(clamp(attenuation.rgb, 1e-4f, 1.0f),
                         exit.path_length / attenuation.w);
      if (copy) {
        refracted_light = vkr_metal_tiled_refracted_light(
            frame, exit.position, surface.feedback_roughness,
            material.material_alpha.z);
        refracted = true;
      }
    } else if (copy && (material.flags & 4096u) != 0u) {
      refracted_light = vkr_metal_tiled_refracted_light(
          frame, input.world_position, surface.feedback_roughness,
          material.material_alpha.z);
      refracted = true;
    }
    output.behind = float4(vkr_transmission_compose(
                               no_lobes, absorption, light.base,
                               light.reflectance, transmission, light.metallic),
                           1.0f);
  } else {
    output.color =
        float4((light.diffuse + light.specular + light.emissive) * alpha, 0.0f);
    output.behind = float4(1.0f - alpha);
  }
  output.color.rgb = vkr_metal_tiled_atmosphere(
      frame, output.color.rgb, input.world_position, output.behind.rgb);
  // The copy was taken after the atmosphere draw, so the refracted light
  // takes the share the pane keeps after the media rather than through them.
  if (refracted) {
    output.color.rgb += refracted_light * output.behind.rgb;
    output.behind.rgb = float3(0.0f);
  }
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

// The moving-caster variants (ADR-108): shadowed point and spot lights, and
// the moving casters' shadows of the baked lamps, for frames that select any.

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_casters_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_CASTERS, false, false, false>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_casters_probes_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_CASTERS, false, true, false>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_casters_decals_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_CASTERS, false, false, true>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_casters_probes_decals_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_CASTERS, false, true, true>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_casters_coverage_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_CASTERS, true, false, false>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_casters_coverage_probes_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_CASTERS, true, true, false>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_casters_coverage_decals_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_CASTERS, true, false, true>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_casters_coverage_probes_decals_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_CASTERS, true, true, true>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledBlendOutput vkr_metal_tiled_blend_casters_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_blend<VKR_METAL_TILED_LIGHTING_CASTERS, false>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledBlendOutput vkr_metal_tiled_blend_casters_probes_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_blend<VKR_METAL_TILED_LIGHTING_CASTERS, true>(
      input, root, front_facing, float3(0.0f));
}

// The moving-caster variants with rectangle lights, for frames that select
// baked lamps and have rectangle lights.

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_casters_all_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_CASTERS_ALL, false, false, false>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_casters_all_probes_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_CASTERS_ALL, false, true, false>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_casters_all_decals_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_CASTERS_ALL, false, false, true>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_casters_all_probes_decals_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_CASTERS_ALL, false, true, true>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_casters_all_coverage_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_CASTERS_ALL, true, false, false>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_casters_all_coverage_probes_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_CASTERS_ALL, true, true, false>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_casters_all_coverage_decals_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_CASTERS_ALL, true, false, true>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledForwardOutput vkr_metal_tiled_forward_casters_all_coverage_probes_decals_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_CASTERS_ALL, true, true, true>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledBlendOutput vkr_metal_tiled_blend_casters_all_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_blend<VKR_METAL_TILED_LIGHTING_CASTERS_ALL, false>(
      input, root, front_facing, float3(0.0f));
}

fragment VkrMetalTiledBlendOutput vkr_metal_tiled_blend_casters_all_probes_fragment(
    VkrMetalTiledVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_tiled_blend<VKR_METAL_TILED_LIGHTING_CASTERS_ALL, true>(
      input, root, front_facing, float3(0.0f));
}
