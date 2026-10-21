// View streaming (ADR-131): resamples the frame's finished Scene image into
// the BGRA8 video surface an encoder reads, display-encoded as sRGB. One
// thread per surface pixel filters the Scene bilinearly at the pixel's
// centre, so a surface of the Scene's size receives each Scene pixel. A
// display-encoded source (8-bit UNORM) is copied as stored; any other source
// reads linear values, which are clamped to [0, 1] and encoded.

struct VkrMetalPacketViewExportRoot {
  texture2d<float, access::sample> source;
  texture2d<float, access::write> destination;
  uint2 extent;
  // 1 when the source stores display-encoded values.
  uint source_encoded;
  uint reserved;
};

static_assert(sizeof(VkrMetalPacketViewExportRoot) == 32,
              "Metal view export root must remain 32 bytes");

static float3 vkr_view_export_encode_srgb(float3 linear) {
  const float3 low = linear * 12.92f;
  const float3 high = 1.055f * pow(linear, float3(1.0f / 2.4f)) - 0.055f;
  return select(high, low, linear <= float3(0.0031308f));
}

kernel void vkr_metal_packet_view_export(
    constant VkrMetalPacketViewExportRoot &root [[buffer(0)]],
    uint2 pixel [[thread_position_in_grid]]) {
  if (any(pixel >= root.extent)) {
    return;
  }
  constexpr sampler bilinear(filter::linear, address::clamp_to_edge);
  const float2 uv = (float2(pixel) + 0.5f) / float2(root.extent);
  float3 color = saturate(root.source.sample(bilinear, uv, level(0.0f)).rgb);
  if (root.source_encoded == 0u) {
    color = vkr_view_export_encode_srgb(color);
  }
  root.destination.write(float4(color, 1.0f), pixel);
}
