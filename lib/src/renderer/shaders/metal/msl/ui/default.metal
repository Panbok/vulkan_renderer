struct VkrMetalPacketUiRoot {
  device VkrMetalPacketTextVertex *vertices;
  texture2d<float, access::sample> texture;
  float4 target_unit_range;
  float2 rect_extent;
  uint mode;
  uint flags;
  float4 corner_radii;
};

struct VkrMetalPacketUiOutput {
  float4 position [[position]];
  float2 texcoord [[user(TEXCOORD)]];
  float4 color [[user(COLOR)]];
};

static_assert(sizeof(VkrMetalPacketUiRoot) == 64,
              "VkrMetalPacketUiRoot must remain 64 bytes");

VkrMetalPacketUiOutput vkr_metal_packet_ui_vertex_output(
    uint vertex_id, constant VkrMetalPacketUiRoot *root) {
  VkrMetalPacketTextVertex vertex_data = root->vertices[vertex_id];
  float2 target = max(root->target_unit_range.xy, float2(1.0));
  VkrMetalPacketUiOutput output;
  output.position =
      float4(vertex_data.position.x / target.x * 2.0 - 1.0,
             vertex_data.position.y / target.y * 2.0 - 1.0, 0.0, 1.0);
  output.texcoord = vertex_data.texcoord;
  output.color = vertex_data.color;
  return output;
}

vertex VkrMetalPacketUiOutput vkr_metal_packet_ui_vertex(
    uint vertex_id [[vertex_id]],
    constant VkrMetalPacketUiRoot *root [[buffer(1)]]) {
  return vkr_metal_packet_ui_vertex_output(vertex_id, root);
}

fragment float4 vkr_metal_packet_ui_fragment(
    VkrMetalPacketUiOutput input [[stage_in]],
    constant VkrMetalPacketUiRoot *root [[buffer(1)]]) {
  constexpr sampler ui_sampler(coord::normalized, address::clamp_to_edge,
                               filter::linear);
  if (root->mode == 0u) {
    if ((root->flags & 1u) == 0u)
      return input.color;
    return input.color * root->texture.sample(ui_sampler, input.texcoord);
  }

  float4 atlas = root->texture.sample(ui_sampler, input.texcoord);
  float alpha = atlas.a;
  if (root->mode == 1u) {
    float2 dx = dfdx(input.texcoord);
    float2 dy = dfdy(input.texcoord);
    float2 gradient_squared =
        max(dx * dx + dy * dy, float2(1e-12, 1e-12));
    float2 screen_tex_size = rsqrt(gradient_squared);
    float range =
        max(0.5 * dot(root->target_unit_range.zw, screen_tex_size), 1.0);
    float signed_distance =
        max(min(atlas.r, atlas.g), min(max(atlas.r, atlas.g), atlas.b)) - 0.5;
    alpha = saturate(range * signed_distance + 0.5);
  }
  return float4(input.color.rgb, input.color.a * alpha);
}

vertex VkrMetalPacketUiOutput vkr_metal_packet_ui_rect_vertex(
    uint vertex_id [[vertex_id]],
    constant VkrMetalPacketUiRoot *root [[buffer(1)]]) {
  return vkr_metal_packet_ui_vertex_output(vertex_id, root);
}

fragment float4 vkr_metal_packet_ui_rect_fragment(
    VkrMetalPacketUiOutput input [[stage_in]],
    constant VkrMetalPacketUiRoot *root [[buffer(1)]]) {
  float2 local = input.texcoord * root->rect_extent;
  float2 centered = local - root->rect_extent * 0.5;
  bool left = centered.x < 0.0;
  bool top = centered.y < 0.0;
  float radius = left ? (top ? root->corner_radii.x : root->corner_radii.w)
                      : (top ? root->corner_radii.y : root->corner_radii.z);
  float2 q = abs(centered) - (root->rect_extent * 0.5 - radius);
  float distance = length(max(q, float2(0.0))) + min(max(q.x, q.y), 0.0) -
                   radius;
  float edge = max(fwidth(distance), 1e-4);
  float alpha = saturate(0.5 - distance / edge);
  return float4(input.color.rgb, input.color.a * alpha);
}
