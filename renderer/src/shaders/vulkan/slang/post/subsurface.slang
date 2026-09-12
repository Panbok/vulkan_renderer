struct VkrVkSubsurfaceRoot {
  VkrSubsurfaceGpuParams params;
  float4x4 inverse_view_projection;
  VkrVulkanPacketFrameRoot *frame;
  uint2 frame_padding;
  VkrGpuVisibleDrawRow *visible_rows;
  uint2 visible_padding;
  uint hdr;
  uint source;
  uint depth;
  uint normal;
  uint vbuffer;
  uint profile_bank;
  uint albedo;
  uint specular;
  uint clearcoat;
  uint sheen;
  uint anisotropy;
  uint destination;
  uint source_sampler;
  uint reserved[3];
};
struct VkrVkSubsurfacePush {
  VkrVkSubsurfaceRoot *root;
  uint2 reserved;
};
[[vk::push_constant]] ConstantBuffer<VkrVkSubsurfacePush> g_subsurface_push;

float3 vkr_vk_subsurface_world(VkrVkSubsurfaceRoot *root, uint2 pixel,
                               float depth) {
  float2 ndc =
      vkr_vk_resolve_ndc(float2(pixel) + 0.5f, root.params.dimensions.xy);
  float4 world = mul(root.inverse_view_projection, float4(ndc, depth, 1.0f));
  return world.xyz / max(abs(world.w), 1e-7f) * sign(world.w);
}
float vkr_vk_subsurface_roughness(VkrVkSubsurfaceRoot *root, uint2 pixel,
                                  uint visible, float3 normal,
                                  float roughness) {
  uint2 limit = root.params.dimensions.xy - 1u;
  uint2 px = min(pixel + uint2(1u, 0u), limit),
        py = min(pixel + uint2(0u, 1u), limit);
  float3 nx = vkr_vk_octahedral_decode(
             g_textures[NonUniformResourceIndex(root.normal)]
                 .Load(int3(px, 0))
                 .xy),
         ny = vkr_vk_octahedral_decode(
             g_textures[NonUniformResourceIndex(root.normal)]
                 .Load(int3(py, 0))
                 .xy);
  float3 dx =
      (nx - normal) * (g_uint2_textures[NonUniformResourceIndex(root.vbuffer)]
                                   .Load(int3(px, 0))
                                   .x == visible
                           ? 1.0f
                           : 0.0f);
  float3 dy =
      (ny - normal) * (g_uint2_textures[NonUniformResourceIndex(root.vbuffer)]
                                   .Load(int3(py, 0))
                                   .x == visible
                           ? 1.0f
                           : 0.0f);
  return vkr_ggx_filter_roughness(roughness,
                                  0.25f * (dot(dx, dx) + dot(dy, dy)));
}
float3 vkr_vk_subsurface_receiver(VkrVkSubsurfaceRoot *root, uint2 pixel,
                                  uint visible, float3 world, float3 normal) {
  float3 view = normalize(root.frame.view_position.xyz - world);
  float4 specular =
      g_textures[NonUniformResourceIndex(root.specular)].Load(int3(pixel, 0));
  float roughness = vkr_vk_subsurface_roughness(root, pixel, visible, normal,
                                                clamp(specular.w, 0.04f, 1.0f));
  VkrGgxMaterialEnergy energy = packet_prepare_gbuffer_brdf(
      root.frame, normal, view, roughness, saturate(specular.rgb),
      g_textures[NonUniformResourceIndex(root.anisotropy)].Load(
          int3(pixel, 0)));
  float3 boundary = energy.diffuse_weight;
  float4 coat =
      g_textures[NonUniformResourceIndex(root.clearcoat)].Load(int3(pixel, 0));
  if (vkr_clearcoat_active(coat.x)) {
    VkrClearcoatLayer layer = packet_prepare_clearcoat(
        root.frame, coat.x, coat.y, vkr_vk_octahedral_decode(coat.zw), view);
    boundary *= layer.base_transmission;
  }
  float4 sheen =
      g_textures[NonUniformResourceIndex(root.sheen)].Load(int3(pixel, 0));
  if (vkr_sheen_active(sheen.rgb)) {
    VkrSheenLayer layer =
        packet_prepare_sheen(root.frame, sheen.rgb, sheen.w, normal, view,
            packet_sheen_allocation_reserve(root.frame, max(dot(normal, view), 0.0f), sheen.w));
    boundary *= layer.base_transmission;
  }
  return boundary * vkr_subsurface_root_amplitude(
                        g_textures[NonUniformResourceIndex(root.albedo)]
                            .Load(int3(pixel, 0))
                            .rgb);
}
[shader("compute")][numthreads(8, 8, 1)] void
vk_subsurface_gather(uint3 thread_id : SV_DispatchThreadID) {
  VkrVkSubsurfaceRoot *root = g_subsurface_push.root;
  uint2 pixel = thread_id.xy;
  if (any(pixel >= root.params.dimensions.xy))
    return;
  float4 original =
      g_textures[NonUniformResourceIndex(root.hdr)].Load(int3(pixel, 0));
  if (root.profile_bank == 0u) {
    g_storage_2d[NonUniformResourceIndex(root.destination)][pixel] = original;
    return;
  }
  uint visible = g_uint2_textures[NonUniformResourceIndex(root.vbuffer)]
                     .Load(int3(pixel, 0))
                     .x;
  if (visible == 0u) {
    g_storage_2d[NonUniformResourceIndex(root.destination)][pixel] = original;
    return;
  }
  VkrGpuVisibleDrawRow center_row = root.visible_rows[visible - 1u];
  float4 material =
      root.frame.materials[center_row.material_index].material_subsurface;
  if (material.x <= 0.0f || uint(material.y) >= root.params.dimensions.z) {
    g_storage_2d[NonUniformResourceIndex(root.destination)][pixel] = original;
    return;
  }
  float3 center_source =
      g_textures[NonUniformResourceIndex(root.source)].Load(int3(pixel, 0)).rgb;
  float depth = g_depth_textures[NonUniformResourceIndex(root.depth)].Load(
      int3(pixel, 0));
  float3 world = vkr_vk_subsurface_world(root, pixel, depth);
  float3 normal = vkr_vk_octahedral_decode(
      g_textures[NonUniformResourceIndex(root.normal)].Load(int3(pixel, 0)).xy);
  float3 receiver =
      vkr_vk_subsurface_receiver(root, pixel, visible, world, normal);
  uint profile = uint(material.y);
  float4 profile_data =
      g_textures[root.profile_bank].Load(int3(uint2(64u, profile), 0));
  float largest = max(profile_data.x, max(profile_data.y, profile_data.z));
  float view_depth =
      root.params.projection.x * root.params.projection.y /
      (root.params.projection.y -
       depth * (root.params.projection.y - root.params.projection.x));
  float2 pixels_per_metre = root.params.projection.zw / view_depth;
  float largest_pixel_scale = max(pixels_per_metre.x, pixels_per_metre.y);
  float cap_scale =
      min(largest, 32.0f / (profile_data.w * largest_pixel_scale));
  float2 pixel_scale = cap_scale * pixels_per_metre;
  // One pixel of plane tolerance covers raster-depth quantization on slopes;
  // the profile term bounds sharing between nearby parallel surfaces.
  float tolerance = max(largest * 0.5f, 1.0f / largest_pixel_scale);
  float3 filtered = float3(0.0f), weights = float3(0.0f);
  for (uint tap = 0u; tap < 32u; ++tap) {
    float4 row0 =
        g_textures[root.profile_bank].Load(int3(uint2(2u * tap, profile), 0));
    float4 row1 = g_textures[root.profile_bank].Load(
        int3(uint2(2u * tap + 1u, profile), 0));
    float2 position = float2(pixel) + row0.xy * pixel_scale;
    int2 base = int2(floor(position));
    float guide = 1.0f;
    float coupling = material.x;
    for (int y = 0; y < 2; ++y) {
      for (int x = 0; x < 2; ++x) {
        int2 signed_q = base + int2(x, y);
        if (any(signed_q < int2(0)) ||
            any(signed_q >= int2(root.params.dimensions.xy))) {
          guide = 0.0f;
          continue;
        }
        uint2 q = uint2(signed_q);
        uint tap_visible =
            g_uint2_textures[NonUniformResourceIndex(root.vbuffer)]
                .Load(int3(q, 0))
                .x;
        if (tap_visible == 0u) {
          guide = 0.0f;
          continue;
        }
        VkrGpuVisibleDrawRow tap_row = root.visible_rows[tap_visible - 1u];
        float4 tap_material =
            root.frame.materials[tap_row.material_index].material_subsurface;
        if (tap_row.instance_index != center_row.instance_index ||
            uint(tap_material.y) != profile) {
          guide = 0.0f;
          continue;
        }
        coupling = min(coupling, tap_material.x);
        float3 tap_normal = vkr_vk_octahedral_decode(
            g_textures[NonUniformResourceIndex(root.normal)]
                .Load(int3(q, 0))
                .xy);
        float3 tap_world = vkr_vk_subsurface_world(
            root, q,
            g_depth_textures[NonUniformResourceIndex(root.depth)].Load(
                int3(q, 0)));
        guide =
            min(guide, vkr_subsurface_surface_weight(world, tap_world, normal,
                                                     tap_normal, tolerance));
      }
    }
    if (guide <= 0.0f)
      continue;
    float3 weight = float3(row0.z, row0.w, row1.x) * guide;
    float2 uv = (position + 0.5f) / float2(root.params.dimensions.xy);
    filtered += (g_textures[NonUniformResourceIndex(root.source)]
                         .SampleLevel(g_samplers[root.source_sampler], uv, 0.0f)
                         .rgb *
                     coupling -
                 center_source * material.x) *
                weight;
    weights += weight;
  }
  // Per-channel fallback handles a rejected narrow profile without borrowing
  // another channel's mass. Constant fields cancel before HDR composition.
  float3 average = float3(weights.x > 0.0f ? filtered.x / weights.x : 0.0f,
                          weights.y > 0.0f ? filtered.y / weights.y : 0.0f,
                          weights.z > 0.0f ? filtered.z / weights.z : 0.0f);
  g_storage_2d[NonUniformResourceIndex(root.destination)][pixel] =
      float4(original.rgb + receiver * average, original.a);
}
