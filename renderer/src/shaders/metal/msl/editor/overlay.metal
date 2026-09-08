struct VkrMetalPacketEditorOverlayRoot {
  device const VkrPackedStaticVertex *vertices;
  device const VkrGpuGeometryDecodeRecord *decode;
  float4x4 model_view_projection;
  float4 color;
  uint object_id;
  uint reserved;
  constant VkrDisplayOutputParams *display_output;
};

static_assert(sizeof(VkrMetalPacketEditorOverlayRoot) == 112,
              "Metal editor overlay root must remain 112 bytes");

struct VkrMetalPacketEditorOverlayOutput {
  float4 position [[position]];
  float4 color [[user(COLOR)]];
  uint object_id [[user(OBJECT_ID), flat]];
};

vertex VkrMetalPacketEditorOverlayOutput vkr_metal_packet_editor_overlay_vertex(
    uint vertex_id [[vertex_id]],
    constant VkrMetalPacketEditorOverlayRoot *root [[buffer(1)]]) {
  VkrGpuDecodedVertex decoded =
      vkr_decode_packed_vertex(root->vertices[vertex_id], *root->decode);
  VkrMetalPacketEditorOverlayOutput output;
  output.position = root->model_view_projection * float4(decoded.position, 1.0);
  output.color = root->color;
  output.object_id = root->object_id;
  return output;
}

fragment float4 vkr_metal_packet_editor_overlay_fragment(
    VkrMetalPacketEditorOverlayOutput input [[stage_in]],
    constant VkrMetalPacketEditorOverlayRoot *root [[buffer(1)]]) {
  input.color.rgb = vkr_display_output_ui(input.color.rgb, *root->display_output);
  return input.color;
}

fragment uint vkr_metal_packet_editor_overlay_picking_fragment(
    VkrMetalPacketEditorOverlayOutput input [[stage_in]]) {
  return input.object_id;
}
