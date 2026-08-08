struct VkrMetalPacketTonemapRoot {
  texture2d<float, access::read> source;
  float exposure;
  uint3 reserved;
};

vertex VkrMetalPacketTonemapOutput
vkr_metal_packet_tonemap_vertex(uint vertex_id [[vertex_id]]) {
  const float2 positions[3] = {float2(-1.0, -1.0), float2(3.0, -1.0),
                               float2(-1.0, 3.0)};
  VkrMetalPacketTonemapOutput output;
  output.position = float4(positions[vertex_id], 0.0, 1.0);
  return output;
}

fragment float4 vkr_metal_packet_tonemap_fragment(
    VkrMetalPacketTonemapOutput input [[stage_in]],
    constant VkrMetalPacketTonemapRoot *root [[buffer(1)]]) {
  float4 hdr = root->source.read(uint2(input.position.xy), 0);
  float3 color = max(hdr.rgb * root->exposure, 0.0);
  if (root->reserved.x != 0u) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    color = clamp((color * (a * color + b)) / (color * (c * color + d) + e),
                  0.0, 1.0);
  } else {
    color = clamp(color, 0.0, 1.0);
  }
  return float4(color, hdr.a);
}
