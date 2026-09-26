/* Selection outline: a full-screen pass over the Scene image that colors
 * pixels just outside the selection mask. The mask shares the Scene image
 * extent, so fragment and mask pixels coincide. */
struct alignas(16) VkrMetalPacketSelectionOutlineRoot {
  texture2d<float, access::read> mask;
  float4 color;
  uint radius_px;
  uint reserved;
  constant VkrDisplayOutputParams *display_output;
};

static_assert(sizeof(VkrMetalPacketSelectionOutlineRoot) == 48,
              "Metal selection outline root must remain 48 bytes");

struct VkrMetalPacketSelectionOutlineOutput {
  float4 position [[position]];
};

vertex VkrMetalPacketSelectionOutlineOutput
vkr_metal_packet_selection_outline_vertex(uint vertex_id [[vertex_id]]) {
  const float2 positions[3] = {float2(-1.0, -1.0), float2(3.0, -1.0),
                               float2(-1.0, 3.0)};
  VkrMetalPacketSelectionOutlineOutput output;
  output.position = float4(positions[vertex_id], 0.0, 1.0);
  return output;
}

/* Twelve taps reach two pixels: the four axis neighbors at one and two
 * pixels plus the four diagonals. Inside pixels keep the scene unchanged. */
fragment float4 vkr_metal_packet_selection_outline_fragment(
    VkrMetalPacketSelectionOutlineOutput input [[stage_in]],
    constant VkrMetalPacketSelectionOutlineRoot *root [[buffer(1)]]) {
  const int2 extent =
      int2(int(root->mask.get_width()), int(root->mask.get_height()));
  const int2 pixel = int2(input.position.xy);
  if (any(pixel >= extent) || root->mask.read(uint2(pixel)).r > 0.5)
    discard_fragment();
  const int reach = int(max(root->radius_px, 1u));
  const int2 offsets[12] = {int2(1, 0),  int2(-1, 0), int2(0, 1),
                            int2(0, -1), int2(1, 1),  int2(-1, 1),
                            int2(1, -1), int2(-1, -1), int2(2, 0),
                            int2(-2, 0), int2(0, 2),  int2(0, -2)};
  float coverage = 0.0;
  for (uint i = 0; i < 12u; ++i) {
    const int2 offset = offsets[i] * max(reach / 2, 1);
    const int2 sample_pixel = clamp(pixel + offset, int2(0), extent - 1);
    coverage = max(coverage, root->mask.read(uint2(sample_pixel)).r);
  }
  if (coverage <= 0.0)
    discard_fragment();
  float4 color = root->color;
  color.rgb = vkr_display_output_ui(color.rgb, *root->display_output);
  color.a *= coverage;
  return color;
}
