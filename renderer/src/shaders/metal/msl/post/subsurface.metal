struct VkrMetalPacketSubsurfaceRoot {
  VkrSubsurfaceGpuParams params;
  float4x4 inverse_view_projection;
  constant VkrMetalPacketFrameRoot *frame;
  device VkrGpuVisibleDrawRow *visible_rows;
  texture2d<float, access::read> hdr;
  texture2d<float, access::sample> source;
  texture2d<float, access::read> depth;
  texture2d<float, access::read> normal;
  texture2d<uint, access::read> vbuffer;
  texture2d<float, access::read> profile_bank;
  texture2d<float, access::read> albedo;
  texture2d<float, access::read> specular;
  texture2d<float, access::read> clearcoat;
  texture2d<float, access::read> sheen;
  texture2d<float, access::read> anisotropy;
  texture2d<float, access::write> destination;
};
constexpr sampler vkr_subsurface_linear(coord::normalized,
                                        address::clamp_to_edge, filter::linear);

float3 vkr_metal_subsurface_world(constant VkrMetalPacketSubsurfaceRoot &root,
                                  uint2 pixel, float depth) {
  float2 ndc = vkr_metal_packet_resolve_ndc(float2(pixel) + 0.5f,
                                            root.params.dimensions.xy);
  float4 world = (root.inverse_view_projection * float4(ndc, depth, 1.0f));
  return world.xyz / max(abs(world.w), 1e-7f) * sign(world.w);
}
float vkr_metal_subsurface_roughness(
    constant VkrMetalPacketSubsurfaceRoot &root, uint2 pixel, uint visible,
    float3 normal, float roughness) {
  uint2 limit = root.params.dimensions.xy - 1u;
  uint2 px = min(pixel + uint2(1u, 0u), limit),
        py = min(pixel + uint2(0u, 1u), limit);
  float3 nx = vkr_metal_packet_octahedral_decode(root.normal.read(px).xy),
         ny = vkr_metal_packet_octahedral_decode(root.normal.read(py).xy);
  float3 dx =
      (nx - normal) * (root.vbuffer.read(px).x == visible ? 1.0f : 0.0f);
  float3 dy =
      (ny - normal) * (root.vbuffer.read(py).x == visible ? 1.0f : 0.0f);
  return vkr_ggx_filter_roughness(roughness,
                                  0.25f * (dot(dx, dx) + dot(dy, dy)));
}
float3
vkr_metal_subsurface_receiver(constant VkrMetalPacketSubsurfaceRoot &root,
                              uint2 pixel, uint visible, float3 world,
                              float3 normal) {
  float3 view = normalize(root.frame->view_position.xyz - world);
  float4 specular = root.specular.read(pixel);
  float roughness = vkr_metal_subsurface_roughness(
      root, pixel, visible, normal, clamp(specular.w, 0.04f, 1.0f));
  VkrGgxMaterialEnergy energy = vkr_metal_prepare_gbuffer_brdf(
      root.frame, normal, view, roughness, saturate(specular.rgb),
      root.anisotropy.read(pixel));
  float3 boundary = energy.diffuse_weight;
  float4 coat = root.clearcoat.read(pixel);
  if (vkr_clearcoat_active(coat.x)) {
    VkrClearcoatLayer layer = vkr_metal_packet_prepare_clearcoat(
        root.frame, coat.x, coat.y, vkr_metal_packet_octahedral_decode(coat.zw),
        view);
    boundary *= layer.base_transmission;
  }
  float4 sheen = root.sheen.read(pixel);
  if (vkr_sheen_active(sheen.rgb)) {
    VkrSheenLayer layer = vkr_metal_packet_prepare_sheen(root.frame, sheen.rgb,
                                                         sheen.w, normal, view);
    boundary *= layer.base_transmission;
  }
  return boundary * vkr_subsurface_root_amplitude(root.albedo.read(pixel).rgb);
}
kernel void
vkr_metal_packet_subsurface_gather(constant VkrMetalPacketSubsurfaceRoot &root
                                   [[buffer(0)]],
                                   uint2 pixel [[thread_position_in_grid]]) {
  if (any(pixel >= root.params.dimensions.xy))
    return;
  float4 original = root.hdr.read(pixel);
  uint visible = root.vbuffer.read(pixel).x;
  if (visible == 0u) {
    root.destination.write(original, pixel);
    return;
  }
  VkrGpuVisibleDrawRow center_row = root.visible_rows[visible - 1u];
  float4 material =
      root.frame->materials[center_row.material_index].material_subsurface;
  if (material.x <= 0.0f || uint(material.y) >= root.params.dimensions.z) {
    root.destination.write(original, pixel);
    return;
  }
  float3 center_source = root.source.read(pixel).rgb;
  float depth = root.depth.read(pixel).x;
  float3 world = vkr_metal_subsurface_world(root, pixel, depth);
  float3 normal =
      vkr_metal_packet_octahedral_decode(root.normal.read(pixel).xy);
  float3 receiver =
      vkr_metal_subsurface_receiver(root, pixel, visible, world, normal);
  uint profile = uint(material.y);
  float4 profile_data = root.profile_bank.read(uint2(64u, profile));
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
    float4 row0 = root.profile_bank.read(uint2(2u * tap, profile));
    float4 row1 = root.profile_bank.read(uint2(2u * tap + 1u, profile));
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
        uint tap_visible = root.vbuffer.read(q).x;
        if (tap_visible == 0u) {
          guide = 0.0f;
          continue;
        }
        VkrGpuVisibleDrawRow tap_row = root.visible_rows[tap_visible - 1u];
        float4 tap_material =
            root.frame->materials[tap_row.material_index].material_subsurface;
        if (tap_row.instance_index != center_row.instance_index ||
            uint(tap_material.y) != profile) {
          guide = 0.0f;
          continue;
        }
        coupling = min(coupling, tap_material.x);
        float3 tap_normal =
            vkr_metal_packet_octahedral_decode(root.normal.read(q).xy);
        float3 tap_world =
            vkr_metal_subsurface_world(root, q, root.depth.read(q).x);
        guide =
            min(guide, vkr_subsurface_surface_weight(world, tap_world, normal,
                                                     tap_normal, tolerance));
      }
    }
    if (guide <= 0.0f)
      continue;
    float3 weight = float3(row0.z, row0.w, row1.x) * guide;
    float2 uv = (position + 0.5f) / float2(root.params.dimensions.xy);
    filtered += (root.source.sample(vkr_subsurface_linear, uv).rgb * coupling -
                 center_source * material.x) *
                weight;
    weights += weight;
  }
  // Per-channel fallback handles a rejected narrow profile without borrowing
  // another channel's mass. Constant fields cancel before HDR composition.
  float3 average = float3(weights.x > 0.0f ? filtered.x / weights.x : 0.0f,
                          weights.y > 0.0f ? filtered.y / weights.y : 0.0f,
                          weights.z > 0.0f ? filtered.z / weights.z : 0.0f);
  root.destination.write(float4(original.rgb + receiver * average, original.a),
                         pixel);
}

static_assert(sizeof(VkrSubsurfaceGpuParams) == 32, "Subsurface params ABI");
static_assert(sizeof(VkrMetalPacketSubsurfaceRoot) == 208,
              "Subsurface root ABI");
